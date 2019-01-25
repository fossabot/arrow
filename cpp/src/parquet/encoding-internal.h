// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "arrow/util/bit-stream-utils.h"
#include "arrow/util/bit-util.h"
#include "arrow/util/hashing.h"
#include "arrow/util/macros.h"
#include "arrow/util/rle-encoding.h"

#include "parquet/decoding.h"
#include "parquet/encoding.h"
#include "parquet/exception.h"
#include "parquet/schema.h"
#include "parquet/types.h"
#include "parquet/util/memory.h"

namespace parquet {

namespace BitUtil = ::arrow::BitUtil;

class ColumnDescriptor;

// ----------------------------------------------------------------------
// Dictionary encoding and decoding

template <typename Type>
class DictionaryDecoder : public TypedDecoder<Type> {
 public:
  typedef typename Type::c_type T;

  // Initializes the dictionary with values from 'dictionary'. The data in
  // dictionary is not guaranteed to persist in memory after this call so the
  // dictionary decoder needs to copy the data out if necessary.
  explicit DictionaryDecoder(const ColumnDescriptor* descr,
                             ::arrow::MemoryPool* pool = ::arrow::default_memory_pool())
      : TypedDecoder<Type>(descr, Encoding::RLE_DICTIONARY),
        dictionary_(0, pool),
        byte_array_data_(AllocateBuffer(pool, 0)) {}

  // Perform type-specific initiatialization
  void SetDict(TypedDecoder<Type>* dictionary);

  void SetData(int num_values, const uint8_t* data, int len) override {
    num_values_ = num_values;
    if (len == 0) return;
    uint8_t bit_width = *data;
    ++data;
    --len;
    idx_decoder_ = ::arrow::util::RleDecoder(data, len, bit_width);
  }

  int Decode(T* buffer, int max_values) override {
    max_values = std::min(max_values, num_values_);
    int decoded_values =
        idx_decoder_.GetBatchWithDict(dictionary_.data(), buffer, max_values);
    if (decoded_values != max_values) {
      ParquetException::EofException();
    }
    num_values_ -= max_values;
    return max_values;
  }

  int DecodeSpaced(T* buffer, int num_values, int null_count, const uint8_t* valid_bits,
                   int64_t valid_bits_offset) override {
    int decoded_values =
        idx_decoder_.GetBatchWithDictSpaced(dictionary_.data(), buffer, num_values,
                                            null_count, valid_bits, valid_bits_offset);
    if (decoded_values != num_values) {
      ParquetException::EofException();
    }
    return decoded_values;
  }

 private:
  using TypedDecoder<Type>::num_values_;

  // Only one is set.
  Vector<T> dictionary_;

  // Data that contains the byte array data (byte_array_dictionary_ just has the
  // pointers).
  std::shared_ptr<ResizableBuffer> byte_array_data_;

  ::arrow::util::RleDecoder idx_decoder_;
};

template <typename Type>
inline void DictionaryDecoder<Type>::SetDict(TypedDecoder<Type>* dictionary) {
  int num_dictionary_values = dictionary->values_left();
  dictionary_.Resize(num_dictionary_values);
  dictionary->Decode(dictionary_.data(), num_dictionary_values);
}

template <>
inline void DictionaryDecoder<BooleanType>::SetDict(
    TypedDecoder<BooleanType>* dictionary) {
  ParquetException::NYI("Dictionary encoding is not implemented for boolean values");
}

template <>
inline void DictionaryDecoder<ByteArrayType>::SetDict(
    TypedDecoder<ByteArrayType>* dictionary) {
  int num_dictionary_values = dictionary->values_left();
  dictionary_.Resize(num_dictionary_values);
  dictionary->Decode(&dictionary_[0], num_dictionary_values);

  int total_size = 0;
  for (int i = 0; i < num_dictionary_values; ++i) {
    total_size += dictionary_[i].len;
  }
  if (total_size > 0) {
    PARQUET_THROW_NOT_OK(byte_array_data_->Resize(total_size, false));
  }

  int offset = 0;
  uint8_t* bytes_data = byte_array_data_->mutable_data();
  for (int i = 0; i < num_dictionary_values; ++i) {
    memcpy(bytes_data + offset, dictionary_[i].ptr, dictionary_[i].len);
    dictionary_[i].ptr = bytes_data + offset;
    offset += dictionary_[i].len;
  }
}

template <>
inline void DictionaryDecoder<FLBAType>::SetDict(TypedDecoder<FLBAType>* dictionary) {
  int num_dictionary_values = dictionary->values_left();
  dictionary_.Resize(num_dictionary_values);
  dictionary->Decode(&dictionary_[0], num_dictionary_values);

  int fixed_len = descr_->type_length();
  int total_size = num_dictionary_values * fixed_len;

  PARQUET_THROW_NOT_OK(byte_array_data_->Resize(total_size, false));
  uint8_t* bytes_data = byte_array_data_->mutable_data();
  for (int32_t i = 0, offset = 0; i < num_dictionary_values; ++i, offset += fixed_len) {
    memcpy(bytes_data + offset, dictionary_[i].ptr, fixed_len);
    dictionary_[i].ptr = bytes_data + offset;
  }
}

// ----------------------------------------------------------------------
// Dictionary encoder

template <typename DType>
struct DictEncoderTraits {
  using c_type = typename DType::c_type;
  using MemoTableType = ::arrow::internal::ScalarMemoTable<c_type>;
};

template <>
struct DictEncoderTraits<ByteArrayType> {
  using MemoTableType = ::arrow::internal::BinaryMemoTable;
};

template <>
struct DictEncoderTraits<FLBAType> {
  using MemoTableType = ::arrow::internal::BinaryMemoTable;
};

// Initially 1024 elements
static constexpr int32_t INITIAL_HASH_TABLE_SIZE = 1 << 10;

/// See the dictionary encoding section of https://github.com/Parquet/parquet-format.
/// The encoding supports streaming encoding. Values are encoded as they are added while
/// the dictionary is being constructed. At any time, the buffered values can be
/// written out with the current dictionary size. More values can then be added to
/// the encoder, including new dictionary entries.
template <typename DType>
class DictEncoder : public TypedEncoder<DType> {
  using MemoTableType = typename DictEncoderTraits<DType>::MemoTableType;

 public:
  typedef typename DType::c_type T;

  explicit DictEncoder(const ColumnDescriptor* desc,
                       ::arrow::MemoryPool* allocator = ::arrow::default_memory_pool())
      : TypedEncoder<DType>(desc, Encoding::PLAIN_DICTIONARY, allocator),
        allocator_(allocator),
        dict_encoded_size_(0),
        type_length_(desc->type_length()),
        memo_table_(INITIAL_HASH_TABLE_SIZE) {}

  ~DictEncoder() override { DCHECK(buffered_indices_.empty()); }

  void set_type_length(int type_length) { type_length_ = type_length; }

  /// Returns a conservative estimate of the number of bytes needed to encode the buffered
  /// indices. Used to size the buffer passed to WriteIndices().
  int64_t EstimatedDataEncodedSize() override {
    // Note: because of the way RleEncoder::CheckBufferFull() is called, we have to
    // reserve
    // an extra "RleEncoder::MinBufferSize" bytes. These extra bytes won't be used
    // but not reserving them would cause the encoder to fail.
    return 1 +
           ::arrow::util::RleEncoder::MaxBufferSize(
               bit_width(), static_cast<int>(buffered_indices_.size())) +
           ::arrow::util::RleEncoder::MinBufferSize(bit_width());
  }

  /// The minimum bit width required to encode the currently buffered indices.
  int bit_width() const {
    if (ARROW_PREDICT_FALSE(num_entries() == 0)) return 0;
    if (ARROW_PREDICT_FALSE(num_entries() == 1)) return 1;
    return BitUtil::Log2(num_entries());
  }

  /// Writes out any buffered indices to buffer preceded by the bit width of this data.
  /// Returns the number of bytes written.
  /// If the supplied buffer is not big enough, returns -1.
  /// buffer must be preallocated with buffer_len bytes. Use EstimatedDataEncodedSize()
  /// to size buffer.
  int WriteIndices(uint8_t* buffer, int buffer_len);

  int dict_encoded_size() { return dict_encoded_size_; }

  /// Encode value. Note that this does not actually write any data, just
  /// buffers the value's index to be written later.
  inline void Put(const T& value);
  void Put(const T* values, int num_values) override;

  std::shared_ptr<Buffer> FlushValues() override {
    std::shared_ptr<ResizableBuffer> buffer =
        AllocateBuffer(this->allocator_, EstimatedDataEncodedSize());
    int result_size = WriteIndices(buffer->mutable_data(),
                                   static_cast<int>(EstimatedDataEncodedSize()));
    PARQUET_THROW_NOT_OK(buffer->Resize(result_size, false));
    return buffer;
  }

  void PutSpaced(const T* src, int num_values, const uint8_t* valid_bits,
                 int64_t valid_bits_offset) override {
    ::arrow::internal::BitmapReader valid_bits_reader(valid_bits, valid_bits_offset,
                                                      num_values);
    for (int32_t i = 0; i < num_values; i++) {
      if (valid_bits_reader.IsSet()) {
        Put(src[i]);
      }
      valid_bits_reader.Next();
    }
  }

  /// Writes out the encoded dictionary to buffer. buffer must be preallocated to
  /// dict_encoded_size() bytes.
  void WriteDict(uint8_t* buffer);

  /// The number of entries in the dictionary.
  int num_entries() const { return memo_table_.size(); }

 private:
  /// Clears all the indices (but leaves the dictionary).
  void ClearIndices() { buffered_indices_.clear(); }

  ::arrow::MemoryPool* allocator_;

  /// Indices that have not yet be written out by WriteIndices().
  std::vector<int> buffered_indices_;

  /// The number of bytes needed to encode the dictionary.
  int dict_encoded_size_;

  /// Size of each encoded dictionary value. -1 for variable-length types.
  int type_length_;

  MemoTableType memo_table_;
};

template <typename DType>
void DictEncoder<DType>::Put(const T* src, int num_values) {
  for (int32_t i = 0; i < num_values; i++) {
    Put(src[i]);
  }
}

template <typename DType>
inline void DictEncoder<DType>::Put(const T& v) {
  // Put() implementation for primitive types
  auto on_found = [](int32_t memo_index) {};
  auto on_not_found = [this](int32_t memo_index) {
    dict_encoded_size_ += static_cast<int>(sizeof(T));
  };

  auto memo_index = memo_table_.GetOrInsert(v, on_found, on_not_found);
  buffered_indices_.push_back(memo_index);
}

template <>
inline void DictEncoder<ByteArrayType>::Put(const ByteArray& v) {
  static const uint8_t empty[] = {0};

  auto on_found = [](int32_t memo_index) {};
  auto on_not_found = [&](int32_t memo_index) {
    dict_encoded_size_ += static_cast<int>(v.len + sizeof(uint32_t));
  };

  DCHECK(v.ptr != nullptr || v.len == 0);
  const void* ptr = (v.ptr != nullptr) ? v.ptr : empty;
  auto memo_index =
      memo_table_.GetOrInsert(ptr, static_cast<int32_t>(v.len), on_found, on_not_found);
  buffered_indices_.push_back(memo_index);
}

template <>
inline void DictEncoder<FLBAType>::Put(const FixedLenByteArray& v) {
  static const uint8_t empty[] = {0};

  auto on_found = [](int32_t memo_index) {};
  auto on_not_found = [this](int32_t memo_index) { dict_encoded_size_ += type_length_; };

  DCHECK(v.ptr != nullptr || type_length_ == 0);
  const void* ptr = (v.ptr != nullptr) ? v.ptr : empty;
  auto memo_index = memo_table_.GetOrInsert(ptr, type_length_, on_found, on_not_found);
  buffered_indices_.push_back(memo_index);
}

template <typename DType>
inline void DictEncoder<DType>::WriteDict(uint8_t* buffer) {
  // For primitive types, only a memcpy
  DCHECK_EQ(static_cast<size_t>(dict_encoded_size_), sizeof(T) * memo_table_.size());
  memo_table_.CopyValues(0 /* start_pos */, reinterpret_cast<T*>(buffer));
}

// ByteArray and FLBA already have the dictionary encoded in their data heaps
template <>
inline void DictEncoder<ByteArrayType>::WriteDict(uint8_t* buffer) {
  memo_table_.VisitValues(0, [&](const ::arrow::util::string_view& v) {
    uint32_t len = static_cast<uint32_t>(v.length());
    memcpy(buffer, &len, sizeof(uint32_t));
    buffer += sizeof(uint32_t);
    memcpy(buffer, v.data(), v.length());
    buffer += v.length();
  });
}

template <>
inline void DictEncoder<FLBAType>::WriteDict(uint8_t* buffer) {
  memo_table_.VisitValues(0, [&](const ::arrow::util::string_view& v) {
    DCHECK_EQ(v.length(), static_cast<size_t>(type_length_));
    memcpy(buffer, v.data(), type_length_);
    buffer += type_length_;
  });
}

template <typename DType>
inline int DictEncoder<DType>::WriteIndices(uint8_t* buffer, int buffer_len) {
  // Write bit width in first byte
  *buffer = static_cast<uint8_t>(bit_width());
  ++buffer;
  --buffer_len;

  ::arrow::util::RleEncoder encoder(buffer, buffer_len, bit_width());
  for (int index : buffered_indices_) {
    if (!encoder.Put(index)) return -1;
  }
  encoder.Flush();

  ClearIndices();
  return 1 + encoder.len();
}

// ----------------------------------------------------------------------
// DeltaBitPackDecoder

template <typename DType>
class DeltaBitPackDecoder : public TypedDecoder<DType> {
 public:
  typedef typename DType::c_type T;

  explicit DeltaBitPackDecoder(const ColumnDescriptor* descr,
                               ::arrow::MemoryPool* pool = ::arrow::default_memory_pool())
      : TypedDecoder<DType>(descr, Encoding::DELTA_BINARY_PACKED), pool_(pool) {
    if (DType::type_num != Type::INT32 && DType::type_num != Type::INT64) {
      throw ParquetException("Delta bit pack encoding should only be for integer data.");
    }
  }

  virtual void SetData(int num_values, const uint8_t* data, int len) {
    num_values_ = num_values;
    decoder_ = BitUtil::BitReader(data, len);
    values_current_block_ = 0;
    values_current_mini_block_ = 0;
  }

  virtual int Decode(T* buffer, int max_values) {
    return GetInternal(buffer, max_values);
  }

 private:
  using TypedDecoder<DType>::num_values_;

  void InitBlock() {
    int32_t block_size;
    if (!decoder_.GetVlqInt(&block_size)) ParquetException::EofException();
    if (!decoder_.GetVlqInt(&num_mini_blocks_)) ParquetException::EofException();
    if (!decoder_.GetVlqInt(&values_current_block_)) {
      ParquetException::EofException();
    }
    if (!decoder_.GetZigZagVlqInt(&last_value_)) ParquetException::EofException();

    delta_bit_widths_ = AllocateBuffer(pool_, num_mini_blocks_);
    uint8_t* bit_width_data = delta_bit_widths_->mutable_data();

    if (!decoder_.GetZigZagVlqInt(&min_delta_)) ParquetException::EofException();
    for (int i = 0; i < num_mini_blocks_; ++i) {
      if (!decoder_.GetAligned<uint8_t>(1, bit_width_data + i)) {
        ParquetException::EofException();
      }
    }
    values_per_mini_block_ = block_size / num_mini_blocks_;
    mini_block_idx_ = 0;
    delta_bit_width_ = bit_width_data[0];
    values_current_mini_block_ = values_per_mini_block_;
  }

  template <typename T>
  int GetInternal(T* buffer, int max_values) {
    max_values = std::min(max_values, num_values_);
    const uint8_t* bit_width_data = delta_bit_widths_->data();
    for (int i = 0; i < max_values; ++i) {
      if (ARROW_PREDICT_FALSE(values_current_mini_block_ == 0)) {
        ++mini_block_idx_;
        if (mini_block_idx_ < static_cast<size_t>(delta_bit_widths_->size())) {
          delta_bit_width_ = bit_width_data[mini_block_idx_];
          values_current_mini_block_ = values_per_mini_block_;
        } else {
          InitBlock();
          buffer[i] = last_value_;
          continue;
        }
      }

      // TODO: the key to this algorithm is to decode the entire miniblock at once.
      int64_t delta;
      if (!decoder_.GetValue(delta_bit_width_, &delta)) ParquetException::EofException();
      delta += min_delta_;
      last_value_ += static_cast<int32_t>(delta);
      buffer[i] = last_value_;
      --values_current_mini_block_;
    }
    num_values_ -= max_values;
    return max_values;
  }

  ::arrow::MemoryPool* pool_;
  BitUtil::BitReader decoder_;
  int32_t values_current_block_;
  int32_t num_mini_blocks_;
  uint64_t values_per_mini_block_;
  uint64_t values_current_mini_block_;

  int32_t min_delta_;
  size_t mini_block_idx_;
  std::shared_ptr<ResizableBuffer> delta_bit_widths_;
  int delta_bit_width_;

  int32_t last_value_;
};

// ----------------------------------------------------------------------
// DELTA_LENGTH_BYTE_ARRAY

class DeltaLengthByteArrayDecoder : public TypedDecoder<ByteArrayType> {
 public:
  explicit DeltaLengthByteArrayDecoder(
      const ColumnDescriptor* descr,
      ::arrow::MemoryPool* pool = ::arrow::default_memory_pool())
      : TypedDecoder<ByteArrayType>(descr, Encoding::DELTA_LENGTH_BYTE_ARRAY),
        len_decoder_(nullptr, pool) {}

  virtual void SetData(int num_values, const uint8_t* data, int len) {
    num_values_ = num_values;
    if (len == 0) return;
    int total_lengths_len = *reinterpret_cast<const int*>(data);
    data += 4;
    len_decoder_.SetData(num_values, data, total_lengths_len);
    data_ = data + total_lengths_len;
    len_ = len - 4 - total_lengths_len;
  }

  virtual int Decode(ByteArray* buffer, int max_values) {
    max_values = std::min(max_values, num_values_);
    std::vector<int> lengths(max_values);
    len_decoder_.Decode(lengths.data(), max_values);
    for (int i = 0; i < max_values; ++i) {
      buffer[i].len = lengths[i];
      buffer[i].ptr = data_;
      data_ += lengths[i];
      len_ -= lengths[i];
    }
    num_values_ -= max_values;
    return max_values;
  }

 private:
  using TypedDecoder<ByteArrayType>::num_values_;
  using TypedDecoder<ByteArrayType>::data_;
  using TypedDecoder<ByteArrayType>::len_;
  DeltaBitPackDecoder<Int32Type> len_decoder_;
};

// ----------------------------------------------------------------------
// DELTA_BYTE_ARRAY

class DeltaByteArrayDecoder : public TypedDecoder<ByteArrayType> {
 public:
  explicit DeltaByteArrayDecoder(
      const ColumnDescriptor* descr,
      ::arrow::MemoryPool* pool = ::arrow::default_memory_pool())
      : TypedDecoder<ByteArrayType>(descr, Encoding::DELTA_BYTE_ARRAY),
        prefix_len_decoder_(nullptr, pool),
        suffix_decoder_(nullptr, pool),
        last_value_(0, nullptr) {}

  virtual void SetData(int num_values, const uint8_t* data, int len) {
    num_values_ = num_values;
    if (len == 0) return;
    int prefix_len_length = *reinterpret_cast<const int*>(data);
    data += 4;
    len -= 4;
    prefix_len_decoder_.SetData(num_values, data, prefix_len_length);
    data += prefix_len_length;
    len -= prefix_len_length;
    suffix_decoder_.SetData(num_values, data, len);
  }

  // TODO: this doesn't work and requires memory management. We need to allocate
  // new strings to store the results.
  virtual int Decode(ByteArray* buffer, int max_values) {
    max_values = std::min(max_values, num_values_);
    for (int i = 0; i < max_values; ++i) {
      int prefix_len = 0;
      prefix_len_decoder_.Decode(&prefix_len, 1);
      ByteArray suffix = {0, nullptr};
      suffix_decoder_.Decode(&suffix, 1);
      buffer[i].len = prefix_len + suffix.len;

      uint8_t* result = reinterpret_cast<uint8_t*>(malloc(buffer[i].len));
      memcpy(result, last_value_.ptr, prefix_len);
      memcpy(result + prefix_len, suffix.ptr, suffix.len);

      buffer[i].ptr = result;
      last_value_ = buffer[i];
    }
    num_values_ -= max_values;
    return max_values;
  }

 private:
  using TypedDecoder<ByteArrayType>::num_values_;

  DeltaBitPackDecoder<Int32Type> prefix_len_decoder_;
  DeltaLengthByteArrayDecoder suffix_decoder_;
  ByteArray last_value_;
};

// ----------------------------------------------------------------------
// Encoder and decoder factory functions

}  // namespace parquet

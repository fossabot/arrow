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

#include "arrow/compute/kernels/hash.h"

#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "arrow/builder.h"
#include "arrow/compute/context.h"
#include "arrow/compute/kernel.h"
#include "arrow/compute/kernels/util-internal.h"
#include "arrow/util/hash-util.h"

namespace arrow {
namespace compute {

namespace {

// Initially 1024 elements
static constexpr int64_t kInitialHashTableSize = 1 << 10;

typedef int32_t hash_slot_t;
static constexpr hash_slot_t kHashSlotEmpty = std::numeric_limits<int32_t>::max();

// The maximum load factor for the hash table before resizing.
static constexpr double kMaxHashTableLoad = 0.7;

enum class SIMDMode : char { NOSIMD, SSE4, AVX2 };

#define CHECK_IMPLEMENTED(KERNEL, FUNCNAME, TYPE)                  \
  if (!KERNEL) {                                                   \
    std::stringstream ss;                                          \
    ss << FUNCNAME << " not implemented for " << type->ToString(); \
    return Status::NotImplemented(ss.str());                       \
  }

Status NewHashTable(int64_t size, MemoryPool* pool, std::shared_ptr<Buffer>* out) {
  auto hash_table = std::make_shared<PoolBuffer>(pool);

  RETURN_NOT_OK(hash_table->Resize(sizeof(hash_slot_t) * size));
  int32_t* slots = reinterpret_cast<hash_slot_t*>(hash_table->mutable_data());
  std::fill(slots, slots + size, kHashSlotEmpty);

  *out = hash_table;
  return Status::OK();
}

// This is a slight design concession -- some hash actions have the possibility
// of failure. Rather than introduce extra error checking into all actions, we
// will raise an internal exception so that only the actions where errors can
// occur will experience the extra overhead
class HashException : public std::exception {
 public:
  explicit HashException(const std::string& msg, StatusCode code = StatusCode::Invalid)
      : msg_(msg), code_(code) {}

  ~HashException() throw() {}

  const char* what() const throw() override;

  StatusCode code() const { return code_; }

 private:
  std::string msg_;
  StatusCode code_;
};

const char* HashException::what() const throw() { return msg_.c_str(); }

// TODO(wesm): we do not need this macro yet

// #define HASH_THROW_NOT_OK(EXPR)                     \
//   do {                                              \
//     Status _s = (EXPR);                             \
//     if (ARROW_PREDICT_FALSE(!_s.ok())) {            \
//       throw HashException(_s.message(), _s.code()); \
//     }                                               \
//   } while (false)

class HashTable {
 public:
  HashTable(const std::shared_ptr<DataType>& type, MemoryPool* pool)
      : type_(type),
        pool_(pool),
        initialized_(false),
        hash_table_(nullptr),
        hash_slots_(nullptr),
        hash_table_size_(0),
        mod_bitmask_(0) {}

  virtual ~HashTable() {}

  virtual Status Append(const ArrayData& input) = 0;
  virtual Status Flush(std::vector<Datum>* out) = 0;
  virtual Status GetDictionary(std::shared_ptr<ArrayData>* out) = 0;

 protected:
  Status Init(int64_t elements);

  std::shared_ptr<DataType> type_;
  MemoryPool* pool_;
  bool initialized_;

  // The hash table contains integer indices that reference the set of observed
  // distinct values
  std::shared_ptr<Buffer> hash_table_;
  hash_slot_t* hash_slots_;

  /// Size of the table. Must be a power of 2.
  int64_t hash_table_size_;

  // Store hash_table_size_ - 1, so that j & mod_bitmask_ is equivalent to j %
  // hash_table_size_, but uses far fewer CPU cycles
  int64_t mod_bitmask_;
};

Status HashTable::Init(int64_t elements) {
  DCHECK_EQ(elements, BitUtil::NextPower2(elements));
  RETURN_NOT_OK(NewHashTable(elements, pool_, &hash_table_));
  hash_slots_ = reinterpret_cast<hash_slot_t*>(hash_table_->mutable_data());
  hash_table_size_ = elements;
  mod_bitmask_ = elements - 1;
  initialized_ = true;
  return Status::OK();
}

template <typename Type, typename Action, typename Enable = void>
class HashTableKernel : public HashTable {};

// Types of hash actions
//
// unique: append to dictionary when not found, no-op with slot
// dictionary-encode: append to dictionary when not found, append slot #
// match: raise or set null when not found, otherwise append slot #
// isin: set false when not found, otherwise true
// value counts: append to dictionary when not found, increment count for slot

template <typename Type, typename Enable = void>
class HashDictionary {};

// ----------------------------------------------------------------------
// Hash table pass for nulls

template <typename Type, typename Action>
class HashTableKernel<Type, Action, enable_if_null<Type>> : public HashTable {
 public:
  using HashTable::HashTable;

  Status Init() {
    // No-op, do not even need to initialize hash table
    return Status::OK();
  }

  Status Append(const ArrayData& arr) override {
    if (!initialized_) {
      RETURN_NOT_OK(Init());
    }
    auto action = static_cast<Action*>(this);
    RETURN_NOT_OK(action->Reserve(arr.length));
    for (int64_t i = 0; i < arr.length; ++i) {
      action->ObserveNull();
    }
    return Status::OK();
  }

  Status GetDictionary(std::shared_ptr<ArrayData>* out) override {
    // TODO(wesm): handle null being a valid dictionary value
    auto null_array = std::make_shared<NullArray>(0);
    *out = null_array->data();
    return Status::OK();
  }
};

// ----------------------------------------------------------------------
// Hash table pass for primitive types

template <typename Type>
struct HashDictionary<Type, enable_if_primitive_ctype<Type>> {
  using T = typename Type::c_type;

  HashDictionary(MemoryPool* pool) : pool_(pool), size(0), capacity(0) {}

  Status Init(MemoryPool* pool) {
    this->buffer = std::make_shared<PoolBuffer>(pool);
    this->size = 0;
    return Resize(kInitialHashTableSize);
  }

  Status DoubleSize() { return Resize(this->size * 2); }

  Status Resize(const int64_t elements) {
    RETURN_NOT_OK(this->buffer->Resize(elements * sizeof(T)));

    this->capacity = elements;
    this->values = reinterpret_cast<T*>(this->buffer->mutable_data());
    return Status::OK();
  }

  std::shared_ptr<ResizableBuffer> buffer;
  T* values;
  int64_t size;
  int64_t capacity;
};

template <typename Type, typename Action>
class HashTableKernel<Type, Action, enable_if_primitive_ctype<Type>> : public HashTable {
 public:
  using T = typename Type::c_type;

  HashTableKernel(const std::shared_ptr<DataType>& type, MemoryPool* pool)
    : HashTable(type, pool),
      dict_(pool) {}

  Status Init() {
    RETURN_NOT_OK(dict_.Init(pool_));
    return HashTable::Init(kInitialHashTableSize);
  }

  Status Append(const ArrayData& arr) override {
    if (!initialized_) {
      RETURN_NOT_OK(Init());
    }

    const T* values = GetValues<T>(arr, 1);
    auto action = static_cast<Action*>(this);

    RETURN_NOT_OK(action->Reserve(arr.length));

#define HASH_INNER_LOOP()                                               \
    int64_t j = HashValue(value) & mod_bitmask_;                        \
    hash_slot_t slot = hash_slots_[j];                                  \
                                                                        \
    // Find an empty slot                                               \
    while (kHashSlotEmpty != slot && dict_.values[slot] != value) {     \
      ++j;                                                              \
      if (ARROW_PREDICT_FALSE(j == hash_table_size_)) {                 \
        j = 0;                                                          \
      }                                                                 \
      slot = hash_slots_[j];                                            \
    }                                                                   \
                                                                        \
    if (slot == kHashSlotEmpty) {                                       \
      if (!Action::allow_expand) {                                      \
        throw HashException("Encountered new dictionary value");        \
      }                                                                 \
                                                                        \
      // Not in the hash table, so we insert it now                     \
      slot = static_cast<hash_slot_t>(dict_.size);                      \
      hash_slots_[j] = slot;                                            \
      dict_.values[dict_.size++] = value;                               \
                                                                        \
      action->ObserveNotFound(slot);                                    \
                                                                        \
      if (ARROW_PREDICT_FALSE(dict_.size > hash_table_size_ * kMaxHashTableLoad)) { \
        RETURN_NOT_OK(action->DoubleSize());                            \
      }                                                                 \
    } else {                                                            \
      action->ObserveFound(slot);                                       \
    }

    if (arr.null_count != 0) {
      internal::BitmapReader valid_reader(arr.buffers[0]->data(), arr.offset,
                                          arr.length);
      for (int64_t i = 0; i < arr.length; ++i) {
        const T value = values[i];
        const bool is_null = valid_reader.IsNotSet();
        valid_reader.Next();

        if (is_null) {
          action->ObserveNull();
          continue;
        }

        HASH_INNER_LOOP();
      }
    } else {
      for (int64_t i = 0; i < arr.length; ++i) {
        HASH_INNER_LOOP();
      }
    }

#undef HASH_INNER_LOOP

    return Status::OK();
  }

  Status GetDictionary(std::shared_ptr<ArrayData>* out) override {
    // TODO(wesm): handle null being in the dictionary
    auto dict_data = dict_.buffer;
    RETURN_NOT_OK(dict_data->Resize(dict_.size * sizeof(T), false));

    BufferVector buffers = {nullptr, dict_data};
    *out = std::make_shared<ArrayData>(type_, dict_.size, std::move(buffers), 0);
    return Status::OK();
  }

 protected:
  int64_t HashValue(const T& value) const {
    // TODO(wesm): Use faster hash function for C types
    return HashUtil::Hash(&value, sizeof(T), 0);
  }

  Status DoubleTableSize() {
    int64_t new_size = hash_table_size_ * 2;

    std::shared_ptr<Buffer> new_hash_table;
    RETURN_NOT_OK(NewHashTable(new_size, pool_, &new_hash_table));
    int32_t* new_hash_slots =
        reinterpret_cast<hash_slot_t*>(new_hash_table->mutable_data());
    int64_t new_mod_bitmask = new_size - 1;

    for (int i = 0; i < hash_table_size_; ++i) {
      hash_slot_t index = hash_slots_[i];

      if (index == kHashSlotEmpty) {
        continue;
      }

      // Compute the hash value mod the new table size to start looking for an
      // empty slot
      const T value = dict_.values[index];

      // Find empty slot in the new hash table
      int64_t j = HashValue(value) & new_mod_bitmask;
      while (kHashSlotEmpty != new_hash_slots[j]) {
        ++j;
        if (ARROW_PREDICT_FALSE(j == hash_table_size_)) {
          j = 0;
        }
      }

      // Copy the old slot index to the new hash table
      new_hash_slots[j] = index;
    }

    hash_table_ = new_hash_table;
    hash_slots_ = reinterpret_cast<hash_slot_t*>(hash_table_->mutable_data());
    hash_table_size_ = new_size;
    mod_bitmask_ = new_size - 1;

    return dict_.Resize(new_size);
  }

  HashDictionary<Type> dict_;
};

// ----------------------------------------------------------------------
// Hash table pass for variable-length binary types

template <typename Type, typename Action>
class HashTableKernel<Type, Action, enable_if_binary<Type>> : public HashTable {
 public:
  HashTableKernel(const std::shared_ptr<DataType>& type, MemoryPool* pool)
    : HashTable(type, pool),
      offsets_builder_(pool)
      value_data_builder_(pool) {}

  Status Init() {
    return HashTable::Init(kInitialHashTableSize);
  }

  Status Append(const ArrayData& arr) override {
    if (!initialized_) {
      RETURN_NOT_OK(Init());
    }

    internal::BitmapReader valid_reader(arr.buffers[0]->data(), arr.offset, arr.length);

    const int32_t* offsets = GetValues<int32_t>(arr, 1);
    const uint8_t* data = GetValues<uint8_t>(arr, 2);

    auto action = static_cast<Action*>(this);
    RETURN_NOT_OK(action->Reserve(arr.length));

    for (int64_t i = 0; i < arr.length; ++i) {
      const bool is_null = valid_reader.IsNotSet();
      valid_reader.Next();

      if (is_null) {
        action->ObserveNull();
        continue;
      }

      const int32_t position = *offsets++;
      const int32_t length = *offsets - position;
      const uint8_t* value = data + position;

      int64_t j = HashValue(value, length) & mod_bitmask_;
      hash_slot_t slot = hash_slots_[j];

      // Find an empty slot
      const int32_t* dict_offsets = dict_offsets_.data();
      const int32_t slot_length = dict_offsets[slot + 1] - dict_offsets[slot];
      const uint8_t* dict_data = dict_data_.data();
      while (kHashSlotEmpty != slot &&
             !(slot_length == length &&
               0 == memcmp(value,  + offsets_data[slot], length))) {
        ++j;
        if (ARROW_PREDICT_FALSE(j == hash_table_size_)) {
          j = 0;
        }
        slot = hash_slots_[j];
      }

      if (slot == kHashSlotEmpty) {
        if (!Action::allow_expand) {
          throw HashException("Encountered new dictionary value");
        }

        // Not in the hash table, so we insert it now
        slot = static_cast<hash_slot_t>(offsets_builder_.length());
        hash_slots_[j] = slot;
        dict_.values[dict_.size++] = value;

        action->ObserveNotFound(slot);

        if (ARROW_PREDICT_FALSE(dict_.size > hash_table_size_ * kMaxHashTableLoad)) {
          RETURN_NOT_OK(action->DoubleSize());
        }
      } else {
        action->ObserveFound(slot);
      }
    }

    return Status::OK();
  }

  Status GetDictionary(std::shared_ptr<ArrayData>* out) override {
    // TODO(wesm): handle null being in the dictionary
    auto dict_data = dict_.buffer;
    RETURN_NOT_OK(dict_data->Resize(dict_.size * sizeof(T), false));

    BufferVector buffers = {nullptr, dict_data};
    *out = std::make_shared<ArrayData>(type_, dict_.size, std::move(buffers), 0);
    return Status::OK();
  }

 protected:
  int64_t HashValue(const uint8_t* data, int32_t length) const {
    return HashUtil::Hash(data, length, 0);
  }

  Status DoubleTableSize() {
    int64_t new_size = hash_table_size_ * 2;

    std::shared_ptr<Buffer> new_hash_table;
    RETURN_NOT_OK(NewHashTable(new_size, pool_, &new_hash_table));
    int32_t* new_hash_slots =
        reinterpret_cast<hash_slot_t*>(new_hash_table->mutable_data());
    int64_t new_mod_bitmask = new_size - 1;

    const int32_t* dict_offsets = dict_offsets_.data();
    const uint8_t* dict_data = dict_data_.data();

    for (int i = 0; i < hash_table_size_; ++i) {
      hash_slot_t index = hash_slots_[i];

      if (slot == kHashSlotEmpty) {
        continue;
      }

      // Compute the hash value mod the new table size to start looking for an
      // empty slot
      const int32_t length = dict_offsets[index + 1] - dict_offsets[index];
      const uint8_t* value = dict_data + dict_offsets[index];

      // Find an empty slot in the new hash table
      int64_t j = HashValue(value, length) & new_mod_bitmask;
      while (kHashSlotEmpty != new_hash_slots[j]) {
        ++j;
        if (ARROW_PREDICT_FALSE(j == hash_table_size_)) {
          j = 0;
        }
      }

      // Copy the old slot index to the new hash table
      new_hash_slots[j] = index;
    }

    hash_table_ = new_hash_table;
    hash_slots_ = reinterpret_cast<hash_slot_t*>(hash_table_->mutable_data());
    hash_table_size_ = new_size;
    mod_bitmask_ = new_size - 1;

    return Status::OK();
  }

  TypedBufferBuilder<int32_t> dict_offsets_;
  TypedBufferBuilder<uint8_t> dict_data_;
  int32_t dict_size_;
};

// ----------------------------------------------------------------------
// Unique implementation

template <typename Type>
class UniqueImpl : public HashTableKernel<Type, UniqueImpl<Type>> {
 public:
  static constexpr bool allow_expand = true;
  using Base = HashTableKernel<Type, UniqueImpl<Type>>;
  using Base::Base;

  Status Reserve(const int64_t length) { return Status::OK(); }

  void ObserveFound(const hash_slot_t slot) {}
  void ObserveNull() {}
  void ObserveNotFound(const hash_slot_t slot) {}

  Status DoubleSize() { return Base::DoubleTableSize(); }

  Status Append(const ArrayData& input) override { return Base::Append(input); }

  Status Flush(std::vector<Datum>* out) override {
    // No-op
    return Status::OK();
  }
};

// ----------------------------------------------------------------------
// Dictionary encode implementation

template <typename Type>
class DictEncodeImpl : public HashTableKernel<Type, DictEncodeImpl<Type>> {
 public:
  static constexpr bool allow_expand = true;
  using Base = HashTableKernel<Type, DictEncodeImpl>;

  DictEncodeImpl(const std::shared_ptr<DataType>& type, MemoryPool* pool)
      : Base(type, pool), indices_builder_(pool) {}

  Status Reserve(const int64_t length) { return indices_builder_.Reserve(length); }

  void ObserveNull() { indices_builder_.UnsafeAppendToBitmap(false); }

  void ObserveFound(const hash_slot_t slot) { indices_builder_.UnsafeAppend(slot); }

  void ObserveNotFound(const hash_slot_t slot) { return ObserveFound(slot); }

  Status DoubleSize() { return Base::DoubleTableSize(); }

  Status Flush(std::vector<Datum>* out) override {
    std::shared_ptr<ArrayData> result;
    RETURN_NOT_OK(indices_builder_.FinishInternal(&result));
    out->push_back(Datum(result));
    return Status::OK();
  }

  using Base::Append;

 private:
  Int32Builder indices_builder_;
};

// ----------------------------------------------------------------------
// Kernel wrapper for generic hash table kernels

class HashKernelImpl : public HashKernel {
 public:
  explicit HashKernelImpl(std::unique_ptr<HashTable> hasher)
      : hasher_(std::move(hasher)) {}

  Status Call(FunctionContext* ctx, const ArrayData& input,
              std::vector<Datum>* out) override {
    RETURN_NOT_OK(Append(ctx, input));
    return Flush(out);
  }

  Status Append(FunctionContext* ctx, const ArrayData& input) override {
    std::lock_guard<std::mutex> guard(lock_);
    try {
      RETURN_NOT_OK(hasher_->Append(input));
    } catch (const HashException& e) {
      return Status(e.code(), e.what());
    }
    return Status::OK();
  }

  Status Flush(std::vector<Datum>* out) override { return hasher_->Flush(out); }

  Status GetDictionary(std::shared_ptr<ArrayData>* out) override {
    return hasher_->GetDictionary(out);
  }

 private:
  std::mutex lock_;
  std::unique_ptr<HashTable> hasher_;
};

}  // namespace

Status GetUniqueKernel(FunctionContext* ctx, const std::shared_ptr<DataType>& type,
                       std::unique_ptr<HashKernel>* out) {
  std::unique_ptr<HashTable> hasher;

#define UNIQUE_CASE(InType)                                         \
  case InType::type_id:                                             \
    hasher.reset(new UniqueImpl<InType>(type, ctx->memory_pool())); \
    break

  switch (type->id()) {
    UNIQUE_CASE(NullType);
    // UNIQUE_CASE(BooleanType);
    UNIQUE_CASE(UInt8Type);
    UNIQUE_CASE(Int8Type);
    UNIQUE_CASE(UInt16Type);
    UNIQUE_CASE(Int16Type);
    UNIQUE_CASE(UInt32Type);
    UNIQUE_CASE(Int32Type);
    UNIQUE_CASE(UInt64Type);
    UNIQUE_CASE(Int64Type);
    UNIQUE_CASE(FloatType);
    UNIQUE_CASE(DoubleType);
    // UNIQUE_CASE(Date32Type);
    // UNIQUE_CASE(Date64Type);
    // UNIQUE_CASE(Time32Type);
    // UNIQUE_CASE(Time64Type);
    // UNIQUE_CASE(TimestampType);
    // UNIQUE_CASE(BinaryType);
    // UNIQUE_CASE(StringType);
    // UNIQUE_CASE(FixedSizeBinaryType);
    default:
      break;
  }

#undef UNIQUE_CASE

  CHECK_IMPLEMENTED(hasher, "unique", type);
  out->reset(new HashKernelImpl(std::move(hasher)));
  return Status::OK();
}

Status GetDictionaryEncodeKernel(FunctionContext* ctx,
                                 const std::shared_ptr<DataType>& type,
                                 std::unique_ptr<HashKernel>* out) {
  std::unique_ptr<HashTable> hasher;

#define DICTIONARY_ENCODE_CASE(InType)                                  \
  case InType::type_id:                                                 \
    hasher.reset(new DictEncodeImpl<InType>(type, ctx->memory_pool())); \
    break

  switch (type->id()) {
    DICTIONARY_ENCODE_CASE(NullType);
    // DICTIONARY_ENCODE_CASE(BooleanType);
    DICTIONARY_ENCODE_CASE(UInt8Type);
    DICTIONARY_ENCODE_CASE(Int8Type);
    DICTIONARY_ENCODE_CASE(UInt16Type);
    DICTIONARY_ENCODE_CASE(Int16Type);
    DICTIONARY_ENCODE_CASE(UInt32Type);
    DICTIONARY_ENCODE_CASE(Int32Type);
    DICTIONARY_ENCODE_CASE(UInt64Type);
    DICTIONARY_ENCODE_CASE(Int64Type);
    DICTIONARY_ENCODE_CASE(FloatType);
    DICTIONARY_ENCODE_CASE(DoubleType);
    // DICTIONARY_ENCODE_CASE(Date32Type);
    // DICTIONARY_ENCODE_CASE(Date64Type);
    // DICTIONARY_ENCODE_CASE(Time32Type);
    // DICTIONARY_ENCODE_CASE(Time64Type);
    // DICTIONARY_ENCODE_CASE(TimestampType);
    // DICTIONARY_ENCODE_CASE(BinaryType);
    // DICTIONARY_ENCODE_CASE(StringType);
    // DICTIONARY_ENCODE_CASE(FixedSizeBinaryType);
    default:
      break;
  }

#undef DICTIONARY_ENCODE_CASE

  CHECK_IMPLEMENTED(hasher, "dictionary-encode", type);
  out->reset(new HashKernelImpl(std::move(hasher)));
  return Status::OK();
}

namespace {

Status InvokeHash(FunctionContext* ctx, HashKernel* func, const Datum& value,
                  std::vector<Datum>* kernel_outputs,
                  std::shared_ptr<Array>* dictionary) {
  if (value.kind() == Datum::ARRAY) {
    RETURN_NOT_OK(func->Call(ctx, *value.array(), kernel_outputs));
  } else if (value.kind() == Datum::CHUNKED_ARRAY) {
    const ChunkedArray& array = *value.chunked_array();
    for (int i = 0; i < array.num_chunks(); i++) {
      RETURN_NOT_OK(func->Call(ctx, *(array.chunk(i)->data()), kernel_outputs));
    }
  }
  std::shared_ptr<ArrayData> dict_data;
  RETURN_NOT_OK(func->GetDictionary(&dict_data));
  *dictionary = MakeArray(dict_data);
  return Status::OK();
}

}  // namespace

Status Unique(FunctionContext* ctx, const Datum& value, std::shared_ptr<Array>* out) {
  // TODO(wesm): Must we be more rigorous than DCHECK
  DCHECK(value.is_arraylike());

  std::unique_ptr<HashKernel> func;
  RETURN_NOT_OK(GetUniqueKernel(ctx, value.type(), &func));

  std::vector<Datum> dummy_outputs;
  return InvokeHash(ctx, func.get(), value, &dummy_outputs, out);
}

Status DictionaryEncode(FunctionContext* ctx, const Datum& value, Datum* out) {
  // TODO(wesm): Must we be more rigorous than DCHECK
  DCHECK(value.is_arraylike());

  std::unique_ptr<HashKernel> func;
  RETURN_NOT_OK(GetDictionaryEncodeKernel(ctx, value.type(), &func));

  std::shared_ptr<Array> dictionary;
  std::vector<Datum> indices_outputs;
  RETURN_NOT_OK(InvokeHash(ctx, func.get(), value, &indices_outputs, &dictionary));

  // Create the dictionary type
  DCHECK_EQ(indices_outputs[0].kind(), Datum::ARRAY);
  std::shared_ptr<DataType> dict_type =
      ::arrow::dictionary(indices_outputs[0].array()->type, dictionary);

  // Create DictionaryArray for each piece yielded by the kernel invocations
  std::vector<std::shared_ptr<Array>> dict_chunks;
  for (const Datum& datum : indices_outputs) {
    dict_chunks.emplace_back(
        std::make_shared<DictionaryArray>(dict_type, MakeArray(datum.array())));
  }

  // Create right kind of datum
  // TODO(wesm): Create some generalizable pattern for this
  if (value.kind() == Datum::ARRAY) {
    out->value = dict_chunks[0]->data();
  } else if (value.kind() == Datum::CHUNKED_ARRAY) {
    out->value = std::make_shared<ChunkedArray>(dict_chunks);
  }

  return Status::OK();
}

}  // namespace compute
}  // namespace arrow

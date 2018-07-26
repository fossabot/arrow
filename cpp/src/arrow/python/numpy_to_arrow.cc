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

// Functions for pandas conversion via NumPy

#define ARROW_NO_DEFAULT_MEMORY_POOL

#include "arrow/python/numpy_to_arrow.h"
#include "arrow/python/numpy_interop.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "arrow/array.h"
#include "arrow/status.h"
#include "arrow/table.h"
#include "arrow/type_fwd.h"
#include "arrow/type_traits.h"
#include "arrow/util/bit-util.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/decimal.h"
#include "arrow/util/logging.h"
#include "arrow/util/macros.h"
#include "arrow/visitor_inline.h"

#include "arrow/compute/context.h"
#include "arrow/compute/kernels/cast.h"

#include "arrow/python/builtin_convert.h"
#include "arrow/python/common.h"
#include "arrow/python/config.h"
#include "arrow/python/decimal.h"
#include "arrow/python/helpers.h"
#include "arrow/python/iterators.h"
#include "arrow/python/numpy-internal.h"
#include "arrow/python/numpy_convert.h"
#include "arrow/python/type_traits.h"
#include "arrow/python/util/datetime.h"

namespace arrow {
namespace py {

using internal::NumPyTypeSize;

// ----------------------------------------------------------------------
// Conversion utilities

namespace {

constexpr int64_t kMillisecondsInDay = 86400000;

inline bool PyObject_is_integer(PyObject* obj) {
  return !PyBool_Check(obj) && PyArray_IsIntegerScalar(obj);
}

Status CheckFlatNumpyArray(PyArrayObject* numpy_array, int np_type) {
  if (PyArray_NDIM(numpy_array) != 1) {
    return Status::Invalid("only handle 1-dimensional arrays");
  }

  const int received_type = PyArray_DESCR(numpy_array)->type_num;
  if (received_type != np_type) {
    std::stringstream ss;
    ss << "trying to convert NumPy type " << GetNumPyTypeName(np_type) << " but got "
       << GetNumPyTypeName(received_type);
    return Status::Invalid(ss.str());
  }

  return Status::OK();
}

Status AllocateNullBitmap(MemoryPool* pool, int64_t length,
                          std::shared_ptr<ResizableBuffer>* out) {
  int64_t null_bytes = BitUtil::BytesForBits(length);
  std::shared_ptr<ResizableBuffer> null_bitmap;
  RETURN_NOT_OK(AllocateResizableBuffer(pool, null_bytes, &null_bitmap));

  // Padding zeroed by AllocateResizableBuffer
  memset(null_bitmap->mutable_data(), 0, static_cast<size_t>(null_bytes));
  *out = null_bitmap;
  return Status::OK();
}

// ----------------------------------------------------------------------
// Conversion from NumPy-in-Pandas to Arrow null bitmap

template <int TYPE>
inline int64_t ValuesToBitmap(PyArrayObject* arr, uint8_t* bitmap) {
  typedef internal::npy_traits<TYPE> traits;
  typedef typename traits::value_type T;

  int64_t null_count = 0;

  Ndarray1DIndexer<T> values(arr);
  for (int i = 0; i < values.size(); ++i) {
    if (traits::isnull(values[i])) {
      ++null_count;
    } else {
      BitUtil::SetBit(bitmap, i);
    }
  }

  return null_count;
}

class NumPyNullsConverter {
 public:
  /// Convert the given array's null values to a null bitmap.
  /// The null bitmap is only allocated if null values are ever possible.
  static Status Convert(MemoryPool* pool, PyArrayObject* arr,
                        bool use_pandas_null_sentinels,
                        std::shared_ptr<ResizableBuffer>* out_null_bitmap_,
                        int64_t* out_null_count) {
    NumPyNullsConverter converter(pool, arr, use_pandas_null_sentinels);
    RETURN_NOT_OK(VisitNumpyArrayInline(arr, &converter));
    *out_null_bitmap_ = converter.null_bitmap_;
    *out_null_count = converter.null_count_;
    return Status::OK();
  }

  template <int TYPE>
  Status Visit(PyArrayObject* arr) {
    typedef internal::npy_traits<TYPE> traits;

    const bool null_sentinels_possible =
        // Always treat Numpy's NaT as null
        TYPE == NPY_DATETIME ||
        // Observing pandas's null sentinels
        (use_pandas_null_sentinels_ && traits::supports_nulls);

    if (null_sentinels_possible) {
      RETURN_NOT_OK(AllocateNullBitmap(pool_, PyArray_SIZE(arr), &null_bitmap_));
      null_count_ = ValuesToBitmap<TYPE>(arr, null_bitmap_->mutable_data());
    }
    return Status::OK();
  }

 protected:
  NumPyNullsConverter(MemoryPool* pool, PyArrayObject* arr,
                      bool use_pandas_null_sentinels)
      : pool_(pool),
        arr_(arr),
        use_pandas_null_sentinels_(use_pandas_null_sentinels),
        null_bitmap_data_(nullptr),
        null_count_(0) {}

  MemoryPool* pool_;
  PyArrayObject* arr_;
  bool use_pandas_null_sentinels_;
  std::shared_ptr<ResizableBuffer> null_bitmap_;
  uint8_t* null_bitmap_data_;
  int64_t null_count_;
};

// Returns null count
int64_t MaskToBitmap(PyArrayObject* mask, int64_t length, uint8_t* bitmap) {
  int64_t null_count = 0;

  Ndarray1DIndexer<uint8_t> mask_values(mask);
  for (int i = 0; i < length; ++i) {
    if (mask_values[i]) {
      ++null_count;
      BitUtil::ClearBit(bitmap, i);
    } else {
      BitUtil::SetBit(bitmap, i);
    }
  }
  return null_count;
}

}  // namespace

/// Append as many string objects from NumPy arrays to a `BinaryBuilder` as we
/// can fit
///
/// \param[in] offset starting offset for appending
/// \param[out] end_offset ending offset where we stopped appending. Will
/// be length of arr if fully consumed
static Status AppendObjectBinaries(PyArrayObject* arr, PyArrayObject* mask,
                                   int64_t offset, BinaryBuilder* builder,
                                   int64_t* end_offset) {
  Ndarray1DIndexer<PyObject*> objects(arr);
  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask != nullptr) {
    mask_values.Init(mask);
    have_mask = true;
  }

  for (; offset < objects.size(); ++offset) {
    PyObject* obj = objects[offset];
    if ((have_mask && mask_values[offset]) || internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder->AppendNull());
      continue;
    }
    bool is_full;
    RETURN_NOT_OK(internal::BuilderAppend(builder, obj, &is_full));
    if (is_full) {
      break;
    }
  }

  // If we consumed the whole array, this will be the length of arr
  *end_offset = offset;
  return Status::OK();
}

/// Append as many string objects from NumPy arrays to a `StringBuilder` as we
/// can fit
///
/// \param[in] offset starting offset for appending
/// \param[in] check_valid if set to true and the input array
/// contains values that cannot be converted to unicode, returns
/// a Status code containing a Python exception message
/// \param[out] end_offset ending offset where we stopped appending. Will
/// be length of arr if fully consumed
/// \param[out] have_bytes true if we encountered any PyBytes object
static Status AppendObjectStrings(PyArrayObject* arr, PyArrayObject* mask, int64_t offset,
                                  bool check_valid, StringBuilder* builder,
                                  int64_t* end_offset, bool* have_bytes) {
  PyObject* obj;

  Ndarray1DIndexer<PyObject*> objects(arr);
  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask != nullptr) {
    mask_values.Init(mask);
    have_mask = true;
  }

  for (; offset < objects.size(); ++offset) {
    OwnedRef tmp_obj;
    obj = objects[offset];
    if ((have_mask && mask_values[offset]) || internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder->AppendNull());
      continue;
    }
    if (internal::IsPyBinary(obj)) {
      *have_bytes = true;
    }
    bool is_full;
    RETURN_NOT_OK(internal::BuilderAppend(builder, obj, check_valid, &is_full));
    if (is_full) {
      break;
    }
  }

  // If we consumed the whole array, this will be the length of arr
  *end_offset = offset;
  return Status::OK();
}

static Status AppendObjectFixedWidthBytes(PyArrayObject* arr, PyArrayObject* mask,
                                          int byte_width, int64_t offset,
                                          FixedSizeBinaryBuilder* builder,
                                          int64_t* end_offset) {
  PyObject* obj;

  Ndarray1DIndexer<PyObject*> objects(arr);
  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask != nullptr) {
    mask_values.Init(mask);
    have_mask = true;
  }

  for (; offset < objects.size(); ++offset) {
    OwnedRef tmp_obj;
    obj = objects[offset];
    if ((have_mask && mask_values[offset]) || internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder->AppendNull());
      continue;
    }
    bool is_full;
    RETURN_NOT_OK(internal::BuilderAppend(builder, obj, &is_full));
    if (is_full) {
      break;
    }
  }

  // If we consumed the whole array, this will be the length of arr
  *end_offset = offset;
  return Status::OK();
}

// ----------------------------------------------------------------------
// Conversion from NumPy-in-Pandas to Arrow

class NumPyConverter {
 public:
  NumPyConverter(MemoryPool* pool, PyObject* ao, PyObject* mo,
                 const std::shared_ptr<DataType>& type, bool use_pandas_null_sentinels)
      : pool_(pool),
        type_(type),
        arr_(reinterpret_cast<PyArrayObject*>(ao)),
        dtype_(PyArray_DESCR(arr_)),
        mask_(nullptr),
        use_pandas_null_sentinels_(use_pandas_null_sentinels),
        decimal_type_(),
        null_bitmap_data_(nullptr),
        null_count_(0) {
    if (mo != nullptr && mo != Py_None) {
      mask_ = reinterpret_cast<PyArrayObject*>(mo);
    }
    length_ = static_cast<int64_t>(PyArray_SIZE(arr_));
    itemsize_ = static_cast<int>(PyArray_DESCR(arr_)->elsize);
    stride_ = static_cast<int64_t>(PyArray_STRIDES(arr_)[0]);

    PyAcquireGIL lock;
    Status status = internal::ImportDecimalType(&decimal_type_);
    DCHECK_OK(status);
  }

  bool is_strided() const { return itemsize_ != stride_; }

  Status Convert();

  const ArrayVector& result() const { return out_arrays_; }

  template <typename T>
  typename std::enable_if<std::is_base_of<PrimitiveCType, T>::value ||
                              std::is_same<BooleanType, T>::value,
                          Status>::type
  Visit(const T& type) {
    return VisitNative<T>();
  }

  Status Visit(const HalfFloatType& type) { return VisitNative<UInt16Type>(); }

  Status Visit(const Date32Type& type) { return VisitNative<Date32Type>(); }
  Status Visit(const Date64Type& type) { return VisitNative<Date64Type>(); }
  Status Visit(const TimestampType& type) { return VisitNative<TimestampType>(); }
  Status Visit(const Time32Type& type) { return VisitNative<Int32Type>(); }
  Status Visit(const Time64Type& type) { return VisitNative<Int64Type>(); }

  Status Visit(const NullType& type) { return TypeNotImplemented(type.ToString()); }

  // NumPy ascii string arrays
  Status Visit(const BinaryType& type);

  // NumPy unicode arrays
  Status Visit(const StringType& type);

  Status Visit(const StructType& type);

  Status Visit(const FixedSizeBinaryType& type);

  Status Visit(const Decimal128Type& type) { return TypeNotImplemented(type.ToString()); }

  Status Visit(const DictionaryType& type) { return TypeNotImplemented(type.ToString()); }

  Status Visit(const NestedType& type) { return TypeNotImplemented(type.ToString()); }

 protected:
  Status InitNullBitmap() {
    RETURN_NOT_OK(AllocateNullBitmap(pool_, length_, &null_bitmap_));
    null_bitmap_data_ = null_bitmap_->mutable_data();
    return Status::OK();
  }

  // ----------------------------------------------------------------------
  // Traditional visitor conversion for non-object arrays

  template <typename ArrowType>
  Status ConvertData(std::shared_ptr<Buffer>* data);

  template <typename T>
  Status PushBuilderResult(T* builder) {
    std::shared_ptr<Array> out;
    RETURN_NOT_OK(builder->Finish(&out));
    out_arrays_.emplace_back(out);
    return Status::OK();
  }

  template <int TYPE, typename BuilderType>
  Status AppendNdarrayToBuilder(PyArrayObject* array, BuilderType* builder) {
    typedef internal::npy_traits<TYPE> traits;
    typedef typename traits::value_type T;

    const bool null_sentinels_possible =
        (use_pandas_null_sentinels_ && traits::supports_nulls);

    // TODO(wesm): Vector append when not strided
    Ndarray1DIndexer<T> values(array);
    if (null_sentinels_possible) {
      for (int64_t i = 0; i < values.size(); ++i) {
        if (traits::isnull(values[i])) {
          RETURN_NOT_OK(builder->AppendNull());
        } else {
          RETURN_NOT_OK(builder->Append(values[i]));
        }
      }
    } else {
      for (int64_t i = 0; i < values.size(); ++i) {
        RETURN_NOT_OK(builder->Append(values[i]));
      }
    }
    return Status::OK();
  }

  Status PushArray(const std::shared_ptr<ArrayData>& data) {
    out_arrays_.emplace_back(MakeArray(data));
    return Status::OK();
  }

  template <typename ArrowType>
  Status VisitNative() {
    if (mask_ != nullptr) {
      RETURN_NOT_OK(InitNullBitmap());
      null_count_ = MaskToBitmap(mask_, length_, null_bitmap_data_);
    } else {
      RETURN_NOT_OK(NumPyNullsConverter::Convert(pool_, arr_, use_pandas_null_sentinels_,
                                                 &null_bitmap_, &null_count_));
    }

    std::shared_ptr<Buffer> data;
    RETURN_NOT_OK(ConvertData<ArrowType>(&data));

    auto arr_data = ArrayData::Make(type_, length_, {null_bitmap_, data}, null_count_, 0);
    return PushArray(arr_data);
  }

  Status TypeNotImplemented(std::string type_name) {
    std::stringstream ss;
    ss << "NumPyConverter doesn't implement <" << type_name << "> conversion. ";
    return Status::NotImplemented(ss.str());
  }

  // ----------------------------------------------------------------------
  // Conversion logic for various object dtype arrays

  Status ConvertObjects();

  template <int ITEM_TYPE, typename ArrowType>
  Status ConvertTypedLists(const std::shared_ptr<DataType>& type, ListBuilder* builder,
                           PyObject* list);

  template <typename ArrowType>
  Status ConvertDates();

  Status ConvertBooleans();
  Status ConvertObjectStrings();
  Status ConvertObjectFloats();
  Status ConvertObjectBytes();
  Status ConvertObjectFixedWidthBytes(const std::shared_ptr<DataType>& type);
  Status ConvertObjectIntegers();
  Status ConvertLists(const std::shared_ptr<DataType>& type);
  Status ConvertLists(const std::shared_ptr<DataType>& type, ListBuilder* builder,
                      PyObject* list);
  Status ConvertDecimals();
  Status ConvertDateTimes();
  Status ConvertTimes();
  Status ConvertObjectsInfer();
  Status ConvertObjectsInferAndCast();

  MemoryPool* pool_;
  std::shared_ptr<DataType> type_;
  PyArrayObject* arr_;
  PyArray_Descr* dtype_;
  PyArrayObject* mask_;
  int64_t length_;
  int64_t stride_;
  int itemsize_;

  bool use_pandas_null_sentinels_;

  OwnedRefNoGIL decimal_type_;

  // Used in visitor pattern
  ArrayVector out_arrays_;

  std::shared_ptr<ResizableBuffer> null_bitmap_;
  uint8_t* null_bitmap_data_;
  int64_t null_count_;
};

Status NumPyConverter::Convert() {
  if (PyArray_NDIM(arr_) != 1) {
    return Status::Invalid("only handle 1-dimensional arrays");
  }

  if (dtype_->type_num == NPY_OBJECT) {
    return ConvertObjects();
  }

  if (type_ == nullptr) {
    return Status::Invalid("Must pass data type for non-object arrays");
  }

  // Visit the type to perform conversion
  return VisitTypeInline(*type_, this);
}

namespace {

Status CastBuffer(const std::shared_ptr<DataType>& in_type,
                  const std::shared_ptr<Buffer>& input, const int64_t length,
                  const std::shared_ptr<Buffer>& valid_bitmap, const int64_t null_count,
                  const std::shared_ptr<DataType>& out_type, MemoryPool* pool,
                  std::shared_ptr<Buffer>* out) {
  // Must cast
  auto tmp_data = ArrayData::Make(in_type, length, {valid_bitmap, input}, null_count);

  std::shared_ptr<Array> tmp_array = MakeArray(tmp_data);
  std::shared_ptr<Array> casted_array;

  compute::FunctionContext context(pool);
  compute::CastOptions cast_options;
  cast_options.allow_int_overflow = false;
  cast_options.allow_time_truncate = false;

  RETURN_NOT_OK(
      compute::Cast(&context, *tmp_array, out_type, cast_options, &casted_array));
  *out = casted_array->data()->buffers[1];
  return Status::OK();
}

template <typename FromType, typename ToType>
Status StaticCastBuffer(const Buffer& input, const int64_t length, MemoryPool* pool,
                        std::shared_ptr<Buffer>* out) {
  std::shared_ptr<Buffer> result;
  RETURN_NOT_OK(AllocateBuffer(pool, sizeof(ToType) * length, &result));

  auto in_values = reinterpret_cast<const FromType*>(input.data());
  auto out_values = reinterpret_cast<ToType*>(result->mutable_data());
  for (int64_t i = 0; i < length; ++i) {
    *out_values++ = static_cast<ToType>(*in_values++);
  }
  *out = result;
  return Status::OK();
}

template <typename T>
void CopyStridedBytewise(int8_t* input_data, int64_t length, int64_t stride,
                         T* output_data) {
  // Passing input_data as non-const is a concession to PyObject*
  for (int64_t i = 0; i < length; ++i) {
    memcpy(output_data + i, input_data, sizeof(T));
    input_data += stride;
  }
}

template <typename T>
void CopyStridedNatural(T* input_data, int64_t length, int64_t stride, T* output_data) {
  // Passing input_data as non-const is a concession to PyObject*
  int64_t j = 0;
  for (int64_t i = 0; i < length; ++i) {
    output_data[i] = input_data[j];
    j += stride;
  }
}

template <typename ArrowType>
Status CopyStridedArray(PyArrayObject* arr, const int64_t length, MemoryPool* pool,
                        std::shared_ptr<Buffer>* out) {
  using traits = internal::arrow_traits<ArrowType::type_id>;
  using T = typename traits::T;

  // Strided, must copy into new contiguous memory
  std::shared_ptr<Buffer> new_buffer;
  RETURN_NOT_OK(AllocateBuffer(pool, sizeof(T) * length, &new_buffer));

  const int64_t stride = PyArray_STRIDES(arr)[0];
  if (stride % sizeof(T) == 0) {
    const int64_t stride_elements = stride / sizeof(T);
    CopyStridedNatural(reinterpret_cast<T*>(PyArray_DATA(arr)), length, stride_elements,
                       reinterpret_cast<T*>(new_buffer->mutable_data()));
  } else {
    CopyStridedBytewise(reinterpret_cast<int8_t*>(PyArray_DATA(arr)), length, stride,
                        reinterpret_cast<T*>(new_buffer->mutable_data()));
  }

  *out = new_buffer;
  return Status::OK();
}

}  // namespace

template <typename ArrowType>
inline Status NumPyConverter::ConvertData(std::shared_ptr<Buffer>* data) {
  if (is_strided()) {
    RETURN_NOT_OK(CopyStridedArray<ArrowType>(arr_, length_, pool_, data));
  } else {
    // Can zero-copy
    *data = std::make_shared<NumPyBuffer>(reinterpret_cast<PyObject*>(arr_));
  }

  std::shared_ptr<DataType> input_type;
  RETURN_NOT_OK(NumPyDtypeToArrow(reinterpret_cast<PyObject*>(dtype_), &input_type));

  if (!input_type->Equals(*type_)) {
    RETURN_NOT_OK(CastBuffer(input_type, *data, length_, nullptr, 0, type_, pool_, data));
  }

  return Status::OK();
}

template <>
inline Status NumPyConverter::ConvertData<BooleanType>(std::shared_ptr<Buffer>* data) {
  int64_t nbytes = BitUtil::BytesForBits(length_);
  std::shared_ptr<Buffer> buffer;
  RETURN_NOT_OK(AllocateBuffer(pool_, nbytes, &buffer));

  Ndarray1DIndexer<uint8_t> values(arr_);

  uint8_t* bitmap = buffer->mutable_data();

  memset(bitmap, 0, nbytes);
  for (int i = 0; i < length_; ++i) {
    if (values[i] > 0) {
      BitUtil::SetBit(bitmap, i);
    }
  }

  *data = buffer;
  return Status::OK();
}

template <>
inline Status NumPyConverter::ConvertData<Date32Type>(std::shared_ptr<Buffer>* data) {
  if (is_strided()) {
    RETURN_NOT_OK(CopyStridedArray<Date32Type>(arr_, length_, pool_, data));
  } else {
    // Can zero-copy
    *data = std::make_shared<NumPyBuffer>(reinterpret_cast<PyObject*>(arr_));
  }

  std::shared_ptr<DataType> input_type;

  auto date_dtype = reinterpret_cast<PyArray_DatetimeDTypeMetaData*>(dtype_->c_metadata);
  if (dtype_->type_num == NPY_DATETIME) {
    // If we have inbound datetime64[D] data, this needs to be downcasted
    // separately here from int64_t to int32_t, because this data is not
    // supported in compute::Cast
    if (date_dtype->meta.base == NPY_FR_D) {
      // TODO(wesm): How pedantic do we really want to be about checking for int32
      // overflow here?
      Status s = StaticCastBuffer<int64_t, int32_t>(**data, length_, pool_, data);
      RETURN_NOT_OK(s);
    } else {
      RETURN_NOT_OK(NumPyDtypeToArrow(reinterpret_cast<PyObject*>(dtype_), &input_type));
      if (!input_type->Equals(*type_)) {
        // The null bitmap was already computed in VisitNative()
        RETURN_NOT_OK(CastBuffer(input_type, *data, length_, null_bitmap_, null_count_,
                                 type_, pool_, data));
      }
    }
  } else {
    RETURN_NOT_OK(NumPyDtypeToArrow(reinterpret_cast<PyObject*>(dtype_), &input_type));
    if (!input_type->Equals(*type_)) {
      RETURN_NOT_OK(
          CastBuffer(input_type, *data, length_, nullptr, 0, type_, pool_, data));
    }
  }

  return Status::OK();
}

template <>
inline Status NumPyConverter::ConvertData<Date64Type>(std::shared_ptr<Buffer>* data) {
  if (is_strided()) {
    RETURN_NOT_OK(CopyStridedArray<Date64Type>(arr_, length_, pool_, data));
  } else {
    // Can zero-copy
    *data = std::make_shared<NumPyBuffer>(reinterpret_cast<PyObject*>(arr_));
  }

  std::shared_ptr<DataType> input_type;

  auto date_dtype = reinterpret_cast<PyArray_DatetimeDTypeMetaData*>(dtype_->c_metadata);
  if (dtype_->type_num == NPY_DATETIME) {
    // If we have inbound datetime64[D] data, this needs to be downcasted
    // separately here from int64_t to int32_t, because this data is not
    // supported in compute::Cast
    if (date_dtype->meta.base == NPY_FR_D) {
      std::shared_ptr<Buffer> result;
      RETURN_NOT_OK(AllocateBuffer(pool_, sizeof(int64_t) * length_, &result));

      auto in_values = reinterpret_cast<const int64_t*>((*data)->data());
      auto out_values = reinterpret_cast<int64_t*>(result->mutable_data());
      for (int64_t i = 0; i < length_; ++i) {
        *out_values++ = kMillisecondsInDay * (*in_values++);
      }
      *data = result;
    } else {
      RETURN_NOT_OK(NumPyDtypeToArrow(reinterpret_cast<PyObject*>(dtype_), &input_type));
      if (!input_type->Equals(*type_)) {
        // The null bitmap was already computed in VisitNative()
        RETURN_NOT_OK(CastBuffer(input_type, *data, length_, null_bitmap_, null_count_,
                                 type_, pool_, data));
      }
    }
  } else {
    RETURN_NOT_OK(NumPyDtypeToArrow(reinterpret_cast<PyObject*>(dtype_), &input_type));
    if (!input_type->Equals(*type_)) {
      RETURN_NOT_OK(
          CastBuffer(input_type, *data, length_, nullptr, 0, type_, pool_, data));
    }
  }

  return Status::OK();
}

template <typename T>
struct UnboxDate {};

template <>
struct UnboxDate<Date32Type> {
  static int32_t Unbox(PyObject* obj) {
    return PyDate_to_days(reinterpret_cast<PyDateTime_Date*>(obj));
  }
};

template <>
struct UnboxDate<Date64Type> {
  static int64_t Unbox(PyObject* obj) {
    return PyDate_to_ms(reinterpret_cast<PyDateTime_Date*>(obj));
  }
};

template <typename ArrowType>
Status NumPyConverter::ConvertDates() {
  PyAcquireGIL lock;

  using BuilderType = typename TypeTraits<ArrowType>::BuilderType;

  Ndarray1DIndexer<PyObject*> objects(arr_);

  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  BuilderType builder(pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  /// We have to run this in this compilation unit, since we cannot use the
  /// datetime API otherwise
  PyDateTime_IMPORT;

  PyObject* obj;
  for (int64_t i = 0; i < length_; ++i) {
    obj = objects[i];
    if ((have_mask && mask_values[i]) || internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder.AppendNull());
    } else if (PyDate_Check(obj)) {
      RETURN_NOT_OK(builder.Append(UnboxDate<ArrowType>::Unbox(obj)));
    } else {
      std::stringstream ss;
      ss << "Error converting from Python objects to Date: ";
      RETURN_NOT_OK(InvalidConversion(obj, "datetime.date", &ss));
      return Status::Invalid(ss.str());
    }
  }

  return PushBuilderResult(&builder);
}

Status NumPyConverter::ConvertDecimals() {
  PyAcquireGIL lock;

  internal::DecimalMetadata max_decimal_metadata;
  Ndarray1DIndexer<PyObject*> objects(arr_);

  if (type_ == NULLPTR) {
    for (PyObject* object : objects) {
      if (!internal::PandasObjectIsNull(object)) {
        RETURN_NOT_OK(max_decimal_metadata.Update(object));
      }
    }

    type_ =
        ::arrow::decimal(max_decimal_metadata.precision(), max_decimal_metadata.scale());
  }

  Decimal128Builder builder(type_, pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  const auto& decimal_type = checked_cast<const DecimalType&>(*type_);

  for (PyObject* object : objects) {
    const int is_decimal = PyObject_IsInstance(object, decimal_type_.obj());

    if (is_decimal == 1) {
      Decimal128 value;
      RETURN_NOT_OK(internal::DecimalFromPythonDecimal(object, decimal_type, &value));
      RETURN_NOT_OK(builder.Append(value));
    } else if (is_decimal == 0 && internal::PandasObjectIsNull(object)) {
      RETURN_NOT_OK(builder.AppendNull());
    } else {
      // PyObject_IsInstance could error and set an exception
      RETURN_IF_PYERROR();
      std::stringstream ss;
      ss << "Error converting from Python objects to Decimal: ";
      RETURN_NOT_OK(InvalidConversion(object, "decimal.Decimal", &ss));
      return Status::Invalid(ss.str());
    }
  }
  return PushBuilderResult(&builder);
}

Status NumPyConverter::ConvertDateTimes() {
  // Convert array of datetime.datetime objects to Arrow
  PyAcquireGIL lock;
  PyDateTime_IMPORT;

  Ndarray1DIndexer<PyObject*> objects(arr_);

  // datetime.datetime stores microsecond resolution
  TimestampBuilder builder(::arrow::timestamp(TimeUnit::MICRO), pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  PyObject* obj = NULLPTR;
  for (int64_t i = 0; i < length_; ++i) {
    obj = objects[i];
    if (PyDateTime_Check(obj)) {
      RETURN_NOT_OK(
          builder.Append(PyDateTime_to_us(reinterpret_cast<PyDateTime_DateTime*>(obj))));
    } else if (internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder.AppendNull());
    } else {
      std::stringstream ss;
      ss << "Error converting from Python objects to Timestamp: ";
      RETURN_NOT_OK(InvalidConversion(obj, "datetime.datetime", &ss));
      return Status::Invalid(ss.str());
    }
  }
  return PushBuilderResult(&builder);
}

Status NumPyConverter::ConvertTimes() {
  // Convert array of datetime.time objects to Arrow
  PyAcquireGIL lock;
  PyDateTime_IMPORT;

  Ndarray1DIndexer<PyObject*> objects(arr_);

  // datetime.time stores microsecond resolution
  Time64Builder builder(::arrow::time64(TimeUnit::MICRO), pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  PyObject* obj = NULLPTR;
  for (int64_t i = 0; i < length_; ++i) {
    obj = objects[i];
    if (PyTime_Check(obj)) {
      RETURN_NOT_OK(builder.Append(PyTime_to_us(obj)));
    } else if (internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder.AppendNull());
    } else {
      std::stringstream ss;
      ss << "Error converting from Python objects to Time: ";
      RETURN_NOT_OK(InvalidConversion(obj, "datetime.time", &ss));
      return Status::Invalid(ss.str());
    }
  }
  return PushBuilderResult(&builder);
}

Status NumPyConverter::ConvertObjectStrings() {
  PyAcquireGIL lock;

  // The output type at this point is inconclusive because there may be bytes
  // and unicode mixed in the object array
  StringBuilder builder(pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  // If the creator of this NumPyConverter specified a type,
  // then we want to force the output type to be utf8. If
  // the input data is PyBytes and not PyUnicode and
  // not convertible to utf8, the call to AppendObjectStrings
  // below will fail because we pass force_string as the
  // value for check_valid.
  bool force_string = type_ != nullptr && type_->Equals(utf8());
  bool global_have_bytes = false;
  if (length_ == 0) {
    // Produce an empty chunk
    std::shared_ptr<Array> chunk;
    RETURN_NOT_OK(builder.Finish(&chunk));
    out_arrays_.emplace_back(std::move(chunk));
  } else {
    int64_t offset = 0;
    while (offset < length_) {
      bool chunk_have_bytes = false;
      // Always set check_valid to true when force_string is true
      RETURN_NOT_OK(AppendObjectStrings(arr_, mask_, offset,
                                        force_string /* check_valid */, &builder, &offset,
                                        &chunk_have_bytes));

      global_have_bytes = global_have_bytes | chunk_have_bytes;
      std::shared_ptr<Array> chunk;
      RETURN_NOT_OK(builder.Finish(&chunk));
      out_arrays_.emplace_back(std::move(chunk));
    }
  }

  // If we saw bytes, convert it to a binary array. If
  // force_string was set to true, the input data could
  // have been bytes but we've checked to make sure that
  // it can be converted to utf-8 in the call to
  // AppendObjectStrings. In that case, we can safely leave
  // it as a utf8 type.
  if (!force_string && global_have_bytes) {
    for (size_t i = 0; i < out_arrays_.size(); ++i) {
      auto binary_data = out_arrays_[i]->data()->Copy();
      binary_data->type = ::arrow::binary();
      out_arrays_[i] = std::make_shared<BinaryArray>(binary_data);
    }
  }
  return Status::OK();
}

Status NumPyConverter::ConvertObjectFloats() {
  PyAcquireGIL lock;

  Ndarray1DIndexer<PyObject*> objects(arr_);
  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  DoubleBuilder builder(pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  PyObject* obj;
  for (int64_t i = 0; i < objects.size(); ++i) {
    obj = objects[i];
    if ((have_mask && mask_values[i]) || internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder.AppendNull());
    } else if (PyFloat_Check(obj)) {
      double val = PyFloat_AsDouble(obj);
      RETURN_IF_PYERROR();
      RETURN_NOT_OK(builder.Append(val));
    } else {
      std::stringstream ss;
      ss << "Error converting from Python objects to Double: ";
      RETURN_NOT_OK(InvalidConversion(obj, "float", &ss));
      return Status::Invalid(ss.str());
    }
  }

  return PushBuilderResult(&builder);
}

Status NumPyConverter::ConvertObjectIntegers() {
  PyAcquireGIL lock;

  Int64Builder builder(pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  Ndarray1DIndexer<PyObject*> objects(arr_);
  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  PyObject* obj;
  for (int64_t i = 0; i < objects.size(); ++i) {
    obj = objects[i];
    if ((have_mask && mask_values[i]) || internal::PandasObjectIsNull(obj)) {
      RETURN_NOT_OK(builder.AppendNull());
    } else {
      int64_t val;
      RETURN_NOT_OK(internal::CIntFromPython(obj, &val));
      RETURN_NOT_OK(builder.Append(val));
    }
  }

  return PushBuilderResult(&builder);
}

Status NumPyConverter::ConvertObjectBytes() {
  PyAcquireGIL lock;

  BinaryBuilder builder(binary(), pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  if (length_ == 0) {
    // Produce an empty chunk
    std::shared_ptr<Array> chunk;
    RETURN_NOT_OK(builder.Finish(&chunk));
    out_arrays_.emplace_back(std::move(chunk));
  } else {
    int64_t offset = 0;
    while (offset < length_) {
      RETURN_NOT_OK(AppendObjectBinaries(arr_, mask_, offset, &builder, &offset));
      std::shared_ptr<Array> chunk;
      RETURN_NOT_OK(builder.Finish(&chunk));
      out_arrays_.emplace_back(std::move(chunk));
    }
  }
  return Status::OK();
}

Status NumPyConverter::ConvertObjectFixedWidthBytes(
    const std::shared_ptr<DataType>& type) {
  PyAcquireGIL lock;

  const int32_t byte_width = checked_cast<const FixedSizeBinaryType&>(*type).byte_width();

  // The output type at this point is inconclusive because there may be bytes
  // and unicode mixed in the object array
  FixedSizeBinaryBuilder builder(type, pool_);
  RETURN_NOT_OK(builder.Resize(length_));

  if (length_ == 0) {
    // Produce an empty chunk
    std::shared_ptr<Array> chunk;
    RETURN_NOT_OK(builder.Finish(&chunk));
    out_arrays_.emplace_back(std::move(chunk));
  } else {
    int64_t offset = 0;
    while (offset < length_) {
      RETURN_NOT_OK(AppendObjectFixedWidthBytes(arr_, mask_, byte_width, offset, &builder,
                                                &offset));

      std::shared_ptr<Array> chunk;
      RETURN_NOT_OK(builder.Finish(&chunk));
      out_arrays_.emplace_back(std::move(chunk));
    }
  }
  return Status::OK();
}

Status NumPyConverter::ConvertBooleans() {
  PyAcquireGIL lock;

  Ndarray1DIndexer<PyObject*> objects(arr_);
  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  int64_t nbytes = BitUtil::BytesForBits(length_);
  std::shared_ptr<Buffer> data;
  RETURN_NOT_OK(AllocateBuffer(pool_, nbytes, &data));
  uint8_t* bitmap = data->mutable_data();
  memset(bitmap, 0, nbytes);

  int64_t null_count = 0;
  PyObject* obj;
  for (int64_t i = 0; i < length_; ++i) {
    obj = objects[i];
    if ((have_mask && mask_values[i]) || internal::PandasObjectIsNull(obj)) {
      ++null_count;
    } else if (obj == Py_True) {
      BitUtil::SetBit(bitmap, i);
      BitUtil::SetBit(null_bitmap_data_, i);
    } else if (obj == Py_False) {
      BitUtil::SetBit(null_bitmap_data_, i);
    } else {
      std::stringstream ss;
      ss << "Error converting from Python objects to Boolean: ";
      RETURN_NOT_OK(InvalidConversion(obj, "bool", &ss));
      return Status::Invalid(ss.str());
    }
  }

  out_arrays_.push_back(
      std::make_shared<BooleanArray>(length_, data, null_bitmap_, null_count));
  return Status::OK();
}

Status NumPyConverter::ConvertObjectsInfer() {
  Ndarray1DIndexer<PyObject*> objects;

  PyAcquireGIL lock;
  objects.Init(arr_);
  PyDateTime_IMPORT;

  for (int64_t i = 0; i < length_; ++i) {
    PyObject* obj = objects[i];
    if (internal::PandasObjectIsNull(obj)) {
      continue;
    } else if (PyUnicode_Check(obj) || internal::IsPyBinary(obj)) {
      // The exact Arrow type (Binary or String) will be decided based on
      // Python object types
      return ConvertObjectStrings();
    } else if (PyFloat_Check(obj)) {
      return ConvertObjectFloats();
    } else if (PyBool_Check(obj)) {
      return ConvertBooleans();
    } else if (PyObject_is_integer(obj)) {
      return ConvertObjectIntegers();
    } else if (PyDateTime_Check(obj)) {
      return ConvertDateTimes();
    } else if (PyDate_Check(obj)) {
      // We could choose Date32 or Date64
      return ConvertDates<Date32Type>();
    } else if (PyTime_Check(obj)) {
      return ConvertTimes();
    } else if (PyObject_IsInstance(obj, decimal_type_.obj()) == 1) {
      return ConvertDecimals();
    } else if (PyList_Check(obj)) {
      if (PyList_Size(obj) == 0 && i < length_ - 1) {
        // Iterate until we find a non-empty list or the enclosing sequence is empty
        continue;
      }
      std::shared_ptr<DataType> inferred_type;
      RETURN_NOT_OK(InferArrowType(obj, &inferred_type));
      return ConvertLists(inferred_type);
    } else if (PyArray_Check(obj)) {
      std::shared_ptr<DataType> inferred_type;
      PyArray_Descr* dtype = PyArray_DESCR(reinterpret_cast<PyArrayObject*>(obj));

      if (dtype->type_num == NPY_OBJECT) {
        RETURN_NOT_OK(InferArrowType(obj, &inferred_type));
      } else {
        RETURN_NOT_OK(
            NumPyDtypeToArrow(reinterpret_cast<PyObject*>(dtype), &inferred_type));
      }
      return ConvertLists(inferred_type);
    } else {
      const std::string supported_types =
          "string, bool, float, int, date, time, decimal, bytearray, list, array";
      std::stringstream ss;
      ss << "Error inferring Arrow type for Python object array. ";
      RETURN_NOT_OK(InvalidConversion(obj, supported_types, &ss));
      return Status::Invalid(ss.str());
    }
  }
  out_arrays_.push_back(std::make_shared<NullArray>(length_));
  return Status::OK();
}

Status NumPyConverter::ConvertObjectsInferAndCast() {
  size_t position = out_arrays_.size();
  RETURN_NOT_OK(ConvertObjectsInfer());
  DCHECK_EQ(position + 1, out_arrays_.size());
  std::shared_ptr<Array> arr = out_arrays_[position];

  // Perform cast
  compute::FunctionContext context(pool_);
  compute::CastOptions options;
  options.allow_int_overflow = false;

  std::shared_ptr<Array> casted;
  RETURN_NOT_OK(compute::Cast(&context, *arr, type_, options, &casted));

  // Replace with casted values
  out_arrays_[position] = casted;

  return Status::OK();
}

Status NumPyConverter::ConvertObjects() {
  // Python object arrays are annoying, since we could have one of:
  //
  // * Strings
  // * Booleans with nulls
  // * decimal.Decimals
  // * Mixed type (not supported at the moment by arrow format)
  //
  // Additionally, nulls may be encoded either as np.nan or None. So we have to
  // do some type inference and conversion

  RETURN_NOT_OK(InitNullBitmap());

  // This means we received an explicit type from the user
  if (type_) {
    switch (type_->id()) {
      case Type::STRING:
        return ConvertObjectStrings();
      case Type::BINARY:
        return ConvertObjectBytes();
      case Type::FIXED_SIZE_BINARY:
        return ConvertObjectFixedWidthBytes(type_);
      case Type::BOOL:
        return ConvertBooleans();
      case Type::DATE32:
        return ConvertDates<Date32Type>();
      case Type::DATE64:
        return ConvertDates<Date64Type>();
      case Type::LIST: {
        const auto& list_field = checked_cast<const ListType&>(*type_);
        return ConvertLists(list_field.value_field()->type());
      }
      case Type::DECIMAL:
        return ConvertDecimals();
      default:
        return ConvertObjectsInferAndCast();
    }
  } else {
    // Re-acquire GIL
    return ConvertObjectsInfer();
  }
}

// Like VisitIterable, but the function takes a second boolean argument
// deducted from `have_mask` and `mask_values`
template <class BinaryFunction>
Status LoopPySequenceWithMasks(PyObject* sequence,
                               const Ndarray1DIndexer<uint8_t>& mask_values,
                               bool have_mask, BinaryFunction&& func) {
  if (have_mask) {
    int64_t i = 0;
    return internal::VisitIterable(sequence,
                                   [&](PyObject* obj, bool* keep_going /* unused */) {
                                     return func(obj, mask_values[i++] != 0);
                                   });
  } else {
    return internal::VisitIterable(
        sequence,
        [&](PyObject* obj, bool* keep_going /* unused */) { return func(obj, false); });
  }
}

template <int ITEM_TYPE, typename ArrowType>
inline Status NumPyConverter::ConvertTypedLists(const std::shared_ptr<DataType>& type,
                                                ListBuilder* builder, PyObject* list) {
  typedef internal::npy_traits<ITEM_TYPE> traits;
  typedef typename traits::BuilderClass BuilderT;

  PyAcquireGIL lock;

  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  auto value_builder = checked_cast<BuilderT*>(builder->value_builder());

  auto foreach_item = [&](PyObject* object, bool mask) {
    if (mask || internal::PandasObjectIsNull(object)) {
      return builder->AppendNull();
    } else if (PyArray_Check(object)) {
      auto numpy_array = reinterpret_cast<PyArrayObject*>(object);
      RETURN_NOT_OK(builder->Append(true));

      // TODO(uwe): Support more complex numpy array structures
      RETURN_NOT_OK(CheckFlatNumpyArray(numpy_array, ITEM_TYPE));

      return AppendNdarrayToBuilder<ITEM_TYPE, BuilderT>(numpy_array, value_builder);
    } else if (PyList_Check(object)) {
      int64_t size;
      std::shared_ptr<DataType> inferred_type;
      RETURN_NOT_OK(builder->Append(true));
      RETURN_NOT_OK(InferArrowTypeAndSize(object, &size, &inferred_type));
      if (inferred_type->id() != Type::NA && inferred_type->id() != type->id()) {
        std::stringstream ss;
        ss << inferred_type->ToString() << " cannot be converted to " << type->ToString();
        return Status::TypeError(ss.str());
      }
      return AppendPySequence(object, size, type, value_builder,
                              use_pandas_null_sentinels_);
    } else {
      return Status::TypeError("Unsupported Python type for list items");
    }
  };

  return LoopPySequenceWithMasks(list, mask_values, have_mask, foreach_item);
}

template <>
inline Status NumPyConverter::ConvertTypedLists<NPY_OBJECT, NullType>(
    const std::shared_ptr<DataType>& type, ListBuilder* builder, PyObject* list) {
  PyAcquireGIL lock;

  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  auto value_builder = checked_cast<NullBuilder*>(builder->value_builder());

  auto foreach_item = [&](PyObject* object, bool mask) {
    if (mask || internal::PandasObjectIsNull(object)) {
      return builder->AppendNull();
    } else if (PyArray_Check(object)) {
      auto numpy_array = reinterpret_cast<PyArrayObject*>(object);
      RETURN_NOT_OK(builder->Append(true));

      // TODO(uwe): Support more complex numpy array structures
      RETURN_NOT_OK(CheckFlatNumpyArray(numpy_array, NPY_OBJECT));

      for (int64_t i = 0; i < static_cast<int64_t>(PyArray_SIZE(numpy_array)); ++i) {
        RETURN_NOT_OK(value_builder->AppendNull());
      }
      return Status::OK();
    } else if (PyList_Check(object)) {
      RETURN_NOT_OK(builder->Append(true));
      const Py_ssize_t size = PySequence_Size(object);
      for (Py_ssize_t i = 0; i < size; ++i) {
        RETURN_NOT_OK(value_builder->AppendNull());
      }
      return Status::OK();
    } else {
      return Status::TypeError("Unsupported Python type for list items");
    }
  };

  return LoopPySequenceWithMasks(list, mask_values, have_mask, foreach_item);
}

template <>
inline Status NumPyConverter::ConvertTypedLists<NPY_OBJECT, BinaryType>(
    const std::shared_ptr<DataType>& type, ListBuilder* builder, PyObject* list) {
  PyAcquireGIL lock;

  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  auto value_builder = checked_cast<BinaryBuilder*>(builder->value_builder());

  auto foreach_item = [&](PyObject* object, bool mask) {
    if (mask || internal::PandasObjectIsNull(object)) {
      return builder->AppendNull();
    } else if (PyArray_Check(object)) {
      auto numpy_array = reinterpret_cast<PyArrayObject*>(object);
      RETURN_NOT_OK(builder->Append(true));

      // TODO(uwe): Support more complex numpy array structures
      RETURN_NOT_OK(CheckFlatNumpyArray(numpy_array, NPY_OBJECT));

      int64_t offset = 0;
      RETURN_NOT_OK(
          AppendObjectBinaries(numpy_array, nullptr, 0, value_builder, &offset));
      if (offset < PyArray_SIZE(numpy_array)) {
        return Status::CapacityError("Array cell value exceeded 2GB");
      }
      return Status::OK();
    } else if (PyList_Check(object)) {
      int64_t size;
      std::shared_ptr<DataType> inferred_type;
      RETURN_NOT_OK(builder->Append(true));
      RETURN_NOT_OK(InferArrowTypeAndSize(object, &size, &inferred_type));
      if (inferred_type->id() != Type::NA && inferred_type->id() != Type::BINARY) {
        std::stringstream ss;
        ss << inferred_type->ToString() << " cannot be converted to BINARY.";
        return Status::TypeError(ss.str());
      }
      return AppendPySequence(object, size, type, value_builder,
                              use_pandas_null_sentinels_);
    } else {
      return Status::TypeError("Unsupported Python type for list items");
    }
  };

  return LoopPySequenceWithMasks(list, mask_values, have_mask, foreach_item);
}

template <>
inline Status NumPyConverter::ConvertTypedLists<NPY_OBJECT, StringType>(
    const std::shared_ptr<DataType>& type, ListBuilder* builder, PyObject* list) {
  PyAcquireGIL lock;
  // TODO: If there are bytes involed, convert to Binary representation
  bool have_bytes = false;

  Ndarray1DIndexer<uint8_t> mask_values;

  bool have_mask = false;
  if (mask_ != nullptr) {
    mask_values.Init(mask_);
    have_mask = true;
  }

  auto value_builder = checked_cast<StringBuilder*>(builder->value_builder());

  auto foreach_item = [&](PyObject* object, bool mask) {
    if (mask || internal::PandasObjectIsNull(object)) {
      return builder->AppendNull();
    } else if (PyArray_Check(object)) {
      auto numpy_array = reinterpret_cast<PyArrayObject*>(object);
      RETURN_NOT_OK(builder->Append(true));

      // TODO(uwe): Support more complex numpy array structures
      RETURN_NOT_OK(CheckFlatNumpyArray(numpy_array, NPY_OBJECT));

      int64_t offset = 0;
      // If a type was specified and it was utf8, then we set
      // check_valid to true. If any of the input cannot be
      // converted, then we will exit early here.
      bool check_valid = type_ != nullptr && type_->Equals(::arrow::utf8());
      RETURN_NOT_OK(AppendObjectStrings(numpy_array, nullptr, 0, check_valid,
                                        value_builder, &offset, &have_bytes));
      if (offset < PyArray_SIZE(numpy_array)) {
        return Status::CapacityError("Array cell value exceeded 2GB");
      }
      return Status::OK();
    } else if (PyList_Check(object)) {
      int64_t size;
      std::shared_ptr<DataType> inferred_type;
      RETURN_NOT_OK(builder->Append(true));
      RETURN_NOT_OK(InferArrowTypeAndSize(object, &size, &inferred_type));
      if (inferred_type->id() != Type::NA && inferred_type->id() != Type::STRING) {
        std::stringstream ss;
        ss << inferred_type->ToString() << " cannot be converted to STRING.";
        return Status::TypeError(ss.str());
      }
      return AppendPySequence(object, size, type, value_builder,
                              use_pandas_null_sentinels_);
    } else {
      return Status::TypeError("Unsupported Python type for list items");
    }
  };

  return LoopPySequenceWithMasks(list, mask_values, have_mask, foreach_item);
}

#define LIST_CASE(TYPE, NUMPY_TYPE, ArrowType)                            \
  case Type::TYPE: {                                                      \
    return ConvertTypedLists<NUMPY_TYPE, ArrowType>(type, builder, list); \
  }

Status NumPyConverter::ConvertLists(const std::shared_ptr<DataType>& type,
                                    ListBuilder* builder, PyObject* list) {
  switch (type->id()) {
    LIST_CASE(NA, NPY_OBJECT, NullType)
    LIST_CASE(UINT8, NPY_UINT8, UInt8Type)
    LIST_CASE(INT8, NPY_INT8, Int8Type)
    LIST_CASE(UINT16, NPY_UINT16, UInt16Type)
    LIST_CASE(INT16, NPY_INT16, Int16Type)
    LIST_CASE(UINT32, NPY_UINT32, UInt32Type)
    LIST_CASE(INT32, NPY_INT32, Int32Type)
    LIST_CASE(UINT64, NPY_UINT64, UInt64Type)
    LIST_CASE(INT64, NPY_INT64, Int64Type)
    LIST_CASE(TIMESTAMP, NPY_DATETIME, TimestampType)
    LIST_CASE(HALF_FLOAT, NPY_FLOAT16, HalfFloatType)
    LIST_CASE(FLOAT, NPY_FLOAT, FloatType)
    LIST_CASE(DOUBLE, NPY_DOUBLE, DoubleType)
    LIST_CASE(BINARY, NPY_OBJECT, BinaryType)
    LIST_CASE(STRING, NPY_OBJECT, StringType)
    case Type::LIST: {
      const auto& list_type = checked_cast<const ListType&>(*type);
      auto value_builder = checked_cast<ListBuilder*>(builder->value_builder());

      return internal::VisitIterable(
          list, [this, &builder, &value_builder, &list_type](
                    PyObject* object, bool* keep_going /* unused */) {
            if (internal::PandasObjectIsNull(object)) {
              return builder->AppendNull();
            } else {
              RETURN_NOT_OK(builder->Append(true));
              return ConvertLists(list_type.value_type(), value_builder, object);
            }
          });
    }
    default: {
      std::stringstream ss;
      ss << "Unknown list item type: ";
      ss << type->ToString();
      return Status::TypeError(ss.str());
    }
  }
}

Status NumPyConverter::ConvertLists(const std::shared_ptr<DataType>& type) {
  std::unique_ptr<ArrayBuilder> array_builder;
  RETURN_NOT_OK(MakeBuilder(pool_, arrow::list(type), &array_builder));
  auto list_builder = checked_cast<ListBuilder*>(array_builder.get());
  RETURN_NOT_OK(ConvertLists(type, list_builder, reinterpret_cast<PyObject*>(arr_)));
  return PushBuilderResult(list_builder);
}

Status NumPyConverter::Visit(const BinaryType& type) {
  BinaryBuilder builder(pool_);

  auto data = reinterpret_cast<const uint8_t*>(PyArray_DATA(arr_));

  int item_length = 0;
  if (mask_ != nullptr) {
    Ndarray1DIndexer<uint8_t> mask_values(mask_);
    for (int64_t i = 0; i < length_; ++i) {
      if (mask_values[i]) {
        RETURN_NOT_OK(builder.AppendNull());
      } else {
        // This is annoying. NumPy allows strings to have nul-terminators, so
        // we must check for them here
        for (item_length = 0; item_length < itemsize_; ++item_length) {
          if (data[item_length] == 0) {
            break;
          }
        }
        RETURN_NOT_OK(builder.Append(data, item_length));
      }
      data += stride_;
    }
  } else {
    for (int64_t i = 0; i < length_; ++i) {
      for (item_length = 0; item_length < itemsize_; ++item_length) {
        // Look for nul-terminator
        if (data[item_length] == 0) {
          break;
        }
      }
      RETURN_NOT_OK(builder.Append(data, item_length));
      data += stride_;
    }
  }

  std::shared_ptr<Array> result;
  RETURN_NOT_OK(builder.Finish(&result));
  return PushArray(result->data());
}

Status NumPyConverter::Visit(const FixedSizeBinaryType& type) {
  auto byte_width = type.byte_width();

  if (itemsize_ != byte_width) {
    std::stringstream ss;
    ss << "Got bytestring of length " << itemsize_ << " (expected " << byte_width << ")";
    return Status::Invalid(ss.str());
  }

  FixedSizeBinaryBuilder builder(::arrow::fixed_size_binary(byte_width), pool_);
  auto data = reinterpret_cast<const uint8_t*>(PyArray_DATA(arr_));

  if (mask_ != nullptr) {
    Ndarray1DIndexer<uint8_t> mask_values(mask_);
    RETURN_NOT_OK(builder.AppendValues(data, length_, mask_values.data()));
  } else {
    RETURN_NOT_OK(builder.AppendValues(data, length_));
  }

  std::shared_ptr<Array> result;
  RETURN_NOT_OK(builder.Finish(&result));
  return PushArray(result->data());
}

namespace {

// NumPy unicode is UCS4/UTF32 always
constexpr int kNumPyUnicodeSize = 4;

Status AppendUTF32(const char* data, int itemsize, int byteorder,
                   StringBuilder* builder) {
  // The binary \x00\x00\x00\x00 indicates a nul terminator in NumPy unicode,
  // so we need to detect that here to truncate if necessary. Yep.
  int actual_length = 0;
  for (; actual_length < itemsize / kNumPyUnicodeSize; ++actual_length) {
    const char* code_point = data + actual_length * kNumPyUnicodeSize;
    if ((*code_point == '\0') && (*(code_point + 1) == '\0') &&
        (*(code_point + 2) == '\0') && (*(code_point + 3) == '\0')) {
      break;
    }
  }

  OwnedRef unicode_obj(PyUnicode_DecodeUTF32(data, actual_length * kNumPyUnicodeSize,
                                             nullptr, &byteorder));
  RETURN_IF_PYERROR();
  OwnedRef utf8_obj(PyUnicode_AsUTF8String(unicode_obj.obj()));
  if (utf8_obj.obj() == NULL) {
    PyErr_Clear();
    return Status::Invalid("failed converting UTF32 to UTF8");
  }

  const int32_t length = static_cast<int32_t>(PyBytes_GET_SIZE(utf8_obj.obj()));
  if (builder->value_data_length() + length > kBinaryMemoryLimit) {
    return Status::CapacityError("Encoded string length exceeds maximum size (2GB)");
  }
  return builder->Append(PyBytes_AS_STRING(utf8_obj.obj()), length);
}

}  // namespace

Status NumPyConverter::Visit(const StringType& type) {
  StringBuilder builder(pool_);

  auto data = reinterpret_cast<const char*>(PyArray_DATA(arr_));

  char numpy_byteorder = PyArray_DESCR(arr_)->byteorder;

  // For Python C API, -1 is little-endian, 1 is big-endian
  int byteorder = numpy_byteorder == '>' ? 1 : -1;

  PyAcquireGIL gil_lock;

  if (mask_ != nullptr) {
    Ndarray1DIndexer<uint8_t> mask_values(mask_);
    for (int64_t i = 0; i < length_; ++i) {
      if (mask_values[i]) {
        RETURN_NOT_OK(builder.AppendNull());
      } else {
        RETURN_NOT_OK(AppendUTF32(data, itemsize_, byteorder, &builder));
      }
      data += stride_;
    }
  } else {
    for (int64_t i = 0; i < length_; ++i) {
      RETURN_NOT_OK(AppendUTF32(data, itemsize_, byteorder, &builder));
      data += stride_;
    }
  }

  std::shared_ptr<Array> result;
  RETURN_NOT_OK(builder.Finish(&result));
  return PushArray(result->data());
}

Status NumPyConverter::Visit(const StructType& type) {
  std::vector<NumPyConverter> sub_converters;
  std::vector<OwnedRefNoGIL> sub_arrays;

  {
    PyAcquireGIL gil_lock;

    // Create converters for each struct type field
    if (dtype_->fields == NULL || !PyDict_Check(dtype_->fields)) {
      return Status::TypeError("Expected struct array");
    }

    for (auto field : type.children()) {
      PyObject* tup = PyDict_GetItemString(dtype_->fields, field->name().c_str());
      if (tup == NULL) {
        std::stringstream ss;
        ss << "Missing field '" << field->name() << "' in struct array";
        return Status::TypeError(ss.str());
      }
      PyArray_Descr* sub_dtype =
          reinterpret_cast<PyArray_Descr*>(PyTuple_GET_ITEM(tup, 0));
      DCHECK(PyArray_DescrCheck(sub_dtype));
      int offset = static_cast<int>(PyLong_AsLong(PyTuple_GET_ITEM(tup, 1)));
      RETURN_IF_PYERROR();
      Py_INCREF(sub_dtype); /* PyArray_GetField() steals ref */
      PyObject* sub_array = PyArray_GetField(arr_, sub_dtype, offset);
      RETURN_IF_PYERROR();
      sub_arrays.emplace_back(sub_array);
      sub_converters.emplace_back(pool_, sub_array, nullptr /* mask */, field->type(),
                                  use_pandas_null_sentinels_);
    }
  }

  std::vector<ArrayVector> groups;
  int64_t null_count = 0;

  // Compute null bitmap and store it as a Boolean Array to include it
  // in the rechunking below
  {
    if (mask_ != nullptr) {
      RETURN_NOT_OK(InitNullBitmap());
      null_count = MaskToBitmap(mask_, length_, null_bitmap_data_);
    }
    groups.push_back({std::make_shared<BooleanArray>(length_, null_bitmap_)});
  }

  // Convert child data
  for (auto& converter : sub_converters) {
    RETURN_NOT_OK(converter.Convert());
    groups.push_back(converter.result());
    const auto& group = groups.back();
    int64_t n = 0;
    for (const auto& array : group) {
      n += array->length();
    }
  }
  // Ensure the different array groups are chunked consistently
  groups = ::arrow::internal::RechunkArraysConsistently(groups);
  for (const auto& group : groups) {
    int64_t n = 0;
    for (const auto& array : group) {
      n += array->length();
    }
  }

  // Make struct array chunks by combining groups
  size_t ngroups = groups.size();
  size_t nchunks = groups[0].size();
  for (size_t chunk = 0; chunk < nchunks; chunk++) {
    // First group has the null bitmaps as Boolean Arrays
    const auto& null_data = groups[0][chunk]->data();
    DCHECK_EQ(null_data->type->id(), Type::BOOL);
    DCHECK_EQ(null_data->buffers.size(), 2);
    const auto& null_buffer = null_data->buffers[1];
    // Careful: the rechunked null bitmap may have a non-zero offset
    // to its buffer, and it may not even start on a byte boundary
    int64_t null_offset = null_data->offset;
    std::shared_ptr<Buffer> fixed_null_buffer;

    if (!null_buffer) {
      fixed_null_buffer = null_buffer;
    } else if (null_offset % 8 == 0) {
      fixed_null_buffer =
          std::make_shared<Buffer>(null_buffer,
                                   // byte offset
                                   null_offset / 8,
                                   // byte size
                                   BitUtil::BytesForBits(null_data->length));
    } else {
      RETURN_NOT_OK(CopyBitmap(pool_, null_buffer->data(), null_offset, null_data->length,
                               &fixed_null_buffer));
    }

    // Create struct array chunk and populate it
    auto arr_data =
        ArrayData::Make(type_, null_data->length, null_count ? kUnknownNullCount : 0, 0);
    arr_data->buffers.push_back(fixed_null_buffer);
    // Append child chunks
    for (size_t i = 1; i < ngroups; i++) {
      arr_data->child_data.push_back(groups[i][chunk]->data());
    }
    RETURN_NOT_OK(PushArray(arr_data));
  }

  return Status::OK();
}

Status NdarrayToArrow(MemoryPool* pool, PyObject* ao, PyObject* mo,
                      bool use_pandas_null_sentinels,
                      const std::shared_ptr<DataType>& type,
                      std::shared_ptr<ChunkedArray>* out) {
  if (!PyArray_Check(ao)) {
    return Status::Invalid("Input object was not a NumPy array");
  }
  NumPyConverter converter(pool, ao, mo, type, use_pandas_null_sentinels);
  RETURN_NOT_OK(converter.Convert());
  const auto& output_arrays = converter.result();
  DCHECK_GT(output_arrays.size(), 0);
  *out = std::make_shared<ChunkedArray>(output_arrays);
  return Status::OK();
}

}  // namespace py
}  // namespace arrow

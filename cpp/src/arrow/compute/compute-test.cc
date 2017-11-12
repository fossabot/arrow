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

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <sstream>
#include <vector>

#include "gtest/gtest.h"

#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/builder.h"
#include "arrow/compare.h"
#include "arrow/ipc/test-common.h"
#include "arrow/memory_pool.h"
#include "arrow/pretty_print.h"
#include "arrow/status.h"
#include "arrow/test-common.h"
#include "arrow/test-util.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"

#include "arrow/compute/context.h"
#include "arrow/compute/kernel.h"
#include "arrow/compute/kernels/cast.h"
#include "arrow/compute/kernels/hash.h"

using std::shared_ptr;
using std::vector;

namespace arrow {
namespace compute {

class ComputeFixture {
 public:
  ComputeFixture() : ctx_(default_memory_pool()) {}

 protected:
  FunctionContext ctx_;
};

// ----------------------------------------------------------------------
// Cast

static void AssertBufferSame(const Array& left, const Array& right, int buffer_index) {
  ASSERT_EQ(left.data()->buffers[buffer_index].get(),
            right.data()->buffers[buffer_index].get());
}

class TestCast : public ComputeFixture, public TestBase {
 public:
  void CheckPass(const Array& input, const Array& expected,
                 const shared_ptr<DataType>& out_type, const CastOptions& options) {
    shared_ptr<Array> result;
    ASSERT_OK(Cast(&ctx_, input, out_type, options, &result));
    ASSERT_ARRAYS_EQUAL(expected, *result);
  }

  template <typename InType, typename I_TYPE>
  void CheckFails(const shared_ptr<DataType>& in_type, const vector<I_TYPE>& in_values,
                  const vector<bool>& is_valid, const shared_ptr<DataType>& out_type,
                  const CastOptions& options) {
    shared_ptr<Array> input, result;
    if (is_valid.size() > 0) {
      ArrayFromVector<InType, I_TYPE>(in_type, is_valid, in_values, &input);
    } else {
      ArrayFromVector<InType, I_TYPE>(in_type, in_values, &input);
    }
    ASSERT_RAISES(Invalid, Cast(&ctx_, *input, out_type, options, &result));
  }

  void CheckZeroCopy(const Array& input, const shared_ptr<DataType>& out_type) {
    shared_ptr<Array> result;
    ASSERT_OK(Cast(&ctx_, input, out_type, {}, &result));
    AssertBufferSame(input, *result, 0);
    AssertBufferSame(input, *result, 1);
  }

  template <typename InType, typename I_TYPE, typename OutType, typename O_TYPE>
  void CheckCase(const shared_ptr<DataType>& in_type, const vector<I_TYPE>& in_values,
                 const vector<bool>& is_valid, const shared_ptr<DataType>& out_type,
                 const vector<O_TYPE>& out_values, const CastOptions& options) {
    shared_ptr<Array> input, expected;
    if (is_valid.size() > 0) {
      ArrayFromVector<InType, I_TYPE>(in_type, is_valid, in_values, &input);
      ArrayFromVector<OutType, O_TYPE>(out_type, is_valid, out_values, &expected);
    } else {
      ArrayFromVector<InType, I_TYPE>(in_type, in_values, &input);
      ArrayFromVector<OutType, O_TYPE>(out_type, out_values, &expected);
    }
    CheckPass(*input, *expected, out_type, options);

    // Check a sliced variant
    if (input->length() > 1) {
      CheckPass(*input->Slice(1), *expected->Slice(1), out_type, options);
    }
  }
};

TEST_F(TestCast, SameTypeZeroCopy) {
  vector<bool> is_valid = {true, false, true, true, true};
  vector<int32_t> v1 = {0, 1, 2, 3, 4};

  shared_ptr<Array> arr;
  ArrayFromVector<Int32Type, int32_t>(int32(), is_valid, v1, &arr);

  shared_ptr<Array> result;
  ASSERT_OK(Cast(&this->ctx_, *arr, int32(), {}, &result));

  AssertBufferSame(*arr, *result, 0);
  AssertBufferSame(*arr, *result, 1);
}

TEST_F(TestCast, ToBoolean) {
  CastOptions options;

  vector<bool> is_valid = {true, false, true, true, true};

  // int8, should suffice for other integers
  vector<int8_t> v1 = {0, 1, 127, -1, 0};
  vector<bool> e1 = {false, true, true, true, false};
  CheckCase<Int8Type, int8_t, BooleanType, bool>(int8(), v1, is_valid, boolean(), e1,
                                                 options);

  // floating point
  vector<double> v2 = {1.0, 0, 0, -1.0, 5.0};
  vector<bool> e2 = {true, false, false, true, true};
  CheckCase<DoubleType, double, BooleanType, bool>(float64(), v2, is_valid, boolean(), e2,
                                                   options);
}

TEST_F(TestCast, ToIntUpcast) {
  CastOptions options;
  options.allow_int_overflow = false;

  vector<bool> is_valid = {true, false, true, true, true};

  // int8 to int32
  vector<int8_t> v1 = {0, 1, 127, -1, 0};
  vector<int32_t> e1 = {0, 1, 127, -1, 0};
  CheckCase<Int8Type, int8_t, Int32Type, int32_t>(int8(), v1, is_valid, int32(), e1,
                                                  options);

  // bool to int8
  vector<bool> v2 = {false, true, false, true, true};
  vector<int8_t> e2 = {0, 1, 0, 1, 1};
  CheckCase<BooleanType, bool, Int8Type, int8_t>(boolean(), v2, is_valid, int8(), e2,
                                                 options);

  // uint8 to int16, no overflow/underrun
  vector<uint8_t> v3 = {0, 100, 200, 255, 0};
  vector<int16_t> e3 = {0, 100, 200, 255, 0};
  CheckCase<UInt8Type, uint8_t, Int16Type, int16_t>(uint8(), v3, is_valid, int16(), e3,
                                                    options);

  // floating point to integer
  vector<double> v4 = {1.5, 0, 0.5, -1.5, 5.5};
  vector<int32_t> e4 = {1, 0, 0, -1, 5};
  CheckCase<DoubleType, double, Int32Type, int32_t>(float64(), v4, is_valid, int32(), e4,
                                                    options);
}

TEST_F(TestCast, OverflowInNullSlot) {
  CastOptions options;
  options.allow_int_overflow = false;

  vector<bool> is_valid = {true, false, true, true, true};

  vector<int32_t> v11 = {0, 70000, 2000, 1000, 0};
  vector<int16_t> e11 = {0, 0, 2000, 1000, 0};

  shared_ptr<Array> expected;
  ArrayFromVector<Int16Type, int16_t>(int16(), is_valid, e11, &expected);

  auto buf = std::make_shared<Buffer>(reinterpret_cast<const uint8_t*>(v11.data()),
                                      static_cast<int64_t>(v11.size()));
  Int32Array tmp11(5, buf, expected->null_bitmap(), -1);

  CheckPass(tmp11, *expected, int16(), options);
}

TEST_F(TestCast, ToIntDowncastSafe) {
  CastOptions options;
  options.allow_int_overflow = false;

  vector<bool> is_valid = {true, false, true, true, true};

  // int16 to uint8, no overflow/underrun
  vector<int16_t> v5 = {0, 100, 200, 1, 2};
  vector<uint8_t> e5 = {0, 100, 200, 1, 2};
  CheckCase<Int16Type, int16_t, UInt8Type, uint8_t>(int16(), v5, is_valid, uint8(), e5,
                                                    options);

  // int16 to uint8, with overflow
  vector<int16_t> v6 = {0, 100, 256, 0, 0};
  CheckFails<Int16Type>(int16(), v6, is_valid, uint8(), options);

  // underflow
  vector<int16_t> v7 = {0, 100, -1, 0, 0};
  CheckFails<Int16Type>(int16(), v7, is_valid, uint8(), options);

  // int32 to int16, no overflow
  vector<int32_t> v8 = {0, 1000, 2000, 1, 2};
  vector<int16_t> e8 = {0, 1000, 2000, 1, 2};
  CheckCase<Int32Type, int32_t, Int16Type, int16_t>(int32(), v8, is_valid, int16(), e8,
                                                    options);

  // int32 to int16, overflow
  vector<int32_t> v9 = {0, 1000, 2000, 70000, 0};
  CheckFails<Int32Type>(int32(), v9, is_valid, int16(), options);

  // underflow
  vector<int32_t> v10 = {0, 1000, 2000, -70000, 0};
  CheckFails<Int32Type>(int32(), v9, is_valid, int16(), options);
}

TEST_F(TestCast, ToIntDowncastUnsafe) {
  CastOptions options;
  options.allow_int_overflow = true;

  vector<bool> is_valid = {true, false, true, true, true};

  // int16 to uint8, no overflow/underrun
  vector<int16_t> v5 = {0, 100, 200, 1, 2};
  vector<uint8_t> e5 = {0, 100, 200, 1, 2};
  CheckCase<Int16Type, int16_t, UInt8Type, uint8_t>(int16(), v5, is_valid, uint8(), e5,
                                                    options);

  // int16 to uint8, with overflow
  vector<int16_t> v6 = {0, 100, 256, 0, 0};
  vector<uint8_t> e6 = {0, 100, 0, 0, 0};
  CheckCase<Int16Type, int16_t, UInt8Type, uint8_t>(int16(), v6, is_valid, uint8(), e6,
                                                    options);

  // underflow
  vector<int16_t> v7 = {0, 100, -1, 0, 0};
  vector<uint8_t> e7 = {0, 100, 255, 0, 0};
  CheckCase<Int16Type, int16_t, UInt8Type, uint8_t>(int16(), v7, is_valid, uint8(), e7,
                                                    options);

  // int32 to int16, no overflow
  vector<int32_t> v8 = {0, 1000, 2000, 1, 2};
  vector<int16_t> e8 = {0, 1000, 2000, 1, 2};
  CheckCase<Int32Type, int32_t, Int16Type, int16_t>(int32(), v8, is_valid, int16(), e8,
                                                    options);

  // int32 to int16, overflow
  // TODO(wesm): do we want to allow this? we could set to null
  vector<int32_t> v9 = {0, 1000, 2000, 70000, 0};
  vector<int16_t> e9 = {0, 1000, 2000, 4464, 0};
  CheckCase<Int32Type, int32_t, Int16Type, int16_t>(int32(), v9, is_valid, int16(), e9,
                                                    options);

  // underflow
  // TODO(wesm): do we want to allow this? we could set overflow to null
  vector<int32_t> v10 = {0, 1000, 2000, -70000, 0};
  vector<int16_t> e10 = {0, 1000, 2000, -4464, 0};
  CheckCase<Int32Type, int32_t, Int16Type, int16_t>(int32(), v10, is_valid, int16(), e10,
                                                    options);
}

TEST_F(TestCast, TimestampToTimestamp) {
  CastOptions options;

  auto CheckTimestampCast = [this](
      const CastOptions& options, TimeUnit::type from_unit, TimeUnit::type to_unit,
      const vector<int64_t>& from_values, const vector<int64_t>& to_values,
      const vector<bool>& is_valid) {
    CheckCase<TimestampType, int64_t, TimestampType, int64_t>(
        timestamp(from_unit), from_values, is_valid, timestamp(to_unit), to_values,
        options);
  };

  vector<bool> is_valid = {true, false, true, true, true};

  // Multiply promotions
  vector<int64_t> v1 = {0, 100, 200, 1, 2};
  vector<int64_t> e1 = {0, 100000, 200000, 1000, 2000};
  CheckTimestampCast(options, TimeUnit::SECOND, TimeUnit::MILLI, v1, e1, is_valid);

  vector<int64_t> v2 = {0, 100, 200, 1, 2};
  vector<int64_t> e2 = {0, 100000000L, 200000000L, 1000000, 2000000};
  CheckTimestampCast(options, TimeUnit::SECOND, TimeUnit::MICRO, v2, e2, is_valid);

  vector<int64_t> v3 = {0, 100, 200, 1, 2};
  vector<int64_t> e3 = {0, 100000000000L, 200000000000L, 1000000000L, 2000000000L};
  CheckTimestampCast(options, TimeUnit::SECOND, TimeUnit::NANO, v3, e3, is_valid);

  vector<int64_t> v4 = {0, 100, 200, 1, 2};
  vector<int64_t> e4 = {0, 100000, 200000, 1000, 2000};
  CheckTimestampCast(options, TimeUnit::MILLI, TimeUnit::MICRO, v4, e4, is_valid);

  vector<int64_t> v5 = {0, 100, 200, 1, 2};
  vector<int64_t> e5 = {0, 100000000L, 200000000L, 1000000, 2000000};
  CheckTimestampCast(options, TimeUnit::MILLI, TimeUnit::NANO, v5, e5, is_valid);

  vector<int64_t> v6 = {0, 100, 200, 1, 2};
  vector<int64_t> e6 = {0, 100000, 200000, 1000, 2000};
  CheckTimestampCast(options, TimeUnit::MICRO, TimeUnit::NANO, v6, e6, is_valid);

  // Zero copy
  shared_ptr<Array> arr;
  vector<int64_t> v7 = {0, 70000, 2000, 1000, 0};
  ArrayFromVector<TimestampType, int64_t>(timestamp(TimeUnit::SECOND), is_valid, v7,
                                          &arr);
  CheckZeroCopy(*arr, timestamp(TimeUnit::SECOND));

  // Divide, truncate
  vector<int64_t> v8 = {0, 100123, 200456, 1123, 2456};
  vector<int64_t> e8 = {0, 100, 200, 1, 2};

  options.allow_time_truncate = true;
  CheckTimestampCast(options, TimeUnit::MILLI, TimeUnit::SECOND, v8, e8, is_valid);
  CheckTimestampCast(options, TimeUnit::MICRO, TimeUnit::MILLI, v8, e8, is_valid);
  CheckTimestampCast(options, TimeUnit::NANO, TimeUnit::MICRO, v8, e8, is_valid);

  vector<int64_t> v9 = {0, 100123000, 200456000, 1123000, 2456000};
  vector<int64_t> e9 = {0, 100, 200, 1, 2};
  CheckTimestampCast(options, TimeUnit::MICRO, TimeUnit::SECOND, v9, e9, is_valid);
  CheckTimestampCast(options, TimeUnit::NANO, TimeUnit::MILLI, v9, e9, is_valid);

  vector<int64_t> v10 = {0, 100123000000L, 200456000000L, 1123000000L, 2456000000};
  vector<int64_t> e10 = {0, 100, 200, 1, 2};
  CheckTimestampCast(options, TimeUnit::NANO, TimeUnit::SECOND, v10, e10, is_valid);

  // Disallow truncate, failures
  options.allow_time_truncate = false;
  CheckFails<TimestampType>(timestamp(TimeUnit::MILLI), v8, is_valid,
                            timestamp(TimeUnit::SECOND), options);
  CheckFails<TimestampType>(timestamp(TimeUnit::MICRO), v8, is_valid,
                            timestamp(TimeUnit::MILLI), options);
  CheckFails<TimestampType>(timestamp(TimeUnit::NANO), v8, is_valid,
                            timestamp(TimeUnit::MICRO), options);
  CheckFails<TimestampType>(timestamp(TimeUnit::MICRO), v9, is_valid,
                            timestamp(TimeUnit::SECOND), options);
  CheckFails<TimestampType>(timestamp(TimeUnit::NANO), v9, is_valid,
                            timestamp(TimeUnit::MILLI), options);
  CheckFails<TimestampType>(timestamp(TimeUnit::NANO), v10, is_valid,
                            timestamp(TimeUnit::SECOND), options);
}

TEST_F(TestCast, TimestampToDate32_Date64) {
  CastOptions options;

  vector<bool> is_valid = {true, true, false};

  // 2000-01-01, 2000-01-02, null
  vector<int64_t> v_nano = {946684800000000000, 946771200000000000, 0};
  vector<int64_t> v_micro = {946684800000000, 946771200000000, 0};
  vector<int64_t> v_milli = {946684800000, 946771200000, 0};
  vector<int64_t> v_second = {946684800, 946771200, 0};
  vector<int32_t> v_day = {10957, 10958, 0};

  // Simple conversions
  CheckCase<TimestampType, int64_t, Date64Type, int64_t>(
      timestamp(TimeUnit::NANO), v_nano, is_valid, date64(), v_milli, options);
  CheckCase<TimestampType, int64_t, Date64Type, int64_t>(
      timestamp(TimeUnit::MICRO), v_micro, is_valid, date64(), v_milli, options);
  CheckCase<TimestampType, int64_t, Date64Type, int64_t>(
      timestamp(TimeUnit::MILLI), v_milli, is_valid, date64(), v_milli, options);
  CheckCase<TimestampType, int64_t, Date64Type, int64_t>(
      timestamp(TimeUnit::SECOND), v_second, is_valid, date64(), v_milli, options);

  CheckCase<TimestampType, int64_t, Date32Type, int32_t>(
      timestamp(TimeUnit::NANO), v_nano, is_valid, date32(), v_day, options);
  CheckCase<TimestampType, int64_t, Date32Type, int32_t>(
      timestamp(TimeUnit::MICRO), v_micro, is_valid, date32(), v_day, options);
  CheckCase<TimestampType, int64_t, Date32Type, int32_t>(
      timestamp(TimeUnit::MILLI), v_milli, is_valid, date32(), v_day, options);
  CheckCase<TimestampType, int64_t, Date32Type, int32_t>(
      timestamp(TimeUnit::SECOND), v_second, is_valid, date32(), v_day, options);

  // Disallow truncate, failures
  vector<int64_t> v_nano_fail = {946684800000000001, 946771200000000001, 0};
  vector<int64_t> v_micro_fail = {946684800000001, 946771200000001, 0};
  vector<int64_t> v_milli_fail = {946684800001, 946771200001, 0};
  vector<int64_t> v_second_fail = {946684801, 946771201, 0};

  options.allow_time_truncate = false;
  CheckFails<TimestampType>(timestamp(TimeUnit::NANO), v_nano_fail, is_valid, date64(),
                            options);
  CheckFails<TimestampType>(timestamp(TimeUnit::MICRO), v_micro_fail, is_valid, date64(),
                            options);
  CheckFails<TimestampType>(timestamp(TimeUnit::MILLI), v_milli_fail, is_valid, date64(),
                            options);
  CheckFails<TimestampType>(timestamp(TimeUnit::SECOND), v_second_fail, is_valid,
                            date64(), options);

  CheckFails<TimestampType>(timestamp(TimeUnit::NANO), v_nano_fail, is_valid, date32(),
                            options);
  CheckFails<TimestampType>(timestamp(TimeUnit::MICRO), v_micro_fail, is_valid, date32(),
                            options);
  CheckFails<TimestampType>(timestamp(TimeUnit::MILLI), v_milli_fail, is_valid, date32(),
                            options);
  CheckFails<TimestampType>(timestamp(TimeUnit::SECOND), v_second_fail, is_valid,
                            date32(), options);

  // Make sure that nulls are excluded from the truncation checks
  vector<int64_t> v_second_nofail = {946684800, 946771200, 1};
  CheckCase<TimestampType, int64_t, Date64Type, int64_t>(
      timestamp(TimeUnit::SECOND), v_second_nofail, is_valid, date64(), v_milli, options);
  CheckCase<TimestampType, int64_t, Date32Type, int32_t>(
      timestamp(TimeUnit::SECOND), v_second_nofail, is_valid, date32(), v_day, options);
}

TEST_F(TestCast, TimeToTime) {
  CastOptions options;

  vector<bool> is_valid = {true, false, true, true, true};

  // Multiply promotions
  vector<int32_t> v1 = {0, 100, 200, 1, 2};
  vector<int32_t> e1 = {0, 100000, 200000, 1000, 2000};
  CheckCase<Time32Type, int32_t, Time32Type, int32_t>(
      time32(TimeUnit::SECOND), v1, is_valid, time32(TimeUnit::MILLI), e1, options);

  vector<int32_t> v2 = {0, 100, 200, 1, 2};
  vector<int64_t> e2 = {0, 100000000L, 200000000L, 1000000, 2000000};
  CheckCase<Time32Type, int32_t, Time64Type, int64_t>(
      time32(TimeUnit::SECOND), v2, is_valid, time64(TimeUnit::MICRO), e2, options);

  vector<int32_t> v3 = {0, 100, 200, 1, 2};
  vector<int64_t> e3 = {0, 100000000000L, 200000000000L, 1000000000L, 2000000000L};
  CheckCase<Time32Type, int32_t, Time64Type, int64_t>(
      time32(TimeUnit::SECOND), v3, is_valid, time64(TimeUnit::NANO), e3, options);

  vector<int32_t> v4 = {0, 100, 200, 1, 2};
  vector<int64_t> e4 = {0, 100000, 200000, 1000, 2000};
  CheckCase<Time32Type, int32_t, Time64Type, int64_t>(
      time32(TimeUnit::MILLI), v4, is_valid, time64(TimeUnit::MICRO), e4, options);

  vector<int32_t> v5 = {0, 100, 200, 1, 2};
  vector<int64_t> e5 = {0, 100000000L, 200000000L, 1000000, 2000000};
  CheckCase<Time32Type, int32_t, Time64Type, int64_t>(
      time32(TimeUnit::MILLI), v5, is_valid, time64(TimeUnit::NANO), e5, options);

  vector<int64_t> v6 = {0, 100, 200, 1, 2};
  vector<int64_t> e6 = {0, 100000, 200000, 1000, 2000};
  CheckCase<Time64Type, int64_t, Time64Type, int64_t>(
      time64(TimeUnit::MICRO), v6, is_valid, time64(TimeUnit::NANO), e6, options);

  // Zero copy
  shared_ptr<Array> arr;
  vector<int64_t> v7 = {0, 70000, 2000, 1000, 0};
  ArrayFromVector<Time64Type, int64_t>(time64(TimeUnit::MICRO), is_valid, v7, &arr);
  CheckZeroCopy(*arr, time64(TimeUnit::MICRO));

  // Divide, truncate
  vector<int32_t> v8 = {0, 100123, 200456, 1123, 2456};
  vector<int32_t> e8 = {0, 100, 200, 1, 2};

  options.allow_time_truncate = true;
  CheckCase<Time32Type, int32_t, Time32Type, int32_t>(
      time32(TimeUnit::MILLI), v8, is_valid, time32(TimeUnit::SECOND), e8, options);
  CheckCase<Time64Type, int32_t, Time32Type, int32_t>(
      time64(TimeUnit::MICRO), v8, is_valid, time32(TimeUnit::MILLI), e8, options);
  CheckCase<Time64Type, int32_t, Time64Type, int32_t>(
      time64(TimeUnit::NANO), v8, is_valid, time64(TimeUnit::MICRO), e8, options);

  vector<int64_t> v9 = {0, 100123000, 200456000, 1123000, 2456000};
  vector<int32_t> e9 = {0, 100, 200, 1, 2};
  CheckCase<Time64Type, int64_t, Time32Type, int32_t>(
      time64(TimeUnit::MICRO), v9, is_valid, time32(TimeUnit::SECOND), e9, options);
  CheckCase<Time64Type, int64_t, Time32Type, int32_t>(
      time64(TimeUnit::NANO), v9, is_valid, time32(TimeUnit::MILLI), e9, options);

  vector<int64_t> v10 = {0, 100123000000L, 200456000000L, 1123000000L, 2456000000};
  vector<int32_t> e10 = {0, 100, 200, 1, 2};
  CheckCase<Time64Type, int64_t, Time32Type, int32_t>(
      time64(TimeUnit::NANO), v10, is_valid, time32(TimeUnit::SECOND), e10, options);

  // Disallow truncate, failures

  options.allow_time_truncate = false;
  CheckFails<Time32Type>(time32(TimeUnit::MILLI), v8, is_valid, time32(TimeUnit::SECOND),
                         options);
  CheckFails<Time64Type>(time64(TimeUnit::MICRO), v8, is_valid, time32(TimeUnit::MILLI),
                         options);
  CheckFails<Time64Type>(time64(TimeUnit::NANO), v8, is_valid, time64(TimeUnit::MICRO),
                         options);
  CheckFails<Time64Type>(time64(TimeUnit::MICRO), v9, is_valid, time32(TimeUnit::SECOND),
                         options);
  CheckFails<Time64Type>(time64(TimeUnit::NANO), v9, is_valid, time32(TimeUnit::MILLI),
                         options);
  CheckFails<Time64Type>(time64(TimeUnit::NANO), v10, is_valid, time32(TimeUnit::SECOND),
                         options);
}

TEST_F(TestCast, DateToDate) {
  CastOptions options;

  vector<bool> is_valid = {true, false, true, true, true};

  constexpr int64_t F = 86400000;

  // Multiply promotion
  vector<int32_t> v1 = {0, 100, 200, 1, 2};
  vector<int64_t> e1 = {0, 100 * F, 200 * F, F, 2 * F};
  CheckCase<Date32Type, int32_t, Date64Type, int64_t>(date32(), v1, is_valid, date64(),
                                                      e1, options);

  // Zero copy
  shared_ptr<Array> arr;
  vector<int32_t> v2 = {0, 70000, 2000, 1000, 0};
  vector<int64_t> v3 = {0, 70000, 2000, 1000, 0};
  ArrayFromVector<Date32Type, int32_t>(date32(), is_valid, v2, &arr);
  CheckZeroCopy(*arr, date32());

  ArrayFromVector<Date64Type, int64_t>(date64(), is_valid, v3, &arr);
  CheckZeroCopy(*arr, date64());

  // Divide, truncate
  vector<int64_t> v8 = {0, 100 * F + 123, 200 * F + 456, F + 123, 2 * F + 456};
  vector<int32_t> e8 = {0, 100, 200, 1, 2};

  options.allow_time_truncate = true;
  CheckCase<Date64Type, int64_t, Date32Type, int32_t>(date64(), v8, is_valid, date32(),
                                                      e8, options);

  // Disallow truncate, failures
  options.allow_time_truncate = false;
  CheckFails<Date64Type>(date64(), v8, is_valid, date32(), options);
}

TEST_F(TestCast, ToDouble) {
  CastOptions options;
  vector<bool> is_valid = {true, false, true, true, true};

  // int16 to double
  vector<int16_t> v1 = {0, 100, 200, 1, 2};
  vector<double> e1 = {0, 100, 200, 1, 2};
  CheckCase<Int16Type, int16_t, DoubleType, double>(int16(), v1, is_valid, float64(), e1,
                                                    options);

  // float to double
  vector<float> v2 = {0, 100, 200, 1, 2};
  vector<double> e2 = {0, 100, 200, 1, 2};
  CheckCase<FloatType, float, DoubleType, double>(float32(), v2, is_valid, float64(), e2,
                                                  options);

  // bool to double
  vector<bool> v3 = {true, true, false, false, true};
  vector<double> e3 = {1, 1, 0, 0, 1};
  CheckCase<BooleanType, bool, DoubleType, double>(boolean(), v3, is_valid, float64(), e3,
                                                   options);
}

TEST_F(TestCast, UnsupportedTarget) {
  vector<bool> is_valid = {true, false, true, true, true};
  vector<int32_t> v1 = {0, 1, 2, 3, 4};

  shared_ptr<Array> arr;
  ArrayFromVector<Int32Type, int32_t>(int32(), is_valid, v1, &arr);

  shared_ptr<Array> result;
  ASSERT_RAISES(NotImplemented, Cast(&this->ctx_, *arr, utf8(), {}, &result));
}

TEST_F(TestCast, DateTimeZeroCopy) {
  vector<bool> is_valid = {true, false, true, true, true};

  shared_ptr<Array> arr;
  vector<int32_t> v1 = {0, 70000, 2000, 1000, 0};
  ArrayFromVector<Int32Type, int32_t>(int32(), is_valid, v1, &arr);

  CheckZeroCopy(*arr, time32(TimeUnit::SECOND));
  CheckZeroCopy(*arr, date32());

  vector<int64_t> v2 = {0, 70000, 2000, 1000, 0};
  ArrayFromVector<Int64Type, int64_t>(int64(), is_valid, v2, &arr);

  CheckZeroCopy(*arr, time64(TimeUnit::MICRO));
  CheckZeroCopy(*arr, date64());
  CheckZeroCopy(*arr, timestamp(TimeUnit::NANO));
}

TEST_F(TestCast, FromNull) {
  // Null casts to everything
  const int length = 10;

  NullArray arr(length);

  shared_ptr<Array> result;
  ASSERT_OK(Cast(&ctx_, arr, int32(), {}, &result));

  ASSERT_EQ(length, result->length());
  ASSERT_EQ(length, result->null_count());

  // OK to look at bitmaps
  ASSERT_ARRAYS_EQUAL(*result, *result);
}

TEST_F(TestCast, PreallocatedMemory) {
  CastOptions options;
  options.allow_int_overflow = false;

  vector<bool> is_valid = {true, false, true, true, true};

  const int64_t length = 5;

  shared_ptr<Array> arr;
  vector<int32_t> v1 = {0, 70000, 2000, 1000, 0};
  vector<int64_t> e1 = {0, 70000, 2000, 1000, 0};
  ArrayFromVector<Int32Type, int32_t>(int32(), is_valid, v1, &arr);

  auto out_type = int64();

  std::unique_ptr<UnaryKernel> kernel;
  ASSERT_OK(GetCastFunction(*int32(), out_type, options, &kernel));

  auto out_data = std::make_shared<ArrayData>(out_type, length);

  shared_ptr<Buffer> out_values;
  ASSERT_OK(this->ctx_.Allocate(length * sizeof(int64_t), &out_values));

  out_data->buffers.push_back(nullptr);
  out_data->buffers.push_back(out_values);

  vector<Datum> out = {Datum(out_data)};
  ASSERT_OK(kernel->Call(&this->ctx_, *arr->data(), &out));

  // Buffer address unchanged
  ASSERT_EQ(out_values.get(), out_data->buffers[1].get());

  shared_ptr<Array> result = MakeArray(out_data);
  shared_ptr<Array> expected;
  ArrayFromVector<Int64Type, int64_t>(int64(), is_valid, e1, &expected);

  ASSERT_ARRAYS_EQUAL(*expected, *result);
}

template <typename TestType>
class TestDictionaryCast : public TestCast {};

typedef ::testing::Types<NullType, UInt8Type, Int8Type, UInt16Type, Int16Type, Int32Type,
                         UInt32Type, UInt64Type, Int64Type, FloatType, DoubleType,
                         Date32Type, Date64Type, FixedSizeBinaryType, BinaryType>
    TestTypes;

TYPED_TEST_CASE(TestDictionaryCast, TestTypes);

TYPED_TEST(TestDictionaryCast, Basic) {
  CastOptions options;
  shared_ptr<Array> plain_array =
      TestBase::MakeRandomArray<typename TypeTraits<TypeParam>::ArrayType>(10, 2);

  Datum out;
  ASSERT_OK(DictionaryEncode(&this->ctx_, Datum(plain_array->data()), &out));

  this->CheckPass(*MakeArray(out.array()), *plain_array, plain_array->type(), options);
}

/*TYPED_TEST(TestDictionaryCast, Reverse) {
  CastOptions options;
  shared_ptr<Array> plain_array =
      TestBase::MakeRandomArray<typename TypeTraits<TypeParam>::ArrayType>(10, 2);

  shared_ptr<Array> dict_array;
  ASSERT_OK(EncodeArrayToDictionary(*plain_array, this->pool_, &dict_array));

  this->CheckPass(*plain_array, *dict_array, dict_array->type(), options);
}*/

// ----------------------------------------------------------------------
// Dictionary tests

template <typename Type>
class TestHashKernelPrimitive : public ComputeFixture, public TestBase {
 public:
  using T = typename Type::c_type;

  void CheckUnique(const vector<T>& in_values, const vector<bool>& in_is_valid,
                   const vector<T>& out_values, const vector<bool>& out_is_valid) {
    shared_ptr<Array> input, expected;
    if (in_is_valid.size() > 0) {
      ArrayFromVector<Type, T>(in_is_valid, in_values, &input);
    } else {
      ArrayFromVector<Type, T>(in_values, &input);
    }

    if (out_is_valid.size() > 0) {
      ArrayFromVector<Type, T>(out_is_valid, out_values, &expected);
    } else {
      ArrayFromVector<Type, T>(out_values, &expected);
    }

    shared_ptr<Array> result;
    ASSERT_OK(Unique(&ctx_, Datum(input), &result));
    ASSERT_ARRAYS_EQUAL(*expected, *result);
  }
};

typedef ::testing::Types<Int8Type, UInt8Type, Int16Type, UInt16Type, Int32Type,
                         UInt32Type, Int64Type, UInt64Type, FloatType, DoubleType>
    PrimitiveDictionaries;

TYPED_TEST_CASE(TestHashKernelPrimitive, PrimitiveDictionaries);

TYPED_TEST(TestHashKernelPrimitive, EncodeBasic) {
  this->CheckUnique({2, 1, 2, 1}, {true, false, true, true}, {2, 1}, {});
}

// TYPED_TEST(TestDictionaryEncode, Basic) {
//   DictionaryBuilder<TypeParam> builder(default_memory_pool());
//   ASSERT_OK(builder.Append(static_cast<typename TypeParam::c_type>(1)));
//   ASSERT_OK(builder.Append(static_cast<typename TypeParam::c_type>(2)));
//   ASSERT_OK(builder.Append(static_cast<typename TypeParam::c_type>(1)));

//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Build expected data
//   NumericBuilder<TypeParam> dict_builder;
//   ASSERT_OK(dict_builder.Append(static_cast<typename TypeParam::c_type>(1)));
//   ASSERT_OK(dict_builder.Append(static_cast<typename TypeParam::c_type>(2)));
//   shared_ptr<Array> dict_array;
//   ASSERT_OK(dict_builder.Finish(&dict_array));
//   auto dtype = std::make_shared<DictionaryType>(int8(), dict_array);

//   Int8Builder int_builder;
//   ASSERT_OK(int_builder.Append(0));
//   ASSERT_OK(int_builder.Append(1));
//   ASSERT_OK(int_builder.Append(0));
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// TYPED_TEST(TestDictionaryEncode, ArrayConversion) {
//   NumericBuilder<TypeParam> builder;
//   // DictionaryBuilder<TypeParam> builder;
//   ASSERT_OK(builder.Append(static_cast<typename TypeParam::c_type>(1)));
//   ASSERT_OK(builder.Append(static_cast<typename TypeParam::c_type>(2)));
//   ASSERT_OK(builder.Append(static_cast<typename TypeParam::c_type>(1)));

//   shared_ptr<Array> intermediate_result;
//   ASSERT_OK(builder.Finish(&intermediate_result));
//   DictionaryBuilder<TypeParam> dictionary_builder(default_memory_pool());
//   ASSERT_OK(dictionary_builder.AppendArray(*intermediate_result));
//   shared_ptr<Array> result;
//   ASSERT_OK(dictionary_builder.Finish(&result));

//   // Build expected data
//   NumericBuilder<TypeParam> dict_builder;
//   ASSERT_OK(dict_builder.Append(static_cast<typename TypeParam::c_type>(1)));
//   ASSERT_OK(dict_builder.Append(static_cast<typename TypeParam::c_type>(2)));
//   shared_ptr<Array> dict_array;
//   ASSERT_OK(dict_builder.Finish(&dict_array));
//   auto dtype = std::make_shared<DictionaryType>(int8(), dict_array);

//   Int8Builder int_builder;
//   ASSERT_OK(int_builder.Append(0));
//   ASSERT_OK(int_builder.Append(1));
//   ASSERT_OK(int_builder.Append(0));
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// TYPED_TEST(TestDictionaryEncode, DoubleTableSize) {
//   using Scalar = typename TypeParam::c_type;
//   // Skip this test for (u)int8
//   if (sizeof(Scalar) > 1) {
//     // Build the dictionary Array
//     DictionaryBuilder<TypeParam> builder(default_memory_pool());
//     // Build expected data
//     NumericBuilder<TypeParam> dict_builder;
//     Int16Builder int_builder;

//     // Fill with 1024 different values
//     for (int64_t i = 0; i < 1024; i++) {
//       ASSERT_OK(builder.Append(static_cast<Scalar>(i)));
//       ASSERT_OK(dict_builder.Append(static_cast<Scalar>(i)));
//       ASSERT_OK(int_builder.Append(static_cast<uint16_t>(i)));
//     }
//     // Fill with an already existing value
//     for (int64_t i = 0; i < 1024; i++) {
//       ASSERT_OK(builder.Append(static_cast<Scalar>(1)));
//       ASSERT_OK(int_builder.Append(1));
//     }

//     // Finalize result
//     shared_ptr<Array> result;
//     ASSERT_OK(builder.Finish(&result));

//     // Finalize expected data
//     shared_ptr<Array> dict_array;
//     ASSERT_OK(dict_builder.Finish(&dict_array));
//     auto dtype = std::make_shared<DictionaryType>(int16(), dict_array);
//     shared_ptr<Array> int_array;
//     ASSERT_OK(int_builder.Finish(&int_array));

//     DictionaryArray expected(dtype, int_array);
//     ASSERT_TRUE(expected.Equals(result));
//   }
// }

// TEST(TestStringDictionaryEncode, UniqueBasic) {
//   // Build the dictionary Array
//   StringUniqueBuilder builder(default_memory_pool());
//   ASSERT_OK(builder.Append("test"));
//   ASSERT_OK(builder.Append("test2"));
//   ASSERT_OK(builder.Append("test"));

//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Build expected data
//   StringBuilder str_builder;
//   ASSERT_OK(str_builder.Append("test"));
//   ASSERT_OK(str_builder.Append("test2"));
//   shared_ptr<Array> expected;
//   ASSERT_OK(str_builder.Finish(&expected));

//   ASSERT_TRUE(expected->Equals(result));
// }

// TEST(TestStringDictionaryEncode, Basic) {
//   // Build the dictionary Array
//   StringDictionaryBuilder builder(default_memory_pool());
//   ASSERT_OK(builder.Append("test"));
//   ASSERT_OK(builder.Append("test2"));
//   ASSERT_OK(builder.Append("test"));

//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Build expected data
//   StringBuilder str_builder;
//   ASSERT_OK(str_builder.Append("test"));
//   ASSERT_OK(str_builder.Append("test2"));
//   shared_ptr<Array> str_array;
//   ASSERT_OK(str_builder.Finish(&str_array));
//   auto dtype = std::make_shared<DictionaryType>(int8(), str_array);

//   Int8Builder int_builder;
//   ASSERT_OK(int_builder.Append(0));
//   ASSERT_OK(int_builder.Append(1));
//   ASSERT_OK(int_builder.Append(0));
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// TEST(TestStringDictionaryEncode, DoubleTableSize) {
//   // Build the dictionary Array
//   StringDictionaryBuilder builder(default_memory_pool());
//   // Build expected data
//   StringBuilder str_builder;
//   Int16Builder int_builder;

//   // Fill with 1024 different values
//   for (int64_t i = 0; i < 1024; i++) {
//     std::stringstream ss;
//     ss << "test" << i;
//     ASSERT_OK(builder.Append(ss.str()));
//     ASSERT_OK(str_builder.Append(ss.str()));
//     ASSERT_OK(int_builder.Append(static_cast<uint16_t>(i)));
//   }
//   // Fill with an already existing value
//   for (int64_t i = 0; i < 1024; i++) {
//     ASSERT_OK(builder.Append("test1"));
//     ASSERT_OK(int_builder.Append(1));
//   }

//   // Finalize result
//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Finalize expected data
//   shared_ptr<Array> str_array;
//   ASSERT_OK(str_builder.Finish(&str_array));
//   auto dtype = std::make_shared<DictionaryType>(int16(), str_array);
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// TEST(TestFixedSizeBinaryDictionaryBuilder, UniqueBasic) {
//   // Build the dictionary Array
//   UniqueBuilder<FixedSizeBinaryType> builder(arrow::fixed_size_binary(4),
//                                              default_memory_pool());
//   vector<uint8_t> test{12, 12, 11, 12};
//   vector<uint8_t> test2{12, 12, 11, 11};
//   ASSERT_OK(builder.Append(test.data()));
//   ASSERT_OK(builder.Append(test2.data()));
//   ASSERT_OK(builder.Append(test.data()));

//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Build expected data
//   FixedSizeBinaryBuilder fsb_builder(arrow::fixed_size_binary(4));
//   ASSERT_OK(fsb_builder.Append(test.data()));
//   ASSERT_OK(fsb_builder.Append(test2.data()));
//   shared_ptr<Array> expected;
//   ASSERT_OK(fsb_builder.Finish(&expected));

//   ASSERT_TRUE(expected->Equals(result));
// }

// TEST(TestFixedSizeBinaryDictionaryBuilder, Basic) {
//   // Build the dictionary Array
//   DictionaryBuilder<FixedSizeBinaryType> builder(arrow::fixed_size_binary(4),
//                                                  default_memory_pool());
//   vector<uint8_t> test{12, 12, 11, 12};
//   vector<uint8_t> test2{12, 12, 11, 11};
//   ASSERT_OK(builder.Append(test.data()));
//   ASSERT_OK(builder.Append(test2.data()));
//   ASSERT_OK(builder.Append(test.data()));

//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Build expected data
//   FixedSizeBinaryBuilder fsb_builder(arrow::fixed_size_binary(4));
//   ASSERT_OK(fsb_builder.Append(test.data()));
//   ASSERT_OK(fsb_builder.Append(test2.data()));
//   shared_ptr<Array> fsb_array;
//   ASSERT_OK(fsb_builder.Finish(&fsb_array));
//   auto dtype = std::make_shared<DictionaryType>(int8(), fsb_array);

//   Int8Builder int_builder;
//   ASSERT_OK(int_builder.Append(0));
//   ASSERT_OK(int_builder.Append(1));
//   ASSERT_OK(int_builder.Append(0));
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// TEST(TestFixedSizeBinaryDictionaryBuilder, DoubleTableSize) {
//   // Build the dictionary Array
//   DictionaryBuilder<FixedSizeBinaryType> builder(arrow::fixed_size_binary(4),
//                                                  default_memory_pool());
//   // Build expected data
//   FixedSizeBinaryBuilder fsb_builder(arrow::fixed_size_binary(4));
//   Int16Builder int_builder;

//   // Fill with 1024 different values
//   for (int64_t i = 0; i < 1024; i++) {
//     vector<uint8_t> value{12, 12, static_cast<uint8_t>(i / 128),
//                                static_cast<uint8_t>(i % 128)};
//     ASSERT_OK(builder.Append(value.data()));
//     ASSERT_OK(fsb_builder.Append(value.data()));
//     ASSERT_OK(int_builder.Append(static_cast<uint16_t>(i)));
//   }
//   // Fill with an already existing value
//   vector<uint8_t> known_value{12, 12, 0, 1};
//   for (int64_t i = 0; i < 1024; i++) {
//     ASSERT_OK(builder.Append(known_value.data()));
//     ASSERT_OK(int_builder.Append(1));
//   }

//   // Finalize result
//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Finalize expected data
//   shared_ptr<Array> fsb_array;
//   ASSERT_OK(fsb_builder.Finish(&fsb_array));
//   auto dtype = std::make_shared<DictionaryType>(int16(), fsb_array);
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// TEST(TestFixedSizeBinaryDictionaryBuilder, InvalidTypeAppend) {
//   // Build the dictionary Array
//   DictionaryBuilder<FixedSizeBinaryType> builder(arrow::fixed_size_binary(4),
//                                                  default_memory_pool());
//   // Build an array with different byte width
//   FixedSizeBinaryBuilder fsb_builder(arrow::fixed_size_binary(5));
//   vector<uint8_t> value{100, 1, 1, 1, 1};
//   ASSERT_OK(fsb_builder.Append(value.data()));
//   shared_ptr<Array> fsb_array;
//   ASSERT_OK(fsb_builder.Finish(&fsb_array));

//   ASSERT_RAISES(Invalid, builder.AppendArray(*fsb_array));
// }

// TEST(TestDecimalDictionaryBuilder, Basic) {
//   // Build the dictionary Array
//   const auto& decimal_type = arrow::decimal(2, 0);
//   DictionaryBuilder<FixedSizeBinaryType> builder(decimal_type, default_memory_pool());

//   // Test data
//   vector<Decimal128> test{12, 12, 11, 12};
//   for (const auto& value : test) {
//     ASSERT_OK(builder.Append(value.ToBytes().data()));
//   }

//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Build expected data
//   FixedSizeBinaryBuilder decimal_builder(decimal_type);
//   ASSERT_OK(decimal_builder.Append(Decimal128(12).ToBytes()));
//   ASSERT_OK(decimal_builder.Append(Decimal128(11).ToBytes()));

//   shared_ptr<Array> decimal_array;
//   ASSERT_OK(decimal_builder.Finish(&decimal_array));
//   auto dtype = arrow::dictionary(int8(), decimal_array);

//   Int8Builder int_builder;
//   ASSERT_OK(int_builder.Append({0, 0, 1, 0}));
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// TEST(TestDecimalDictionaryBuilder, DoubleTableSize) {
//   const auto& decimal_type = arrow::decimal(21, 0);

//   // Build the dictionary Array
//   DictionaryBuilder<FixedSizeBinaryType> builder(decimal_type, default_memory_pool());

//   // Build expected data
//   FixedSizeBinaryBuilder fsb_builder(decimal_type);
//   Int16Builder int_builder;

//   // Fill with 1024 different values
//   for (int64_t i = 0; i < 1024; i++) {
//     const uint8_t bytes[] = {0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              0,
//                              12,
//                              12,
//                              static_cast<uint8_t>(i / 128),
//                              static_cast<uint8_t>(i % 128)};
//     ASSERT_OK(builder.Append(bytes));
//     ASSERT_OK(fsb_builder.Append(bytes));
//     ASSERT_OK(int_builder.Append(static_cast<uint16_t>(i)));
//   }
//   // Fill with an already existing value
//   const uint8_t known_value[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 12, 12, 0, 1};
//   for (int64_t i = 0; i < 1024; i++) {
//     ASSERT_OK(builder.Append(known_value));
//     ASSERT_OK(int_builder.Append(1));
//   }

//   // Finalize result
//   shared_ptr<Array> result;
//   ASSERT_OK(builder.Finish(&result));

//   // Finalize expected data
//   shared_ptr<Array> fsb_array;
//   ASSERT_OK(fsb_builder.Finish(&fsb_array));

//   auto dtype = std::make_shared<DictionaryType>(int16(), fsb_array);
//   shared_ptr<Array> int_array;
//   ASSERT_OK(int_builder.Finish(&int_array));

//   DictionaryArray expected(dtype, int_array);
//   ASSERT_TRUE(expected.Equals(result));
// }

// ----------------------------------------------------------------------
// Unique tests

// template <>
// void TestUniqueKernel<StringType>::SetUp() {
//   StringUniqueBuilder builder(default_memory_pool());
//   ASSERT_OK(builder.Append("test"));
//   ASSERT_OK(builder.Append("test2"));
//   ASSERT_OK(builder.Append("test"));
//   ASSERT_OK(builder.Finish(&input_array));

//   StringBuilder str_builder;
//   ASSERT_OK(str_builder.Append("test"));
//   ASSERT_OK(str_builder.Append("test2"));
//   ASSERT_OK(str_builder.Finish(&expected_array));
// }

// template <>
// void TestUniqueKernel<FixedSizeBinaryType>::SetUp() {
//   FixedSizeBinaryBuilder builder(arrow::fixed_size_binary(4));
//   vector<uint8_t> test{12, 12, 11, 12};
//   vector<uint8_t> test2{12, 12, 11, 11};
//   ASSERT_OK(builder.Append(test.data()));
//   ASSERT_OK(builder.Append(test2.data()));
//   ASSERT_OK(builder.Append(test.data()));
//   ASSERT_OK(builder.Finish(&input_array));

//   FixedSizeBinaryBuilder fsb_builder(arrow::fixed_size_binary(4));
//   ASSERT_OK(fsb_builder.Append(test.data()));
//   ASSERT_OK(fsb_builder.Append(test2.data()));
//   ASSERT_OK(fsb_builder.Finish(&expected_array));
// }

// typedef ::testing::Types<Int8Type, UInt8Type, Int16Type, UInt16Type, Int32Type,
//                          UInt32Type, Int64Type, UInt64Type, FloatType, DoubleType,
//                          StringType, FixedSizeBinaryType>
//     UniqueTestTypes;

// TYPED_TEST_CASE(TestUniqueKernel, UniqueTestTypes);

// TYPED_TEST(TestUniqueKernel, Array) {
//   shared_ptr<Array> result;
//   ASSERT_OK(Unique(&this->ctx_, *this->input_array, &result));
//   ASSERT_TRUE(this->expected_array->Equals(result));
// }

// TYPED_TEST(TestUniqueKernel, ChunkedArray) {
//   // Case 1: Simple array wrapped in a ChunkedArray
//   ChunkedArray carray_case1({this->input_array});
//   shared_ptr<Array> result;
//   ASSERT_OK(Unique(&this->ctx_, carray_case1, &result));
//   ASSERT_TRUE(this->expected_array->Equals(result));

//   // Case 2: Multiple arrays in a ChunkedArray
//   ChunkedArray carray_case2({this->input_array, this->input_array});
//   ASSERT_OK(Unique(&this->ctx_, carray_case2, &result));
//   ASSERT_TRUE(this->expected_array->Equals(result));
// }

// TYPED_TEST(TestUniqueKernel, Column) {
//   Column column(field("column", this->input_array->type()), this->input_array);
//   shared_ptr<Array> result;
//   ASSERT_OK(Unique(&this->ctx_, column, &result));
//   ASSERT_TRUE(this->expected_array->Equals(result));
// }

}  // namespace compute
}  // namespace arrow

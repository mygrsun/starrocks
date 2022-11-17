// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

#include <arrow/builder.h>
#include <arrow/memory_pool.h>
#include <arrow/testing/gtest_util.h>
#include <arrow/testing/util.h>
#include <gtest/gtest.h>
#include <testutil/parallel_test.h>
#include <util/guard.h>

#include <utility>

#include "arrow/array/builder_base.h"
#include "arrow/type_fwd.h"
#include "arrow/type_traits.h"
#include "exec/vectorized/arrow_to_starrocks_converter.h"
#include "runtime/datetime_value.h"

#define ASSERT_STATUS_OK(stmt)    \
    do {                          \
        auto status = (stmt);     \
        ASSERT_TRUE(status.ok()); \
    } while (0)

namespace starrocks::vectorized {

class ArrowConverterTest : public ::testing::Test {
public:
    void SetUp() override { vectorized::date::init_date_cache(); }
};

template <typename ArrowType, bool is_nullable = false, typename CType = typename arrow::TypeTraits<ArrowType>::CType,
          typename BuilderType = typename arrow::TypeTraits<ArrowType>::BuilderType>
static inline std::shared_ptr<arrow::Array> create_constant_array(int64_t num_elements, CType value, size_t& counter) {
    auto type = arrow::TypeTraits<ArrowType>::type_singleton();
    size_t i = 0;
    auto builder_fn = [value, &i](BuilderType* builder) {
        if constexpr (is_nullable) {
            if (i % 2 == 0) {
                builder->UnsafeAppendNull();
            } else {
                builder->UnsafeAppend(CType(value));
            }
            ++i;
        } else {
            builder->UnsafeAppend(CType(value));
        }
    };
    EXPECT_OK_AND_ASSIGN(auto array, arrow::ArrayFromBuilderVisitor(type, num_elements, builder_fn));
    counter += num_elements;
    return array;
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType,
          typename ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType>
void add_arrow_to_column(Column* column, size_t num_elements, ArrowCppType value, size_t& counter) {
    ASSERT_EQ(column->size(), counter);
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_array<ArrowType>(num_elements, value, counter);
    auto conv_func = get_arrow_converter(AT, PT, false, false);
    ASSERT_TRUE(conv_func != nullptr);
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    ASSERT_STATUS_OK(conv_func(array.get(), 0, array->length(), column, column->size(), nullptr, filter_data, nullptr));
    ASSERT_EQ(column->size(), counter);
    auto* data = &(down_cast<ColumnType*>(column)->get_data().front());
    for (auto i = 0; i < num_elements; ++i) {
        ASSERT_EQ(data[counter - num_elements + i], CppType(value));
    }
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType,
          typename ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType>
void add_arrow_to_nullable_column(Column* column, size_t num_elements, ArrowCppType value, size_t& counter) {
    ASSERT_TRUE(column->is_nullable());
    ASSERT_EQ(column->size(), counter);
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_array<ArrowType, true>(num_elements, value, counter);
    auto conv_func = get_arrow_converter(AT, PT, true, false);
    ASSERT_TRUE(conv_func != nullptr);
    auto* nullable_column = down_cast<NullableColumn*>(column);
    auto* data_column = down_cast<ColumnType*>(nullable_column->data_column().get());
    auto* null_column = nullable_column->null_column().get();
    fill_null_column(array.get(), 0, array->length(), null_column, null_column->size());
    auto* null_data = &null_column->get_data().front() + counter - num_elements;
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    ASSERT_STATUS_OK(conv_func(array.get(), 0, array->length(), data_column, data_column->size(), null_data,
                               filter_data, nullptr));
    ASSERT_EQ(data_column->size(), counter);
    auto* data = &(down_cast<ColumnType*>(data_column)->get_data().front());
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        if (i % 2 == 0) {
            ASSERT_EQ(null_data[i], DATUM_NULL);
        } else {
            ASSERT_EQ(null_data[i], DATUM_NOT_NULL);
            ASSERT_EQ(data[idx], CppType(value));
        }
    }
}

PARALLEL_TEST(ArrowConverterTest, test_copyable_converter_int8) {
    auto col = Int8Column::create();
    col->reserve(4096);
    size_t counter = 0;
    add_arrow_to_column<ArrowTypeId::INT8, TYPE_TINYINT, arrow::Int8Type>(col.get(), 11, int8_t(1), counter);
    add_arrow_to_column<ArrowTypeId::INT8, TYPE_TINYINT, arrow::Int8Type>(col.get(), 1, int8_t(255), counter);
    add_arrow_to_column<ArrowTypeId::INT8, TYPE_TINYINT, arrow::Int8Type>(col.get(), 1, int8_t(0), counter);
    add_arrow_to_column<ArrowTypeId::INT8, TYPE_TINYINT, arrow::Int8Type>(col.get(), 13, int8_t(127), counter);
    add_arrow_to_column<ArrowTypeId::INT8, TYPE_TINYINT, arrow::Int8Type>(col.get(), 3, int8_t(-128), counter);
    add_arrow_to_column<ArrowTypeId::INT8, TYPE_TINYINT, arrow::Int8Type>(col.get(), 5, int8_t(-127), counter);
}

PARALLEL_TEST(ArrowConverterTest, test_copyable_converter_uint64) {
    auto col = Int64Column::create();
    col->reserve(4096);
    size_t counter = 0;
    add_arrow_to_column<ArrowTypeId::UINT64, TYPE_BIGINT, arrow::UInt64Type>(col.get(), 11, uint64_t(1L), counter);
    add_arrow_to_column<ArrowTypeId::UINT64, TYPE_BIGINT, arrow::UInt64Type>(col.get(), 1, uint64_t(-1L), counter);
    add_arrow_to_column<ArrowTypeId::UINT64, TYPE_BIGINT, arrow::UInt64Type>(col.get(), 1, uint64_t(0L), counter);
    add_arrow_to_column<ArrowTypeId::UINT64, TYPE_BIGINT, arrow::UInt64Type>(
            col.get(), 13, std::numeric_limits<uint64>::max(), counter);
    add_arrow_to_column<ArrowTypeId::UINT64, TYPE_BIGINT, arrow::UInt64Type>(col.get(), 3, 0xdeadbeef'deadbeef,
                                                                             counter);
    add_arrow_to_column<ArrowTypeId::UINT64, TYPE_BIGINT, arrow::UInt64Type>(col.get(), 5, 0x8080'8080'8080'8080,
                                                                             counter);
}

PARALLEL_TEST(ArrowConverterTest, test_assignable_converter_float) {
    auto col = DoubleColumn::create();
    col->reserve(4096);
    size_t counter = 0;
    add_arrow_to_column<ArrowTypeId::FLOAT, TYPE_DOUBLE, arrow::FloatType>(col.get(), 11, 0.0f, counter);
    add_arrow_to_column<ArrowTypeId::FLOAT, TYPE_DOUBLE, arrow::FloatType>(col.get(), 1, 3.1415926f, counter);
    add_arrow_to_column<ArrowTypeId::FLOAT, TYPE_DOUBLE, arrow::FloatType>(col.get(), 1, -3.1415926f, counter);
    add_arrow_to_column<ArrowTypeId::FLOAT, TYPE_DOUBLE, arrow::FloatType>(col.get(), 13,
                                                                           std::numeric_limits<float>::max(), counter);
    add_arrow_to_column<ArrowTypeId::FLOAT, TYPE_DOUBLE, arrow::FloatType>(
            col.get(), 3, std::numeric_limits<float>::lowest(), counter);
    add_arrow_to_column<ArrowTypeId::FLOAT, TYPE_DOUBLE, arrow::FloatType>(col.get(), 5, -0.0f, counter);
}

PARALLEL_TEST(ArrowConverterTest, test_nullable_copyable_converter_int32) {
    auto data_column = Int32Column::create();
    auto null_column = NullColumn::create();
    auto col = NullableColumn::create(data_column, null_column);
    col->reserve(4096);
    size_t counter = 0;
    add_arrow_to_nullable_column<ArrowTypeId::INT32, TYPE_INT, arrow::Int32Type>(col.get(), 11, 0, counter);
    add_arrow_to_nullable_column<ArrowTypeId::INT32, TYPE_INT, arrow::Int32Type>(col.get(), 13, 1, counter);
    add_arrow_to_nullable_column<ArrowTypeId::INT32, TYPE_INT, arrow::Int32Type>(col.get(), 9, -1, counter);
    add_arrow_to_nullable_column<ArrowTypeId::INT32, TYPE_INT, arrow::Int32Type>(
            col.get(), 13, std::numeric_limits<int32_t>::max(), counter);
    add_arrow_to_nullable_column<ArrowTypeId::INT32, TYPE_INT, arrow::Int32Type>(
            col.get(), 9, std::numeric_limits<int32_t>::lowest(), counter);
    add_arrow_to_nullable_column<ArrowTypeId::INT32, TYPE_INT, arrow::Int32Type>(col.get(), 9, 0xdeadbeef, counter);
}

PARALLEL_TEST(ArrowConverterTest, test_nullable_assignable_converter_uint16) {
    auto data_column = Int128Column::create();
    auto null_column = NullColumn::create();
    auto col = NullableColumn::create(data_column, null_column);
    col->reserve(4096);
    size_t counter = 0;
    add_arrow_to_nullable_column<ArrowTypeId::UINT16, TYPE_LARGEINT, arrow::UInt16Type>(col.get(), 11, uint16_t(0),
                                                                                        counter);
    add_arrow_to_nullable_column<ArrowTypeId::UINT16, TYPE_LARGEINT, arrow::UInt16Type>(col.get(), 11, uint16_t(1),
                                                                                        counter);
    add_arrow_to_nullable_column<ArrowTypeId::UINT16, TYPE_LARGEINT, arrow::UInt16Type>(col.get(), 13, uint16_t(-1),
                                                                                        counter);
    add_arrow_to_nullable_column<ArrowTypeId::UINT16, TYPE_LARGEINT, arrow::UInt16Type>(
            col.get(), 8, std::numeric_limits<uint16_t>::max(), counter);
    add_arrow_to_nullable_column<ArrowTypeId::UINT16, TYPE_LARGEINT, arrow::UInt16Type>(col.get(), 4, uint16_t(0x0fff),
                                                                                        counter);
    add_arrow_to_nullable_column<ArrowTypeId::UINT16, TYPE_LARGEINT, arrow::UInt16Type>(col.get(), 5, uint16_t(0xff00),
                                                                                        counter);
}

template <typename ArrowType, bool is_nullable = false>
static inline std::shared_ptr<arrow::Array> create_constant_binary_array(int64_t num_elements, const std::string& value,
                                                                         size_t& counter) {
    using offset_type = typename ArrowType::offset_type;
    size_t offsets_in_bytes = (num_elements + 1) * sizeof(offset_type);
    auto offsets_buf_res = arrow::AllocateBuffer(offsets_in_bytes);
    std::shared_ptr<arrow::Buffer> offsets_buf = std::move(offsets_buf_res.ValueOrDie());
    auto* offsets = (offset_type*)offsets_buf->mutable_data();
    offsets[0] = 0;

    auto value_size = value.size();
    size_t data_in_bytes = value_size * num_elements;
    auto data_buf_res = arrow::AllocateBuffer(data_in_bytes);
    std::shared_ptr<arrow::Buffer> data_buf = std::move(data_buf_res.ValueOrDie());
    auto* data = data_buf->mutable_data();

    auto null_bitmap_in_bytes = (num_elements + 7) / 8;
    auto null_bitmap_res = arrow::AllocateBuffer(null_bitmap_in_bytes);
    std::shared_ptr<arrow::Buffer> null_bitmap = std::move(null_bitmap_res.ValueOrDie());
    auto nulls = null_bitmap->mutable_data();
    auto data_off = 0;
    for (auto i = 0; i < num_elements; ++i) {
        if (is_nullable && i % 2 == 0) {
            arrow::BitUtil::ClearBit(nulls, i);
        } else {
            arrow::BitUtil::SetBit(nulls, i);
            memcpy(data + data_off, value.data(), value_size);
            data_off += value_size;
        }
        offsets[i + 1] = data_off;
    }

    using ArrayType = typename arrow::TypeTraits<ArrowType>::ArrayType;
    auto array = std::make_shared<ArrayType>(num_elements, offsets_buf, data_buf, null_bitmap);
    counter += num_elements;
    return std::static_pointer_cast<arrow::Array>(array);
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType, typename ArrowCppType>
void add_arrow_to_binary_column(Column* column, size_t num_elements, ArrowCppType value, size_t& counter,
                                bool strict_mode, bool fail) {
    ASSERT_EQ(column->size(), counter);
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_binary_array<ArrowType, false>(num_elements, value, counter);
    auto conv_func = get_arrow_converter(AT, PT, false, strict_mode);
    ASSERT_TRUE(conv_func != nullptr);
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), column, column->size(), nullptr, filter_data, nullptr);
    ASSERT_TRUE(status.ok());
    auto* binary_column = down_cast<ColumnType*>(column);
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        auto s = binary_column->get_slice(idx);
        if (fail) {
            ASSERT_EQ(filter[i], 0);
            ASSERT_EQ(s.size, 0);
        } else {
            ASSERT_EQ(filter[i], 1);
            ASSERT_EQ(s.to_string(), value);
        }
    }
}

template <typename T>
using TestCaseArray = std::vector<std::tuple<size_t, T, bool>>;

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType, typename ArrowCppType>
void test_binary(const TestCaseArray<ArrowCppType>& test_cases, bool strict_mode) {
    using ColumnType = RunTimeColumnType<PT>;
    auto col = ColumnType::create();
    col->reserve(4096);
    size_t counter = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto value = std::get<1>(tc);
        auto fail = std::get<2>(tc);
        add_arrow_to_binary_column<AT, PT, ArrowType, ArrowCppType>(col.get(), num_elements, value, counter,
                                                                    strict_mode, fail);
    }
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType, typename ArrowCppType>
void add_arrow_to_nullable_binary_column(Column* column, size_t num_elements, ArrowCppType value, size_t& counter,
                                         bool strict_mode, bool fail) {
    ASSERT_EQ(column->size(), counter);
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_binary_array<ArrowType, true>(num_elements, value, counter);
    auto conv_func = get_arrow_converter(AT, PT, true, strict_mode);
    ASSERT_TRUE(conv_func != nullptr);

    auto* nullable_column = down_cast<NullableColumn*>(column);
    auto* binary_column = down_cast<ColumnType*>(nullable_column->data_column().get());
    auto* null_column = down_cast<NullColumn*>(nullable_column->null_column().get());
    fill_null_column(array.get(), 0, array->length(), null_column, null_column->size());
    auto* null_data = &null_column->get_data().front() + counter - num_elements;
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), binary_column, binary_column->size(), null_data,
                            filter_data, nullptr);
    ASSERT_TRUE(status.ok());
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        auto s = binary_column->get_slice(idx);
        if (i % 2 == 0) {
            ASSERT_EQ(filter_data[i], 1);
            ASSERT_EQ(null_data[i], DATUM_NULL);
            ASSERT_EQ(s.size, 0);
        } else if (fail) {
            ASSERT_EQ(filter_data[i], strict_mode ? 0 : 1);
            ASSERT_EQ(s.size, 0);
        } else {
            ASSERT_EQ(filter_data[i], 1);
            ASSERT_EQ(null_data[i], DATUM_NOT_NULL);
            ASSERT_EQ(s.to_string(), value);
        }
    }
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType, typename ArrowCppType>
void test_nullable_binary(const TestCaseArray<ArrowCppType>& test_cases, bool strict_mode) {
    using ColumnType = RunTimeColumnType<PT>;
    auto binary_column = ColumnType::create();
    auto null_column = NullColumn::create();
    auto col = NullableColumn::create(binary_column, null_column);
    col->reserve(4096);
    size_t counter = 0;
    auto expect_num_rows = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto value = std::get<1>(tc);
        auto fail = std::get<2>(tc);
        expect_num_rows += num_elements;
        add_arrow_to_nullable_binary_column<AT, PT, ArrowType, ArrowCppType>(col.get(), num_elements, value, counter,
                                                                             strict_mode, fail);
    }
    ASSERT_EQ(binary_column->size(), expect_num_rows);
}

void test_nullable_binary_with_strict_mode(const TestCaseArray<std::string>& test_cases, bool strict_mode) {
    test_nullable_binary<ArrowTypeId::BINARY, TYPE_VARCHAR, arrow::BinaryType, std::string>(test_cases, strict_mode);
    test_nullable_binary<ArrowTypeId::BINARY, TYPE_CHAR, arrow::BinaryType, std::string>(test_cases, strict_mode);
    test_nullable_binary<ArrowTypeId::STRING, TYPE_VARCHAR, arrow::StringType, std::string>(test_cases, strict_mode);
    test_nullable_binary<ArrowTypeId::STRING, TYPE_CHAR, arrow::StringType, std::string>(test_cases, strict_mode);
    test_nullable_binary<ArrowTypeId::LARGE_BINARY, TYPE_VARCHAR, arrow::LargeBinaryType, std::string>(test_cases,
                                                                                                       strict_mode);
    test_nullable_binary<ArrowTypeId::LARGE_BINARY, TYPE_CHAR, arrow::LargeBinaryType, std::string>(test_cases,
                                                                                                    strict_mode);
    test_nullable_binary<ArrowTypeId::LARGE_STRING, TYPE_VARCHAR, arrow::LargeStringType, std::string>(test_cases,
                                                                                                       strict_mode);
    test_nullable_binary<ArrowTypeId::LARGE_STRING, TYPE_CHAR, arrow::LargeStringType, std::string>(test_cases,
                                                                                                    strict_mode);
}

void test_binary_with_strict_mode(const TestCaseArray<std::string>& test_cases, bool strict_mode) {
    test_binary<ArrowTypeId::BINARY, TYPE_VARCHAR, arrow::BinaryType, std::string>(test_cases, strict_mode);
    test_binary<ArrowTypeId::BINARY, TYPE_CHAR, arrow::BinaryType, std::string>(test_cases, strict_mode);
    test_binary<ArrowTypeId::STRING, TYPE_VARCHAR, arrow::StringType, std::string>(test_cases, strict_mode);
    test_binary<ArrowTypeId::STRING, TYPE_CHAR, arrow::StringType, std::string>(test_cases, strict_mode);
    test_binary<ArrowTypeId::LARGE_BINARY, TYPE_VARCHAR, arrow::LargeBinaryType, std::string>(test_cases, strict_mode);
    test_binary<ArrowTypeId::LARGE_BINARY, TYPE_CHAR, arrow::LargeBinaryType, std::string>(test_cases, strict_mode);
    test_binary<ArrowTypeId::LARGE_STRING, TYPE_VARCHAR, arrow::LargeStringType, std::string>(test_cases, strict_mode);
    test_binary<ArrowTypeId::LARGE_STRING, TYPE_CHAR, arrow::LargeStringType, std::string>(test_cases, strict_mode);
}

PARALLEL_TEST(ArrowConverterTest, test_binary_strict) {
    auto test_cases = TestCaseArray<std::string>{
            {13, std::string(""), false},
            {7, std::string("a"), false},
            {7, std::string("甲乙丙丁"), false},
            {9, std::string("Make impossible possible"), false},
            {17, std::string("三十年众生牛马，六十年诸佛龙象"), false},
    };
    test_binary_with_strict_mode(test_cases, true);
}

PARALLEL_TEST(ArrowConverterTest, test_binary_nonstrict) {
    auto test_cases = TestCaseArray<std::string>{
            {13, std::string(""), false},
            {7, std::string("a"), false},
            {7, std::string("甲乙丙丁"), false},
            {9, std::string("Make impossible possible"), false},
            {17, std::string("三十年众生牛马，六十年诸佛龙象"), false},
    };
    test_binary_with_strict_mode(test_cases, false);
}

PARALLEL_TEST(ArrowConverterTest, test_nullable_binary_strict) {
    auto test_cases = TestCaseArray<std::string>{
            {13, std::string(""), false},
            {7, std::string("a"), false},
            {7, std::string("甲乙丙丁"), false},
            {9, std::string("Make impossible possible"), false},
            {17, std::string("三十年众生牛马，六十年诸佛龙象"), false},
    };
    test_nullable_binary_with_strict_mode(test_cases, true);
}

PARALLEL_TEST(ArrowConverterTest, test_nullable_binary_nonstrict) {
    auto test_cases = TestCaseArray<std::string>{
            {13, std::string(""), false},
            {7, std::string("a"), false},
            {7, std::string("甲乙丙丁"), false},
            {9, std::string("Make impossible possible"), false},
            {17, std::string("三十年众生牛马，六十年诸佛龙象"), false},
    };
    test_nullable_binary_with_strict_mode(test_cases, false);
}

template <PrimitiveType PT>
void convert_binary_fail(size_t limit, bool strict_mode) {
    auto test_cases_pass = TestCaseArray<std::string>{
            {7, std::string(limit, 'x'), false},
            {8, std::string(limit + 1, 'x'), true},
            {9, std::string(limit, 'x'), false},
            {10, std::string(limit + 2, 'x'), true},
    };
    test_nullable_binary<ArrowTypeId::STRING, PT, arrow::StringType, std::string>(test_cases_pass, strict_mode);
    test_nullable_binary<ArrowTypeId::BINARY, PT, arrow::BinaryType, std::string>(test_cases_pass, strict_mode);
    test_nullable_binary<ArrowTypeId::LARGE_STRING, PT, arrow::LargeStringType, std::string>(test_cases_pass,
                                                                                             strict_mode);
    test_nullable_binary<ArrowTypeId::LARGE_BINARY, PT, arrow::LargeBinaryType, std::string>(test_cases_pass,
                                                                                             strict_mode);
    test_binary<ArrowTypeId::STRING, PT, arrow::StringType, std::string>(test_cases_pass, strict_mode);
    test_binary<ArrowTypeId::BINARY, PT, arrow::BinaryType, std::string>(test_cases_pass, strict_mode);
    test_binary<ArrowTypeId::LARGE_STRING, PT, arrow::LargeStringType, std::string>(test_cases_pass, strict_mode);
    test_binary<ArrowTypeId::LARGE_BINARY, PT, arrow::LargeBinaryType, std::string>(test_cases_pass, strict_mode);
}

PARALLEL_TEST(ArrowConverterTest, test_char_fail_strict) {
    convert_binary_fail<TYPE_CHAR>(TypeDescriptor::MAX_CHAR_LENGTH, true);
}

PARALLEL_TEST(ArrowConverterTest, test_varchar_fail_strict) {
    convert_binary_fail<TYPE_VARCHAR>(TypeDescriptor::MAX_VARCHAR_LENGTH, true);
}

PARALLEL_TEST(ArrowConverterTest, test_char_fail_nonstrict) {
    convert_binary_fail<TYPE_CHAR>(TypeDescriptor::MAX_CHAR_LENGTH, false);
}

PARALLEL_TEST(ArrowConverterTest, test_varchar_fail_nonstrict) {
    convert_binary_fail<TYPE_VARCHAR>(TypeDescriptor::MAX_VARCHAR_LENGTH, false);
}

template <int bytes_width, bool is_nullable = false>
static inline std::shared_ptr<arrow::Array> create_constant_fixed_size_binary_array(int64_t num_elements,
                                                                                    const std::string& value,
                                                                                    size_t& counter) {
    auto data_buf_size = bytes_width * num_elements;
    auto data_buf_res = arrow::AllocateBuffer(data_buf_size);
    std::shared_ptr<arrow::Buffer> data_buf = std::move(data_buf_res.ValueOrDie());
    auto* p = data_buf->mutable_data();

    auto null_bitmap_in_bytes = (num_elements + 7) / 8;
    auto null_bitmap_res = arrow::AllocateBuffer(null_bitmap_in_bytes);
    std::shared_ptr<arrow::Buffer> null_bitmap = std::move(null_bitmap_res.ValueOrDie());
    auto* nulls = null_bitmap->mutable_data();

    for (auto i = 0; i < num_elements; ++i) {
        if (is_nullable && i % 2 == 0) {
            arrow::BitUtil::ClearBit(nulls, i);
        } else {
            arrow::BitUtil::SetBit(nulls, i);
        }
        memcpy(p, value.c_str(), std::min(value.size() + 1, (std::string::size_type)bytes_width));
        p += bytes_width;
    }
    auto type = std::make_shared<arrow::FixedSizeBinaryType>(bytes_width);
    auto array = std::make_shared<arrow::FixedSizeBinaryArray>(type, num_elements, data_buf, null_bitmap);
    counter += num_elements;
    return std::static_pointer_cast<arrow::Array>(array);
}

template <int bytes_width, PrimitiveType PT>
void add_fixed_size_binary_array_to_binary_column(Column* column, size_t num_elements, const std::string& value,
                                                  size_t& counter, bool fail) {
    ASSERT_EQ(column->size(), counter);
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_fixed_size_binary_array<bytes_width, false>(num_elements, value, counter);
    auto conv_func = get_arrow_converter(ArrowTypeId::FIXED_SIZE_BINARY, PT, false, false);
    ASSERT_TRUE(conv_func != nullptr);
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), column, column->size(), nullptr, filter_data, nullptr);
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(column->size(), counter);
    auto* binary_column = down_cast<ColumnType*>(column);
    auto slice_size = std::min((std::string::size_type)bytes_width, value.size());
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        auto s = binary_column->get_slice(idx);
        if (fail) {
            ASSERT_EQ(filter[i], 0);
            ASSERT_EQ(s.size, 0);
        } else {
            ASSERT_EQ(filter[i], 1);
            ASSERT_TRUE(memequal(s.data, slice_size, value.data(), slice_size));
        }
    }
}

template <int bytes_width, PrimitiveType PT>
void add_fixed_size_binary_array_to_nullable_binary_column(Column* column, size_t num_elements,
                                                           const std::string& value, size_t& counter, bool fail) {
    ASSERT_EQ(column->size(), counter);
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_fixed_size_binary_array<bytes_width, true>(num_elements, value, counter);
    auto conv_func = get_arrow_converter(ArrowTypeId::FIXED_SIZE_BINARY, PT, true, false);
    ASSERT_TRUE(conv_func != nullptr);

    auto* nullable_column = down_cast<NullableColumn*>(column);
    auto* binary_column = down_cast<BinaryColumn*>(nullable_column->data_column().get());
    auto* null_column = down_cast<NullColumn*>(nullable_column->null_column().get());
    fill_null_column(array.get(), 0, array->length(), null_column, null_column->size());
    auto* null_data = &null_column->get_data().front() + counter - num_elements;
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), binary_column, binary_column->size(), null_data,
                            filter_data, nullptr);
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(binary_column->size(), counter);
    auto slice_size = std::min((std::string::size_type)bytes_width, value.size());
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        auto s = binary_column->get_slice(idx);
        ASSERT_EQ(filter[i], 1);
        if (i % 2 == 0) {
            ASSERT_EQ(null_data[i], DATUM_NULL);
            ASSERT_EQ(s.size, 0);
        } else if (fail) {
            ASSERT_EQ(null_data[i], DATUM_NULL);
            ASSERT_EQ(s.size, 0);
        } else {
            ASSERT_EQ(null_data[i], DATUM_NOT_NULL);
            ASSERT_TRUE(memequal(s.data, slice_size, value.data(), slice_size));
        }
    }
}

template <int bytes_width, PrimitiveType PT>
void PARALLEL_TESTixed_size_binary(const TestCaseArray<std::string>& test_cases) {
    using ColumnType = RunTimeColumnType<PT>;
    auto col = ColumnType::create();
    col->reserve(4096);
    size_t counter = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto value = std::get<1>(tc);
        auto fail = std::get<2>(tc);
        add_fixed_size_binary_array_to_binary_column<bytes_width, PT>(col.get(), num_elements, value, counter, fail);
    }
}

template <int bytes_width, PrimitiveType PT>
void test_nullable_fixed_size_binary(const TestCaseArray<std::string>& test_cases) {
    using ColumnType = RunTimeColumnType<PT>;
    auto binary_column = ColumnType::create();
    auto null_column = NullColumn::create();
    auto col = NullableColumn::create(binary_column, null_column);
    col->reserve(4096);
    size_t counter = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto value = std::get<1>(tc);
        auto fail = std::get<2>(tc);
        add_fixed_size_binary_array_to_nullable_binary_column<bytes_width, PT>(col.get(), num_elements, value, counter,
                                                                               fail);
    }
}

PARALLEL_TEST(ArrowConverterTest, PARALLEL_TESTixed_size_binary_pass) {
    auto test_cases = TestCaseArray<std::string>{
            {3, std::string(""), false},
            {3, std::string("a"), false},
            {3, std::string(255, 'x'), false},
    };
    PARALLEL_TESTixed_size_binary<10, TYPE_CHAR>(test_cases);
    PARALLEL_TESTixed_size_binary<255, TYPE_CHAR>(test_cases);
    test_nullable_fixed_size_binary<10, TYPE_CHAR>(test_cases);
    test_nullable_fixed_size_binary<255, TYPE_CHAR>(test_cases);
    PARALLEL_TESTixed_size_binary<10, TYPE_VARCHAR>(test_cases);
    PARALLEL_TESTixed_size_binary<255, TYPE_VARCHAR>(test_cases);
    PARALLEL_TESTixed_size_binary<TypeDescriptor::MAX_VARCHAR_LENGTH, TYPE_VARCHAR>(test_cases);
    test_nullable_fixed_size_binary<10, TYPE_VARCHAR>(test_cases);
    test_nullable_fixed_size_binary<255, TYPE_VARCHAR>(test_cases);
    test_nullable_fixed_size_binary<TypeDescriptor::MAX_VARCHAR_LENGTH, TYPE_VARCHAR>(test_cases);
}

PARALLEL_TEST(ArrowConverterTest, PARALLEL_TESTixed_size_binary_fail) {
    auto test_cases = TestCaseArray<std::string>{
            {2, std::string(255, 'x'), true},
    };
    PARALLEL_TESTixed_size_binary<256, TYPE_CHAR>(test_cases);
    test_nullable_fixed_size_binary<256, TYPE_CHAR>(test_cases);
    PARALLEL_TESTixed_size_binary<TypeDescriptor::MAX_VARCHAR_LENGTH + 1, TYPE_VARCHAR>(test_cases);
    test_nullable_fixed_size_binary<TypeDescriptor::MAX_VARCHAR_LENGTH + 1, TYPE_VARCHAR>(test_cases);
}

template <typename ArrowType, bool is_nullable, typename ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType>
std::shared_ptr<arrow::Array> create_constant_datetime_array(size_t num_elements, ArrowCppType value,
                                                             const std::shared_ptr<arrow::DataType>& type,
                                                             size_t& counter) {
    std::vector<std::shared_ptr<arrow::Buffer>> buffers;
    buffers.resize(2);
    size_t null_bitmap_in_bytes = (num_elements + 7) / 8;
    size_t data_buff_in_bytes = num_elements * sizeof(value);
    auto buffer0_res = arrow::AllocateBuffer(null_bitmap_in_bytes);
    buffers[0] = std::move(buffer0_res.ValueOrDie());
    auto buffer1_res = arrow::AllocateBuffer(data_buff_in_bytes);
    buffers[1] = std::move(buffer1_res.ValueOrDie());
    auto* nulls = buffers[0]->mutable_data();
    auto* data = (ArrowCppType*)buffers[1]->mutable_data();

    for (auto i = 0; i < num_elements; ++i) {
        if (is_nullable && (i % 2 == 0)) {
            arrow::BitUtil::ClearBit(nulls, i);
        } else {
            arrow::BitUtil::SetBit(nulls, i);
        }
        data[i] = value;
    }
    using ArrayType = typename arrow::TypeTraits<ArrowType>::ArrayType;
    auto array_data = std::make_shared<arrow::ArrayData>(type, num_elements, buffers);
    auto array = std::make_shared<ArrayType>(array_data);
    counter += num_elements;
    return std::static_pointer_cast<arrow::Array>(array);
}

VALUE_GUARD(ArrowTypeId, Date32ATGuard, at_is_date32, ArrowTypeId::DATE32)
VALUE_GUARD(ArrowTypeId, Date64ATGuard, at_is_date64, ArrowTypeId::DATE64)
VALUE_GUARD(ArrowTypeId, TimestampATGuard, at_is_timestamp, ArrowTypeId::TIMESTAMP)
VALUE_GUARD(ArrowTypeId, DateTimeATGuard, at_is_datetime, ArrowTypeId::DATE32, ArrowTypeId::DATE64,
            ArrowTypeId::TIMESTAMP)

template <ArrowTypeId AT, typename ArrowType, typename ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType>
ArrowCppType string_to_arrow_datetime(std::shared_ptr<ArrowType> type, const std::string& value) {
    ArrowCppType datetime_value = {};
    TimestampValue tv;
    tv.from_string(value.c_str(), value.size());
    auto unix_seconds = tv.to_unix_second();
    if constexpr (at_is_date32<AT>) {
        datetime_value = (ArrowCppType)(unix_seconds / (24 * 3600));
    } else if constexpr (at_is_date64<AT>) {
        datetime_value = unix_seconds * 1000L;
    } else if constexpr (at_is_timestamp<AT>) {
        arrow::TimeUnit::type unit = type->unit();
        DateTimeValue dtv;
        dtv.from_unixtime(unix_seconds, "UTC");
        dtv.unix_timestamp(&unix_seconds, type->timezone());
        switch (unit) {
        case arrow::TimeUnit::SECOND:
            datetime_value = unix_seconds;
            break;
        case arrow::TimeUnit::MILLI:
            datetime_value = unix_seconds * 1000L;
            break;
        case arrow::TimeUnit::MICRO:
            datetime_value = unix_seconds * 1000'000L;
            break;
        case arrow::TimeUnit::NANO:
            datetime_value = unix_seconds * 1000'000'000L;
            break;
        default:
            assert(false);
        }
    } else {
        static_assert(at_is_datetime<AT>, "Not supported type of datetime");
    }
    return datetime_value;
}

template <PrimitiveType PT, typename CppType = RunTimeCppType<PT>>
CppType string_to_datetime(const std::string& value) {
    if constexpr (pt_is_date<PT>) {
        DateValue dv;
        dv.from_string(value.c_str(), value.size());
        return dv;
    } else if constexpr (pt_is_datetime<PT>) {
        TimestampValue tv;
        tv.from_string(value.c_str(), value.size());
        return tv;
    } else {
        static_assert(pt_is_date_or_datetime<PT>, "Invalid type times");
    }
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType,
          typename ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType, typename CppType = RunTimeCppType<PT>>
void add_arrow_to_datetime_column(std::shared_ptr<ArrowType> type, Column* column, size_t num_elements,
                                  ArrowCppType arrow_datetime, CppType datetime, size_t& counter, bool fail) {
    ASSERT_EQ(column->size(), counter);
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_datetime_array<ArrowType, false>(num_elements, arrow_datetime, type, counter);
    auto conv_func = get_arrow_converter(AT, PT, false, false);
    ASSERT_TRUE(conv_func != nullptr);
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), column, column->size(), nullptr, filter_data, nullptr);
    if (fail) {
        ASSERT_FALSE(status.ok());
        return;
    }
    ASSERT_EQ(column->size(), counter);
    auto* datetime_column = down_cast<ColumnType*>(column);
    auto* datetime_data = &datetime_column->get_data().front();
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        ASSERT_EQ(datetime_data[idx], datetime);
    }
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType,
          typename ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType, typename CppType = RunTimeCppType<PT>>
void add_arrow_to_nullable_datetime_column(std::shared_ptr<ArrowType> type, Column* column, size_t num_elements,
                                           ArrowCppType arrow_datetime, CppType datetime, size_t& counter, bool fail) {
    ASSERT_EQ(column->size(), counter);
    using ColumnType = RunTimeColumnType<PT>;
    auto array = create_constant_datetime_array<ArrowType, true>(num_elements, arrow_datetime, type, counter);
    auto conv_func = get_arrow_converter(AT, PT, true, false);
    ASSERT_TRUE(conv_func != nullptr);

    ASSERT_TRUE(column->is_nullable());
    auto* nullable_column = down_cast<NullableColumn*>(column);
    auto* datetime_column = down_cast<ColumnType*>(nullable_column->data_column().get());
    auto* null_column = down_cast<NullColumn*>(nullable_column->null_column().get());
    fill_null_column(array.get(), 0, num_elements, null_column, null_column->size());
    auto* null_data = &null_column->get_data().front() + counter - num_elements;
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), datetime_column, datetime_column->size(), null_data,
                            filter_data, nullptr);
    if (fail) {
        ASSERT_FALSE(status.ok());
        return;
    }
    ASSERT_EQ(column->size(), counter);
    auto* datetime_data = &datetime_column->get_data().front();
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        if (i % 2 == 0) {
            ASSERT_EQ(null_data[i], DATUM_NULL);
        } else {
            ASSERT_EQ(datetime_data[idx], datetime);
            ASSERT_EQ(null_data[i], DATUM_NOT_NULL);
        }
    }
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType>
void test_datetime(std::shared_ptr<ArrowType> type, const TestCaseArray<std::string>& test_cases) {
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    using ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType;
    auto col = ColumnType::create();
    col->reserve(4096);
    size_t counter = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto value = std::get<1>(tc);
        auto fail = std::get<2>(tc);
        ArrowCppType arrow_datetime = string_to_arrow_datetime<AT, ArrowType>(type, value);
        CppType datetime = string_to_datetime<PT>(value);
        add_arrow_to_datetime_column<AT, PT>(type, col.get(), num_elements, arrow_datetime, datetime, counter, fail);
    }
}

template <ArrowTypeId AT, PrimitiveType PT, typename ArrowType>
void test_nullable_datetime(std::shared_ptr<ArrowType> type, const TestCaseArray<std::string>& test_cases) {
    using CppType = RunTimeCppType<PT>;
    using ColumnType = RunTimeColumnType<PT>;
    using ArrowCppType = typename arrow::TypeTraits<ArrowType>::CType;
    auto datetime_column = ColumnType::create();
    auto null_column = NullColumn::create();
    auto col = NullableColumn::create(datetime_column, null_column);
    col->reserve(4096);
    size_t counter = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto value = std::get<1>(tc);
        auto fail = std::get<2>(tc);
        ArrowCppType arrow_datetime = string_to_arrow_datetime<AT, ArrowType>(type, value);
        CppType datetime = string_to_datetime<PT>(value);
        add_arrow_to_nullable_datetime_column<AT, PT>(type, col.get(), num_elements, arrow_datetime, datetime, counter,
                                                      fail);
    }
}

PARALLEL_TEST(ArrowConverterTest, test_date32_to_date) {
    auto type = std::make_shared<arrow::Date32Type>();
    auto test_cases = TestCaseArray<std::string>{
            {7, "2021-09-30", false},
            {9, "1972-01-01", false},
            {11, "1988-12-13", false},
            {13, "2014-06-06", false},
    };
    test_datetime<ArrowTypeId::DATE32, TYPE_DATE, arrow::Date32Type>(type, test_cases);
    test_nullable_datetime<ArrowTypeId::DATE32, TYPE_DATE, arrow::Date32Type>(type, test_cases);
}

PARALLEL_TEST(ArrowConverterTest, test_date32_to_datetime) {
    auto type = std::make_shared<arrow::Date32Type>();
    auto test_cases = TestCaseArray<std::string>{
            {7, "2021-09-30", false},
            {9, "1972-01-01", false},
            {11, "1988-12-13", false},
            {13, "2014-06-06", false},
    };
    test_datetime<ArrowTypeId::DATE32, TYPE_DATETIME, arrow::Date32Type>(type, test_cases);
    test_nullable_datetime<ArrowTypeId::DATE32, TYPE_DATETIME, arrow::Date32Type>(type, test_cases);
}
PARALLEL_TEST(ArrowConverterTest, test_date64_to_date) {
    auto type = std::make_shared<arrow::Date64Type>();
    auto test_cases = TestCaseArray<std::string>{
            {7, "1999-12-31 00:00:00", false},
            {9, "2007-09-01 06:30:00", false},
            {11, "1988-12-13 14:45:01", false},
            {13, "2014-06-06 12:00:05", false},
    };
    test_datetime<ArrowTypeId::DATE64, TYPE_DATE, arrow::Date64Type>(type, test_cases);
    test_nullable_datetime<ArrowTypeId::DATE64, TYPE_DATE, arrow::Date64Type>(type, test_cases);
}
PARALLEL_TEST(ArrowConverterTest, test_date64_to_datetime) {
    auto type = std::make_shared<arrow::Date64Type>();
    auto test_cases = TestCaseArray<std::string>{
            {7, "1999-12-31 00:00:00", false},
            {9, "2007-09-01 06:30:00", false},
            {11, "2016-04-13 11:46:01", false},
            {13, "2014-06-06 12:00:05", false},
    };
    test_datetime<ArrowTypeId::DATE64, TYPE_DATETIME, arrow::Date64Type>(type, test_cases);
    test_nullable_datetime<ArrowTypeId::DATE64, TYPE_DATETIME, arrow::Date64Type>(type, test_cases);
}
PARALLEL_TEST(ArrowConverterTest, test_timestamp_to_date) {
    auto test_cases = TestCaseArray<std::string>{
            {7, "1999-12-31 00:00:00", false},
            {9, "2007-09-01 06:30:00", false},
            {11, "2016-04-13 11:46:01", false},
            {13, "2014-06-06 12:00:05", false},
    };
    auto time_unit_and_zones = std::vector<std::tuple<arrow::TimeUnit::type, std::string>>{
            {arrow::TimeUnit::SECOND, "UTC"},          {arrow::TimeUnit::MILLI, "UTC"},
            {arrow::TimeUnit::MICRO, "UTC"},           {arrow::TimeUnit::NANO, "UTC"},
            {arrow::TimeUnit::SECOND, "CST"},          {arrow::TimeUnit::MILLI, "CST"},
            {arrow::TimeUnit::MICRO, "CST"},           {arrow::TimeUnit::NANO, "CST"},
            {arrow::TimeUnit::SECOND, "+07:00"},       {arrow::TimeUnit::MILLI, "-06:00"},
            {arrow::TimeUnit::MICRO, "Asia/Shanghai"}, {arrow::TimeUnit::NANO, "Europe/Zurich"},
    };
    for (auto& uc : time_unit_and_zones) {
        auto unit = std::get<0>(uc);
        auto tz = std::get<1>(uc);
        auto type = std::make_shared<arrow::TimestampType>(unit, tz);
        test_datetime<ArrowTypeId::TIMESTAMP, TYPE_DATE, arrow::TimestampType>(type, test_cases);
        test_nullable_datetime<ArrowTypeId::TIMESTAMP, TYPE_DATE, arrow::TimestampType>(type, test_cases);
    }
}
PARALLEL_TEST(ArrowConverterTest, test_timestamp_to_datetime) {
    auto test_cases = TestCaseArray<std::string>{
            {7, "1999-12-31 00:00:00", false},
            {9, "2007-09-01 06:30:00", false},
            {11, "2016-04-13 11:46:01", false},
            {13, "2014-06-06 12:00:05", false},
    };
    auto time_unit_and_zones = std::vector<std::tuple<arrow::TimeUnit::type, std::string>>{
            {arrow::TimeUnit::SECOND, "UTC"},          {arrow::TimeUnit::MILLI, "UTC"},
            {arrow::TimeUnit::MICRO, "UTC"},           {arrow::TimeUnit::NANO, "UTC"},
            {arrow::TimeUnit::SECOND, "CST"},          {arrow::TimeUnit::MILLI, "CST"},
            {arrow::TimeUnit::MICRO, "CST"},           {arrow::TimeUnit::NANO, "CST"},
            {arrow::TimeUnit::SECOND, "+07:00"},       {arrow::TimeUnit::MILLI, "-06:00"},
            {arrow::TimeUnit::MICRO, "Asia/Shanghai"}, {arrow::TimeUnit::NANO, "Europe/Zurich"},
    };
    for (auto& uc : time_unit_and_zones) {
        auto unit = std::get<0>(uc);
        auto tz = std::get<1>(uc);
        auto type = std::make_shared<arrow::TimestampType>(unit, tz);
        test_datetime<ArrowTypeId::TIMESTAMP, TYPE_DATETIME, arrow::TimestampType>(type, test_cases);
        test_nullable_datetime<ArrowTypeId::TIMESTAMP, TYPE_DATETIME, arrow::TimestampType>(type, test_cases);
    }
}

template <bool is_nullable>
std::shared_ptr<arrow::Array> create_const_decimal_array(size_t num_elements,
                                                         const std::shared_ptr<arrow::Decimal128Type>& type,
                                                         int128_t decimal, size_t& counter) {
    std::vector<std::shared_ptr<arrow::Buffer>> buffers;
    buffers.resize(2);
    auto byte_width = type->byte_width();
    auto buffer0_res = arrow::AllocateBuffer((num_elements + 7) / 8);
    buffers[0] = std::move(buffer0_res.ValueOrDie());
    auto buffer1_res = arrow::AllocateBuffer(type->byte_width() * num_elements);
    buffers[1] = std::move(buffer1_res.ValueOrDie());
    auto* nulls = buffers[0]->mutable_data();
    auto* data = buffers[1]->mutable_data();
    for (auto i = 0; i < num_elements; ++i) {
        if (is_nullable && (i % 2 == 0)) {
            arrow::BitUtil::ClearBit(nulls, i);
        } else {
            arrow::BitUtil::SetBit(nulls, i);
            memcpy(data + i * byte_width, &decimal, sizeof(decimal));
        }
    }
    auto array_data = std::make_shared<arrow::ArrayData>(type, num_elements, buffers);
    auto array = std::make_shared<arrow::Decimal128Array>(array_data);
    counter += num_elements;
    return array;
}
template <PrimitiveType PT, typename CppType = RunTimeCppType<PT>>
void add_arrow_to_decimal_column(const std::shared_ptr<arrow::Decimal128Type>& type, Column* column,
                                 size_t num_elements, int128_t value, CppType expect_value, size_t& counter,
                                 bool fail) {
    using ColumnType = RunTimeColumnType<PT>;
    ASSERT_EQ(column->size(), counter);
    auto array = create_const_decimal_array<false>(num_elements, std::move(type), value, counter);
    auto conv_func = get_arrow_converter(ArrowTypeId::DECIMAL, PT, false, false);
    ASSERT_TRUE(conv_func != nullptr);
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), column, column->size(), nullptr, filter_data, nullptr);
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(column->size(), counter);
    auto* decimal_column = down_cast<ColumnType*>(column);
    auto* decimal_data = &decimal_column->get_data().front();
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        if (!fail) {
            ASSERT_EQ(decimal_data[idx], expect_value);
        } else {
            ASSERT_EQ(filter_data[i], 0);
        }
    }
}

template <PrimitiveType PT, typename CppType = RunTimeCppType<PT>>
void add_arrow_to_nullable_decimal_column(const std::shared_ptr<arrow::Decimal128Type>& type, Column* column,
                                          size_t num_elements, int128_t value, CppType expect_value, size_t& counter,
                                          bool fail) {
    using ColumnType = RunTimeColumnType<PT>;
    ASSERT_EQ(column->size(), counter);
    auto array = create_const_decimal_array<true>(num_elements, std::move(type), value, counter);
    auto conv_func = get_arrow_converter(ArrowTypeId::DECIMAL, PT, true, false);
    ASSERT_TRUE(conv_func != nullptr);
    auto* nullable_column = down_cast<NullableColumn*>(column);
    auto* decimal_column = down_cast<ColumnType*>(nullable_column->data_column().get());
    auto* null_column = down_cast<NullColumn*>(nullable_column->null_column().get());
    fill_null_column(array.get(), 0, array->length(), null_column, null_column->size());
    auto* null_data = &null_column->get_data().front() + counter - num_elements;
    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    auto status = conv_func(array.get(), 0, array->length(), decimal_column, decimal_column->size(), null_data,
                            filter_data, nullptr);
    ASSERT_TRUE(status.ok());
    ASSERT_EQ(decimal_column->size(), counter);
    auto* decimal_data = &decimal_column->get_data().front();
    for (auto i = 0; i < num_elements; ++i) {
        auto idx = counter - num_elements + i;
        if (i % 2 == 0 || fail) {
            ASSERT_EQ(null_data[i], DATUM_NULL);
        } else {
            ASSERT_EQ(null_data[i], DATUM_NOT_NULL);
            ASSERT_EQ(decimal_data[idx], expect_value);
        }
    }
}

template <PrimitiveType FromPT, PrimitiveType ToPT, typename FromCppType = RunTimeCppType<FromPT>,
          typename ToCppType = RunTimeCppType<ToPT>>
bool decimal_to_decimal(FromCppType from_value, ToCppType* to_value, int adjust_scale) {
    if (adjust_scale == 0) {
        return DecimalV3Cast::to_decimal_trivial<FromCppType, ToCppType, true>(from_value, to_value);
    } else if (adjust_scale > 0) {
        auto scale_factor = get_scale_factor<ToCppType>(adjust_scale);
        return DecimalV3Cast::to_decimal<FromCppType, ToCppType, ToCppType, true, true>(from_value, scale_factor,
                                                                                        to_value);
    } else {
        auto scale_factor = get_scale_factor<FromCppType>(-adjust_scale);
        return DecimalV3Cast::to_decimal<FromCppType, ToCppType, FromCppType, false, true>(from_value, scale_factor,
                                                                                           to_value);
    }
}

template <PrimitiveType PT>
void test_decimal(std::shared_ptr<arrow::Decimal128Type> type, const TestCaseArray<std::string>& test_cases,
                  int precision, int scale) {
    using ColumnType = RunTimeColumnType<PT>;
    using CppType = RunTimeCppType<PT>;
    ColumnPtr col;
    if constexpr (pt_is_decimalv2<PT>) {
        col = ColumnType::create();
    } else {
        col = ColumnType::create(precision, scale);
    }
    col->reserve(4096);
    size_t counter = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto s = std::get<1>(tc);
        int128_t value;
        auto has_error =
                DecimalV3Cast::from_string<int128_t>(&value, type->precision(), type->scale(), s.c_str(), s.size());
        ASSERT_FALSE(has_error);
        CppType expect_value;
        bool overflow;
        if constexpr (pt_is_decimalv2<PT>) {
            int128_t v;
            overflow = decimal_to_decimal<TYPE_DECIMAL128, TYPE_DECIMAL128>(value, &v, 9 - type->scale());
            expect_value.set_value(v);
        } else {
            overflow = decimal_to_decimal<TYPE_DECIMAL128, PT>(value, &expect_value, scale - type->scale());
        }
        add_arrow_to_decimal_column<PT>(type, col.get(), num_elements, value, expect_value, counter, overflow);
    }
}

template <PrimitiveType PT>
void test_nullable_decimal(std::shared_ptr<arrow::Decimal128Type> type, const TestCaseArray<std::string>& test_cases,
                           int precision, int scale) {
    using ColumnType = RunTimeColumnType<PT>;
    using CppType = RunTimeCppType<PT>;
    ColumnPtr decimal_column;
    if constexpr (pt_is_decimalv2<PT>) {
        decimal_column = ColumnType::create();
    } else {
        decimal_column = ColumnType::create(precision, scale);
    }
    auto null_column = NullColumn::create();
    auto col = NullableColumn::create(decimal_column, null_column);
    col->reserve(4096);
    size_t counter = 0;
    for (auto& tc : test_cases) {
        auto num_elements = std::get<0>(tc);
        auto s = std::get<1>(tc);
        int128_t value;
        auto has_error =
                DecimalV3Cast::from_string<int128_t>(&value, type->precision(), type->scale(), s.c_str(), s.size());
        ASSERT_FALSE(has_error);
        CppType expect_value;
        bool overflow;
        if constexpr (pt_is_decimalv2<PT>) {
            int128_t v;
            overflow = decimal_to_decimal<TYPE_DECIMAL128, TYPE_DECIMAL128>(value, &v, 9 - type->scale());
            expect_value.set_value(v);
        } else {
            overflow = decimal_to_decimal<TYPE_DECIMAL128, PT>(value, &expect_value, scale - type->scale());
        }
        add_arrow_to_nullable_decimal_column<PT>(type, col.get(), num_elements, value, expect_value, counter, overflow);
    }
}

PARALLEL_TEST(ArrowConverterTest, test_decimalv2) {
    {
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.1234567", false},
                {4, "-22.1234567", false},
                {6, "99999999999.9999999", false},
                {8, "-99999999999.9999999", false},
                {9, "9999999999999.9999999", false},
        };

        // no scale
        auto type_p38s9 = std::make_shared<arrow::Decimal128Type>(38, 9);
        test_decimal<TYPE_DECIMALV2>(type_p38s9, test_cases, 38, 9);
        test_nullable_decimal<TYPE_DECIMALV2>(type_p38s9, test_cases, 38, 9);
        // scale down by 24
        auto type_p38s31 = std::make_shared<arrow::Decimal128Type>(38, 25);
        test_decimal<TYPE_DECIMAL128>(type_p38s31, test_cases, 38, 9);
        test_nullable_decimal<TYPE_DECIMAL128>(type_p38s31, test_cases, 38, 9);
    }
    {
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.12345", false},
                {4, "-22.12345", false},
                {6, "99999999999.99999", false},
                {8, "-99999999999.99999", false},
                {9, "9999999999999.99999", false},
        };
        //scale up by 4
        auto type_p38s25 = std::make_shared<arrow::Decimal128Type>(38, 5);
        test_decimal<TYPE_DECIMALV2>(type_p38s25, test_cases, 38, 9);
        test_nullable_decimal<TYPE_DECIMALV2>(type_p38s25, test_cases, 38, 9);
    }
}

PARALLEL_TEST(ArrowConverterTest, test_decimal32) {
    {
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.111", false},     {4, "22.111", false},      {6, "333.111", false},      {8, "4444.111", false},
                {9, "55555.111", false}, {10, "666666.111", false}, {11, "7777777.111", false},
        };

        // no scale
        auto type_p38s3 = std::make_shared<arrow::Decimal128Type>(38, 3);
        test_decimal<TYPE_DECIMAL32>(type_p38s3, test_cases, 9, 3);
        test_nullable_decimal<TYPE_DECIMAL32>(type_p38s3, test_cases, 9, 3);

        // scale down by 16
        auto type_p38s10 = std::make_shared<arrow::Decimal128Type>(38, 10);
        test_decimal<TYPE_DECIMAL32>(type_p38s10, test_cases, 9, 3);
        test_nullable_decimal<TYPE_DECIMAL32>(type_p38s10, test_cases, 9, 3);
    }
    {
        //scale up by 2
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.111", false},    {4, "22.111", false},    {6, "333.111", false},
                {8, "4444.111", false}, {9, "55555.111", false},
        };
        auto type_p38s3 = std::make_shared<arrow::Decimal128Type>(38, 3);
        test_decimal<TYPE_DECIMAL32>(type_p38s3, test_cases, 9, 5);
        test_nullable_decimal<TYPE_DECIMAL32>(type_p38s3, test_cases, 9, 5);
    }
}

PARALLEL_TEST(ArrowConverterTest, test_decimal64) {
    {
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.1234567", false},
                {4, "-22.1234567", false},
                {6, "99999999999.9999999", false},
                {8, "-99999999999.9999999", false},
                {9, "9999999999999.9999999", false},
        };

        // no scale
        auto type_p38s7 = std::make_shared<arrow::Decimal128Type>(38, 7);
        test_decimal<TYPE_DECIMAL64>(type_p38s7, test_cases, 18, 7);
        test_nullable_decimal<TYPE_DECIMAL64>(type_p38s7, test_cases, 18, 7);
        // scale down by 4
        auto type_p38s11 = std::make_shared<arrow::Decimal128Type>(38, 11);
        test_decimal<TYPE_DECIMAL64>(type_p38s11, test_cases, 18, 7);
        test_nullable_decimal<TYPE_DECIMAL64>(type_p38s11, test_cases, 18, 7);
    }
    {
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.12345", false},
                {4, "-22.12345", false},
                {6, "99999999999.99999", false},
                {8, "-99999999999.99999", false},
                {9, "9999999999999.99999", false},
        };
        //scale up by 2
        auto type_p38s5 = std::make_shared<arrow::Decimal128Type>(38, 5);
        test_decimal<TYPE_DECIMAL64>(type_p38s5, test_cases, 18, 7);
        test_nullable_decimal<TYPE_DECIMAL64>(type_p38s5, test_cases, 18, 7);
    }
}

PARALLEL_TEST(ArrowConverterTest, test_decimal128) {
    {
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.1234567", false},
                {4, "-22.1234567", false},
                {6, "99999999999.9999999", false},
                {8, "-99999999999.9999999", false},
                {9, "9999999999999.9999999", false},
        };

        // no scale
        auto type_p38s7 = std::make_shared<arrow::Decimal128Type>(38, 7);
        test_decimal<TYPE_DECIMAL128>(type_p38s7, test_cases, 38, 7);
        test_nullable_decimal<TYPE_DECIMAL128>(type_p38s7, test_cases, 38, 7);
        // scale down by 18
        auto type_p38s25 = std::make_shared<arrow::Decimal128Type>(38, 25);
        test_decimal<TYPE_DECIMAL128>(type_p38s25, test_cases, 38, 7);
        test_nullable_decimal<TYPE_DECIMAL128>(type_p38s25, test_cases, 38, 7);
    }
    {
        auto test_cases = TestCaseArray<std::string>{
                {2, "1.12345", false},
                {4, "-22.12345", false},
                {6, "99999999999.99999", false},
                {8, "-99999999999.99999", false},
                {9, "9999999999999.99999", false},
        };
        //scale up by 2
        auto type_p38s25 = std::make_shared<arrow::Decimal128Type>(38, 25);
        test_decimal<TYPE_DECIMAL128>(type_p38s25, test_cases, 38, 27);
        test_nullable_decimal<TYPE_DECIMAL128>(type_p38s25, test_cases, 38, 27);
    }
}

static std::shared_ptr<arrow::Array> create_map_array(int64_t num_elements, const std::map<std::string, int>& value,
                                                      size_t& counter) {
    auto key_builder = std::make_shared<arrow::StringBuilder>();
    auto item_builder = std::make_shared<arrow::Int32Builder>();
    arrow::TypeTraits<arrow::MapType>::BuilderType builder(arrow::default_memory_pool(), key_builder, item_builder);

    for (int i = 0; i < num_elements; i++) {
        builder.Append();
        for (auto& [key, value] : value) {
            key_builder->Append(key);
            item_builder->Append(value);
        }
        counter += 1;
    }
    return builder.Finish().ValueOrDie();
}

static std::string map_to_json(const std::map<std::string, int>& m) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (auto& [key, value] : m) {
        if (!first) {
            oss << ", ";
        }
        oss << "\"" << key << "\""
            << ": " << value;
        first = false;
    }
    oss << "}";
    return oss.str();
}

void add_arrow_map_to_json_column(Column* column, size_t num_elements, const std::map<std::string, int>& value,
                                  size_t& counter) {
    ASSERT_EQ(column->size(), counter);
    auto array = create_map_array(num_elements, value, counter);
    auto conv_func = get_arrow_converter(ArrowTypeId::MAP, TYPE_JSON, false, false);
    ASSERT_TRUE(conv_func != nullptr);

    Column::Filter filter;
    filter.resize(array->length(), 1);
    auto* filter_data = &filter.front();
    ASSERT_STATUS_OK(conv_func(array.get(), 0, array->length(), column, column->size(), nullptr, filter_data, nullptr));
    ASSERT_EQ(column->size(), counter);
    for (auto i = 0; i < num_elements; ++i) {
        const JsonValue* json = column->get(i).get_json();
        std::string json_str = json->to_string_uncheck();

        ASSERT_EQ(map_to_json(value), json_str);
    }
}

PARALLEL_TEST(ArrowConverterTest, test_map_to_json) {
    auto json_column = JsonColumn::create();
    json_column->reserve(4096);
    size_t counter = 0;
    std::map<std::string, int> map_value = {
            {"hehe", 1},
            {"haha", 2},
    };
    for (int i = 1; i < 10; i++) {
        add_arrow_map_to_json_column(json_column.get(), i, map_value, counter);
    }
}

} // namespace starrocks::vectorized

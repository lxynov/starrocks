// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exprs/agg/data_sketch/ds_kll.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "base/string/slice.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/fixed_length_column.h"
#include "column/nullable_column.h"
#include "column/vectorized_fwd.h"
#include "exprs/agg/aggregate_factory.h"
#include "exprs/agg/aggregate_state_allocator.h"
#include "exprs/agg/base_aggregate_test.h"
#include "exprs/ds_sketch_functions.h"
#include "simdjson.h"
#include "testutil/function_utils.h"
#include "types/date_value.h"
#include "types/timestamp_value.h"

namespace starrocks {

namespace {

struct ParsedBucket {
    std::string lower;
    std::string upper;
    double rank;
};

struct ParsedHistogram {
    uint64_t count = 0;
    bool has_bounds = false;
    std::string min;
    std::string max;
    std::vector<ParsedBucket> buckets;
};

ParsedHistogram parse_histogram(const std::string& json) {
    ParsedHistogram histogram;
    simdjson::dom::parser parser;
    simdjson::dom::element doc = parser.parse(json);
    histogram.count = doc["count"].get_uint64();
    if (!doc["min"].is_null()) {
        histogram.has_bounds = true;
        histogram.min = std::string(std::string_view(doc["min"]));
        histogram.max = std::string(std::string_view(doc["max"]));
    }
    for (simdjson::dom::element bucket : doc["buckets"].get_array()) {
        simdjson::dom::array fields = bucket.get_array();
        histogram.buckets.push_back({std::string(std::string_view(fields.at(0))),
                                     std::string(std::string_view(fields.at(1))), double(fields.at(2))});
    }
    return histogram;
}

template <LogicalType LT>
std::string serialize(const DsKllSketchImpl<LT>& sketch) {
    std::string bytes;
    sketch.serialize(&bytes);
    return bytes;
}

std::string histogram_json(const DsKllSketch& sketch, int32_t num_buckets) {
    std::string json;
    Status st = sketch.histogram_json(num_buckets, &json);
    EXPECT_TRUE(st.ok()) << st;
    return json;
}

} // namespace

class DsKllTest : public testing::Test {
public:
    void SetUp() override {
        _utils = std::make_unique<FunctionUtils>();
        _ctx = _utils->get_fn_ctx();
        _allocator = std::make_unique<CountingAllocatorWithHook>();
        tls_agg_state_allocator = _allocator.get();
    }

    void TearDown() override {
        _utils.reset();
        tls_agg_state_allocator = nullptr;
        _allocator.reset();
    }

protected:
    std::unique_ptr<FunctionUtils> _utils;
    FunctionContext* _ctx = nullptr;
    std::unique_ptr<CountingAllocatorWithHook> _allocator;
};

// Below k values the sketch keeps every value, so the buckets are exact.
TEST_F(DsKllTest, small_input_is_exact) {
    int64_t memory_usage = 0;
    {
        DsKllSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
        for (int32_t i = 1; i <= 100; ++i) {
            sketch.update(i);
        }
        ASSERT_EQ(100, sketch.count());
        ASSERT_GT(memory_usage, 0);

        ParsedHistogram histogram = parse_histogram(histogram_json(sketch, 4));
        ASSERT_EQ(100, histogram.count);
        ASSERT_EQ("1", histogram.min);
        ASSERT_EQ("100", histogram.max);
        ASSERT_EQ(4, histogram.buckets.size());
        const std::vector<ParsedBucket> expected = {
                {"1", "25", 0.25}, {"25", "50", 0.5}, {"50", "75", 0.75}, {"75", "100", 1.0}};
        for (size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(expected[i].lower, histogram.buckets[i].lower) << i;
            EXPECT_EQ(expected[i].upper, histogram.buckets[i].upper) << i;
            EXPECT_DOUBLE_EQ(expected[i].rank, histogram.buckets[i].rank) << i;
        }
    }
    // Everything the sketch allocated is returned when it goes away.
    ASSERT_EQ(0, memory_usage);
}

// The bounds stay within the rank error of the ranks that were asked for once the sketch starts sampling.
TEST_F(DsKllTest, large_input_is_within_rank_error) {
    int64_t memory_usage = 0;
    DsKllSketchImpl<TYPE_BIGINT> sketch(DsItemType::from(TYPE_BIGINT, 0, 0), &memory_usage);
    const int64_t num_values = 1000000;
    // A multiplicative step visits every value of [0, num_values) once, in a scrambled order.
    for (int64_t i = 0; i < num_values; ++i) {
        sketch.update((i * 7919) % num_values);
    }
    ASSERT_EQ(num_values, sketch.count());
    // The sketch is sublinear: a million values fit in a few tens of kilobytes.
    ASSERT_LT(serialize(sketch).size(), 64 * 1024);

    const int32_t num_buckets = 64;
    ParsedHistogram histogram = parse_histogram(histogram_json(sketch, num_buckets));
    ASSERT_EQ("0", histogram.min);
    ASSERT_EQ(std::to_string(num_values - 1), histogram.max);
    ASSERT_EQ(num_buckets, histogram.buckets.size());
    for (size_t i = 0; i < histogram.buckets.size(); ++i) {
        const ParsedBucket& bucket = histogram.buckets[i];
        // Four times the documented 99% error keeps the test from ever being flaky.
        const double tolerance = 4 * 0.0068;
        const double true_rank = (std::stod(bucket.upper) + 1) / num_values;
        EXPECT_NEAR(true_rank, bucket.rank, tolerance) << i;
        EXPECT_NEAR(static_cast<double>(i + 1) / num_buckets, bucket.rank, tolerance) << i;
        // Buckets are contiguous: each one starts where the one before it ends.
        EXPECT_EQ(i == 0 ? histogram.min : histogram.buckets[i - 1].upper, bucket.lower) << i;
    }
    ASSERT_DOUBLE_EQ(1.0, histogram.buckets.back().rank);
}

// A value that spans several of the requested ranks becomes one bound, with the rank of all its rows.
TEST_F(DsKllTest, repeated_values_collapse_buckets) {
    int64_t memory_usage = 0;
    DsKllSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
    for (int i = 0; i < 90; ++i) {
        sketch.update(5);
    }
    for (int i = 0; i < 10; ++i) {
        sketch.update(9);
    }

    ParsedHistogram histogram = parse_histogram(histogram_json(sketch, 4));
    ASSERT_EQ(100, histogram.count);
    ASSERT_EQ(2, histogram.buckets.size());
    EXPECT_EQ("5", histogram.buckets[0].lower);
    EXPECT_EQ("5", histogram.buckets[0].upper);
    EXPECT_DOUBLE_EQ(0.9, histogram.buckets[0].rank);
    EXPECT_EQ("5", histogram.buckets[1].lower);
    EXPECT_EQ("9", histogram.buckets[1].upper);
    EXPECT_DOUBLE_EQ(1.0, histogram.buckets[1].rank);

    // A single distinct value is a single bucket.
    DsKllSketchImpl<TYPE_INT> constant(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
    constant.update(7);
    constant.update(7);
    histogram = parse_histogram(histogram_json(constant, 64));
    ASSERT_EQ(1, histogram.buckets.size());
    EXPECT_EQ("7", histogram.buckets[0].lower);
    EXPECT_EQ("7", histogram.buckets[0].upper);
    EXPECT_DOUBLE_EQ(1.0, histogram.buckets[0].rank);
}

TEST_F(DsKllTest, invalid_bucket_count) {
    int64_t memory_usage = 0;
    DsKllSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
    sketch.update(1);
    std::string json;
    ASSERT_TRUE(sketch.histogram_json(0, &json).is_invalid_argument());
    ASSERT_TRUE(sketch.histogram_json(-1, &json).is_invalid_argument());
    ASSERT_TRUE(sketch.histogram_json(DsKllSketch::MAX_HISTOGRAM_BUCKETS + 1, &json).is_invalid_argument());
    ASSERT_TRUE(sketch.histogram_json(DsKllSketch::MAX_HISTOGRAM_BUCKETS, &json).ok());
}

// Values come back as the text a query returns for them, whatever the type.
TEST_F(DsKllTest, values_are_rendered_like_a_cast_to_varchar) {
    int64_t memory_usage = 0;
    {
        DsKllSketchImpl<TYPE_DATE> sketch(DsItemType::from(TYPE_DATE, 0, 0), &memory_usage);
        sketch.update(DateValue::create(2024, 1, 31));
        sketch.update(DateValue::create(2023, 12, 1));
        ParsedHistogram histogram = parse_histogram(histogram_json(sketch, 2));
        EXPECT_EQ("2023-12-01", histogram.min);
        EXPECT_EQ("2024-01-31", histogram.max);
    }
    {
        DsKllSketchImpl<TYPE_DATETIME> sketch(DsItemType::from(TYPE_DATETIME, 0, 0), &memory_usage);
        sketch.update(TimestampValue::create(2024, 1, 31, 23, 59, 58));
        ParsedHistogram histogram = parse_histogram(histogram_json(sketch, 2));
        EXPECT_EQ("2024-01-31 23:59:58", histogram.min);
    }
    {
        DsKllSketchImpl<TYPE_DECIMAL64> sketch(DsItemType::from(TYPE_DECIMAL64, 10, 2), &memory_usage);
        sketch.update(150);
        sketch.update(-25);
        ParsedHistogram histogram = parse_histogram(histogram_json(sketch, 2));
        EXPECT_EQ("-0.25", histogram.min);
        EXPECT_EQ("1.50", histogram.max);
    }
    {
        DsKllSketchImpl<TYPE_LARGEINT> sketch(DsItemType::from(TYPE_LARGEINT, 0, 0), &memory_usage);
        const __int128 big = static_cast<__int128>(1) << 100;
        sketch.update(big);
        sketch.update(-big);
        ParsedHistogram histogram = parse_histogram(histogram_json(sketch, 2));
        EXPECT_EQ("-1267650600228229401496703205376", histogram.min);
        EXPECT_EQ("1267650600228229401496703205376", histogram.max);
    }
    {
        DsKllSketchImpl<TYPE_DOUBLE> sketch(DsItemType::from(TYPE_DOUBLE, 0, 0), &memory_usage);
        sketch.update(0.1 + 0.2);
        sketch.update(-1.5);
        // NaN has no rank and is not counted.
        sketch.update(std::nan(""));
        ASSERT_EQ(2, sketch.count());
        ParsedHistogram histogram = parse_histogram(histogram_json(sketch, 2));
        EXPECT_EQ("-1.5", histogram.min);
        // The text round-trips to the same double.
        EXPECT_EQ(0.1 + 0.2, std::stod(histogram.max));
    }
}

TEST_F(DsKllTest, serialize_and_merge) {
    int64_t memory_usage = 0;
    const DsItemType item_type = DsItemType::from(TYPE_INT, 0, 0);
    DsKllSketchImpl<TYPE_INT> left(item_type, &memory_usage);
    DsKllSketchImpl<TYPE_INT> right(item_type, &memory_usage);
    for (int32_t i = 1; i <= 50; ++i) {
        left.update(i);
        right.update(i + 50);
    }

    // The serialized sketch names its value type, so it can be read back without being told.
    const std::string left_bytes = serialize(left);
    auto restored_or = DsKllSketch::deserialize(Slice(left_bytes), &memory_usage);
    ASSERT_TRUE(restored_or.ok()) << restored_or.status();
    std::unique_ptr<DsKllSketch> restored = std::move(restored_or).value();
    ASSERT_EQ(item_type, restored->item_type());
    ASSERT_EQ(50, restored->count());

    const std::string right_bytes = serialize(right);
    ASSERT_TRUE(restored->merge(Slice(right_bytes)).ok());
    ASSERT_EQ(100, restored->count());
    ParsedHistogram histogram = parse_histogram(histogram_json(*restored, 2));
    ASSERT_EQ("1", histogram.min);
    ASSERT_EQ("100", histogram.max);
    ASSERT_EQ(2, histogram.buckets.size());
    EXPECT_EQ("50", histogram.buckets[0].upper);

    // A sketch of another type is refused rather than reinterpreted.
    DsKllSketchImpl<TYPE_BIGINT> other(DsItemType::from(TYPE_BIGINT, 0, 0), &memory_usage);
    other.update(1);
    const std::string other_bytes = serialize(other);
    ASSERT_TRUE(restored->merge(Slice(other_bytes)).is_invalid_argument());
    ASSERT_TRUE(
            DsKllSketchImpl<TYPE_INT>::deserialize(Slice(other_bytes), &memory_usage).status().is_invalid_argument());

    // The same decimal layout with another scale holds different values.
    DsKllSketchImpl<TYPE_DECIMAL64> scale2(DsItemType::from(TYPE_DECIMAL64, 10, 2), &memory_usage);
    DsKllSketchImpl<TYPE_DECIMAL64> scale4(DsItemType::from(TYPE_DECIMAL64, 10, 4), &memory_usage);
    scale2.update(1);
    scale4.update(1);
    const std::string scale4_bytes = serialize(scale4);
    ASSERT_TRUE(scale2.merge(Slice(scale4_bytes)).is_invalid_argument());
}

TEST_F(DsKllTest, malformed_sketch) {
    int64_t memory_usage = 0;
    ASSERT_TRUE(DsKllSketch::deserialize(Slice("abc"), &memory_usage).status().is_invalid_argument());
    ASSERT_TRUE(DsKllSketch::deserialize(Slice("not a sketch at all"), &memory_usage).status().is_invalid_argument());

    DsKllSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
    for (int32_t i = 0; i < 1000; ++i) {
        sketch.update(i);
    }
    std::string bytes = serialize(sketch);
    // Cut in the middle of the values.
    ASSERT_TRUE(DsKllSketch::deserialize(Slice(bytes.data(), bytes.size() / 2), &memory_usage)
                        .status()
                        .is_invalid_argument());
    // An unknown header version.
    bytes[1] = 99;
    ASSERT_TRUE(DsKllSketch::deserialize(Slice(bytes), &memory_usage).status().is_invalid_argument());
}

// ds_kll over two partial states, merged and finalized, then read with ds_kll_histogram.
TEST_F(DsKllTest, aggregate_function) {
    std::vector<TypeDescriptor> arg_types = {TypeDescriptor(TYPE_INT)};
    std::unique_ptr<FunctionContext> ctx(
            FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
    const AggregateFunction* func = get_aggregate_function("ds_kll", TYPE_INT, TYPE_VARBINARY, false);
    ASSERT_NE(nullptr, func);

    auto update = [&](AggDataPtr state, int32_t begin, int32_t end) {
        auto column = Int32Column::create();
        for (int32_t i = begin; i < end; ++i) {
            column->append(i);
        }
        const Column* raw_columns[] = {column.get()};
        func->update_batch_single_state(ctx.get(), column->size(), raw_columns, state);
    };

    auto state1 = ManagedAggrState::create(ctx.get(), func);
    auto state2 = ManagedAggrState::create(ctx.get(), func);
    update(state1->state(), 0, 500);
    update(state2->state(), 500, 1000);

    MutableColumnPtr partial1 = BinaryColumn::create();
    MutableColumnPtr partial2 = BinaryColumn::create();
    func->serialize_to_column(ctx.get(), state1->state(), partial1.get());
    func->serialize_to_column(ctx.get(), state2->state(), partial2.get());

    auto merged = ManagedAggrState::create(ctx.get(), func);
    func->merge(ctx.get(), partial1.get(), merged->state(), 0);
    func->merge(ctx.get(), partial2.get(), merged->state(), 0);
    ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

    MutableColumnPtr result = BinaryColumn::create();
    func->finalize_to_column(ctx.get(), merged->state(), result.get());
    ASSERT_EQ(1, result->size());

    Columns columns{std::move(result), ColumnHelper::create_const_column<TYPE_INT>(4, 1)};
    auto json_or = DsSketchFunctions::ds_kll_histogram(_ctx, columns);
    ASSERT_TRUE(json_or.ok()) << json_or.status();
    ColumnPtr json_column = ColumnHelper::unfold_const_column(TYPE_VARCHAR_DESC, 1, json_or.value());
    ParsedHistogram histogram = parse_histogram(
            down_cast<const BinaryColumn*>(ColumnHelper::get_data_column(json_column.get()))->get_slice(0).to_string());
    ASSERT_EQ(1000, histogram.count);
    ASSERT_EQ("0", histogram.min);
    ASSERT_EQ("999", histogram.max);
    ASSERT_EQ(4, histogram.buckets.size());
    EXPECT_NEAR(0.25, histogram.buckets[0].rank, 0.03);
    EXPECT_NEAR(0.5, histogram.buckets[1].rank, 0.03);

    // A state that saw no row serializes to nothing, and merging nothing is a no-op.
    auto empty = ManagedAggrState::create(ctx.get(), func);
    MutableColumnPtr empty_partial = BinaryColumn::create();
    func->serialize_to_column(ctx.get(), empty->state(), empty_partial.get());
    ASSERT_EQ(0, down_cast<BinaryColumn*>(empty_partial.get())->get_slice(0).size);
    func->merge(ctx.get(), empty_partial.get(), merged->state(), 0);
    ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

    // Merging bytes that are not a sketch fails the query instead of producing a wrong result.
    MutableColumnPtr garbage = BinaryColumn::create();
    down_cast<BinaryColumn*>(garbage.get())->append(Slice("garbage bytes"));
    func->merge(ctx.get(), garbage.get(), merged->state(), 0);
    ASSERT_TRUE(ctx->has_error());
}

// The streaming pre-aggregation path turns each row into a sketch of its own.
TEST_F(DsKllTest, convert_to_serialize_format) {
    std::vector<TypeDescriptor> arg_types = {TypeDescriptor(TYPE_INT)};
    std::unique_ptr<FunctionContext> ctx(
            FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
    const AggregateFunction* func = get_aggregate_function("ds_kll", TYPE_INT, TYPE_VARBINARY, false);

    auto column = Int32Column::create();
    column->append(3);
    column->append(8);
    Columns src{column};
    MutableColumnPtr dst = BinaryColumn::create();
    func->convert_to_serialize_format(ctx.get(), src, 2, dst);
    ASSERT_EQ(2, dst->size());

    auto merged = ManagedAggrState::create(ctx.get(), func);
    func->merge(ctx.get(), dst.get(), merged->state(), 0);
    func->merge(ctx.get(), dst.get(), merged->state(), 1);
    ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();
    MutableColumnPtr result = BinaryColumn::create();
    func->finalize_to_column(ctx.get(), merged->state(), result.get());

    int64_t memory_usage = 0;
    auto sketch_or = DsKllSketch::deserialize(down_cast<BinaryColumn*>(result.get())->get_slice(0), &memory_usage);
    ASSERT_TRUE(sketch_or.ok()) << sketch_or.status();
    ParsedHistogram histogram = parse_histogram(histogram_json(*sketch_or.value(), 2));
    ASSERT_EQ(2, histogram.count);
    ASSERT_EQ("3", histogram.min);
    ASSERT_EQ("8", histogram.max);
}

TEST_F(DsKllTest, union_aggregate_function) {
    std::vector<TypeDescriptor> arg_types = {TYPE_VARBINARY_DESC};
    std::unique_ptr<FunctionContext> ctx(
            FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
    const AggregateFunction* func = get_aggregate_function("ds_kll_union", TYPE_VARBINARY, TYPE_VARBINARY, false);
    ASSERT_NE(nullptr, func);

    int64_t memory_usage = 0;
    auto sketches = BinaryColumn::create();
    for (int32_t part = 0; part < 3; ++part) {
        DsKllSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
        for (int32_t i = 0; i < 100; ++i) {
            sketch.update(part * 100 + i);
        }
        sketches->append(Slice(serialize(sketch)));
    }

    auto state = ManagedAggrState::create(ctx.get(), func);
    const Column* raw_columns[] = {sketches.get()};
    func->update_batch_single_state(ctx.get(), sketches->size(), raw_columns, state->state());
    ASSERT_FALSE(ctx->has_error()) << ctx->error_msg();

    // The union has the same partial and final form, so it also merges across stages.
    MutableColumnPtr partial = BinaryColumn::create();
    func->serialize_to_column(ctx.get(), state->state(), partial.get());
    auto merged = ManagedAggrState::create(ctx.get(), func);
    func->merge(ctx.get(), partial.get(), merged->state(), 0);
    MutableColumnPtr result = BinaryColumn::create();
    func->finalize_to_column(ctx.get(), merged->state(), result.get());

    auto sketch_or = DsKllSketch::deserialize(down_cast<BinaryColumn*>(result.get())->get_slice(0), &memory_usage);
    ASSERT_TRUE(sketch_or.ok()) << sketch_or.status();
    ASSERT_EQ(300, sketch_or.value()->count());
    ParsedHistogram histogram = parse_histogram(histogram_json(*sketch_or.value(), 3));
    ASSERT_EQ("0", histogram.min);
    ASSERT_EQ("299", histogram.max);

    // Sketches of different value types do not mix.
    DsKllSketchImpl<TYPE_DOUBLE> other(DsItemType::from(TYPE_DOUBLE, 0, 0), &memory_usage);
    other.update(1.0);
    auto mismatched = BinaryColumn::create();
    mismatched->append(Slice(serialize(other)));
    const Column* mismatched_columns[] = {mismatched.get()};
    func->update_batch_single_state(ctx.get(), 1, mismatched_columns, state->state());
    ASSERT_TRUE(ctx->has_error());
}

TEST_F(DsKllTest, histogram_function_nulls_and_errors) {
    int64_t memory_usage = 0;
    DsKllSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
    sketch.update(1);
    const std::string bytes = serialize(sketch);

    auto sketches = BinaryColumn::create();
    sketches->append(Slice(bytes));
    sketches->append(Slice(bytes));
    auto nulls = NullColumn::create();
    nulls->append(0);
    nulls->append(1);
    Columns columns{NullableColumn::create(std::move(sketches), std::move(nulls)),
                    ColumnHelper::create_const_column<TYPE_INT>(2, 2)};
    auto result_or = DsSketchFunctions::ds_kll_histogram(_ctx, columns);
    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_FALSE(result_or.value()->is_null(0));
    ASSERT_TRUE(result_or.value()->is_null(1));

    auto garbage = BinaryColumn::create();
    garbage->append(Slice("garbage bytes"));
    Columns bad_sketch{std::move(garbage), ColumnHelper::create_const_column<TYPE_INT>(2, 1)};
    ASSERT_TRUE(DsSketchFunctions::ds_kll_histogram(_ctx, bad_sketch).status().is_invalid_argument());

    auto valid = BinaryColumn::create();
    valid->append(Slice(bytes));
    Columns bad_bucket_count{std::move(valid), ColumnHelper::create_const_column<TYPE_INT>(0, 1)};
    ASSERT_TRUE(DsSketchFunctions::ds_kll_histogram(_ctx, bad_bucket_count).status().is_invalid_argument());
}

} // namespace starrocks

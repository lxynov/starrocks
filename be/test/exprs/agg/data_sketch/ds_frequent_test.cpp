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

#include "exprs/agg/data_sketch/ds_frequent.h"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
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

namespace starrocks {

namespace {

struct ParsedItem {
    std::string value;
    uint64_t estimate;
    uint64_t lower_bound;
    uint64_t upper_bound;
};

struct ParsedItems {
    uint64_t total = 0;
    uint64_t max_error = 0;
    std::vector<ParsedItem> items;
};

ParsedItems parse_items(const std::string& json) {
    ParsedItems parsed;
    simdjson::dom::parser parser;
    simdjson::dom::element doc = parser.parse(json);
    parsed.total = doc["total"].get_uint64();
    parsed.max_error = doc["max_error"].get_uint64();
    for (simdjson::dom::element item : doc["items"].get_array()) {
        simdjson::dom::array fields = item.get_array();
        parsed.items.push_back({std::string(std::string_view(fields.at(0))), fields.at(1).get_uint64(),
                                fields.at(2).get_uint64(), fields.at(3).get_uint64()});
    }
    return parsed;
}

template <LogicalType LT>
std::string serialize(const DsFrequentSketchImpl<LT>& sketch) {
    std::string bytes;
    sketch.serialize(&bytes);
    return bytes;
}

ParsedItems items_of(const DsFrequentSketch& sketch, int32_t max_items) {
    std::string json;
    Status st = sketch.items_json(max_items, &json);
    EXPECT_TRUE(st.ok()) << st;
    return parse_items(json);
}

} // namespace

class DsFrequentTest : public testing::Test {
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

// While every distinct value fits in the map, the counts are exact.
TEST_F(DsFrequentTest, few_distinct_values_are_exact) {
    int64_t memory_usage = 0;
    {
        DsFrequentSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
        for (int32_t value = 1; value <= 10; ++value) {
            for (int32_t i = 0; i < value * 10; ++i) {
                sketch.update(value);
            }
        }
        ASSERT_EQ(550, sketch.total_weight());
        ASSERT_EQ(0, sketch.maximum_error());
        ASSERT_GT(memory_usage, 0);

        ParsedItems parsed = items_of(sketch, 100);
        ASSERT_EQ(550, parsed.total);
        ASSERT_EQ(0, parsed.max_error);
        ASSERT_EQ(10, parsed.items.size());
        // Most frequent first.
        for (size_t i = 0; i < parsed.items.size(); ++i) {
            const int32_t value = 10 - static_cast<int32_t>(i);
            EXPECT_EQ(std::to_string(value), parsed.items[i].value);
            EXPECT_EQ(value * 10, parsed.items[i].estimate);
            EXPECT_EQ(value * 10, parsed.items[i].lower_bound);
            EXPECT_EQ(value * 10, parsed.items[i].upper_bound);
        }

        // Only as many as asked for.
        parsed = items_of(sketch, 3);
        ASSERT_EQ(550, parsed.total);
        ASSERT_EQ(3, parsed.items.size());
        EXPECT_EQ("10", parsed.items[0].value);
        EXPECT_EQ("8", parsed.items[2].value);
        ASSERT_EQ(0, items_of(sketch, 0).items.size());

        std::string json;
        ASSERT_TRUE(sketch.items_json(-1, &json).is_invalid_argument());
    }
    ASSERT_EQ(0, memory_usage);
}

// Values with the same count are listed in value order, whatever order they arrived in.
TEST_F(DsFrequentTest, ties_are_listed_in_value_order) {
    int64_t memory_usage = 0;
    DsFrequentSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
    for (int32_t value : {42, 7, 19, 3, 88}) {
        sketch.update(value);
        sketch.update(value);
    }
    ParsedItems parsed = items_of(sketch, 10);
    ASSERT_EQ(5, parsed.items.size());
    const std::vector<std::string> expected = {"3", "7", "19", "42", "88"};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], parsed.items[i].value);
    }
}

// With more distinct values than the map holds, the frequent ones survive and every true count is within the
// reported bounds.
TEST_F(DsFrequentTest, heavy_hitters_survive_a_long_tail) {
    int64_t memory_usage = 0;
    DsFrequentSketchImpl<TYPE_BIGINT> sketch(DsItemType::from(TYPE_BIGINT, 0, 0), &memory_usage);
    std::map<int64_t, uint64_t> true_counts;
    uint64_t total = 0;
    // 100000 values that occur once each, with five heavy values interleaved.
    for (int64_t i = 0; i < 100000; ++i) {
        sketch.update(1000000 + i);
        ++total;
        for (int64_t heavy = 1; heavy <= 5; ++heavy) {
            if (i % (heavy * 10) == 0) {
                sketch.update(heavy);
                ++true_counts[heavy];
                ++total;
            }
        }
    }
    ASSERT_EQ(total, sketch.total_weight());
    // 3.5 / 2048 of the rows seen.
    ASSERT_GT(sketch.maximum_error(), 0);
    ASSERT_LE(sketch.maximum_error(), static_cast<uint64_t>(std::ceil(3.5 / 2048 * total)));
    // The sketch is bounded however many values go by.
    ASSERT_LT(serialize(sketch).size(), 64 * 1024);

    ParsedItems parsed = items_of(sketch, 5);
    ASSERT_EQ(total, parsed.total);
    ASSERT_EQ(sketch.maximum_error(), parsed.max_error);
    ASSERT_EQ(5, parsed.items.size());
    for (size_t i = 0; i < parsed.items.size(); ++i) {
        const int64_t heavy = static_cast<int64_t>(i) + 1;
        const ParsedItem& item = parsed.items[i];
        EXPECT_EQ(std::to_string(heavy), item.value);
        EXPECT_LE(item.lower_bound, true_counts[heavy]);
        EXPECT_GE(item.upper_bound, true_counts[heavy]);
        EXPECT_EQ(item.upper_bound - item.lower_bound, parsed.max_error);
        EXPECT_EQ(item.estimate, item.upper_bound);
    }
}

// A column of unique values leaves the map empty after each purge. The row count and the error still survive
// serialization, although the library's own format drops both for an empty map.
TEST_F(DsFrequentTest, unique_values_keep_total_and_error) {
    int64_t memory_usage = 0;
    DsFrequentSketchImpl<TYPE_BIGINT> sketch(DsItemType::from(TYPE_BIGINT, 0, 0), &memory_usage);
    // The map holds 1536 values; the one after that triggers the purge, which removes them all.
    const int64_t num_values = 1537;
    for (int64_t i = 0; i < num_values; ++i) {
        sketch.update(i);
    }
    ASSERT_EQ(num_values, sketch.total_weight());
    ASSERT_EQ(1, sketch.maximum_error());
    ASSERT_EQ(0, items_of(sketch, 10).items.size());

    const std::string bytes = serialize(sketch);
    auto restored_or = DsFrequentSketch::deserialize(Slice(bytes), &memory_usage);
    ASSERT_TRUE(restored_or.ok()) << restored_or.status();
    ParsedItems parsed = items_of(*restored_or.value(), 10);
    ASSERT_EQ(num_values, parsed.total);
    ASSERT_EQ(1, parsed.max_error);

    // And they add up across a merge.
    DsFrequentSketchImpl<TYPE_BIGINT> other(DsItemType::from(TYPE_BIGINT, 0, 0), &memory_usage);
    other.update(5);
    other.update(5);
    ASSERT_TRUE(other.merge(Slice(bytes)).ok());
    parsed = items_of(other, 10);
    ASSERT_EQ(num_values + 2, parsed.total);
    ASSERT_EQ(1, parsed.max_error);
    ASSERT_EQ(1, parsed.items.size());
    EXPECT_EQ("5", parsed.items[0].value);
    EXPECT_EQ(2, parsed.items[0].lower_bound);
    EXPECT_EQ(3, parsed.items[0].upper_bound);
}

TEST_F(DsFrequentTest, strings) {
    int64_t memory_usage = 0;
    {
        DsFrequentSketchImpl<TYPE_VARCHAR> sketch(DsItemType::from(TYPE_CHAR, 0, 0), &memory_usage);
        // CHAR values are counted as VARCHAR.
        ASSERT_EQ(TYPE_VARCHAR, sketch.item_type().type);

        const std::string long_value(1000, 'x');
        const std::string quoted = "say \"hi\"\\\n";
        {
            // The sketch copies what it tracks: the buffers the rows came from can go away.
            std::string buffer = "apple";
            for (int i = 0; i < 3; ++i) {
                sketch.update(Slice(buffer));
            }
            buffer = "pear";
            sketch.update(Slice(buffer));
            buffer = long_value;
            sketch.update(Slice(buffer));
            sketch.update(Slice(buffer));
            buffer = quoted;
            sketch.update(Slice(buffer));
            buffer.assign(buffer.size(), '\0');
        }
        sketch.update(Slice(""));
        // The long value is held once, not once per row.
        ASSERT_GE(memory_usage, 1000);
        ASSERT_LT(memory_usage, 2000 + 64 * 1024);

        ParsedItems parsed = items_of(sketch, 10);
        ASSERT_EQ(8, parsed.total);
        ASSERT_EQ(5, parsed.items.size());
        EXPECT_EQ("apple", parsed.items[0].value);
        EXPECT_EQ(3, parsed.items[0].estimate);
        EXPECT_EQ(long_value, parsed.items[1].value);
        EXPECT_EQ(2, parsed.items[1].estimate);
        // The rest occur once and come in byte order; the JSON escapes survive a round trip.
        EXPECT_EQ("", parsed.items[2].value);
        EXPECT_EQ("pear", parsed.items[3].value);
        EXPECT_EQ(quoted, parsed.items[4].value);

        const std::string bytes = serialize(sketch);
        auto restored_or = DsFrequentSketch::deserialize(Slice(bytes), &memory_usage);
        ASSERT_TRUE(restored_or.ok()) << restored_or.status();
        ASSERT_TRUE(restored_or.value()->merge(Slice(bytes)).ok());
        parsed = items_of(*restored_or.value(), 2);
        ASSERT_EQ(16, parsed.total);
        ASSERT_EQ(2, parsed.items.size());
        EXPECT_EQ("apple", parsed.items[0].value);
        EXPECT_EQ(6, parsed.items[0].estimate);
        EXPECT_EQ(long_value, parsed.items[1].value);
        EXPECT_EQ(4, parsed.items[1].estimate);
    }
    // Every string the sketches owned is released with them.
    ASSERT_EQ(0, memory_usage);
}

// Values come back as the text a query returns for them, whatever the type.
TEST_F(DsFrequentTest, values_are_rendered_like_a_cast_to_varchar) {
    int64_t memory_usage = 0;
    {
        DsFrequentSketchImpl<TYPE_DATE> sketch(DsItemType::from(TYPE_DATE, 0, 0), &memory_usage);
        sketch.update(DateValue::create(2024, 2, 29));
        EXPECT_EQ("2024-02-29", items_of(sketch, 1).items[0].value);
    }
    {
        DsFrequentSketchImpl<TYPE_DECIMAL128> sketch(DsItemType::from(TYPE_DECIMAL128, 20, 4), &memory_usage);
        sketch.update(static_cast<__int128>(-12345678));
        EXPECT_EQ("-1234.5678", items_of(sketch, 1).items[0].value);
    }
    {
        DsFrequentSketchImpl<TYPE_BOOLEAN> sketch(DsItemType::from(TYPE_BOOLEAN, 0, 0), &memory_usage);
        sketch.update(1);
        sketch.update(1);
        sketch.update(0);
        ParsedItems parsed = items_of(sketch, 2);
        EXPECT_EQ("1", parsed.items[0].value);
        EXPECT_EQ("0", parsed.items[1].value);
    }
    {
        // 0.0 and -0.0 are one value, and so are all the NaNs.
        DsFrequentSketchImpl<TYPE_DOUBLE> sketch(DsItemType::from(TYPE_DOUBLE, 0, 0), &memory_usage);
        sketch.update(0.0);
        sketch.update(-0.0);
        sketch.update(std::nan(""));
        sketch.update(-std::nan(""));
        sketch.update(std::nan("1"));
        sketch.update(2.5);
        ParsedItems parsed = items_of(sketch, 10);
        ASSERT_EQ(6, parsed.total);
        ASSERT_EQ(3, parsed.items.size());
        EXPECT_EQ(3, parsed.items[0].estimate);
        EXPECT_EQ(2, parsed.items[1].estimate);
        EXPECT_EQ(0.0, std::stod(parsed.items[1].value));
        EXPECT_EQ("2.5", parsed.items[2].value);
    }
}

TEST_F(DsFrequentTest, serialize_and_merge) {
    int64_t memory_usage = 0;
    const DsItemType item_type = DsItemType::from(TYPE_INT, 0, 0);
    DsFrequentSketchImpl<TYPE_INT> left(item_type, &memory_usage);
    DsFrequentSketchImpl<TYPE_INT> right(item_type, &memory_usage);
    for (int i = 0; i < 30; ++i) {
        left.update(1);
        right.update(1);
        right.update(2);
    }
    left.update(3);

    // The serialized sketch names its value type, so it can be read back without being told.
    const std::string left_bytes = serialize(left);
    auto restored_or = DsFrequentSketch::deserialize(Slice(left_bytes), &memory_usage);
    ASSERT_TRUE(restored_or.ok()) << restored_or.status();
    std::unique_ptr<DsFrequentSketch> restored = std::move(restored_or).value();
    ASSERT_EQ(item_type, restored->item_type());
    ASSERT_EQ(31, restored->total_weight());

    const std::string right_bytes = serialize(right);
    ASSERT_TRUE(restored->merge(Slice(right_bytes)).ok());
    ParsedItems parsed = items_of(*restored, 10);
    ASSERT_EQ(91, parsed.total);
    ASSERT_EQ(3, parsed.items.size());
    EXPECT_EQ("1", parsed.items[0].value);
    EXPECT_EQ(60, parsed.items[0].estimate);
    EXPECT_EQ("2", parsed.items[1].value);
    EXPECT_EQ(30, parsed.items[1].estimate);
    EXPECT_EQ("3", parsed.items[2].value);
    EXPECT_EQ(1, parsed.items[2].estimate);

    // A sketch of another type is refused rather than reinterpreted.
    DsFrequentSketchImpl<TYPE_BIGINT> other(DsItemType::from(TYPE_BIGINT, 0, 0), &memory_usage);
    other.update(1);
    const std::string other_bytes = serialize(other);
    ASSERT_TRUE(restored->merge(Slice(other_bytes)).is_invalid_argument());
    ASSERT_TRUE(DsFrequentSketchImpl<TYPE_INT>::deserialize(Slice(other_bytes), &memory_usage)
                        .status()
                        .is_invalid_argument());
}

TEST_F(DsFrequentTest, malformed_sketch) {
    int64_t memory_usage = 0;
    ASSERT_TRUE(DsFrequentSketch::deserialize(Slice("abc"), &memory_usage).status().is_invalid_argument());
    ASSERT_TRUE(DsFrequentSketch::deserialize(Slice("definitely not a sketch, but long enough"), &memory_usage)
                        .status()
                        .is_invalid_argument());

    DsFrequentSketchImpl<TYPE_VARCHAR> sketch(DsItemType::from(TYPE_VARCHAR, 0, 0), &memory_usage);
    for (int i = 0; i < 100; ++i) {
        const std::string value = "value-" + std::to_string(i);
        sketch.update(Slice(value));
    }
    const std::string bytes = serialize(sketch);
    const int64_t memory_before = memory_usage;

    // Cut in the middle of the strings: nothing read so far may leak.
    ASSERT_TRUE(DsFrequentSketch::deserialize(Slice(bytes.data(), bytes.size() - 20), &memory_usage)
                        .status()
                        .is_invalid_argument());
    ASSERT_EQ(memory_before, memory_usage);

    // A value count the bytes cannot hold must not turn into an allocation of that size.
    std::string huge_count = bytes;
    const size_t num_items_offset = DsSketchHeader::SIZE + 2 * sizeof(uint64_t) + 8;
    memset(huge_count.data() + num_items_offset, 0xff, sizeof(uint32_t));
    ASSERT_TRUE(DsFrequentSketch::deserialize(Slice(huge_count), &memory_usage).status().is_invalid_argument());

    // Nor may a hash map size of 2^31.
    std::string huge_map = bytes;
    huge_map[DsSketchHeader::SIZE + 2 * sizeof(uint64_t) + 3] = 31;
    huge_map[DsSketchHeader::SIZE + 2 * sizeof(uint64_t) + 4] = 31;
    ASSERT_TRUE(DsFrequentSketch::deserialize(Slice(huge_map), &memory_usage).status().is_invalid_argument());
    ASSERT_EQ(memory_before, memory_usage);
}

// ds_frequent over two partial states, merged and finalized, then read with ds_frequent_items.
TEST_F(DsFrequentTest, aggregate_function) {
    std::vector<TypeDescriptor> arg_types = {TypeDescriptor::create_varchar_type(20)};
    std::unique_ptr<FunctionContext> ctx(
            FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
    const AggregateFunction* func = get_aggregate_function("ds_frequent", TYPE_VARCHAR, TYPE_VARBINARY, false);
    ASSERT_NE(nullptr, func);
    ASSERT_NE(nullptr, get_aggregate_function("ds_frequent", TYPE_CHAR, TYPE_VARBINARY, false));
    ASSERT_NE(nullptr, get_aggregate_function("ds_frequent", TYPE_DATE, TYPE_VARBINARY, false));

    auto update = [&](AggDataPtr state, const std::vector<std::string>& values) {
        auto column = BinaryColumn::create();
        for (const std::string& value : values) {
            column->append(Slice(value));
        }
        const Column* raw_columns[] = {column.get()};
        func->update_batch_single_state(ctx.get(), column->size(), raw_columns, state);
    };

    auto state1 = ManagedAggrState::create(ctx.get(), func);
    auto state2 = ManagedAggrState::create(ctx.get(), func);
    update(state1->state(), {"a", "b", "a", "c"});
    update(state2->state(), {"a", "b", "d"});

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

    Columns columns{std::move(result), ColumnHelper::create_const_column<TYPE_INT>(2, 1)};
    auto json_or = DsSketchFunctions::ds_frequent_items(_ctx, columns);
    ASSERT_TRUE(json_or.ok()) << json_or.status();
    ColumnPtr json_column = ColumnHelper::unfold_const_column(TYPE_VARCHAR_DESC, 1, json_or.value());
    const std::string json =
            down_cast<const BinaryColumn*>(ColumnHelper::get_data_column(json_column.get()))->get_slice(0).to_string();
    ASSERT_EQ(R"({"total":7,"max_error":0,"items":[["a",3,3,3],["b",2,2,2]]})", json);

    // Merging bytes that are not a sketch fails the query instead of producing a wrong result.
    MutableColumnPtr garbage = BinaryColumn::create();
    down_cast<BinaryColumn*>(garbage.get())->append(Slice("garbage bytes"));
    func->merge(ctx.get(), garbage.get(), merged->state(), 0);
    ASSERT_TRUE(ctx->has_error());
}

TEST_F(DsFrequentTest, union_aggregate_function) {
    std::vector<TypeDescriptor> arg_types = {TYPE_VARBINARY_DESC};
    std::unique_ptr<FunctionContext> ctx(
            FunctionContext::create_test_context(std::move(arg_types), TYPE_VARBINARY_DESC));
    const AggregateFunction* func = get_aggregate_function("ds_frequent_union", TYPE_VARBINARY, TYPE_VARBINARY, false);
    ASSERT_NE(nullptr, func);

    int64_t memory_usage = 0;
    auto sketches = BinaryColumn::create();
    for (int32_t part = 0; part < 3; ++part) {
        DsFrequentSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
        for (int32_t i = 0; i <= part; ++i) {
            sketch.update(7);
        }
        sketch.update(part);
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

    auto sketch_or = DsFrequentSketch::deserialize(down_cast<BinaryColumn*>(result.get())->get_slice(0), &memory_usage);
    ASSERT_TRUE(sketch_or.ok()) << sketch_or.status();
    ParsedItems parsed = items_of(*sketch_or.value(), 1);
    ASSERT_EQ(9, parsed.total);
    ASSERT_EQ(1, parsed.items.size());
    EXPECT_EQ("7", parsed.items[0].value);
    EXPECT_EQ(6, parsed.items[0].estimate);

    // Sketches of different value types do not mix.
    DsFrequentSketchImpl<TYPE_VARCHAR> other(DsItemType::from(TYPE_VARCHAR, 0, 0), &memory_usage);
    other.update(Slice("7"));
    auto mismatched = BinaryColumn::create();
    mismatched->append(Slice(serialize(other)));
    const Column* mismatched_columns[] = {mismatched.get()};
    func->update_batch_single_state(ctx.get(), 1, mismatched_columns, state->state());
    ASSERT_TRUE(ctx->has_error());
}

TEST_F(DsFrequentTest, items_function_nulls_and_errors) {
    int64_t memory_usage = 0;
    DsFrequentSketchImpl<TYPE_INT> sketch(DsItemType::from(TYPE_INT, 0, 0), &memory_usage);
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
    auto result_or = DsSketchFunctions::ds_frequent_items(_ctx, columns);
    ASSERT_TRUE(result_or.ok()) << result_or.status();
    ASSERT_FALSE(result_or.value()->is_null(0));
    ASSERT_TRUE(result_or.value()->is_null(1));

    auto garbage = BinaryColumn::create();
    garbage->append(Slice("garbage bytes"));
    Columns bad_sketch{std::move(garbage), ColumnHelper::create_const_column<TYPE_INT>(2, 1)};
    ASSERT_TRUE(DsSketchFunctions::ds_frequent_items(_ctx, bad_sketch).status().is_invalid_argument());

    auto valid = BinaryColumn::create();
    valid->append(Slice(bytes));
    Columns bad_count{std::move(valid), ColumnHelper::create_const_column<TYPE_INT>(-1, 1)};
    ASSERT_TRUE(DsSketchFunctions::ds_frequent_items(_ctx, bad_count).status().is_invalid_argument());
}

} // namespace starrocks

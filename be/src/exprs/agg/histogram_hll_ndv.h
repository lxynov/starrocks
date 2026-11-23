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

#pragma once

#include "base/hash/unaligned_access.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/runtime_type_traits.h"
#include "column/vectorized_fwd.h"
#include "exprs/agg/aggregate.h"
#include "exprs/agg/histogram.h"
#include "gutil/casts.h"
#include "runtime/mem_pool.h"
#include "types/hll.h"

namespace starrocks {

template <LogicalType LT>
struct HistogramHllNdvState {
    bool initialized = false;
    MemPool mem_pool;
    std::vector<Bucket<LT>> buckets;
    std::vector<HyperLogLog> hlls;
};

// histogram_hll_ndv(col, buckets): estimates how many distinct values of |col| fall in each of the given
// histogram buckets, with one HyperLogLog per bucket, and returns the buckets with that count added.
//
// ARGS_TYPE: the value type, and the constant JSON specification of the buckets
// SERIALIZED_TYPE: TYPE_VARBINARY
// RETURN_TYPE: TYPE_VARCHAR
//
// The intermediate state is the HyperLogLogs alone:
//
//   | number of buckets (4) | then per bucket: size (4) | serialized HyperLogLog |
//
// The buckets themselves are not part of it: they are a constant argument, which every stage is given.
template <LogicalType LT, typename T = RunTimeCppType<LT>>
class HistogramHllNdvAggregateFunction final
        : public AggregateFunctionBatchHelper<HistogramHllNdvState<LT>, HistogramHllNdvAggregateFunction<LT, T>> {
public:
    using ColumnType = RunTimeColumnType<LT>;

    void create_impl(FunctionContext* ctx, HistogramHllNdvState<LT>& state) const {
        DCHECK_EQ(ctx->get_num_args(), 2);
        state.initialized = true;
        std::vector<Bucket<LT>> buckets;

        try {
            simdjson::padded_string padded_buckets_string = ctx->get_constant_column(1)->get(0).get_slice().to_string();
            simdjson::ondemand::parser simdjson_parser;
            simdjson::ondemand::document doc;
            if (auto error = simdjson_parser.iterate(padded_buckets_string).get(doc); error != simdjson::SUCCESS) {
                ctx->set_error("histogram_hll_ndv: can't parse JSON specification of histogram buckets.", false);
                return;
            }
            simdjson::ondemand::array outer_array = doc.get_array();
            for (auto bucket_json : outer_array) {
                simdjson::ondemand::array inner_array = bucket_json.get_array();
                buckets.push_back(Bucket<LT>::from_json(inner_array, ctx->get_arg_type(0), &state.mem_pool));
            }
        } catch (const std::exception&) {
            ctx->set_error("histogram_hll_ndv: can't parse JSON specification of histogram buckets.", false);
            return;
        }

        state.buckets = buckets;
        state.hlls = std::vector<HyperLogLog>(buckets.size());
    }

    void reset(FunctionContext* ctx, const Columns& args, AggDataPtr state) const override {
        auto& state_impl = this->data(state);
        state_impl.initialized = false;
        state_impl.buckets.clear();
        state_impl.hlls.clear();
    }

    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        auto& state_impl = this->data(state);
        if (!state_impl.initialized) {
            create_impl(ctx, state_impl);
        }

        const auto v = GetContainer<LT>::get_data(columns[0], row_num);
        // find the correct bucket for the current value.
        const int index = find_bucket(state_impl.buckets, v);
        uint64_t hash = HashUtil::murmur_hash64A<T>(v, HashUtil::MURMUR_SEED);

        if (hash != 0 && index >= 0) {
            state_impl.hlls[index].update(hash);
        }
    }

    // The index of the bucket that holds |value|, or -1 if none does.
    template <typename ValueType>
    static int find_bucket(std::vector<Bucket<LT>>& buckets, const ValueType& value) {
        auto bucket_it = std::upper_bound(buckets.begin(), buckets.end(), value,
                                          [](auto& v, Bucket<LT>& bucket) { return bucket.is_less_equal_to_upper(v); });
        if (bucket_it == buckets.end() || !bucket_it->is_greater_equal_to_lower(value)) {
            return -1;
        }
        return std::distance(buckets.begin(), bucket_it);
    }

    void update_batch_single_state_with_frame(FunctionContext* ctx, AggDataPtr __restrict state, const Column** columns,
                                              int64_t peer_group_start, int64_t peer_group_end, int64_t frame_start,
                                              int64_t frame_end) const override {
        CHECK(false);
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        DCHECK(column->is_binary());
        const Slice slice = down_cast<const BinaryColumn*>(column)->get_slice(row_num);
        if (slice.size < sizeof(uint32_t)) {
            return;
        }
        const auto* ptr = reinterpret_cast<const uint8_t*>(slice.data);
        const uint8_t* end = ptr + slice.size;
        const uint32_t num_buckets = unaligned_load<uint32_t>(ptr);
        ptr += sizeof(uint32_t);
        if (num_buckets == 0) {
            // The state of an instance that saw no row.
            return;
        }

        auto& state_impl = this->data(state);
        if (!state_impl.initialized) {
            create_impl(ctx, state_impl);
        }
        if (num_buckets != state_impl.hlls.size()) {
            ctx->set_error("histogram_hll_ndv: a partial result does not match the histogram buckets.", false);
            return;
        }
        for (uint32_t i = 0; i < num_buckets; ++i) {
            if (end - ptr < static_cast<ptrdiff_t>(sizeof(uint32_t))) {
                ctx->set_error("histogram_hll_ndv: a partial result is truncated.", false);
                return;
            }
            const uint32_t size = unaligned_load<uint32_t>(ptr);
            ptr += sizeof(uint32_t);
            const Slice hll_slice(reinterpret_cast<const char*>(ptr), size);
            if (end - ptr < static_cast<ptrdiff_t>(size) || !HyperLogLog::is_valid(hll_slice)) {
                ctx->set_error("histogram_hll_ndv: a partial result is malformed.", false);
                return;
            }
            state_impl.hlls[i].merge(HyperLogLog(hll_slice));
            ptr += size;
        }
    }

    void serialize_to_column([[maybe_unused]] FunctionContext* ctx, ConstAggDataPtr __restrict state,
                             Column* to) const override {
        DCHECK(to->is_binary());
        const auto& hlls = this->data(state).hlls;
        size_t max_size = sizeof(uint32_t);
        for (const auto& hll : hlls) {
            max_size += sizeof(uint32_t) + hll.max_serialized_size();
        }
        std::string buffer;
        buffer.resize(max_size);
        auto* ptr = reinterpret_cast<uint8_t*>(buffer.data());
        unaligned_store<uint32_t>(ptr, static_cast<uint32_t>(hlls.size()));
        ptr += sizeof(uint32_t);
        for (const auto& hll : hlls) {
            const size_t size = hll.serialize(ptr + sizeof(uint32_t));
            unaligned_store<uint32_t>(ptr, static_cast<uint32_t>(size));
            ptr += sizeof(uint32_t) + size;
        }
        down_cast<BinaryColumn*>(to)->append(Slice(buffer.data(), ptr - reinterpret_cast<uint8_t*>(buffer.data())));
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        // Each row becomes a state of its own: every HyperLogLog empty except, at most, the one of the bucket the
        // row's value falls in.
        auto* result = down_cast<BinaryColumn*>(dst.get());
        HistogramHllNdvState<LT> spec;
        create_impl(ctx, spec);
        const size_t num_buckets = spec.buckets.size();
        HyperLogLog empty_hll;
        const Column* column = src[0].get();

        std::string buffer;
        for (size_t row = 0; row < chunk_size; ++row) {
            const auto v = GetContainer<LT>::get_data(column, row);
            const int index = find_bucket(spec.buckets, v);
            const uint64_t hash = HashUtil::murmur_hash64A<T>(v, HashUtil::MURMUR_SEED);
            HyperLogLog row_hll;
            if (hash != 0 && index >= 0) {
                row_hll.update(hash);
            }

            buffer.resize(sizeof(uint32_t) +
                          num_buckets * (sizeof(uint32_t) +
                                         std::max(empty_hll.max_serialized_size(), row_hll.max_serialized_size())));
            auto* ptr = reinterpret_cast<uint8_t*>(buffer.data());
            unaligned_store<uint32_t>(ptr, static_cast<uint32_t>(num_buckets));
            ptr += sizeof(uint32_t);
            for (size_t i = 0; i < num_buckets; ++i) {
                const HyperLogLog& hll = static_cast<int>(i) == index ? row_hll : empty_hll;
                const size_t size = hll.serialize(ptr + sizeof(uint32_t));
                unaligned_store<uint32_t>(ptr, static_cast<uint32_t>(size));
                ptr += sizeof(uint32_t) + size;
            }
            result->append(Slice(buffer.data(), ptr - reinterpret_cast<uint8_t*>(buffer.data())));
        }
    }

    std::string toBucketJson(const std::string& lower, const std::string& upper, size_t count, size_t upper_repeats,
                             size_t distinct_count) const {
        return fmt::format(R"(["{}","{}","{}","{}","{}"])", lower, upper, count, upper_repeats, distinct_count);
    }

    void finalize_to_column(FunctionContext* ctx __attribute__((unused)), ConstAggDataPtr __restrict state,
                            Column* to) const override {
        auto& buckets = this->data(state).buckets;
        auto& hlls = this->data(state).hlls;

        const auto& type_desc = ctx->get_arg_type(0);
        TypeInfoPtr type_info = get_type_info(LT, type_desc->precision, type_desc->scale);
        std::string buckets_json;
        if (buckets.empty()) {
            buckets_json = "[]";
        } else {
            buckets_json = "[";

            for (int i = 0; i < buckets.size(); ++i) {
                int64_t distinct_count = hlls[i].estimate_cardinality();
                if (distinct_count == 0) {
                    distinct_count = 1;
                }

                std::string lower_str = datum_to_string(type_info.get(), buckets[i].get_lower_datum());
                std::string upper_str = datum_to_string(type_info.get(), buckets[i].get_upper_datum());
                buckets_json +=
                        toBucketJson(lower_str, upper_str, buckets[i].count, buckets[i].upper_repeats, distinct_count) +
                        ",";
            }

            buckets_json[buckets_json.size() - 1] = ']';
        }

        Slice slice(buckets_json);
        to->append_datum(slice);
    }

    std::string get_name() const override { return "histogram_hll_ndv"; }
};

} // namespace starrocks

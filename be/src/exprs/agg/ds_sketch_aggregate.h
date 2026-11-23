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

#include <memory>
#include <string>

#include "column/binary_column.h"
#include "column/column_helper.h"
#include "column/runtime_type_traits.h"
#include "column/vectorized_fwd.h"
#include "exprs/agg/aggregate.h"
#include "exprs/agg/data_sketch/ds_frequent.h"
#include "exprs/agg/data_sketch/ds_kll.h"
#include "gutil/casts.h"

namespace starrocks {

// The aggregate functions that build and merge serialized sketches. Every sketch family has the same two:
//
//   <family>(col)           builds a sketch of the non-NULL values of |col|
//   <family>_union(sketch)  merges serialized sketches into one
//
// Both return the serialized sketch as VARBINARY, and both use those same bytes as their intermediate state.
// So each instance sketches its own rows and the partial sketches merge into the final one, and a sketch
// stored in a table can be merged again later.
//
// A family is described by a struct with:
//   BUILD_NAME, UNION_NAME        the SQL names of the two functions
//   Sketch                        the sketch behind an interface that does not name the value type
//   TypedSketch<LT>               the sketch of the values of one logical type

struct DsKllFamily {
    static constexpr const char* BUILD_NAME = "ds_kll";
    static constexpr const char* UNION_NAME = "ds_kll_union";
    using Sketch = DsKllSketch;
    template <LogicalType LT>
    using TypedSketch = DsKllSketchImpl<LT>;
};

struct DsFrequentFamily {
    static constexpr const char* BUILD_NAME = "ds_frequent";
    static constexpr const char* UNION_NAME = "ds_frequent_union";
    using Sketch = DsFrequentSketch;
    // CHAR and VARCHAR values are the same bytes and share one sketch type.
    template <LogicalType LT>
    using TypedSketch = DsFrequentSketchImpl<LT == TYPE_CHAR ? TYPE_VARCHAR : LT>;
};

template <typename SketchType>
struct DsSketchState {
    std::unique_ptr<SketchType> sketch;
    // What the sketch holds. The sketch's allocator keeps it up to date, including when the sketch is destroyed.
    int64_t memory_usage = 0;
};

template <typename SketchType>
inline void ds_append_sketch(const SketchType* sketch, Column* to) {
    DCHECK(to->is_binary());
    auto* column = down_cast<BinaryColumn*>(to);
    if (UNLIKELY(sketch == nullptr)) {
        // A state that saw no row. The nullable wrapper turns this into NULL.
        column->append_default();
        return;
    }
    std::string buffer;
    sketch->serialize(&buffer);
    column->append(Slice(buffer));
}

inline void ds_set_error(FunctionContext* ctx, const char* function_name, const Status& status) {
    ctx->set_error((std::string(function_name) + ": " + std::string(status.message())).c_str(), false);
}

// <family>(col)
//
// ARGS_TYPE: the value types the family supports
// SERIALIZED_TYPE: TYPE_VARBINARY
// RETURN_TYPE: TYPE_VARBINARY
template <LogicalType LT, typename Family>
class DsSketchBuildAggregateFunction final
        : public AggregateFunctionBatchHelper<DsSketchState<typename Family::template TypedSketch<LT>>,
                                              DsSketchBuildAggregateFunction<LT, Family>> {
public:
    using TypedSketch = typename Family::template TypedSketch<LT>;

    void reset(FunctionContext* ctx, const Columns& args, AggDataPtr state) const override {
        auto& state_impl = this->data(state);
        const int64_t prev_memory = state_impl.memory_usage;
        state_impl.sketch.reset();
        ctx->add_mem_usage(state_impl.memory_usage - prev_memory);
    }

    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        auto& state_impl = this->data(state);
        const int64_t prev_memory = state_impl.memory_usage;
        if (UNLIKELY(state_impl.sketch == nullptr)) {
            state_impl.sketch = _create_sketch(ctx, &state_impl.memory_usage);
        }
        state_impl.sketch->update(GetContainer<LT>::get_data(columns[0], row_num));
        ctx->add_mem_usage(state_impl.memory_usage - prev_memory);
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        DCHECK(column->is_binary());
        const Slice slice = down_cast<const BinaryColumn*>(column)->get_slice(row_num);
        if (slice.size == 0) {
            return;
        }
        auto& state_impl = this->data(state);
        const int64_t prev_memory = state_impl.memory_usage;
        {
            // Scoped so that the deserialized sketch is gone before the memory delta is taken.
            auto sketch_or = TypedSketch::deserialize(slice, &state_impl.memory_usage);
            if (!sketch_or.ok()) {
                ds_set_error(ctx, Family::BUILD_NAME, sketch_or.status());
                return;
            }
            if (state_impl.sketch == nullptr) {
                state_impl.sketch = std::move(sketch_or).value();
            } else {
                state_impl.sketch->merge(std::move(*sketch_or.value()));
            }
        }
        ctx->add_mem_usage(state_impl.memory_usage - prev_memory);
    }

    void serialize_to_column([[maybe_unused]] FunctionContext* ctx, ConstAggDataPtr __restrict state,
                             Column* to) const override {
        ds_append_sketch(this->data(state).sketch.get(), to);
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        // Each row becomes a sketch of its own.
        auto* result = down_cast<BinaryColumn*>(dst.get());
        const Column* column = src[0].get();
        std::string buffer;
        for (size_t i = 0; i < chunk_size; ++i) {
            int64_t memory_usage = 0;
            auto sketch = _create_sketch(ctx, &memory_usage);
            sketch->update(GetContainer<LT>::get_data(column, i));
            buffer.clear();
            sketch->serialize(&buffer);
            result->append(Slice(buffer));
        }
    }

    void finalize_to_column([[maybe_unused]] FunctionContext* ctx, ConstAggDataPtr __restrict state,
                            Column* to) const override {
        ds_append_sketch(this->data(state).sketch.get(), to);
    }

    std::string get_name() const override { return Family::BUILD_NAME; }

private:
    static std::unique_ptr<TypedSketch> _create_sketch(FunctionContext* ctx, int64_t* memory_usage) {
        // The sketch records the precision and scale of a decimal argument, which only the context knows.
        const auto* arg_type = ctx->get_arg_type(0);
        const DsItemType item_type = arg_type == nullptr ? DsItemType::from(LT, 0, 0)
                                                         : DsItemType::from(LT, arg_type->precision, arg_type->scale);
        return std::make_unique<TypedSketch>(item_type, memory_usage);
    }
};

// <family>_union(sketch)
//
// ARGS_TYPE: TYPE_VARBINARY
// SERIALIZED_TYPE: TYPE_VARBINARY
// RETURN_TYPE: TYPE_VARBINARY
//
// All the sketches of a group must hold values of the same type; the first one decides which.
template <typename Family>
class DsSketchUnionAggregateFunction final
        : public AggregateFunctionBatchHelper<DsSketchState<typename Family::Sketch>,
                                              DsSketchUnionAggregateFunction<Family>> {
public:
    using Sketch = typename Family::Sketch;

    void reset(FunctionContext* ctx, const Columns& args, AggDataPtr state) const override {
        auto& state_impl = this->data(state);
        const int64_t prev_memory = state_impl.memory_usage;
        state_impl.sketch.reset();
        ctx->add_mem_usage(state_impl.memory_usage - prev_memory);
    }

    void update(FunctionContext* ctx, const Column** columns, AggDataPtr __restrict state,
                size_t row_num) const override {
        _merge_serialized(ctx, columns[0], state, row_num);
    }

    void merge(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state, size_t row_num) const override {
        _merge_serialized(ctx, column, state, row_num);
    }

    void serialize_to_column([[maybe_unused]] FunctionContext* ctx, ConstAggDataPtr __restrict state,
                             Column* to) const override {
        ds_append_sketch(this->data(state).sketch.get(), to);
    }

    void convert_to_serialize_format([[maybe_unused]] FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        // The input already is a serialized sketch.
        const Column* src_column = src[0].get();
        const auto* input = down_cast<const BinaryColumn*>(ColumnHelper::get_data_column(src_column));
        auto* result = down_cast<BinaryColumn*>(dst.get());
        for (size_t i = 0; i < chunk_size; ++i) {
            result->append(input->get_slice(src_column->is_constant() ? 0 : i));
        }
    }

    void finalize_to_column([[maybe_unused]] FunctionContext* ctx, ConstAggDataPtr __restrict state,
                            Column* to) const override {
        ds_append_sketch(this->data(state).sketch.get(), to);
    }

    std::string get_name() const override { return Family::UNION_NAME; }

private:
    void _merge_serialized(FunctionContext* ctx, const Column* column, AggDataPtr __restrict state,
                           size_t row_num) const {
        const auto* input = down_cast<const BinaryColumn*>(ColumnHelper::get_data_column(column));
        const Slice slice = input->get_slice(column->is_constant() ? 0 : row_num);
        if (slice.size == 0) {
            return;
        }
        auto& state_impl = this->data(state);
        const int64_t prev_memory = state_impl.memory_usage;
        Status status;
        if (state_impl.sketch == nullptr) {
            auto sketch_or = Sketch::deserialize(slice, &state_impl.memory_usage);
            status = sketch_or.status();
            if (status.ok()) {
                state_impl.sketch = std::move(sketch_or).value();
            }
        } else {
            status = state_impl.sketch->merge(slice);
        }
        if (!status.ok()) {
            ds_set_error(ctx, Family::UNION_NAME, status);
            return;
        }
        ctx->add_mem_usage(state_impl.memory_usage - prev_memory);
    }
};

} // namespace starrocks

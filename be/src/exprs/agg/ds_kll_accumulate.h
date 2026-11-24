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
#include "common/statusor.h"
#include "exprs/agg/aggregate.h"
#include "exprs/agg/data_sketch/ds_kll.h"
#include "exprs/function_context.h"
#include "gutil/casts.h"

namespace starrocks {

// The state of ds_kll_accumulate, which holds a DsKllSketchImpl of the argument's type, and of ds_kll_combine, which
// holds a DsKllSketch because it only sees serialized sketches.
template <typename SketchType>
struct DsKllSketchState {
    std::unique_ptr<SketchType> sketch;
    // What the sketch holds. The sketch's allocator keeps it up to date, including when the sketch is destroyed.
    int64_t memory_usage = 0;
};

template <typename SketchType>
inline void ds_kll_append_sketch(const SketchType* sketch, Column* to) {
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

inline void ds_kll_set_error(FunctionContext* ctx, const char* function_name, const Status& status) {
    ctx->set_error((std::string(function_name) + ": " + std::string(status.message())).c_str(), false);
}

/**
 * ARGS_TYPE: a type in APPLY_FOR_DS_KLL_TYPES, then an optional constant INT k
 * SERIALIZED_TYPE: TYPE_VARBINARY
 * RETURN_TYPE: TYPE_VARBINARY
 */
template <LogicalType LT>
class DsKllAccumulateAggregateFunction final
        : public AggregateFunctionBatchHelper<DsKllSketchState<DsKllSketchImpl<LT>>,
                                              DsKllAccumulateAggregateFunction<LT>> {
public:
    using Sketch = DsKllSketchImpl<LT>;
    static constexpr const char* NAME = "ds_kll_accumulate";

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
            auto k_or = _parse_k(ctx);
            if (!k_or.ok()) {
                ds_kll_set_error(ctx, NAME, k_or.status());
                return;
            }
            state_impl.sketch = std::make_unique<Sketch>(_item_type(ctx), k_or.value(), &state_impl.memory_usage);
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
            auto sketch_or = Sketch::deserialize(slice, &state_impl.memory_usage);
            if (!sketch_or.ok()) {
                ds_kll_set_error(ctx, NAME, sketch_or.status());
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
        ds_kll_append_sketch(this->data(state).sketch.get(), to);
    }

    void convert_to_serialize_format(FunctionContext* ctx, const Columns& src, size_t chunk_size,
                                     MutableColumnPtr& dst) const override {
        // Each row becomes a sketch of its own.
        auto* result = down_cast<BinaryColumn*>(dst.get());
        auto k_or = _parse_k(ctx);
        if (!k_or.ok()) {
            ds_kll_set_error(ctx, NAME, k_or.status());
            result->append_default(chunk_size);
            return;
        }
        const DsItemType item_type = _item_type(ctx);
        const Column* column = src[0].get();
        std::string buffer;
        for (size_t i = 0; i < chunk_size; ++i) {
            int64_t memory_usage = 0;
            Sketch sketch(item_type, k_or.value(), &memory_usage);
            sketch.update(GetContainer<LT>::get_data(column, i));
            buffer.clear();
            sketch.serialize(&buffer);
            result->append(Slice(buffer));
        }
    }

    void finalize_to_column([[maybe_unused]] FunctionContext* ctx, ConstAggDataPtr __restrict state,
                            Column* to) const override {
        ds_kll_append_sketch(this->data(state).sketch.get(), to);
    }

    std::string get_name() const override { return NAME; }

private:
    // k, from ds_kll_accumulate(col[, k]). The sketch records it, so only this function reads it.
    static StatusOr<uint16_t> _parse_k(FunctionContext* ctx) {
        if (ctx->get_num_args() < 2) {
            return DsKllSketch::DEFAULT_K;
        }
        // The frontend only lets an integer literal through; this guards the library, which throws on a bad k.
        if (!ctx->is_notnull_constant_column(1)) {
            return Status::InvalidArgument("k must be a constant integer");
        }
        const int64_t k = ColumnHelper::get_const_value<TYPE_INT>(ctx->get_constant_column(1));
        RETURN_IF_ERROR(DsKllSketch::check_k(k));
        return static_cast<uint16_t>(k);
    }

    static DsItemType _item_type(FunctionContext* ctx) {
        // The sketch records the precision and scale of a decimal argument, which only the context knows.
        const auto* arg_type = ctx->get_arg_type(0);
        return arg_type == nullptr ? DsItemType::from(LT, 0, 0)
                                   : DsItemType::from(LT, arg_type->precision, arg_type->scale);
    }
};

} // namespace starrocks

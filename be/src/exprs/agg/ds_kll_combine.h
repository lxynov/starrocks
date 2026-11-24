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

#include <string>

#include "column/binary_column.h"
#include "column/column_helper.h"
#include "exprs/agg/aggregate.h"
#include "exprs/agg/data_sketch/ds_kll.h"
#include "exprs/agg/ds_kll_accumulate.h"
#include "gutil/casts.h"

namespace starrocks {

// ds_kll_combine(sketch) merges serialized KLL sketches, as ds_kll_accumulate and ds_kll_combine return them, into
// one. It uses the serialized sketch as its intermediate state too.
//
// All the sketches of a group must hold values of the same type; the first one decides which. They can have been
// built with different k: the result has the error of the smallest.
//
// ARGS_TYPE: TYPE_VARBINARY
// SERIALIZED_TYPE: TYPE_VARBINARY
// RETURN_TYPE: TYPE_VARBINARY
class DsKllCombineAggregateFunction final
        : public AggregateFunctionBatchHelper<DsKllSketchState<DsKllSketch>, DsKllCombineAggregateFunction> {
public:
    static constexpr const char* NAME = "ds_kll_combine";

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
        ds_kll_append_sketch(this->data(state).sketch.get(), to);
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
        ds_kll_append_sketch(this->data(state).sketch.get(), to);
    }

    std::string get_name() const override { return NAME; }

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
            auto sketch_or = DsKllSketch::deserialize(slice, &state_impl.memory_usage);
            status = sketch_or.status();
            if (status.ok()) {
                state_impl.sketch = std::move(sketch_or).value();
            }
        } else {
            status = state_impl.sketch->merge(slice);
        }
        if (!status.ok()) {
            ds_kll_set_error(ctx, NAME, status);
            return;
        }
        ctx->add_mem_usage(state_impl.memory_usage - prev_memory);
    }
};

} // namespace starrocks

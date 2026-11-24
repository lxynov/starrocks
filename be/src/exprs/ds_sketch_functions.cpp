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

#include "exprs/ds_sketch_functions.h"

#include <fmt/format.h>

#include <string>

#include "column/column_builder.h"
#include "column/column_helper.h"
#include "column/column_viewer.h"
#include "exprs/agg/data_sketch/ds_kll.h"

namespace starrocks {

StatusOr<ColumnPtr> DsSketchFunctions::ds_kll_estimate(FunctionContext* context, const Columns& columns) {
    ColumnViewer<TYPE_VARBINARY> sketch_viewer(columns[0]);
    ColumnViewer<TYPE_INT> num_buckets_viewer(columns[1]);
    const size_t size = columns[0]->size();
    ColumnBuilder<TYPE_VARCHAR> builder(size);

    std::string json;
    for (size_t row = 0; row < size; ++row) {
        // Zero bytes is what ds_kll_accumulate returns in place of a sketch when it saw no value and cannot return
        // NULL.
        if (sketch_viewer.is_null(row) || num_buckets_viewer.is_null(row) || sketch_viewer.value(row).size == 0) {
            builder.append_null();
            continue;
        }
        int64_t memory_usage = 0;
        auto sketch_or = DsKllSketch::deserialize(sketch_viewer.value(row), &memory_usage);
        if (!sketch_or.ok()) {
            return Status::InvalidArgument(fmt::format("ds_kll_estimate: {}", sketch_or.status().message()));
        }
        json.clear();
        Status status = sketch_or.value()->histogram_json(num_buckets_viewer.value(row), &json);
        if (!status.ok()) {
            return Status::InvalidArgument(fmt::format("ds_kll_estimate: {}", status.message()));
        }
        builder.append(Slice(json));
    }
    return builder.build(ColumnHelper::is_all_const(columns));
}

} // namespace starrocks

#include "gen_cpp/opcode/DsSketchFunctions.inc"

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

#include "exprs/function_helper.h"

namespace starrocks {

// Scalar functions that read a serialized sketch built by ds_kll_accumulate or ds_kll_combine and return what it
// knows as JSON. The sketch carries the type of its values, so one signature serves every value type.
class DsSketchFunctions {
public:
    // ds_kll_estimate(sketch, n): the bounds and ranks of n equal-height buckets. See
    // DsKllSketch::histogram_json for the format. NULL if either argument is NULL or the sketch is zero bytes.
    // ds_kll_histogram(col, n) is the frontend's shorthand for ds_kll_estimate(ds_kll_accumulate(col), n).
    DEFINE_VECTORIZED_FN(ds_kll_estimate);
};

} // namespace starrocks

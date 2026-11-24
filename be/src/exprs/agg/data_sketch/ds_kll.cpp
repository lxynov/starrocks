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

#include <fmt/format.h>

#include <vector>

#include "column/column_helper.h"
#include "gutil/casts.h"

namespace starrocks {

Status DsKllSketch::check_k(int64_t k) {
    if (k < MIN_K || k > MAX_K) {
        return Status::InvalidArgument(fmt::format("k must be between {} and {}, but got {}", MIN_K, MAX_K, k));
    }
    return Status::OK();
}

StatusOr<std::unique_ptr<DsKllSketch>> DsKllSketch::deserialize(const Slice& src, int64_t* memory_usage) {
    DsItemType item_type;
    RETURN_IF_ERROR(DsSketchHeader::decode(src, DsSketchHeader::MAGIC_KLL, &item_type));
    switch (item_type.type) {
#define M(TYPE)                                                                               \
    case TYPE: {                                                                              \
        ASSIGN_OR_RETURN(auto sketch, DsKllSketchImpl<TYPE>::deserialize(src, memory_usage)); \
        return std::unique_ptr<DsKllSketch>(std::move(sketch));                               \
    }
        APPLY_FOR_DS_KLL_TYPES(M)
#undef M
    default:
        return Status::InvalidArgument(fmt::format("unsupported KLL sketch value type {}", item_type.to_string()));
    }
}

template <LogicalType LT>
StatusOr<std::unique_ptr<DsKllSketchImpl<LT>>> DsKllSketchImpl<LT>::deserialize(const Slice& src,
                                                                                int64_t* memory_usage) {
    DsItemType item_type;
    RETURN_IF_ERROR(DsSketchHeader::decode(src, DsSketchHeader::MAGIC_KLL, &item_type));
    if (item_type.type != LT) {
        return Status::InvalidArgument(fmt::format("expected a KLL sketch of {} values, but got one of {} values",
                                                   logical_type_to_string(LT), item_type.to_string()));
    }
    try {
        Sketch sketch = Sketch::deserialize(src.data + DsSketchHeader::SIZE, src.size - DsSketchHeader::SIZE,
                                            DsPodSerde<CppType>(), std::less<CppType>(), Alloc(memory_usage));
        return std::unique_ptr<DsKllSketchImpl>(new DsKllSketchImpl(item_type, memory_usage, std::move(sketch)));
    } catch (const std::exception& e) {
        return Status::InvalidArgument(fmt::format("malformed KLL sketch: {}", e.what()));
    }
}

template <LogicalType LT>
Status DsKllSketchImpl<LT>::merge(const Slice& src) {
    ASSIGN_OR_RETURN(auto other, deserialize(src, _memory_usage));
    if (other->_item_type != _item_type) {
        return Status::InvalidArgument(fmt::format("cannot merge a KLL sketch of {} values into one of {} values",
                                                   other->_item_type.to_string(), _item_type.to_string()));
    }
    merge(std::move(*other));
    return Status::OK();
}

template <LogicalType LT>
void DsKllSketchImpl<LT>::serialize(std::string* dst) const {
    // The library leaves room for the header in front of its own bytes, so the sketch is copied only once.
    auto bytes = _sketch.serialize(DsSketchHeader::SIZE, DsPodSerde<CppType>());
    DsSketchHeader::encode(DsSketchHeader::MAGIC_KLL, _item_type, bytes.data());
    dst->append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

template <LogicalType LT>
Status DsKllSketchImpl<LT>::histogram_json(int32_t num_buckets, std::string* json) const {
    if (num_buckets <= 0 || num_buckets > MAX_HISTOGRAM_BUCKETS) {
        return Status::InvalidArgument(fmt::format("the number of buckets must be between 1 and {}, but got {}",
                                                   MAX_HISTOGRAM_BUCKETS, num_buckets));
    }
    const uint64_t count = _sketch.get_n();
    if (count == 0) {
        json->append(R"({"count":0,"min":null,"max":null,"buckets":[]})");
        return Status::OK();
    }

    const CppType min_value = _sketch.get_min_item();
    const CppType max_value = _sketch.get_max_item();
    const std::less<CppType> less;

    // The upper bounds are the values at ranks 1/n, 2/n, ..., then the maximum. A value frequent enough to span
    // several of those ranks is returned for each of them, and is kept once.
    std::vector<CppType> uppers;
    std::vector<double> ranks;
    uppers.reserve(num_buckets);
    ranks.reserve(num_buckets);
    {
        const auto sorted_view = _sketch.get_sorted_view();
        for (int32_t i = 1; i < num_buckets; ++i) {
            const CppType& quantile = sorted_view.get_quantile(static_cast<double>(i) / num_buckets, true);
            if (uppers.empty() || less(uppers.back(), quantile)) {
                uppers.emplace_back(quantile);
            }
        }
        if (uppers.empty() || less(uppers.back(), max_value)) {
            uppers.emplace_back(max_value);
        }
        // The rank of a bound is read back from the sketch rather than taken as i/n: when the value repeats,
        // the rows at or below it are more than the rank that was asked for.
        for (size_t i = 0; i + 1 < uppers.size(); ++i) {
            ranks.emplace_back(sorted_view.get_rank(uppers[i], true));
        }
        ranks.emplace_back(1.0);
    }

    // One column holding the minimum followed by every upper bound, so all of them are rendered in one pass.
    MutableColumnPtr values = ColumnHelper::create_column(_item_type.to_type_descriptor(), false);
    auto* typed_values = down_cast<RunTimeColumnType<LT>*>(values.get());
    typed_values->append(min_value);
    for (const CppType& upper : uppers) {
        typed_values->append(upper);
    }
    std::vector<std::string> strings;
    RETURN_IF_ERROR(ds_values_to_strings(_item_type, std::move(values), &strings));

    fmt::format_to(std::back_inserter(*json), R"({{"count":{},"min":)", count);
    ds_append_json_string(strings.front(), json);
    json->append(R"(,"max":)");
    ds_append_json_string(strings.back(), json);
    json->append(R"(,"buckets":[)");
    for (size_t i = 0; i < uppers.size(); ++i) {
        json->append(i == 0 ? "[" : ",[");
        ds_append_json_string(strings[i], json);
        json->push_back(',');
        ds_append_json_string(strings[i + 1], json);
        fmt::format_to(std::back_inserter(*json), ",{}]", ranks[i]);
    }
    json->append("]}");
    return Status::OK();
}

#define M(TYPE) template class DsKllSketchImpl<TYPE>;
APPLY_FOR_DS_KLL_TYPES(M)
#undef M

} // namespace starrocks

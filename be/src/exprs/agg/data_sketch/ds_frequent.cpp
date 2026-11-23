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

#include <fmt/format.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "base/hash/unaligned_access.h"
#include "column/binary_column.h"
#include "column/column_helper.h"
#include "gutil/casts.h"

namespace starrocks {

namespace {

// Orders the values of a sketch, to list the ones with equal counts in a stable order.
template <typename T>
bool ds_item_less(const T& lhs, const T& rhs) {
    if constexpr (std::is_floating_point_v<T>) {
        if (std::isnan(lhs) || std::isnan(rhs)) {
            return !std::isnan(lhs) && std::isnan(rhs);
        }
    }
    return lhs < rhs;
}

// The Apache DataSketches preamble of a frequent items sketch:
//   byte 0: preamble longs, 1: serial version, 2: family, 3: lg max map size, 4: lg current map size, 5: flags
// followed, unless the sketch is empty, by the number of values (4 bytes, at offset 8) and one 8-byte weight
// per value (from offset 32).
constexpr size_t FI_PREAMBLE_SIZE = 8;
constexpr size_t FI_NUM_ITEMS_OFFSET = 8;
constexpr size_t FI_WEIGHTS_OFFSET = 32;
constexpr uint8_t FI_EMPTY_FLAG = 1;

// The library sizes its allocations from these fields before it has checked them against the input, so bytes
// that are not a sketch could make it ask for gigabytes. Reject what a sketch built here cannot contain.
Status check_frequent_items_preamble(const Slice& sketch) {
    if (sketch.size < FI_PREAMBLE_SIZE) {
        return Status::InvalidArgument("malformed frequent items sketch: truncated preamble");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(sketch.data);
    const uint8_t lg_max_map_size = bytes[3];
    const uint8_t lg_cur_map_size = bytes[4];
    if (lg_max_map_size != DsFrequentSketch::LG_MAX_MAP_SIZE || lg_cur_map_size > lg_max_map_size) {
        return Status::InvalidArgument(fmt::format("malformed frequent items sketch: map size 2^{} of at most 2^{}",
                                                   lg_cur_map_size, lg_max_map_size));
    }
    if ((bytes[5] & FI_EMPTY_FLAG) != 0) {
        return Status::OK();
    }
    if (sketch.size < FI_WEIGHTS_OFFSET) {
        return Status::InvalidArgument("malformed frequent items sketch: truncated preamble");
    }
    const uint32_t num_items = unaligned_load<uint32_t>(bytes + FI_NUM_ITEMS_OFFSET);
    if (num_items > (sketch.size - FI_WEIGHTS_OFFSET) / sizeof(uint64_t)) {
        return Status::InvalidArgument(fmt::format("malformed frequent items sketch: {} values do not fit in {} bytes",
                                                   num_items, sketch.size));
    }
    return Status::OK();
}

} // namespace

size_t DsStringSerde::serialize(void* ptr, size_t capacity, const DsString* items, unsigned num) const {
    auto* dst = static_cast<uint8_t*>(ptr);
    size_t written = 0;
    for (unsigned i = 0; i < num; ++i) {
        const size_t item_size = sizeof(uint32_t) + items[i].size();
        if (written + item_size > capacity) {
            throw std::out_of_range("not enough room to write the sketch strings");
        }
        unaligned_store<uint32_t>(dst + written, static_cast<uint32_t>(items[i].size()));
        if (items[i].size() > 0) {
            memcpy(dst + written + sizeof(uint32_t), items[i].data(), items[i].size());
        }
        written += item_size;
    }
    return written;
}

size_t DsStringSerde::deserialize(const void* ptr, size_t capacity, DsString* items, unsigned num) const {
    const auto* src = static_cast<const uint8_t*>(ptr);
    size_t read = 0;
    unsigned constructed = 0;
    try {
        for (; constructed < num; ++constructed) {
            if (read + sizeof(uint32_t) > capacity) {
                throw std::out_of_range("truncated sketch strings");
            }
            const uint32_t size = unaligned_load<uint32_t>(src + read);
            read += sizeof(uint32_t);
            if (size > capacity - read) {
                throw std::out_of_range("truncated sketch strings");
            }
            // Constructing from a borrowed string makes one that owns a copy of the bytes.
            const DsString borrowed =
                    DsString::borrow(Slice(reinterpret_cast<const char*>(src + read), size), memory_usage);
            new (&items[constructed]) DsString(borrowed);
            read += size;
        }
    } catch (...) {
        // The caller only destroys the strings once all of them are there.
        for (unsigned i = 0; i < constructed; ++i) {
            items[i].~DsString();
        }
        throw;
    }
    return read;
}

Status DsFrequentSketch::_decode_header(const Slice& src, DsItemType* item_type, uint64_t* total_weight,
                                        uint64_t* lost_error) {
    RETURN_IF_ERROR(DsSketchHeader::decode(src, DsSketchHeader::MAGIC_FREQUENT, item_type));
    if (src.size < HEADER_SIZE) {
        return Status::InvalidArgument(fmt::format("sketch is too short: {} bytes", src.size));
    }
    const auto* extra = reinterpret_cast<const uint8_t*>(src.data) + DsSketchHeader::SIZE;
    *total_weight = unaligned_load<uint64_t>(extra);
    *lost_error = unaligned_load<uint64_t>(extra + sizeof(uint64_t));
    return Status::OK();
}

StatusOr<std::unique_ptr<DsFrequentSketch>> DsFrequentSketch::deserialize(const Slice& src, int64_t* memory_usage) {
    DsItemType item_type;
    RETURN_IF_ERROR(DsSketchHeader::decode(src, DsSketchHeader::MAGIC_FREQUENT, &item_type));
    switch (item_type.type) {
#define M(TYPE)                                                                                    \
    case TYPE: {                                                                                   \
        ASSIGN_OR_RETURN(auto sketch, DsFrequentSketchImpl<TYPE>::deserialize(src, memory_usage)); \
        return std::unique_ptr<DsFrequentSketch>(std::move(sketch));                               \
    }
        APPLY_FOR_DS_FREQUENT_TYPES(M)
#undef M
    default:
        return Status::InvalidArgument(
                fmt::format("unsupported frequent items sketch value type {}", item_type.to_string()));
    }
}

template <LogicalType LT>
StatusOr<std::unique_ptr<DsFrequentSketchImpl<LT>>> DsFrequentSketchImpl<LT>::deserialize(const Slice& src,
                                                                                          int64_t* memory_usage) {
    DsItemType item_type;
    uint64_t total_weight = 0;
    uint64_t lost_error = 0;
    RETURN_IF_ERROR(_decode_header(src, &item_type, &total_weight, &lost_error));
    if (item_type.type != LT) {
        return Status::InvalidArgument(
                fmt::format("expected a frequent items sketch of {} values, but got one of {} values",
                            logical_type_to_string(LT), item_type.to_string()));
    }
    const Slice sketch_bytes(src.data + HEADER_SIZE, src.size - HEADER_SIZE);
    RETURN_IF_ERROR(check_frequent_items_preamble(sketch_bytes));

    std::unique_ptr<DsFrequentSketchImpl> result;
    try {
        Serde serde;
        if constexpr (LT == TYPE_VARCHAR) {
            serde.memory_usage = memory_usage;
        }
        Sketch sketch = Sketch::deserialize(sketch_bytes.data, sketch_bytes.size, serde, DsItemEqual<Item>(),
                                            Alloc(memory_usage));
        result.reset(new DsFrequentSketchImpl(item_type, memory_usage, std::move(sketch)));
    } catch (const std::exception& e) {
        return Status::InvalidArgument(fmt::format("malformed frequent items sketch: {}", e.what()));
    }
    result->_total_weight = total_weight;
    result->_lost_error = lost_error;
    return result;
}

template <LogicalType LT>
Status DsFrequentSketchImpl<LT>::merge(const Slice& src) {
    ASSIGN_OR_RETURN(auto other, deserialize(src, _memory_usage));
    if (other->_item_type != _item_type) {
        return Status::InvalidArgument(
                fmt::format("cannot merge a frequent items sketch of {} values into one of {} values",
                            other->_item_type.to_string(), _item_type.to_string()));
    }
    merge(std::move(*other));
    return Status::OK();
}

template <LogicalType LT>
void DsFrequentSketchImpl<LT>::serialize(std::string* dst) const {
    // The library leaves room for the header in front of its own bytes, so the sketch is copied only once.
    auto bytes = _sketch.serialize(HEADER_SIZE, _serde());
    DsSketchHeader::encode(DsSketchHeader::MAGIC_FREQUENT, _item_type, bytes.data());
    uint8_t* extra = bytes.data() + DsSketchHeader::SIZE;
    unaligned_store<uint64_t>(extra, _total_weight);
    // An empty map is written without its error, which would then read back as zero.
    unaligned_store<uint64_t>(extra + sizeof(uint64_t), _sketch.is_empty() ? maximum_error() : _lost_error);
    dst->append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

template <LogicalType LT>
Status DsFrequentSketchImpl<LT>::items_json(int32_t max_items, std::string* json) const {
    if (max_items < 0) {
        return Status::InvalidArgument(fmt::format("the number of values must not be negative, but got {}", max_items));
    }
    // With a threshold of zero this is every value the sketch tracks.
    auto rows = _sketch.get_frequent_items(datasketches::NO_FALSE_NEGATIVES, 0);
    // The library sorts by count only, which leaves values with equal counts in hash map order, and that order
    // depends on how the partial sketches happened to be merged.
    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.get_lower_bound() != rhs.get_lower_bound()) {
            return lhs.get_lower_bound() > rhs.get_lower_bound();
        }
        return ds_item_less(lhs.get_item(), rhs.get_item());
    });
    const size_t num_items = std::min<size_t>(rows.size(), max_items);

    MutableColumnPtr values = ColumnHelper::create_column(_item_type.to_type_descriptor(), false);
    for (size_t i = 0; i < num_items; ++i) {
        if constexpr (LT == TYPE_VARCHAR) {
            down_cast<BinaryColumn*>(values.get())->append(rows[i].get_item().slice());
        } else {
            down_cast<RunTimeColumnType<LT>*>(values.get())->append(rows[i].get_item());
        }
    }
    std::vector<std::string> strings;
    RETURN_IF_ERROR(ds_values_to_strings(_item_type, std::move(values), &strings));

    const uint64_t max_error = maximum_error();
    fmt::format_to(std::back_inserter(*json), R"({{"total":{},"max_error":{},"items":[)", _total_weight, max_error);
    for (size_t i = 0; i < num_items; ++i) {
        const uint64_t lower_bound = rows[i].get_lower_bound();
        const uint64_t upper_bound = lower_bound + max_error;
        json->append(i == 0 ? "[" : ",[");
        ds_append_json_string(strings[i], json);
        fmt::format_to(std::back_inserter(*json), ",{},{},{}]", upper_bound, lower_bound, upper_bound);
    }
    json->append("]}");
    return Status::OK();
}

#define M(TYPE) template class DsFrequentSketchImpl<TYPE>;
APPLY_FOR_DS_FREQUENT_TYPES(M)
#undef M

} // namespace starrocks

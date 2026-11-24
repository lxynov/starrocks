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

#include <cstdint>
#include <cstring>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "base/string/slice.h"
#include "column/column.h"
#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "types/logical_type.h"
#include "types/type_descriptor.h"

namespace starrocks {

// The SQL type of the values a KLL or frequent items sketch summarizes.
//
// A serialized sketch is a plain VARBINARY, so the functions that read one back (ds_kll_combine,
// ds_kll_estimate, ds_frequent_combine, ds_frequent_estimate) cannot learn the value type from their
// argument. The type therefore travels with the sketch, in the DsSketchHeader in front of it.
struct DsItemType {
    LogicalType type = TYPE_UNKNOWN;
    int precision = 0;
    int scale = 0;

    // CHAR and VARCHAR values are the same bytes, so both are kept as VARCHAR. Precision and scale are kept for
    // decimal types only.
    static DsItemType from(const TypeDescriptor& type_desc);
    static DsItemType from(LogicalType type, int precision, int scale);

    TypeDescriptor to_type_descriptor() const;
    std::string to_string() const;

    bool operator==(const DsItemType& rhs) const {
        return type == rhs.type && precision == rhs.precision && scale == rhs.scale;
    }
    bool operator!=(const DsItemType& rhs) const { return !(*this == rhs); }
};

// The fixed-size prefix that makes a serialized sketch self-describing:
//
//   | magic (1) | version (1) | precision (1) | scale (1) | TPrimitiveType (4, little endian) |
//
// The Apache DataSketches bytes follow it. The value type is written as its thrift id, which is stable across
// versions, because a partial sketch can be merged on a backend running a different build than the one that
// produced it.
struct DsSketchHeader {
    static constexpr size_t SIZE = 8;
    static constexpr uint8_t VERSION = 1;
    static constexpr uint8_t MAGIC_KLL = 'K';
    static constexpr uint8_t MAGIC_FREQUENT = 'F';

    // |dst| must have room for SIZE bytes.
    static void encode(uint8_t magic, const DsItemType& item_type, uint8_t* dst);

    // Fails unless |src| starts with a header of the kind named by |expected_magic|.
    static Status decode(const Slice& src, uint8_t expected_magic, DsItemType* item_type);
};

// Apache DataSketches SerDe for values that are plain bytes. The library's own serde covers only the types for
// which std::is_arithmetic holds, which leaves out dates, timestamps and the wide decimals.
template <typename T>
struct DsPodSerde {
    static_assert(std::is_trivially_copyable_v<T>, "DsPodSerde stores values as raw bytes");

    void serialize(std::ostream& os, const T* items, unsigned num) const {
        os.write(reinterpret_cast<const char*>(items), sizeof(T) * num);
        if (!os.good()) {
            throw std::runtime_error("error writing sketch values");
        }
    }

    void deserialize(std::istream& is, T* items, unsigned num) const {
        is.read(reinterpret_cast<char*>(items), sizeof(T) * num);
        if (!is.good()) {
            throw std::runtime_error("error reading sketch values");
        }
    }

    size_t size_of_item(const T&) const { return sizeof(T); }

    size_t serialize(void* ptr, size_t capacity, const T* items, unsigned num) const {
        const size_t bytes = sizeof(T) * num;
        check_capacity(bytes, capacity);
        memcpy(ptr, items, bytes);
        return bytes;
    }

    size_t deserialize(const void* ptr, size_t capacity, T* items, unsigned num) const {
        const size_t bytes = sizeof(T) * num;
        check_capacity(bytes, capacity);
        memcpy(static_cast<void*>(items), ptr, bytes);
        return bytes;
    }

private:
    static void check_capacity(size_t bytes, size_t capacity) {
        if (bytes > capacity) {
            throw std::out_of_range("sketch values need " + std::to_string(bytes) + " bytes, but only " +
                                    std::to_string(capacity) + " are left");
        }
    }
};

// Renders each value of |values| the way CAST(value AS VARCHAR) does. A value read out of a sketch goes back
// into SQL as a literal and is compared with the text the optimizer prints for a constant, so it has to look
// exactly like the same value selected from the table. |values| must be a non-nullable column of |item_type|.
Status ds_values_to_strings(const DsItemType& item_type, const ColumnPtr& values, std::vector<std::string>* strings);

// Appends |value| to |json| as a JSON string, quotes included.
void ds_append_json_string(std::string_view value, std::string* json);

} // namespace starrocks

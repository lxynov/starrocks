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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "base/hash/hash_util.hpp"
#include "base/string/slice.h"
#include "column/runtime_type_traits.h"
#include "common/status.h"
#include "common/statusor.h"
#include "datasketches/frequent_items_sketch.hpp"
#include "exprs/agg/data_sketch/ds_kll.h"
#include "exprs/agg/data_sketch/ds_sketch_utils.h"
#include "runtime/memory/counting_allocator.h"
#include "types/logical_type.h"

namespace starrocks {

// The value types ds_frequent accepts: everything ds_kll does, plus strings. CHAR values are counted as VARCHAR.
#define APPLY_FOR_DS_FREQUENT_TYPES(M) \
    APPLY_FOR_DS_KLL_TYPES(M)          \
    M(TYPE_VARCHAR)

template <LogicalType LT>
inline constexpr bool ds_frequent_supports = ds_kll_supports<LT> || LT == TYPE_VARCHAR || LT == TYPE_CHAR;

// A string held by a frequent items sketch.
//
// A sketch looks up every row it is given but stores only the values it tracks, so a string comes in two
// forms. One that borrows its bytes costs nothing to build and is what a row is looked up with. A string
// constructed from another one owns its bytes, which is what the sketch stores, and that memory is added to
// |*memory_usage|.
class DsString {
public:
    DsString() = default;

    // Borrows |slice|, which must outlive the returned string. Strings constructed from it own a copy of the
    // bytes, which counts against |memory_usage|.
    static DsString borrow(const Slice& slice, int64_t* memory_usage) { return DsString(slice, memory_usage); }

    // Whatever |other| is, the new string owns its bytes: the sketch keeps what it constructs.
    DsString(const DsString& other) : _memory_usage(other._memory_usage) { _copy_from(other); }

    DsString(DsString&& other) : _memory_usage(other._memory_usage) {
        if (other._owned) {
            _data = other._data;
            _size = other._size;
            _owned = true;
            other._data = nullptr;
            other._size = 0;
            other._owned = false;
        } else {
            _copy_from(other);
        }
    }

    DsString& operator=(const DsString& other) {
        if (this != &other) {
            DsString copy(other);
            _swap(copy);
        }
        return *this;
    }

    DsString& operator=(DsString&& other) {
        if (this != &other) {
            DsString moved(std::move(other));
            _swap(moved);
        }
        return *this;
    }

    ~DsString() {
        if (_owned && _data != nullptr) {
            delete[] _data;
            _add_memory(-static_cast<int64_t>(_size));
        }
    }

    const char* data() const { return _data; }
    size_t size() const { return _size; }
    Slice slice() const { return {_data, _size}; }

    bool operator==(const DsString& rhs) const {
        return _size == rhs._size && (_size == 0 || memcmp(_data, rhs._data, _size) == 0);
    }

    bool operator<(const DsString& rhs) const { return slice().compare(rhs.slice()) < 0; }

private:
    DsString(const Slice& slice, int64_t* memory_usage)
            : _data(slice.data), _size(slice.size), _memory_usage(memory_usage) {}

    void _copy_from(const DsString& other) {
        _size = other._size;
        if (_size > 0) {
            _data = new char[_size];
            memcpy(_data, other._data, _size);
            _owned = true;
            _add_memory(static_cast<int64_t>(_size));
        }
    }

    void _swap(DsString& other) {
        std::swap(_data, other._data);
        std::swap(_size, other._size);
        std::swap(_owned, other._owned);
        std::swap(_memory_usage, other._memory_usage);
    }

    void _add_memory(int64_t bytes) {
        if (_memory_usage != nullptr) {
            *_memory_usage += bytes;
        }
    }

    char* _data = nullptr;
    size_t _size = 0;
    bool _owned = false;
    int64_t* _memory_usage = nullptr;
};

// Apache DataSketches SerDe for DsString: a 4-byte length, then the bytes.
struct DsStringSerde {
    int64_t* memory_usage = nullptr;

    size_t size_of_item(const DsString& item) const { return sizeof(uint32_t) + item.size(); }
    size_t serialize(void* ptr, size_t capacity, const DsString* items, unsigned num) const;
    // On failure no string is left constructed in |items|.
    size_t deserialize(const void* ptr, size_t capacity, DsString* items, unsigned num) const;
};

// The type of the values the sketch of |LT| values stores.
template <LogicalType LT>
using DsFrequentItem = std::conditional_t<LT == TYPE_VARCHAR, DsString, RunTimeCppType<LT>>;

// Hash and equality for the values of a sketch. Floating-point values need care: -0.0 and 0.0 are the same
// value, and so are all the NaNs, as they are for GROUP BY.
template <typename T>
struct DsItemHash {
    size_t operator()(const T& value) const {
        if constexpr (std::is_same_v<T, DsString>) {
            return HashUtil::murmur_hash64A(value.data(), static_cast<int32_t>(value.size()), HashUtil::MURMUR_SEED);
        } else if constexpr (std::is_floating_point_v<T>) {
            T canonical = value;
            if (canonical == 0) {
                canonical = 0;
            } else if (std::isnan(canonical)) {
                canonical = std::numeric_limits<T>::quiet_NaN();
            }
            return HashUtil::murmur_hash64A(&canonical, sizeof(T), HashUtil::MURMUR_SEED);
        } else {
            static_assert(std::has_unique_object_representations_v<T>, "the value is hashed as raw bytes");
            return HashUtil::murmur_hash64A(&value, sizeof(T), HashUtil::MURMUR_SEED);
        }
    }
};

template <typename T>
struct DsItemEqual {
    bool operator()(const T& lhs, const T& rhs) const {
        if constexpr (std::is_floating_point_v<T>) {
            return lhs == rhs || (std::isnan(lhs) && std::isnan(rhs));
        } else {
            return lhs == rhs;
        }
    }
};

// A frequent items sketch of the values of one SQL type, behind an interface that does not name the type. This
// is what the functions that only see a serialized sketch work with: ds_frequent_union and ds_frequent_items.
//
// The sketch counts how often each value occurs, in a hash map of bounded size. When the map is full it
// subtracts the median count from every counter, drops the counters that reach zero, and remembers how much it
// subtracted. So for a value it still tracks, the true count lies between the counter (the lower bound) and the
// counter plus everything subtracted so far (the upper bound). That gap is the same for every value, is at
// most 3.5 / map size of the rows seen, and stays within that after merges.
class DsFrequentSketch {
public:
    // Log2 of the size of the hash map: 2048 entries. The map holds up to three quarters of that, so counts are
    // exact as long as there are no more than 1536 distinct values, and otherwise off by at most 0.17% of the
    // rows seen.
    static constexpr uint8_t LG_MAX_MAP_SIZE = 11;

    virtual ~DsFrequentSketch() = default;

    // Reads back a sketch written by serialize(). Memory the sketch holds is added to |*memory_usage|, which must
    // outlive the sketch.
    static StatusOr<std::unique_ptr<DsFrequentSketch>> deserialize(const Slice& src, int64_t* memory_usage);

    const DsItemType& item_type() const { return _item_type; }

    // Number of values the sketch has seen.
    uint64_t total_weight() const { return _total_weight; }

    // Merges a serialized sketch of the same value type into this one.
    virtual Status merge(const Slice& src) = 0;

    // Appends the serialized sketch, header included, to |dst|.
    virtual void serialize(std::string* dst) const = 0;

    // Lists up to |max_items| of the tracked values, most frequent first, as JSON:
    //
    //   {"total":N,"max_error":E,"items":[["value",estimate,lower_bound,upper_bound],...]}
    //
    // |total| is the number of values seen and |max_error| the gap between the bounds of every value. As in
    // Apache DataSketches, the estimate of a tracked value is its upper bound. A value that is not listed, when
    // fewer than |max_items| come back, occurs at most |max_error| times.
    virtual Status items_json(int32_t max_items, std::string* json) const = 0;

protected:
    // What follows the DsSketchHeader, before the Apache DataSketches bytes: the two numbers its serialized
    // form does not keep when the map happens to be empty, which is the state a purge leaves behind when every
    // counter is equal, as on a column of unique values.
    static constexpr size_t EXTRA_HEADER_SIZE = 2 * sizeof(uint64_t);
    static constexpr size_t HEADER_SIZE = DsSketchHeader::SIZE + EXTRA_HEADER_SIZE;

    explicit DsFrequentSketch(const DsItemType& item_type) : _item_type(item_type) {}

    static Status _decode_header(const Slice& src, DsItemType* item_type, uint64_t* total_weight, uint64_t* lost_error);

    DsItemType _item_type;
    uint64_t _total_weight = 0;
    // The part of the error that sketches merged into this one had accumulated but could not carry in their
    // serialized form.
    uint64_t _lost_error = 0;
};

// The sketch of one concrete value type. ds_frequent works with this class directly, so that adding a row does
// not go through a virtual call.
template <LogicalType LT>
class DsFrequentSketchImpl final : public DsFrequentSketch {
public:
    using Item = DsFrequentItem<LT>;
    using Alloc = STLCountingAllocator<Item>;
    using Sketch = datasketches::frequent_items_sketch<Item, uint64_t, DsItemHash<Item>, DsItemEqual<Item>, Alloc>;
    using Serde = std::conditional_t<LT == TYPE_VARCHAR, DsStringSerde, DsPodSerde<Item>>;

    DsFrequentSketchImpl(const DsItemType& item_type, int64_t* memory_usage)
            : DsFrequentSketch(item_type),
              _memory_usage(memory_usage),
              _sketch(LG_MAX_MAP_SIZE, Sketch::LG_MIN_MAP_SIZE, DsItemEqual<Item>(), Alloc(memory_usage)) {}

    // Fails unless |src| is a sketch of exactly this value type.
    static StatusOr<std::unique_ptr<DsFrequentSketchImpl>> deserialize(const Slice& src, int64_t* memory_usage);

    // |value| is a RunTimeCppType<LT>, which for strings is a Slice that only has to live through the call.
    void update(const RunTimeCppType<LT>& value) {
        if constexpr (LT == TYPE_VARCHAR) {
            // Passed as an lvalue, so the sketch copies the string, and only when it starts tracking it.
            const DsString borrowed = DsString::borrow(value, _memory_usage);
            _sketch.update(borrowed);
        } else {
            _sketch.update(value);
        }
        ++_total_weight;
    }

    void merge(DsFrequentSketchImpl&& other) {
        _sketch.merge(std::move(other._sketch));
        _total_weight += other._total_weight;
        _lost_error += other._lost_error;
    }

    uint64_t maximum_error() const { return _sketch.get_maximum_error() + _lost_error; }

    Status merge(const Slice& src) override;
    void serialize(std::string* dst) const override;
    Status items_json(int32_t max_items, std::string* json) const override;

private:
    DsFrequentSketchImpl(const DsItemType& item_type, int64_t* memory_usage, Sketch&& sketch)
            : DsFrequentSketch(item_type), _memory_usage(memory_usage), _sketch(std::move(sketch)) {}

    Serde _serde() const {
        if constexpr (LT == TYPE_VARCHAR) {
            return DsStringSerde{_memory_usage};
        } else {
            return Serde();
        }
    }

    int64_t* _memory_usage;
    Sketch _sketch;
};

#define M(TYPE) extern template class DsFrequentSketchImpl<TYPE>;
APPLY_FOR_DS_FREQUENT_TYPES(M)
#undef M

} // namespace starrocks

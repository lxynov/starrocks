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
#include <functional>
#include <memory>
#include <string>

#include "base/string/slice.h"
#include "column/runtime_type_traits.h"
#include "common/status.h"
#include "common/statusor.h"
#include "datasketches/kll_sketch.hpp"
#include "exprs/agg/data_sketch/ds_sketch_utils.h"
#include "runtime/memory/counting_allocator.h"
#include "types/logical_type.h"

namespace starrocks {

// Fixed-size types that have a total order.
#define APPLY_FOR_DS_KLL_TYPES(M) \
    M(TYPE_BOOLEAN)               \
    M(TYPE_TINYINT)               \
    M(TYPE_SMALLINT)              \
    M(TYPE_INT)                   \
    M(TYPE_BIGINT)                \
    M(TYPE_LARGEINT)              \
    M(TYPE_FLOAT)                 \
    M(TYPE_DOUBLE)                \
    M(TYPE_DECIMALV2)             \
    M(TYPE_DECIMAL32)             \
    M(TYPE_DECIMAL64)             \
    M(TYPE_DECIMAL128)            \
    M(TYPE_DECIMAL256)            \
    M(TYPE_DATE)                  \
    M(TYPE_DATETIME)

template <LogicalType LT>
inline constexpr bool ds_kll_supports = false;
#define M(TYPE) \
    template <> \
    inline constexpr bool ds_kll_supports<TYPE> = true;
APPLY_FOR_DS_KLL_TYPES(M)
#undef M

// A KLL quantiles sketch of the values of one SQL type, behind an interface that does not name the type. This is
// what the functions that only see a serialized sketch work with: ds_kll_combine and ds_kll_estimate.
//
// KLL answers "which value sits at rank r" from a small sample of the input. The error is on the rank, not on
// the value: the value returned for rank r is a real input value whose true rank is within the sketch's rank
// error of r, for any input and after any number of merges.
class DsKllSketch {
public:
    // k is the size of the sketch's largest buffer. Up to k values, the sketch keeps them all and its answers are
    // exact. Past k, it samples: when a buffer is full, it keeps every other value and moves it one level up,
    // where each value stands for twice as many rows. Each level holds 2/3 as many values as the one above it, so
    // the sketch keeps about k + 2k/3 + 4k/9 + ... = 3k values, however many rows it sees.
    //
    // A bigger k means fewer rows per kept value: a smaller error and a bigger sketch. The rank error of one
    // quantile at 99% confidence is about 2.296 / k^0.9723. The default, 400, gives about 0.68%.
    static constexpr uint16_t DEFAULT_K = 400;
    static constexpr uint16_t MIN_K = datasketches::kll_sketch<double>::MIN_K;
    static constexpr uint16_t MAX_K = datasketches::kll_sketch<double>::MAX_K;

    static Status check_k(int64_t k);

    // TODO(xy)
    // More buckets than this cannot be told apart: the sketch keeps a few thousand values at most.
    static constexpr int32_t MAX_HISTOGRAM_BUCKETS = 65536;

    virtual ~DsKllSketch() = default;

    static StatusOr<std::unique_ptr<DsKllSketch>> deserialize(const Slice& src, int64_t* memory_usage);

    const DsItemType& item_type() const { return _item_type; }

    // Number of values the sketch has seen.
    virtual uint64_t count() const = 0;

    // Merges a serialized sketch of the same value type into this one.
    virtual Status merge(const Slice& src) = 0;

    // Appends the serialized sketch, header included, to |dst|.
    virtual void serialize(std::string* dst) const = 0;

    // Describes |num_buckets| equal-height buckets over the values seen, as JSON:
    //
    //   {"count":N,"min":"v","max":"v","buckets":[["lower","upper",rank],...]}
    //
    // The first bucket starts at the minimum, the last one ends at the maximum, and every other bucket starts at
    // the upper bound of the one before it, which it does not contain. |rank| is the fraction of all the values
    // that are less than or equal to the bucket's upper bound. Fewer buckets come back when the values repeat so
    // much that several of the requested bounds fall on the same value.
    virtual Status histogram_json(int32_t num_buckets, std::string* json) const = 0;

protected:
    explicit DsKllSketch(const DsItemType& item_type) : _item_type(item_type) {}

    DsItemType _item_type;
};

template <LogicalType LT>
class DsKllSketchImpl final : public DsKllSketch {
public:
    using CppType = RunTimeCppType<LT>;
    using Alloc = STLCountingAllocator<CppType>;
    using Sketch = datasketches::kll_sketch<CppType, std::less<CppType>, Alloc>;

    // |k| must pass check_k(); the library throws otherwise.
    DsKllSketchImpl(const DsItemType& item_type, uint16_t k, int64_t* memory_usage)
            : DsKllSketch(item_type),
              _memory_usage(memory_usage),
              _sketch(k, std::less<CppType>(), Alloc(memory_usage)) {
        DCHECK(check_k(k).ok());
    }

    // Fails unless |src| is a sketch of exactly this value type.
    static StatusOr<std::unique_ptr<DsKllSketchImpl>> deserialize(const Slice& src, int64_t* memory_usage);

    // NaN has no rank; the sketch ignores it.
    void update(const CppType& value) { _sketch.update(value); }

    // A sketch built with another k merges too. The result keeps this sketch's k, and its error is that of the
    // smaller of the two.
    void merge(DsKllSketchImpl&& other) { _sketch.merge(std::move(other._sketch)); }

    uint16_t k() const { return _sketch.get_k(); }
    uint64_t count() const override { return _sketch.get_n(); }
    Status merge(const Slice& src) override;
    void serialize(std::string* dst) const override;
    Status histogram_json(int32_t num_buckets, std::string* json) const override;

private:
    DsKllSketchImpl(const DsItemType& item_type, int64_t* memory_usage, Sketch&& sketch)
            : DsKllSketch(item_type), _memory_usage(memory_usage), _sketch(std::move(sketch)) {}

    int64_t* _memory_usage;
    Sketch _sketch;
};

#define M(TYPE) extern template class DsKllSketchImpl<TYPE>;
APPLY_FOR_DS_KLL_TYPES(M)
#undef M

} // namespace starrocks

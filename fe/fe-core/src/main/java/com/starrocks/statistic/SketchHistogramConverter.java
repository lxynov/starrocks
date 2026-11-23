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

package com.starrocks.statistic;

import com.google.common.collect.Lists;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.math.BigInteger;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Turns what the sketch functions return into the histogram the optimizer reads: the most common values with
 * their counts, and buckets of [lower, upper, cumulative count, upper_repeats(, distinct count)].
 *
 * <p>Everything here works on the rows the sketches saw. A count becomes a table-level count only when it is
 * written out, by {@link #scale}.
 */
final class SketchHistogramConverter {
    private SketchHistogramConverter() {
    }

    /** One value a frequent items sketch tracks; its true count lies between the two bounds. */
    record FrequentItem(String value, long estimate, long lowerBound, long upperBound) {
    }

    /** The result of ds_frequent_items: the tracked values, most frequent first. */
    record FrequentItems(long total, long maxError, List<FrequentItem> items) {
        /** With no error, the sketch tracks every distinct value it saw and every count is exact. */
        boolean isExact() {
            return maxError == 0;
        }

        static FrequentItems parse(String json) {
            JsonObject object = JsonParser.parseString(json).getAsJsonObject();
            List<FrequentItem> items = Lists.newArrayList();
            for (JsonElement element : object.getAsJsonArray("items")) {
                JsonArray item = element.getAsJsonArray();
                items.add(new FrequentItem(item.get(0).getAsString(), item.get(1).getAsLong(),
                        item.get(2).getAsLong(), item.get(3).getAsLong()));
            }
            return new FrequentItems(object.get("total").getAsLong(), object.get("max_error").getAsLong(), items);
        }
    }

    /** One bucket as ds_kll_histogram describes it: |rank| is the share of the rows at or below |upper|. */
    record KllBucket(String lower, String upper, double rank) {
    }

    /** The result of ds_kll_histogram. */
    record KllHistogram(long count, List<KllBucket> buckets) {
        static KllHistogram parse(String json) {
            JsonObject object = JsonParser.parseString(json).getAsJsonObject();
            List<KllBucket> buckets = Lists.newArrayList();
            for (JsonElement element : object.getAsJsonArray("buckets")) {
                JsonArray bucket = element.getAsJsonArray();
                buckets.add(new KllBucket(bucket.get(0).getAsString(), bucket.get(1).getAsString(),
                        bucket.get(2).getAsDouble()));
            }
            return new KllHistogram(object.get("count").getAsLong(), buckets);
        }
    }

    /**
     * One bucket of the stored histogram. |count| is cumulative. |distinctCount| is negative when the distinct
     * values of the bucket were not counted.
     */
    record Bucket(String lower, String upper, long count, long upperRepeats, long distinctCount) {
        Bucket withDistinctCount(long newUpperRepeats, long newDistinctCount) {
            return new Bucket(lower, upper, count, newUpperRepeats, newDistinctCount);
        }
    }

    /** A count over the sampled rows, as a count over the table. Truncates, as sort mode does. */
    static long scale(long count, double sampleRatio) {
        return sampleRatio > 0.0 && sampleRatio < 1.0 ? (long) (count / sampleRatio) : count;
    }

    /**
     * Picks the most common values out of the values a frequent items sketch tracks.
     *
     * <p>They are the first |mcvSize| values by estimate. On a column with more distinct values than that, a
     * value must also be held by at least twice as many rows as an evenly spread column would give every value,
     * so that an evenly spread column gets no most common values at all and the optimizer falls back to
     * 1 / NDV. The test is on the lower bound, the count the sketch is sure of. When the sketch is exact that is
     * simply the count. When it is not, its estimates are inflated by an error that is the same for every value
     * it tracks, and judging by them would let through whichever rare values happened to be in the sketch at
     * the end.
     *
     * @param ndv the number of distinct values of the column, or NaN when it is not known
     */
    static List<FrequentItem> selectMostCommonValues(FrequentItems items, long mcvSize, double ndv) {
        if (items == null || items.items().isEmpty() || mcvSize <= 0) {
            return List.of();
        }
        List<FrequentItem> candidates = items.items();
        // The sketch saw at least as many distinct values as it tracks, and exactly that many when it is exact.
        double distinct = Double.isNaN(ndv) ? candidates.size() : Math.max(ndv, candidates.size());
        boolean moreDistinctThanMcv = distinct > mcvSize || !items.isExact();

        List<FrequentItem> selected = Lists.newArrayList();
        // No more distinct values than rows can have been seen.
        double uniformShare = items.total() / Math.max(1.0, Math.min(distinct, items.total()));
        for (FrequentItem candidate : candidates) {
            if (selected.size() >= mcvSize) {
                break;
            }
            if (moreDistinctThanMcv && candidate.lowerBound() < 2 * uniformShare) {
                continue;
            }
            selected.add(candidate);
        }
        return selected;
    }

    /**
     * The count to store for a most common value when all that is known is the sketch's bounds: the middle,
     * which halves the worst case. Exact when the sketch is.
     */
    static long midpoint(FrequentItem item) {
        return item.lowerBound() + (item.upperBound() - item.lowerBound()) / 2;
    }

    /**
     * Builds the buckets of a column from the KLL histogram of its rows that hold no most common value.
     *
     * <p>The cumulative count of a bucket is its rank times the rows the sketch saw. KLL has no per-value
     * counts, so upper_repeats is the per-value estimate the optimizer makes for a value inside the bucket: the
     * bound gets the same estimate as its neighbours. That is the bucket's rows over the number of values the
     * bucket can hold for an integer column, and over NDV / bucket count otherwise - the same divisors the
     * optimizer uses.
     *
     * <p>The sketch describes a bucket as starting at the upper bound of the one before it, which it does not
     * contain. The stored histogram has no way to say that: its buckets include both bounds, so a value on a
     * shared bound would be estimated from whichever of the two buckets is looked at first. For an integer
     * column the bucket is therefore stored as starting one above the bound before it, which is the same set
     * of values with nothing shared. Other types have no next value to move to and keep the shared bound.
     *
     * @param integral whether the column has an integer type, so that its values can be counted from the bounds
     * @param ndv      the number of distinct values of the column, or NaN when it is not known
     */
    static List<Bucket> buildBuckets(KllHistogram histogram, double sampleRatio, boolean integral, double ndv) {
        List<Bucket> buckets = Lists.newArrayList();
        if (histogram == null || histogram.count() <= 0 || histogram.buckets().isEmpty()) {
            return buckets;
        }
        int numBuckets = histogram.buckets().size();
        double valuesPerBucket = Double.isNaN(ndv) ? 1.0 : Math.max(1.0, ndv / numBuckets);
        long totalRows = scale(histogram.count(), sampleRatio);
        long previousCount = 0;
        for (int i = 0; i < numBuckets; i++) {
            KllBucket kllBucket = histogram.buckets().get(i);
            long count = i == numBuckets - 1 ? totalRows :
                    Math.min(totalRows, scale(Math.round(kllBucket.rank() * histogram.count()), sampleRatio));
            // Ranks only grow, but rounding and scaling must not make a count shrink.
            count = Math.max(count, previousCount);
            long bucketRows = count - previousCount;

            String lower = kllBucket.lower();
            if (integral && i > 0) {
                lower = successor(lower);
            }
            double values = valuesPerBucket;
            if (lower.equals(kllBucket.upper())) {
                // A bucket of a single value: every one of its rows holds the upper bound.
                values = 1;
            } else if (integral) {
                values = integersBetween(lower, kllBucket.upper());
            }
            buckets.add(new Bucket(lower, kllBucket.upper(), count, rowsPerValue(bucketRows, values), -1));
            previousCount = count;
        }
        return buckets;
    }

    /**
     * Adds the distinct value count of each bucket, as histogram_hll_ndv returns it in the fifth field. Once a
     * bucket has one, the optimizer divides the bucket's rows by it, so upper_repeats does too.
     *
     * <p>A HyperLogLog built over a sample counts the distinct values of the sample. When |targetTotal| is
     * positive the counts are scaled up so that they add up to it: the distinct values the column is known to
     * have outside its most common values. They are never scaled down - a sample cannot hold more distinct
     * values than the table, so a total above the target means the target is stale.
     */
    static List<Bucket> applyDistinctCounts(List<Bucket> buckets, String hllNdvJson, double targetTotal) {
        JsonArray array = JsonParser.parseString(hllNdvJson).getAsJsonArray();
        if (array.size() != buckets.size()) {
            throw new IllegalStateException("histogram_hll_ndv returned " + array.size() + " buckets for " +
                    buckets.size());
        }
        long[] distinctCounts = new long[buckets.size()];
        long total = 0;
        for (int i = 0; i < distinctCounts.length; i++) {
            distinctCounts[i] = Math.max(1, array.get(i).getAsJsonArray().get(4).getAsLong());
            total += distinctCounts[i];
        }
        double factor = targetTotal > total ? targetTotal / total : 1.0;

        List<Bucket> result = Lists.newArrayList();
        long previousCount = 0;
        for (int i = 0; i < distinctCounts.length; i++) {
            Bucket bucket = buckets.get(i);
            long bucketRows = bucket.count() - previousCount;
            long distinctCount = Math.max(1, Math.round(distinctCounts[i] * factor));
            result.add(bucket.withDistinctCount(rowsPerValue(bucketRows, distinctCount), distinctCount));
            previousCount = bucket.count();
        }
        return result;
    }

    /** The buckets as the JSON the statistics table stores and histogram_hll_ndv takes: every field a string. */
    static String toBucketsJson(List<Bucket> buckets) {
        JsonArray array = new JsonArray();
        for (Bucket bucket : buckets) {
            JsonArray fields = new JsonArray();
            fields.add(bucket.lower());
            fields.add(bucket.upper());
            fields.add(String.valueOf(bucket.count()));
            fields.add(String.valueOf(bucket.upperRepeats()));
            if (bucket.distinctCount() >= 0) {
                fields.add(String.valueOf(bucket.distinctCount()));
            }
            array.add(fields);
        }
        return array.toString();
    }

    /**
     * The single placeholder bucket of a column that gets no real buckets, holding the rows that have none of the
     * most common values, so that the histogram still accounts for every row. Same form as sort mode's.
     */
    static String toPlaceholderBucketJson(long rows) {
        return toBucketsJson(List.of(new Bucket("Infinity", "Infinity", Math.max(0, rows), 0, -1)));
    }

    /** The most common values, in the given order, as the value -> count map the statistics table stores. */
    static Map<String, String> toMostCommonValues(Map<String, Long> sampledCounts, double sampleRatio) {
        Map<String, String> mostCommonValues = new LinkedHashMap<>();
        sampledCounts.forEach((value, count) ->
                mostCommonValues.put(value, String.valueOf(Math.max(1, scale(count, sampleRatio)))));
        return mostCommonValues;
    }

    /** How many rows each of |values| values of a bucket holds, if the bucket's rows are spread evenly. */
    private static long rowsPerValue(long bucketRows, double values) {
        if (bucketRows <= 0) {
            return 0;
        }
        return Math.max(1, Math.min(bucketRows, Math.round(bucketRows / Math.max(1.0, values))));
    }

    /** The integer after |value|, or |value| itself if it is not an integer. LARGEINT does not fit a long. */
    private static String successor(String value) {
        try {
            return new BigInteger(value).add(BigInteger.ONE).toString();
        } catch (NumberFormatException e) {
            return value;
        }
    }

    /** How many integers lie between |lower| and |upper|, both included. */
    private static double integersBetween(String lower, String upper) {
        try {
            return new BigInteger(upper).subtract(new BigInteger(lower)).add(BigInteger.ONE).doubleValue();
        } catch (NumberFormatException e) {
            return 1.0;
        }
    }
}

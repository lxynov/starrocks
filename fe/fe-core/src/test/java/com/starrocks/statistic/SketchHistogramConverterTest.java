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
import com.starrocks.sql.optimizer.statistics.HistogramUtils;
import com.starrocks.statistic.SketchHistogramConverter.Bucket;
import com.starrocks.statistic.SketchHistogramConverter.FrequentItem;
import com.starrocks.statistic.SketchHistogramConverter.FrequentItems;
import com.starrocks.statistic.SketchHistogramConverter.KllHistogram;
import com.starrocks.type.IntegerType;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

public class SketchHistogramConverterTest {
    private static final double UNKNOWN_NDV = Double.NaN;

    private static List<String> values(List<FrequentItem> items) {
        return items.stream().map(FrequentItem::value).collect(Collectors.toList());
    }

    /** |count| values named v0, v1, ... with counts falling from |first| by |step|, exact. */
    private static FrequentItems exactItems(int count, long first, long step) {
        List<FrequentItem> items = Lists.newArrayList();
        long total = 0;
        for (int i = 0; i < count; i++) {
            long rows = first - i * step;
            items.add(new FrequentItem("v" + i, rows, rows, rows));
            total += rows;
        }
        return new FrequentItems(total, 0, items);
    }

    @Test
    public void testParseFrequentItems() {
        FrequentItems items = FrequentItems.parse(
                "{\"total\":1000,\"max_error\":3,\"items\":[[\"a\\\"b\",503,500,503],[\"2024-01-01\",13,10,13]]}");
        Assertions.assertEquals(1000, items.total());
        Assertions.assertEquals(3, items.maxError());
        Assertions.assertFalse(items.isExact());
        Assertions.assertEquals(List.of(new FrequentItem("a\"b", 503, 500, 503),
                new FrequentItem("2024-01-01", 13, 10, 13)), items.items());

        FrequentItems empty = FrequentItems.parse("{\"total\":0,\"max_error\":0,\"items\":[]}");
        Assertions.assertTrue(empty.isExact());
        Assertions.assertTrue(empty.items().isEmpty());
    }

    @Test
    public void testParseKllHistogram() {
        KllHistogram histogram = KllHistogram.parse(
                "{\"count\":200,\"min\":\"1\",\"max\":\"100\",\"buckets\":[[\"1\",\"50\",0.5],[\"50\",\"100\",1]]}");
        Assertions.assertEquals(200, histogram.count());
        Assertions.assertEquals(2, histogram.buckets().size());
        Assertions.assertEquals("50", histogram.buckets().get(0).upper());
        Assertions.assertEquals(0.5, histogram.buckets().get(0).rank());
        Assertions.assertEquals(1.0, histogram.buckets().get(1).rank());

        KllHistogram empty = KllHistogram.parse("{\"count\":0,\"min\":null,\"max\":null,\"buckets\":[]}");
        Assertions.assertEquals(0, empty.count());
        Assertions.assertTrue(empty.buckets().isEmpty());
    }

    @Test
    public void testFewDistinctValuesAreAllMostCommon() {
        // No more distinct values than the list holds: every value belongs on it, however rare.
        FrequentItems items = new FrequentItems(1003, 0, List.of(
                new FrequentItem("a", 1000, 1000, 1000),
                new FrequentItem("b", 2, 2, 2),
                new FrequentItem("c", 1, 1, 1)));
        Assertions.assertEquals(List.of("a", "b", "c"),
                values(SketchHistogramConverter.selectMostCommonValues(items, 100, UNKNOWN_NDV)));
        Assertions.assertEquals(List.of("a", "b", "c"),
                values(SketchHistogramConverter.selectMostCommonValues(items, 100, 3)));

        Assertions.assertTrue(SketchHistogramConverter.selectMostCommonValues(null, 100, 3).isEmpty());
        Assertions.assertTrue(SketchHistogramConverter.selectMostCommonValues(items, 0, 3).isEmpty());
    }

    @Test
    public void testEvenlySpreadColumnGetsNoMostCommonValues() {
        // 200 values of 50 rows each. None is held by twice the uniform share, so none is common.
        FrequentItems items = exactItems(200, 50, 0);
        Assertions.assertTrue(SketchHistogramConverter.selectMostCommonValues(items, 100, UNKNOWN_NDV).isEmpty());
        Assertions.assertTrue(SketchHistogramConverter.selectMostCommonValues(items, 100, 200).isEmpty());
    }

    @Test
    public void testSkewedColumnKeepsValuesAboveTwiceTheUniformShare() {
        // 300 values: five with about 10000 rows each, the rest with 10.
        List<FrequentItem> list = Lists.newArrayList();
        long total = 0;
        for (int i = 0; i < 300; i++) {
            long rows = i < 5 ? 10000 - i : 10;
            list.add(new FrequentItem("v" + i, rows, rows, rows));
            total += rows;
        }
        FrequentItems items = new FrequentItems(total, 0, list);
        // The uniform share is total / 300, about 176 rows: only the five heavy values clear twice that.
        Assertions.assertEquals(List.of("v0", "v1", "v2", "v3", "v4"),
                values(SketchHistogramConverter.selectMostCommonValues(items, 100, UNKNOWN_NDV)));
        // And no more than the list holds.
        Assertions.assertEquals(List.of("v0", "v1"),
                values(SketchHistogramConverter.selectMostCommonValues(items, 2, UNKNOWN_NDV)));
    }

    @Test
    public void testTableNdvDecidesWhenTheSampleSawFewValues() {
        // The sample saw 50 distinct values, but the column is known to have 5000. The list is not big enough
        // for all of them, so the uniform share applies, and it is the rows over those 5000 values.
        List<FrequentItem> list = Lists.newArrayList();
        long total = 0;
        for (int i = 0; i < 50; i++) {
            long rows = i < 10 ? 9000 : 25 + i;
            list.add(new FrequentItem("v" + i, rows, rows, rows));
            total += rows;
        }
        list.sort((a, b) -> Long.compare(b.estimate(), a.estimate()));
        FrequentItems items = new FrequentItems(total, 0, list);
        double uniformShare = total / 5000.0;
        List<FrequentItem> selected = SketchHistogramConverter.selectMostCommonValues(items, 100, 5000);
        Assertions.assertFalse(selected.isEmpty());
        for (FrequentItem item : list) {
            Assertions.assertEquals(item.lowerBound() >= 2 * uniformShare, selected.contains(item), item.value());
        }
    }

    @Test
    public void testInexactSketchIsJudgedByWhatItIsSureOf() {
        // Once the sketch has had to drop counters, every estimate carries the same error. Two heavy values are
        // sure to be common; the other tracked values merely happened to be in the sketch at the end, and their
        // estimates are almost all error.
        List<FrequentItem> list = Lists.newArrayList();
        list.add(new FrequentItem("heavy1", 50600, 50000, 50600));
        list.add(new FrequentItem("heavy2", 20600, 20000, 20600));
        for (int i = 0; i < 1500; i++) {
            list.add(new FrequentItem("tail" + i, 603, 3, 603));
        }
        FrequentItems items = new FrequentItems(1000000, 600, list);
        // 100000 distinct values: 10 rows each if evenly spread.
        Assertions.assertEquals(List.of("heavy1", "heavy2"),
                values(SketchHistogramConverter.selectMostCommonValues(items, 100, 100000)));
        // Without a distinct value count the tracked values stand in for one, which only raises the bar.
        Assertions.assertEquals(List.of("heavy1", "heavy2"),
                values(SketchHistogramConverter.selectMostCommonValues(items, 100, UNKNOWN_NDV)));
    }

    @Test
    public void testMidpointAndScale() {
        Assertions.assertEquals(10, SketchHistogramConverter.midpoint(new FrequentItem("a", 10, 10, 10)));
        Assertions.assertEquals(303, SketchHistogramConverter.midpoint(new FrequentItem("a", 603, 3, 603)));

        Assertions.assertEquals(1000, SketchHistogramConverter.scale(100, 0.1));
        Assertions.assertEquals(100, SketchHistogramConverter.scale(100, 1.0));
        // Truncated, as sort mode does: 7 / 0.3 = 23.33.
        Assertions.assertEquals(23, SketchHistogramConverter.scale(7, 0.3));
    }

    @Test
    public void testBuildBucketsOfIntegerColumn() {
        KllHistogram histogram = KllHistogram.parse("{\"count\":1000,\"min\":\"1\",\"max\":\"1000\",\"buckets\":" +
                "[[\"1\",\"250\",0.25],[\"250\",\"500\",0.5],[\"500\",\"750\",0.75],[\"750\",\"1000\",1]]}");
        List<Bucket> buckets = SketchHistogramConverter.buildBuckets(histogram, 0.1, true, 1000);
        // Counts are cumulative and scaled to the table; each bucket's 2500 rows spread over the 250 integers it
        // holds. A bucket starts one above the bound before it, so that no value belongs to two buckets.
        Assertions.assertEquals(List.of(
                new Bucket("1", "250", 2500, 10, -1),
                new Bucket("251", "500", 5000, 10, -1),
                new Bucket("501", "750", 7500, 10, -1),
                new Bucket("751", "1000", 10000, 10, -1)), buckets);
        Assertions.assertEquals("[[\"1\",\"250\",\"2500\",\"10\"],[\"251\",\"500\",\"5000\",\"10\"]," +
                        "[\"501\",\"750\",\"7500\",\"10\"],[\"751\",\"1000\",\"10000\",\"10\"]]",
                SketchHistogramConverter.toBucketsJson(buckets));
    }

    @Test
    public void testBuildBucketsOfOtherColumnUsesNdv() {
        KllHistogram histogram = KllHistogram.parse("{\"count\":1000,\"min\":\"0.5\",\"max\":\"9.5\",\"buckets\":" +
                "[[\"0.5\",\"4.5\",0.4],[\"4.5\",\"9.5\",1]]}");
        // 100 distinct values over 2 buckets: 50 values a bucket. Not an integer column, so the buckets keep
        // the bound they share.
        List<Bucket> buckets = SketchHistogramConverter.buildBuckets(histogram, 1.0, false, 100);
        Assertions.assertEquals(List.of(
                new Bucket("0.5", "4.5", 400, 8, -1),
                new Bucket("4.5", "9.5", 1000, 12, -1)), buckets);

        // Without a distinct value count nothing says the rows of a bucket are spread at all.
        buckets = SketchHistogramConverter.buildBuckets(histogram, 1.0, false, UNKNOWN_NDV);
        Assertions.assertEquals(400, buckets.get(0).upperRepeats());
        Assertions.assertEquals(600, buckets.get(1).upperRepeats());
    }

    @Test
    public void testBuildBucketsEdgeCases() {
        Assertions.assertTrue(SketchHistogramConverter.buildBuckets(null, 1.0, true, 10).isEmpty());
        Assertions.assertTrue(SketchHistogramConverter.buildBuckets(
                KllHistogram.parse("{\"count\":0,\"min\":null,\"max\":null,\"buckets\":[]}"), 1.0, true, 10).isEmpty());

        // A bucket of a single value: every one of its rows holds the upper bound.
        KllHistogram histogram = KllHistogram.parse("{\"count\":100,\"min\":\"5\",\"max\":\"9\",\"buckets\":" +
                "[[\"5\",\"5\",0.9],[\"5\",\"9\",1]]}");
        List<Bucket> buckets = SketchHistogramConverter.buildBuckets(histogram, 1.0, true, 2);
        Assertions.assertEquals(List.of(
                new Bucket("5", "5", 90, 90, -1),
                new Bucket("6", "9", 100, 3, -1)), buckets);

        // Two neighbouring values: the second bucket is the single value 6.
        histogram = KllHistogram.parse("{\"count\":100,\"min\":\"5\",\"max\":\"6\",\"buckets\":" +
                "[[\"5\",\"5\",0.9],[\"5\",\"6\",1]]}");
        Assertions.assertEquals(List.of(
                new Bucket("5", "5", 90, 90, -1),
                new Bucket("6", "6", 100, 10, -1)), SketchHistogramConverter.buildBuckets(histogram, 1.0, true, 2));

        // Whatever the type: a date is not an integer, but a bucket from a day to the same day holds one value.
        histogram = KllHistogram.parse("{\"count\":100,\"min\":\"2024-01-01\",\"max\":\"2024-01-09\",\"buckets\":" +
                "[[\"2024-01-01\",\"2024-01-01\",0.9],[\"2024-01-01\",\"2024-01-09\",1]]}");
        buckets = SketchHistogramConverter.buildBuckets(histogram, 1.0, false, 1000);
        Assertions.assertEquals(List.of(
                new Bucket("2024-01-01", "2024-01-01", 90, 90, -1),
                new Bucket("2024-01-01", "2024-01-09", 100, 1, -1)), buckets);

        // LARGEINT bounds do not fit a long.
        histogram = KllHistogram.parse("{\"count\":100,\"min\":\"0\",\"max\":\"100000000000000000000000000000\"," +
                "\"buckets\":[[\"0\",\"100000000000000000000000000000\",1]]}");
        buckets = SketchHistogramConverter.buildBuckets(histogram, 1.0, true, 100);
        Assertions.assertEquals(1, buckets.get(0).upperRepeats());

        // The stored buckets parse as the optimizer's.
        List<com.starrocks.sql.optimizer.statistics.Bucket> parsed;
        try {
            parsed = HistogramUtils.convertBuckets("{\"buckets\":" +
                    SketchHistogramConverter.toBucketsJson(buckets) + ",\"mcv\":null}", IntegerType.LARGEINT);
        } catch (Exception e) {
            throw new AssertionError(e);
        }
        Assertions.assertEquals(1, parsed.size());
        Assertions.assertEquals(100, parsed.get(0).getCount());
    }

    @Test
    public void testApplyDistinctCounts() {
        List<Bucket> buckets = List.of(new Bucket("1", "250", 2500, 10, -1), new Bucket("250", "500", 10000, 30, -1));
        String hllNdv = "[[\"1\",\"250\",\"2500\",\"10\",\"50\"],[\"250\",\"500\",\"10000\",\"30\",\"150\"]]";

        // As counted: upper_repeats becomes rows / distinct values.
        List<Bucket> counted = SketchHistogramConverter.applyDistinctCounts(buckets, hllNdv, -1);
        Assertions.assertEquals(List.of(
                new Bucket("1", "250", 2500, 50, 50),
                new Bucket("250", "500", 10000, 50, 150)), counted);
        Assertions.assertEquals("[[\"1\",\"250\",\"2500\",\"50\",\"50\"],[\"250\",\"500\",\"10000\",\"50\",\"150\"]]",
                SketchHistogramConverter.toBucketsJson(counted));

        // Scaled so that the buckets add up to the 1000 distinct values the column is known to have.
        List<Bucket> scaled = SketchHistogramConverter.applyDistinctCounts(buckets, hllNdv, 1000);
        Assertions.assertEquals(List.of(
                new Bucket("1", "250", 2500, 10, 250),
                new Bucket("250", "500", 10000, 10, 750)), scaled);

        // Never scaled down: a sample cannot hold more distinct values than the table.
        Assertions.assertEquals(counted, SketchHistogramConverter.applyDistinctCounts(buckets, hllNdv, 100));

        Assertions.assertThrows(IllegalStateException.class, () -> SketchHistogramConverter.applyDistinctCounts(
                buckets, "[[\"1\",\"250\",\"2500\",\"10\",\"50\"]]", -1));
    }

    @Test
    public void testPlaceholderBucketAndMostCommonValues() {
        Assertions.assertEquals("[[\"Infinity\",\"Infinity\",\"1234\",\"0\"]]",
                SketchHistogramConverter.toPlaceholderBucketJson(1234));
        Assertions.assertEquals("[[\"Infinity\",\"Infinity\",\"0\",\"0\"]]",
                SketchHistogramConverter.toPlaceholderBucketJson(-5));

        Map<String, Long> sampled = new LinkedHashMap<>();
        sampled.put("b", 500L);
        sampled.put("a", 30L);
        sampled.put("c", 0L);
        Map<String, String> stored = SketchHistogramConverter.toMostCommonValues(sampled, 0.1);
        // In the order given, scaled to the table, and never below one row.
        Assertions.assertEquals(List.of("b", "a", "c"), Lists.newArrayList(stored.keySet()));
        Assertions.assertEquals(List.of("5000", "300", "1"), Lists.newArrayList(stored.values()));
    }
}

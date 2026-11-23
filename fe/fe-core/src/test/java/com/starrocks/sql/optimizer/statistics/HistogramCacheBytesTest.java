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

package com.starrocks.sql.optimizer.statistics;

import com.github.benmanes.caffeine.cache.LoadingCache;
import com.starrocks.common.Config;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;

public class HistogramCacheBytesTest {
    private static Histogram histogram(int numBuckets, int numMostCommonValues, int valueLength) {
        List<Bucket> buckets = new ArrayList<>();
        for (int i = 0; i < numBuckets; i++) {
            buckets.add(new Bucket(i, i + 1, 100L * (i + 1), 1L));
        }
        Map<String, Long> mostCommonValues = new HashMap<>();
        for (int i = 0; i < numMostCommonValues; i++) {
            mostCommonValues.put(String.format("%0" + valueLength + "d", i), 1000L + i);
        }
        return new Histogram(buckets, mostCommonValues);
    }

    @Test
    public void testEstimateBytesGrowsWithBucketsAndMostCommonValues() {
        long empty = new Histogram(null, null).estimateBytes();
        Assertions.assertTrue(empty > 0);
        Assertions.assertEquals(empty + 64 * 112, histogram(64, 0, 0).estimateBytes());
        Assertions.assertEquals(empty + 100 * (96 + 2 * 10), histogram(0, 100, 10).estimateBytes());
        // Long values are what makes a histogram big: 100 values of 1000 characters are 200 KB on their own.
        Assertions.assertTrue(histogram(0, 100, 1000).estimateBytes() > 200_000);
    }

    @Test
    public void testWeightOfAnEntry() {
        // A column without a histogram still costs its key and the cache's bookkeeping.
        Assertions.assertEquals(CachedStatisticStorage.HISTOGRAM_CACHE_ENTRY_OVERHEAD_BYTES,
                CachedStatisticStorage.histogramCacheWeight(Optional.empty()));
        Histogram histogram = histogram(64, 100, 10);
        Assertions.assertEquals(CachedStatisticStorage.HISTOGRAM_CACHE_ENTRY_OVERHEAD_BYTES + histogram.estimateBytes(),
                CachedStatisticStorage.histogramCacheWeight(Optional.of(histogram)));
    }

    @Test
    public void testHistogramCachesAreBoundedByBytes() {
        long configured = Config.histogram_cache_max_bytes;
        Config.histogram_cache_max_bytes = 1024 * 1024;
        try {
            CachedStatisticStorage storage = new CachedStatisticStorage();
            LoadingCache<ColumnStatsCacheKey, Optional<Histogram>> cache = storage.histogramCache.synchronous();
            Histogram histogram = histogram(64, 100, 100);
            int weight = CachedStatisticStorage.histogramCacheWeight(Optional.of(histogram));
            // 100 of these are several megabytes.
            Assertions.assertTrue(100L * weight > 3 * 1024 * 1024);

            for (int i = 0; i < 100; i++) {
                cache.put(new ColumnStatsCacheKey(1L, "c" + i), Optional.of(histogram));
            }
            cache.cleanUp();
            long bytes = CachedStatisticStorage.weightedBytes(cache).orElseThrow();
            Assertions.assertTrue(bytes > 0 && bytes <= 1024 * 1024, "bytes: " + bytes);
            Assertions.assertEquals(bytes / weight, cache.estimatedSize());
            Assertions.assertTrue(cache.estimatedSize() < 100);

            // The cache of external table histograms has the same limit of its own.
            Assertions.assertEquals(Optional.of(0L),
                    CachedStatisticStorage.weightedBytes(storage.connectorHistogramCache.synchronous()));
            // The caches of basic statistics are bounded by their entry count and have no size in bytes.
            Assertions.assertTrue(CachedStatisticStorage.weightedBytes(storage.columnStatistics.synchronous()).isEmpty());

            // Raising the limit takes effect on the live cache.
            Config.histogram_cache_max_bytes = 100L * 1024 * 1024;
            storage.refreshHistogramCacheMaximum();
            for (int i = 0; i < 100; i++) {
                cache.put(new ColumnStatsCacheKey(1L, "c" + i), Optional.of(histogram));
            }
            cache.cleanUp();
            Assertions.assertEquals(100, cache.estimatedSize());
            Assertions.assertEquals(100L * weight, CachedStatisticStorage.weightedBytes(cache).orElseThrow());

            // And so does lowering it: what no longer fits is evicted.
            Config.histogram_cache_max_bytes = 10L * weight;
            storage.refreshHistogramCacheMaximum();
            cache.cleanUp();
            Assertions.assertEquals(10, cache.estimatedSize());
        } finally {
            Config.histogram_cache_max_bytes = configured;
        }
    }
}

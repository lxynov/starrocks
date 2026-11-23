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

import com.google.common.annotations.VisibleForTesting;
import com.starrocks.common.Config;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.Map;

/**
 * The analyze properties one histogram collection runs under, parsed once per collect() call.
 *
 * <p>The first four are filled in together by AnalyzeStmtAnalyzer while analyzing the ANALYZE statement,
 * so they share a lifetime and are parsed as a unit. Only {@link NativeHistogramTraits} reads the
 * bucket-NDV mode; the external flavour carries it unused.
 *
 * <p>The collect mode is different: it is a property only when the statement names one. A job that does not
 * follows {@link Config#histogram_collect_mode} as it is when the job runs, so a scheduled job picks up a
 * change of the default.
 */
final class HistogramCollectParams {
    private static final Logger LOG = LogManager.getLogger(HistogramCollectParams.class);

    private final double sampleRatio;
    private final long bucketNum;
    private final long mcvSize;
    private final StatsConstants.HistogramCollectBucketNdvMode bucketNdvMode;
    private final StatsConstants.HistogramCollectMode collectMode;
    private final boolean fullScan;

    HistogramCollectParams(Map<String, String> properties) {
        this.fullScan = Boolean.parseBoolean(properties.get(StatsConstants.HISTOGRAM_FULL_SCAN));
        // A full scan reads every row whatever ratio the job carries.
        this.sampleRatio = fullScan ? 1.0 : Double.parseDouble(properties.get(StatsConstants.HISTOGRAM_SAMPLE_RATIO));
        this.bucketNum = Long.parseLong(properties.get(StatsConstants.HISTOGRAM_BUCKET_NUM));
        this.mcvSize = Long.parseLong(properties.get(StatsConstants.HISTOGRAM_MCV_SIZE));
        this.bucketNdvMode = parseBucketNdvMode(properties.get(StatsConstants.HISTOGRAM_COLLECT_BUCKET_NDV_MODE));
        this.collectMode = resolveCollectMode(properties);
    }

    double sampleRatio() {
        return sampleRatio;
    }

    long bucketNum() {
        return bucketNum;
    }

    long mcvSize() {
        return mcvSize;
    }

    StatsConstants.HistogramCollectBucketNdvMode bucketNdvMode() {
        return bucketNdvMode;
    }

    StatsConstants.HistogramCollectMode collectMode() {
        return collectMode;
    }

    /** Whether the statement asked for every row to be read: ANALYZE FULL TABLE ... UPDATE HISTOGRAM. */
    boolean fullScan() {
        return fullScan;
    }

    /**
     * The mode the statement asked for, or else the configured default. The analyzer rejects a property that
     * names no mode, so only the configuration can be unusable here; it then counts as sort mode, the way an
     * unusable bucket-NDV mode counts as none.
     */
    static StatsConstants.HistogramCollectMode resolveCollectMode(Map<String, String> properties) {
        StatsConstants.HistogramCollectMode mode = StatsConstants.HistogramCollectMode.resolve(properties);
        if (mode == null) {
            LOG.warn("Invalid histogram collect mode {}, using sort.", Config.histogram_collect_mode);
            return StatsConstants.HistogramCollectMode.SORT;
        }
        return mode;
    }

    /**
     * Resolved once per collection, so an unusable mode is reported once rather than once per
     * column, and never fails the analyze job.
     */
    @VisibleForTesting
    static StatsConstants.HistogramCollectBucketNdvMode parseBucketNdvMode(String mode) {
        if ("none".equalsIgnoreCase(mode)) {
            return StatsConstants.HistogramCollectBucketNdvMode.NONE;
        } else if ("sample".equalsIgnoreCase(mode)) {
            return StatsConstants.HistogramCollectBucketNdvMode.SAMPLE;
        } else if ("hll".equalsIgnoreCase(mode)) {
            return StatsConstants.HistogramCollectBucketNdvMode.HLL;
        } else {
            LOG.warn("Invalid histogram collect bucket ndv mode {}.", mode);
            return StatsConstants.HistogramCollectBucketNdvMode.NONE;
        }
    }
}

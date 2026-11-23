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
import com.google.common.base.Stopwatch;
import com.google.common.collect.Lists;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.starrocks.catalog.OlapTable;
import com.starrocks.common.Config;
import com.starrocks.common.DdlException;
import com.starrocks.qe.ConnectContext;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.optimizer.statistics.ColumnStatistic;
import com.starrocks.statistic.SketchHistogramConverter.Bucket;
import com.starrocks.statistic.SketchHistogramConverter.FrequentItem;
import com.starrocks.statistic.SketchHistogramConverter.FrequentItems;
import com.starrocks.statistic.SketchHistogramConverter.KllHistogram;
import com.starrocks.type.Type;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;

import static com.starrocks.statistic.HistogramStatisticsUtils.buildMcvJson;
import static com.starrocks.statistic.HistogramStatisticsUtils.formatSamplePercent;
import static com.starrocks.statistic.HistogramStatisticsUtils.formatSampleRatio;
import static com.starrocks.statistic.HistogramStatisticsUtils.quoteSqlString;

/**
 * Histogram collection for a native table from sketches: a frequent items sketch finds a column's most common
 * values, and a KLL sketch gives the bounds of its buckets.
 *
 * <p>A sketch does not need its input sorted and merges with other sketches of its kind. So, unlike the
 * {@code histogram()} aggregate the sort mode of {@link NativeHistogramTraits} relies on, one query can cover
 * many columns, and it runs as an ordinary multi-stage aggregation: every instance sketches its own rows and
 * the partial sketches merge.
 *
 * <p>The work is done up front, in {@link #beforeCollection}, in up to three passes over the same rows:
 * <ol>
 *   <li>Pass 1 builds a frequent items sketch per column and picks the most common values from it.</li>
 *   <li>Pass 2 covers the columns that get buckets. It builds the KLL sketch over the rows that hold none of
 *   the most common values, as sort mode does, and counts the rows that do hold one exactly. The most common
 *   values have to be known first: KLL's error is a share of all the rows it sees, so on a skewed column a
 *   sketch of every row would be far too coarse for what is left for the buckets.</li>
 *   <li>Pass 3 runs only when the distinct values of each bucket are wanted. It needs the bucket bounds of
 *   pass 2.</li>
 * </ol>
 * {@link #collectColumn} then only hands out what the passes produced.
 */
final class SketchHistogramTraits extends NativeHistogramTraits {
    private static final Logger LOG = LogManager.getLogger(SketchHistogramTraits.class);

    // ds_frequent_items is asked for every value the sketch tracks; its hash map has this many entries.
    @VisibleForTesting
    static final int FREQUENT_ITEMS_LIMIT = 2048;

    // Pass 3 holds one HyperLogLog of up to 16 KB per bucket, about 1 MB per column at 64 buckets, so it takes
    // fewer columns per query than the other passes.
    @VisibleForTesting
    static final int DISTINCT_COUNT_PASS_MAX_COLUMNS = 16;

    // A column is near-unique above this NDV / rows. No value of such a column is common enough to matter, so
    // it is not given a frequent items sketch.
    @VisibleForTesting
    static final double NEAR_UNIQUE_RATIO = 0.95;

    /** What is known about one column, and what the passes have found out about it so far. */
    @VisibleForTesting
    static final class SketchColumn {
        final String name;
        final String quotedName;
        final Type type;
        // Gets real buckets. A string column gets its most common values and a placeholder bucket only.
        final boolean bucketed;
        // From basic statistics; NaN when there are none.
        double ndv = Double.NaN;
        // Gets no frequent items sketch: no most common values are wanted, or the column is near-unique.
        boolean skipFrequentItems = false;

        // Pass 1.
        FrequentItems frequentItems;
        // The non-NULL rows of a column that pass 1 only counts.
        long nonNullRows = 0;
        List<FrequentItem> mcvCandidates = List.of();

        // Pass 2. The counts are over the sampled rows.
        Map<String, Long> mostCommonValues = new LinkedHashMap<>();
        List<Bucket> buckets = List.of();

        SketchColumn(String name, String quotedName, Type type) {
            this.name = name;
            this.quotedName = quotedName;
            this.type = type;
            this.bucketed = !StatisticsCollectJob.shouldSkipHistogramBuckets(type);
        }

        boolean isString() {
            return type.getPrimitiveType().isCharFamily();
        }

        boolean needsNdvFromSample() {
            return Double.isNaN(ndv);
        }
    }

    // One seed for every pass, so that all of them read the same sampled blocks.
    private final long sampleSeed = ThreadLocalRandom.current().nextLong(Long.MAX_VALUE);
    private final Map<String, ColumnHistogram> histograms = new HashMap<>();

    SketchHistogramTraits(StatisticsCollectJob job, HistogramCollectParams params) {
        super(job, params);
    }

    /**
     * Whether a column of |type| is collected from sketches. A column of any other type that reaches a histogram
     * job is collected the way sort mode does.
     */
    @VisibleForTesting
    static boolean isSketchable(Type type) {
        return type.isScalarType() && (type.getPrimitiveType().isCharFamily() || type.isBoolean() ||
                type.isFixedPointType() || type.isFloatingPointType() || type.isDecimalOfAnyVersion() ||
                type.isDate() || type.isDatetime());
    }

    @Override
    void beforeCollection(ConnectContext context, AnalyzeStatus analyzeStatus) throws Exception {
        List<SketchColumn> columns = Lists.newArrayList();
        List<String> columnNames = job.getColumnNames();
        List<Type> columnTypes = job.getColumnTypes();
        for (int i = 0; i < columnNames.size(); i++) {
            if (isSketchable(columnTypes.get(i))) {
                columns.add(new SketchColumn(columnNames.get(i), StatisticUtils.quoting(table, columnNames.get(i)),
                        columnTypes.get(i)));
            }
        }
        if (columns.isEmpty()) {
            return;
        }
        if (params.mcvSize() <= 0) {
            columns.forEach(column -> column.skipFrequentItems = true);
        }
        loadBasicStatistics(columns);

        // Every query is an ordinary aggregation that can be split into stages.
        context.getSessionVariable().setNewPlanerAggStage(0);

        Stopwatch watch = Stopwatch.createStarted();
        List<List<SketchColumn>> frequentBatches = batch(
                columns.stream().filter(c -> !c.skipFrequentItems || !c.bucketed).collect(Collectors.toList()),
                maxColumnsPerQuery());
        List<List<SketchColumn>> bucketBatches = batch(
                columns.stream().filter(c -> c.bucketed).collect(Collectors.toList()), maxColumnsPerQuery());
        boolean collectDistinctCounts = params.bucketNdvMode() != StatsConstants.HistogramCollectBucketNdvMode.NONE;
        // Pass 3 is planned for every bucketed column; a column that ends up without buckets drops out of it.
        int plannedQueries = frequentBatches.size() + bucketBatches.size() + (collectDistinctCounts ?
                batch(columns.stream().filter(c -> c.bucketed).collect(Collectors.toList()),
                        DISTINCT_COUNT_PASS_MAX_COLUMNS).size() : 0);
        int finishedQueries = 0;

        for (List<SketchColumn> batch : frequentBatches) {
            runFrequentPass(context, analyzeStatus, batch);
            reportProgress(analyzeStatus, ++finishedQueries, plannedQueries);
        }
        long frequentPassMs = watch.elapsed(TimeUnit.MILLISECONDS);

        for (List<SketchColumn> batch : bucketBatches) {
            runBucketPass(context, analyzeStatus, batch);
            reportProgress(analyzeStatus, ++finishedQueries, plannedQueries);
        }
        long bucketPassMs = watch.elapsed(TimeUnit.MILLISECONDS) - frequentPassMs;

        if (collectDistinctCounts) {
            List<SketchColumn> withBuckets =
                    columns.stream().filter(c -> !c.buckets.isEmpty()).collect(Collectors.toList());
            for (List<SketchColumn> batch : batch(withBuckets, DISTINCT_COUNT_PASS_MAX_COLUMNS)) {
                runDistinctCountPass(context, analyzeStatus, batch);
                reportProgress(analyzeStatus, ++finishedQueries, plannedQueries);
            }
        }
        long distinctCountPassMs = watch.elapsed(TimeUnit.MILLISECONDS) - frequentPassMs - bucketPassMs;

        for (SketchColumn column : columns) {
            histograms.put(column.name, toColumnHistogram(column));
        }
        LOG.info("sketch histogram passes finished | table={}.{} columns={} sampleRatio={} frequentPassMs={} " +
                        "bucketPassMs={} distinctCountPassMs={}", db.getOriginName(), table.getName(), columns.size(),
                params.sampleRatio(), frequentPassMs, bucketPassMs, distinctCountPassMs);
    }

    @Override
    ColumnHistogram collectColumn(ConnectContext context, AnalyzeStatus analyzeStatus, String columnName,
                                  Type columnType) throws Exception {
        ColumnHistogram histogram = histograms.get(columnName);
        if (histogram != null) {
            return histogram;
        }
        // Not a type sketches are built for: sort and scan it on one thread, as sort mode does.
        int aggStage = context.getSessionVariable().getNewPlannerAggStage();
        context.getSessionVariable().setNewPlanerAggStage(1);
        try {
            return super.collectColumn(context, analyzeStatus, columnName, columnType);
        } finally {
            context.getSessionVariable().setNewPlanerAggStage(aggStage);
        }
    }

    private static int maxColumnsPerQuery() {
        return Math.max(1, Config.histogram_sketch_max_columns_per_query);
    }

    private static void reportProgress(AnalyzeStatus analyzeStatus, int finishedQueries, int plannedQueries) {
        // The last tenth is left for writing the rows back.
        analyzeStatus.setProgress(Math.max(analyzeStatus.getProgress(), finishedQueries * 90L / plannedQueries));
        GlobalStateMgr.getCurrentState().getAnalyzeMgr().addAnalyzeStatus(analyzeStatus);
    }

    /**
     * Splits the columns into the groups one query handles. Fixed-size columns are batched, which bounds what
     * one query holds in sketches. A string column gets a query of its own, because the size of a sketch of
     * strings depends on how long the strings are.
     */
    @VisibleForTesting
    static List<List<SketchColumn>> batch(List<SketchColumn> columns, int maxColumnsPerQuery) {
        List<List<SketchColumn>> batches = Lists.newArrayList();
        List<SketchColumn> fixedSize = Lists.newArrayList();
        for (SketchColumn column : columns) {
            if (column.isString()) {
                batches.add(List.of(column));
            } else {
                fixedSize.add(column);
            }
        }
        batches.addAll(0, Lists.partition(fixedSize, maxColumnsPerQuery));
        return batches;
    }

    /**
     * Looks up each column's distinct value count, which decides what counts as common and how the rows of a
     * bucket spread over its values. A column without basic statistics is not an error: pass 1 then counts
     * the distinct values of the rows it reads.
     */
    private void loadBasicStatistics(List<SketchColumn> columns) {
        List<ColumnStatistic> statistics;
        try {
            statistics = GlobalStateMgr.getCurrentState().getStatisticStorage().getColumnStatistics(table,
                    columns.stream().map(c -> c.name).collect(Collectors.toList()));
        } catch (Exception e) {
            LOG.warn("Failed to load basic statistics of {}.{} for histogram collection", db.getOriginName(),
                    table.getName(), e);
            return;
        }
        long tableRows = table instanceof OlapTable ? ((OlapTable) table).getRowCount() : 0;
        for (int i = 0; i < columns.size() && i < statistics.size(); i++) {
            ColumnStatistic statistic = statistics.get(i);
            if (statistic == null || statistic.isUnknown() || Double.isNaN(statistic.getDistinctValuesCount())) {
                continue;
            }
            SketchColumn column = columns.get(i);
            column.ndv = statistic.getDistinctValuesCount();
            column.skipFrequentItems |= tableRows > 0 && column.ndv / tableRows > NEAR_UNIQUE_RATIO;
        }
    }

    // ------------------------------------------------------------------------------------------------------
    // Pass 1: the most common values
    // ------------------------------------------------------------------------------------------------------

    private void runFrequentPass(ConnectContext context, AnalyzeStatus analyzeStatus, List<SketchColumn> batch)
            throws Exception {
        JsonArray row = singleRow(job.queryStatisticRowsSync(buildFrequentPassQuery(batch), context, analyzeStatus));
        int index = 0;
        for (SketchColumn column : batch) {
            if (column.skipFrequentItems) {
                column.nonNullRows = row.get(index++).getAsLong();
                continue;
            }
            JsonElement frequentItems = row.get(index++);
            double ndv = column.ndv;
            if (column.needsNdvFromSample()) {
                ndv = row.get(index++).getAsLong();
                // Stands in for the column's NDV in every later decision.
                column.ndv = ndv;
            }
            // NULL when the column has no non-NULL row.
            if (frequentItems.isJsonNull()) {
                continue;
            }
            column.frequentItems = FrequentItems.parse(frequentItems.getAsString());
            column.nonNullRows = column.frequentItems.total();
            column.mcvCandidates = SketchHistogramConverter.selectMostCommonValues(column.frequentItems,
                    params.mcvSize(), ndv).stream().filter(item -> isUsableLiteral(column.type, item.value()))
                    .collect(Collectors.toList());
            if (!column.bucketed) {
                // No second pass for this column: the sketch's own counts are the final ones.
                for (FrequentItem item : column.mcvCandidates) {
                    column.mostCommonValues.put(item.value(), SketchHistogramConverter.midpoint(item));
                }
            }
        }
    }

    /**
     * One row: per column, the tracked values of its frequent items sketch, followed by its distinct value count
     * if basic statistics could not tell. A column that gets no sketch is here for its row count alone.
     */
    @VisibleForTesting
    String buildFrequentPassQuery(List<SketchColumn> batch) {
        List<String> outputs = Lists.newArrayList();
        for (SketchColumn column : batch) {
            if (column.skipFrequentItems) {
                outputs.add("count(" + column.quotedName + ")");
                continue;
            }
            outputs.add("ds_frequent_items(ds_frequent(" + column.quotedName + "), " + FREQUENT_ITEMS_LIMIT + ")");
            if (column.needsNdvFromSample()) {
                outputs.add("ndv(" + column.quotedName + ")");
            }
        }
        return "SELECT " + String.join(", ", outputs) + " FROM " + sampledTable(true);
    }

    /**
     * A value is put back into the pass 2 query as a literal. Not-a-number and the infinities of a
     * floating-point column have no literal form; they stay with the rows the buckets are built from.
     */
    private static boolean isUsableLiteral(Type type, String value) {
        if (!type.isFloatingPointType()) {
            return true;
        }
        try {
            return Double.isFinite(Double.parseDouble(value));
        } catch (NumberFormatException e) {
            return false;
        }
    }

    // ------------------------------------------------------------------------------------------------------
    // Pass 2: the buckets, and the exact counts of the most common values
    // ------------------------------------------------------------------------------------------------------

    private void runBucketPass(ConnectContext context, AnalyzeStatus analyzeStatus, List<SketchColumn> batch)
            throws Exception {
        JsonArray row = singleRow(job.queryStatisticRowsSync(buildBucketPassQuery(batch), context, analyzeStatus));
        int index = 0;
        for (SketchColumn column : batch) {
            JsonElement kllHistogram = row.get(index++);
            if (!column.mcvCandidates.isEmpty()) {
                JsonElement exactCounts = row.get(index++);
                if (!exactCounts.isJsonNull()) {
                    // No more values than the sketch holds exactly, so these are counts, not estimates. A
                    // candidate that is missing here matched no row and is not a most common value after all.
                    for (FrequentItem item : FrequentItems.parse(exactCounts.getAsString()).items()) {
                        column.mostCommonValues.put(item.value(), item.lowerBound());
                    }
                }
            }
            // NULL when every non-NULL row holds a most common value.
            if (!kllHistogram.isJsonNull()) {
                column.buckets = SketchHistogramConverter.buildBuckets(KllHistogram.parse(kllHistogram.getAsString()),
                        params.sampleRatio(), column.type.isFixedPointType(), column.ndv);
            }
        }
    }

    /**
     * One row: per column, the KLL histogram of its rows outside the most common values, followed by the exact
     * counts of the most common values if it has any.
     */
    @VisibleForTesting
    String buildBucketPassQuery(List<SketchColumn> batch) {
        List<String> outputs = Lists.newArrayList();
        for (int i = 0; i < batch.size(); i++) {
            SketchColumn column = batch.get(i);
            if (column.mcvCandidates.isEmpty()) {
                outputs.add("ds_kll_histogram(ds_kll(" + valueAlias(i) + "), " + params.bucketNum() + ")");
            } else {
                outputs.add("ds_kll_histogram(ds_kll(if(" + flagAlias(i) + ", NULL, " + valueAlias(i) + ")), " +
                        params.bucketNum() + ")");
                outputs.add("ds_frequent_items(ds_frequent(if(" + flagAlias(i) + ", " + valueAlias(i) + ", NULL)), " +
                        column.mcvCandidates.size() + ")");
            }
        }
        return "SELECT " + String.join(", ", outputs) + " FROM (" + buildFlaggedSubquery(batch, true) + ") t";
    }

    /**
     * The rows of the batch's columns, each with a flag telling whether its value is one of the column's most
     * common values. Passes 2 and 3 split the rows of a column on that flag.
     */
    private String buildFlaggedSubquery(List<SketchColumn> batch, boolean sampled) {
        List<String> outputs = Lists.newArrayList();
        for (int i = 0; i < batch.size(); i++) {
            SketchColumn column = batch.get(i);
            outputs.add(column.quotedName + " AS " + valueAlias(i));
            if (!column.mcvCandidates.isEmpty()) {
                String literals = column.mcvCandidates.stream().map(item -> literal(column.type, item.value()))
                        .collect(Collectors.joining(", "));
                outputs.add(column.quotedName + " IN (" + literals + ") AS " + flagAlias(i));
            }
        }
        return "SELECT " + String.join(", ", outputs) + " FROM " + sampledTable(sampled);
    }

    // The aliases are positional: a name derived from the column's could collide with another column's.
    private static String valueAlias(int index) {
        return "v" + index;
    }

    private static String flagAlias(int index) {
        return "m" + index;
    }

    /**
     * |value| is the text a cast to VARCHAR gave for a value of |type|. Casting the text back gives that value
     * again for every type, which a bare literal does not: 0.1 written into the query is not the FLOAT 0.1.
     */
    @VisibleForTesting
    static String literal(Type type, String value) {
        return "CAST(" + quoteSqlString(value) + " AS " + type.toSql() + ")";
    }

    // ------------------------------------------------------------------------------------------------------
    // Pass 3: the distinct values of each bucket
    // ------------------------------------------------------------------------------------------------------

    private void runDistinctCountPass(ConnectContext context, AnalyzeStatus analyzeStatus, List<SketchColumn> batch)
            throws Exception {
        boolean sampled = isDistinctCountPassSampled();
        JsonArray row = singleRow(job.queryStatisticRowsSync(buildDistinctCountPassQuery(batch), context,
                analyzeStatus));
        for (int i = 0; i < batch.size(); i++) {
            SketchColumn column = batch.get(i);
            JsonElement distinctCounts = row.get(i);
            // NULL or no bucket at all when the query met no row outside the most common values.
            if (distinctCounts.isJsonNull() || "[]".equals(distinctCounts.getAsString())) {
                continue;
            }
            // A sample holds only some of the column's distinct values, and unlike sort mode's estimator a
            // HyperLogLog has no notion of the values seen once to extrapolate from. So the counts of a sample
            // are scaled to what the column is known to have outside its most common values.
            double target = sampled && !Double.isNaN(column.ndv) ? column.ndv - column.mostCommonValues.size() : -1;
            column.buckets = SketchHistogramConverter.applyDistinctCounts(column.buckets, distinctCounts.getAsString(),
                    target);
        }
    }

    /** Sample mode counts over the rows the other passes read; HLL mode reads the whole table, as sort mode does. */
    private boolean isDistinctCountPassSampled() {
        return params.bucketNdvMode() == StatsConstants.HistogramCollectBucketNdvMode.SAMPLE && isSampled();
    }

    /** One row: per column, its buckets with the distinct value count of each added. */
    @VisibleForTesting
    String buildDistinctCountPassQuery(List<SketchColumn> batch) {
        List<String> outputs = Lists.newArrayList();
        for (int i = 0; i < batch.size(); i++) {
            SketchColumn column = batch.get(i);
            String value = column.mcvCandidates.isEmpty() ? valueAlias(i) :
                    "if(" + flagAlias(i) + ", NULL, " + valueAlias(i) + ")";
            outputs.add("histogram_hll_ndv(" + value + ", " +
                    quoteSqlString(SketchHistogramConverter.toBucketsJson(column.buckets)) + ")");
        }
        return "SELECT " + String.join(", ", outputs) + " FROM (" +
                buildFlaggedSubquery(batch, isDistinctCountPassSampled()) + ") t";
    }

    // ------------------------------------------------------------------------------------------------------
    // Shared pieces
    // ------------------------------------------------------------------------------------------------------

    private boolean isSampled() {
        return params.sampleRatio() > 0.0 && params.sampleRatio() < 1.0;
    }

    /**
     * The table, sampled unless |sampled| is false or the job reads every row. The block sample takes the same
     * blocks in every pass. The row filter that replaces it when block sampling is turned off does not, but
     * each pass still draws a sample of the same size, and the rows of one query are always the same rows.
     */
    private String sampledTable(boolean sampled) {
        String tableName = StatisticUtils.quoting(db.getOriginName(), table.getName());
        if (!sampled || !isSampled()) {
            return tableName;
        }
        if (Config.enable_use_table_sample_collect_statistics) {
            return tableName + " SAMPLE('method'='by_block', 'seed'='" + sampleSeed + "', 'percent'='" +
                    formatSamplePercent(params.sampleRatio()) + "')";
        }
        return tableName + " WHERE rand() <= " + formatSampleRatio(params.sampleRatio());
    }

    private static JsonArray singleRow(List<JsonArray> rows) throws DdlException {
        if (rows.size() != 1) {
            throw new DdlException("Expected exactly one row from a histogram sketch query, but got " + rows.size());
        }
        return rows.get(0);
    }

    private ColumnHistogram toColumnHistogram(SketchColumn column) {
        Map<String, String> mostCommonValues =
                SketchHistogramConverter.toMostCommonValues(column.mostCommonValues, params.sampleRatio());
        String buckets;
        if (column.bucketed) {
            buckets = column.buckets.isEmpty() ? null : SketchHistogramConverter.toBucketsJson(column.buckets);
        } else {
            long mcvRows = mostCommonValues.values().stream().mapToLong(Long::parseLong).sum();
            buckets = SketchHistogramConverter.toPlaceholderBucketJson(
                    SketchHistogramConverter.scale(column.nonNullRows, params.sampleRatio()) - mcvRows);
        }
        return new ColumnHistogram(buckets, buildMcvJson(mostCommonValues));
    }

    @VisibleForTesting
    List<ColumnHistogram> collectedHistograms(List<String> columnNames) {
        List<ColumnHistogram> result = new ArrayList<>();
        for (String columnName : columnNames) {
            result.add(histograms.get(columnName));
        }
        return result;
    }
}

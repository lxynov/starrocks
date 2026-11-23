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
import com.google.gson.JsonNull;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.Table;
import com.starrocks.common.Config;
import com.starrocks.qe.ConnectContext;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.ast.StatementBase;
import com.starrocks.sql.optimizer.statistics.CachedStatisticStorage;
import com.starrocks.sql.optimizer.statistics.ColumnStatistic;
import com.starrocks.sql.parser.SqlParser;
import com.starrocks.thrift.TStatisticData;
import com.starrocks.type.DateType;
import com.starrocks.type.FloatType;
import com.starrocks.type.IntegerType;
import com.starrocks.type.Type;
import com.starrocks.type.VarcharType;
import mockit.Mock;
import mockit.MockUp;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;
import java.util.stream.Collectors;

public class SketchHistogramCollectTest extends HistogramStatisticsCollectJobTestBase {
    private static final String SAMPLED_TABLE =
            "`test`.`t0_stats` SAMPLE('method'='by_block', 'seed'='?', 'percent'='10')";

    private static String frequentItems(long total, long maxError, Object... valuesAndCounts) {
        List<String> items = new ArrayList<>();
        for (int i = 0; i < valuesAndCounts.length; i += 2) {
            long count = ((Number) valuesAndCounts[i + 1]).longValue();
            items.add("[\"" + valuesAndCounts[i] + "\"," + (count + maxError) + "," + count + "," +
                    (count + maxError) + "]");
        }
        return "{\"total\":" + total + ",\"max_error\":" + maxError + ",\"items\":[" + String.join(",", items) + "]}";
    }

    @Test
    public void testThreeColumnsWithoutBasicStatistics() throws Exception {
        try (Fixture fixture = new Fixture(connectContext, List.of("v2", "v4", "v7"),
                List.of(IntegerType.BIGINT, DateType.DATE, VarcharType.VARCHAR), "0.1", "2", "none")) {
            // Pass 1, fixed-size columns: v2 has one dominant value, v4 is evenly spread.
            fixture.reply(frequentItems(1000, 0, "7", 900, "8", 60, "9", 40), 3L,
                    frequentItems(30, 0, "2024-01-01", 10, "2024-01-02", 10, "2024-01-03", 10), 3L);
            // Pass 1, the string column on its own.
            fixture.reply(frequentItems(500, 0, "x", 400, "y", 60, "z", 40), 3L);
            // Pass 2: v2's buckets over the rows that are not 7 and the exact count of 7, then v4's buckets.
            fixture.reply("{\"count\":100,\"min\":\"8\",\"max\":\"9\",\"buckets\":[[\"8\",\"8\",0.6],[\"8\",\"9\",1]]}",
                    frequentItems(900, 0, "7", 900),
                    "{\"count\":30,\"min\":\"2024-01-01\",\"max\":\"2024-01-03\",\"buckets\":" +
                            "[[\"2024-01-01\",\"2024-01-02\",0.667],[\"2024-01-02\",\"2024-01-03\",1]]}");

            NativeAnalyzeStatus status = fixture.collect();

            assertSqlStatements(List.of(
                    "SELECT ds_frequent_items(ds_frequent(`v2`), 2048), ndv(`v2`), " +
                            "ds_frequent_items(ds_frequent(`v4`), 2048), ndv(`v4`) FROM " + SAMPLED_TABLE,
                    "SELECT ds_frequent_items(ds_frequent(`v7`), 2048), ndv(`v7`) FROM " + SAMPLED_TABLE,
                    "SELECT ds_kll_histogram(ds_kll(if(m0, NULL, v0)), 64), " +
                            "ds_frequent_items(ds_frequent(if(m0, v0, NULL)), 1), " +
                            "ds_kll_histogram(ds_kll(v1), 64) FROM (SELECT `v2` AS v0, " +
                            "`v2` IN (CAST('7' AS bigint(20))) AS m0, `v4` AS v1 FROM " + SAMPLED_TABLE + ") t"),
                    fixture.queries());
            // Every query is valid SQL.
            for (String query : fixture.rawQueries()) {
                SqlParser.parseOneWithStarRocksDialect(query, connectContext.getSessionVariable());
            }

            // Counts are scaled by the inverse of the sample ratio. Each of v2's buckets is a single integer, so
            // all its rows are upper_repeats; v4 spreads each bucket's rows over NDV / bucket count values.
            String expectedSql = """
                    INSERT INTO _statistics_.histogram_statistics(\
                    table_id, column_name, db_id, table_name, buckets, mcv, update_time) VALUES \
                    (%1$d, 'v2', %2$d, 'test.t0_stats', '[["8","8","600","600"],["9","9","1000","400"]]', \
                    '[["7","9000"]]', NOW()), \
                    (%1$d, 'v4', %2$d, 'test.t0_stats', \
                    '[["2024-01-01","2024-01-02","200","133"],["2024-01-02","2024-01-03","300","67"]]', NULL, NOW()), \
                    (%1$d, 'v7', %2$d, 'test.t0_stats', '[["Infinity","Infinity","1000","0"]]', \
                    '[["x","4000"]]', NOW());
                    """.formatted(fixture.table.getId(), fixture.db.getId()).strip();
            Assertions.assertEquals(List.of(expectedSql), fixture.insertSql());

            Assertions.assertEquals(100, status.getProgress());
            Assertions.assertEquals("sketch", status.getProperties().get(StatsConstants.HISTOGRAM_COLLECT_MODE));
            Assertions.assertEquals("3", status.getProperties().get(StatsConstants.HISTOGRAM_COLLECT_QUERIES));
            Assertions.assertNotNull(status.getProperties().get(StatsConstants.HISTOGRAM_COLLECT_ELAPSED_MS));
            // The queries are ordinary aggregations, free to be split into stages.
            Assertions.assertEquals(0, connectContext.getSessionVariable().getNewPlannerAggStage());
            // What the status reports is not written back into the job's own properties.
            Assertions.assertFalse(fixture.job.getProperties().containsKey(StatsConstants.HISTOGRAM_COLLECT_QUERIES));
        }
    }

    @Test
    public void testBasicStatisticsAndSampledDistinctCounts() throws Exception {
        try (Fixture fixture = new Fixture(connectContext, List.of("v1"), List.of(IntegerType.BIGINT),
                "0.1", "100", "sample")) {
            fixture.ndv("v1", 1000);
            // The NDV is known, so pass 1 does not count distinct values. Nothing is frequent enough.
            fixture.reply(frequentItems(100000, 60, "5", 3, "6", 2));
            fixture.reply("{\"count\":100000,\"min\":\"1\",\"max\":\"1000\",\"buckets\":" +
                    "[[\"1\",\"500\",0.5],[\"500\",\"1000\",1]]}");
            // Pass 3: the sample holds 100 and 300 distinct values in the two buckets.
            fixture.reply("[[\"1\",\"500\",\"500000\",\"1000\",\"100\"],[\"501\",\"1000\",\"1000000\",\"1000\",\"300\"]]");

            NativeAnalyzeStatus status = fixture.collect();

            String buckets = "[[\"1\",\"500\",\"500000\",\"1000\"],[\"501\",\"1000\",\"1000000\",\"1000\"]]";
            assertSqlStatements(List.of(
                    "SELECT ds_frequent_items(ds_frequent(`v1`), 2048) FROM " + SAMPLED_TABLE,
                    "SELECT ds_kll_histogram(ds_kll(v0), 64) FROM (SELECT `v1` AS v0 FROM " + SAMPLED_TABLE + ") t",
                    "SELECT histogram_hll_ndv(v0, '" + buckets + "') FROM (SELECT `v1` AS v0 FROM " +
                            SAMPLED_TABLE + ") t"),
                    fixture.queries());
            for (String query : fixture.rawQueries()) {
                SqlParser.parseOneWithStarRocksDialect(query, connectContext.getSessionVariable());
            }

            // The 400 distinct values of the sample are scaled to the 1000 the column has: 250 and 750. Each
            // bucket's 500000 rows then spread over them.
            String expectedSql = """
                    INSERT INTO _statistics_.histogram_statistics(\
                    table_id, column_name, db_id, table_name, buckets, mcv, update_time) VALUES \
                    (%d, 'v1', %d, 'test.t0_stats', \
                    '[["1","500","500000","2000","250"],["501","1000","1000000","667","750"]]', NULL, NOW());
                    """.formatted(fixture.table.getId(), fixture.db.getId()).strip();
            Assertions.assertEquals(List.of(expectedSql), fixture.insertSql());
            Assertions.assertEquals("3", status.getProperties().get(StatsConstants.HISTOGRAM_COLLECT_QUERIES));
        }
    }

    @Test
    public void testHllDistinctCountsReadTheWholeTableAndAreStoredAsCounted() throws Exception {
        // A list of one value: only the most frequent one makes it.
        try (Fixture fixture = new Fixture(connectContext, List.of("v1"), List.of(IntegerType.BIGINT),
                "0.1", "1", "hll")) {
            fixture.ndv("v1", 1000);
            fixture.reply(frequentItems(1000, 0, "5", 990, "6", 10));
            fixture.reply("{\"count\":10,\"min\":\"6\",\"max\":\"6\",\"buckets\":[[\"6\",\"6\",1]]}",
                    frequentItems(990, 0, "5", 990));
            fixture.reply("[[\"6\",\"6\",\"100\",\"100\",\"1\"]]");

            fixture.collect();

            String flagged = "SELECT `v1` AS v0, `v1` IN (CAST('5' AS bigint(20))) AS m0 FROM ";
            assertSqlStatements(List.of(
                    "SELECT ds_frequent_items(ds_frequent(`v1`), 2048) FROM " + SAMPLED_TABLE,
                    "SELECT ds_kll_histogram(ds_kll(if(m0, NULL, v0)), 64), " +
                            "ds_frequent_items(ds_frequent(if(m0, v0, NULL)), 1) FROM (" + flagged + SAMPLED_TABLE + ") t",
                    // No sample clause: HLL mode counts over every row, as sort mode does.
                    "SELECT histogram_hll_ndv(if(m0, NULL, v0), '[[\"6\",\"6\",\"100\",\"100\"]]') FROM (" + flagged +
                            "`test`.`t0_stats`) t"),
                    fixture.queries());

            String expectedSql = """
                    INSERT INTO _statistics_.histogram_statistics(\
                    table_id, column_name, db_id, table_name, buckets, mcv, update_time) VALUES \
                    (%d, 'v1', %d, 'test.t0_stats', '[["6","6","100","100","1"]]', '[["5","9900"]]', NOW());
                    """.formatted(fixture.table.getId(), fixture.db.getId()).strip();
            Assertions.assertEquals(List.of(expectedSql), fixture.insertSql());
        }
    }

    @Test
    public void testFixedSizeColumnsAreBatchedAndStringsAreNot() throws Exception {
        int maxColumns = Config.histogram_sketch_max_columns_per_query;
        Config.histogram_sketch_max_columns_per_query = 2;
        try (Fixture fixture = new Fixture(connectContext, List.of("v1", "v7", "v2", "v3"),
                List.of(IntegerType.BIGINT, VarcharType.VARCHAR, IntegerType.BIGINT, IntegerType.BIGINT),
                "1", "100", "none")) {
            for (String column : List.of("v1", "v2", "v3", "v7")) {
                fixture.ndv(column, 10);
            }
            // Every column is empty: all the sketch functions return NULL.
            fixture.reply(null, null);
            fixture.reply((Object) null);
            fixture.reply((Object) null);
            fixture.reply(null, null);
            fixture.reply((Object) null);

            NativeAnalyzeStatus status = fixture.collect();

            // The whole table is read, so there is no sample clause.
            assertSqlStatements(List.of(
                    "SELECT ds_frequent_items(ds_frequent(`v1`), 2048), ds_frequent_items(ds_frequent(`v2`), 2048) " +
                            "FROM `test`.`t0_stats`",
                    "SELECT ds_frequent_items(ds_frequent(`v3`), 2048) FROM `test`.`t0_stats`",
                    "SELECT ds_frequent_items(ds_frequent(`v7`), 2048) FROM `test`.`t0_stats`",
                    "SELECT ds_kll_histogram(ds_kll(v0), 64), ds_kll_histogram(ds_kll(v1), 64) FROM " +
                            "(SELECT `v1` AS v0, `v2` AS v1 FROM `test`.`t0_stats`) t",
                    "SELECT ds_kll_histogram(ds_kll(v0), 64) FROM (SELECT `v3` AS v0 FROM `test`.`t0_stats`) t"),
                    fixture.queries());

            // An empty column has neither buckets nor most common values; a string column still gets its
            // placeholder bucket, with no rows in it. Rows are written in the order the columns were given.
            String expectedSql = """
                    INSERT INTO _statistics_.histogram_statistics(\
                    table_id, column_name, db_id, table_name, buckets, mcv, update_time) VALUES \
                    (%1$d, 'v1', %2$d, 'test.t0_stats', NULL, NULL, NOW()), \
                    (%1$d, 'v7', %2$d, 'test.t0_stats', '[["Infinity","Infinity","0","0"]]', NULL, NOW()), \
                    (%1$d, 'v2', %2$d, 'test.t0_stats', NULL, NULL, NOW()), \
                    (%1$d, 'v3', %2$d, 'test.t0_stats', NULL, NULL, NOW());
                    """.formatted(fixture.table.getId(), fixture.db.getId()).strip();
            Assertions.assertEquals(List.of(expectedSql), fixture.insertSql());
            Assertions.assertEquals("5", status.getProperties().get(StatsConstants.HISTOGRAM_COLLECT_QUERIES));
        } finally {
            Config.histogram_sketch_max_columns_per_query = maxColumns;
        }
    }

    @Test
    public void testNearUniqueColumnsGetNoFrequentItemsSketch() throws Exception {
        new MockUp<OlapTable>() {
            @Mock
            public long getRowCount() {
                return 1000;
            }
        };
        try (Fixture fixture = new Fixture(connectContext, List.of("v1", "v2", "v7"),
                List.of(IntegerType.BIGINT, IntegerType.BIGINT, VarcharType.VARCHAR), "1", "100", "none")) {
            fixture.ndv("v1", 990);
            fixture.ndv("v2", 500);
            fixture.ndv("v7", 999);
            // Pass 1 skips v1 altogether; the near-unique string column is only counted.
            fixture.reply(frequentItems(1000, 0, "1", 2));
            fixture.reply(800L);
            fixture.reply("{\"count\":1000,\"min\":\"1\",\"max\":\"990\",\"buckets\":[[\"1\",\"990\",1]]}",
                    "{\"count\":1000,\"min\":\"1\",\"max\":\"500\",\"buckets\":[[\"1\",\"500\",1]]}");

            fixture.collect();

            assertSqlStatements(List.of(
                    "SELECT ds_frequent_items(ds_frequent(`v2`), 2048) FROM `test`.`t0_stats`",
                    "SELECT count(`v7`) FROM `test`.`t0_stats`",
                    "SELECT ds_kll_histogram(ds_kll(v0), 64), ds_kll_histogram(ds_kll(v1), 64) FROM " +
                            "(SELECT `v1` AS v0, `v2` AS v1 FROM `test`.`t0_stats`) t"),
                    fixture.queries());

            String expectedSql = """
                    INSERT INTO _statistics_.histogram_statistics(\
                    table_id, column_name, db_id, table_name, buckets, mcv, update_time) VALUES \
                    (%1$d, 'v1', %2$d, 'test.t0_stats', '[["1","990","1000","1"]]', NULL, NOW()), \
                    (%1$d, 'v2', %2$d, 'test.t0_stats', '[["1","500","1000","2"]]', NULL, NOW()), \
                    (%1$d, 'v7', %2$d, 'test.t0_stats', '[["Infinity","Infinity","800","0"]]', NULL, NOW());
                    """.formatted(fixture.table.getId(), fixture.db.getId()).strip();
            Assertions.assertEquals(List.of(expectedSql), fixture.insertSql());
        }
    }

    @Test
    public void testNoFrequentItemsSketchWhenNoMostCommonValuesAreWanted() throws Exception {
        try (Fixture fixture = new Fixture(connectContext, List.of("v1", "v7"),
                List.of(IntegerType.BIGINT, VarcharType.VARCHAR), "1", "0", "none")) {
            // Pass 1 has nothing to find for v1, and only counts the rows of the string column.
            fixture.reply(40L);
            fixture.reply("{\"count\":10,\"min\":\"1\",\"max\":\"3\",\"buckets\":[[\"1\",\"3\",1]]}");

            fixture.collect();

            assertSqlStatements(List.of(
                    "SELECT count(`v7`) FROM `test`.`t0_stats`",
                    "SELECT ds_kll_histogram(ds_kll(v0), 64) FROM (SELECT `v1` AS v0 FROM `test`.`t0_stats`) t"),
                    fixture.queries());
            String expectedSql = """
                    INSERT INTO _statistics_.histogram_statistics(\
                    table_id, column_name, db_id, table_name, buckets, mcv, update_time) VALUES \
                    (%1$d, 'v1', %2$d, 'test.t0_stats', '[["1","3","10","3"]]', NULL, NOW()), \
                    (%1$d, 'v7', %2$d, 'test.t0_stats', '[["Infinity","Infinity","40","0"]]', NULL, NOW());
                    """.formatted(fixture.table.getId(), fixture.db.getId()).strip();
            Assertions.assertEquals(List.of(expectedSql), fixture.insertSql());
        }
    }

    @Test
    public void testRowFilterReplacesBlockSampleWhenItIsTurnedOff() throws Exception {
        boolean useTableSample = Config.enable_use_table_sample_collect_statistics;
        Config.enable_use_table_sample_collect_statistics = false;
        try (Fixture fixture = new Fixture(connectContext, List.of("v1"), List.of(IntegerType.BIGINT),
                "0.25", "100", "none")) {
            fixture.ndv("v1", 10);
            fixture.reply((Object) null);
            fixture.reply((Object) null);

            fixture.collect();

            assertSqlStatements(List.of(
                    "SELECT ds_frequent_items(ds_frequent(`v1`), 2048) FROM `test`.`t0_stats` WHERE rand() <= 0.25",
                    "SELECT ds_kll_histogram(ds_kll(v0), 64) FROM (SELECT `v1` AS v0 FROM `test`.`t0_stats` " +
                            "WHERE rand() <= 0.25) t"),
                    fixture.queries());
        } finally {
            Config.enable_use_table_sample_collect_statistics = useTableSample;
        }
    }

    @Test
    public void testValuesWithoutALiteralStayInTheBuckets() throws Exception {
        try (Fixture fixture = new Fixture(connectContext, List.of("v1"), List.of(FloatType.FLOAT),
                "1", "100", "none")) {
            fixture.ndv("v1", 3);
            fixture.reply(frequentItems(100, 0, "nan", 50, "0.1", 30, "inf", 20));
            fixture.reply("{\"count\":70,\"min\":\"inf\",\"max\":\"inf\",\"buckets\":[[\"inf\",\"inf\",1]]}",
                    frequentItems(30, 0, "0.1", 30));

            fixture.collect();

            // Not-a-number and infinity cannot be written into the query. The value that can goes back as a
            // cast of its text, which for a FLOAT is not the same as the bare literal 0.1.
            Assertions.assertTrue(fixture.rawQueries().get(1).contains(
                    "`v1` IN (CAST('0.1' AS float)) AS m0"), fixture.rawQueries().get(1));
        }
    }

    @Test
    public void testSortModeIsUntouchedAndReportsItself() throws Exception {
        try (Fixture fixture = new Fixture(connectContext, List.of("v1"), List.of(IntegerType.BIGINT),
                "0.1", "100", "none")) {
            fixture.job.getProperties().remove(StatsConstants.HISTOGRAM_COLLECT_MODE);
            Assertions.assertEquals("sort", Config.histogram_collect_mode);

            NativeAnalyzeStatus status = fixture.collect();

            // Two queries per column through the versioned statistics result, none of the sketch queries.
            Assertions.assertTrue(fixture.queries().isEmpty());
            Assertions.assertEquals(2, fixture.sortModeQueries);
            Assertions.assertEquals(1, connectContext.getSessionVariable().getNewPlannerAggStage());
            Assertions.assertEquals("sort", status.getProperties().get(StatsConstants.HISTOGRAM_COLLECT_MODE));
            Assertions.assertEquals("2", status.getProperties().get(StatsConstants.HISTOGRAM_COLLECT_QUERIES));
        }
    }

    @Test
    public void testConfiguredModeAppliesWhenTheJobNamesNone() {
        String configured = Config.histogram_collect_mode;
        try {
            Map<String, String> properties = new HashMap<>();
            Config.histogram_collect_mode = "sketch";
            Assertions.assertEquals(StatsConstants.HistogramCollectMode.SKETCH,
                    HistogramCollectParams.resolveCollectMode(properties));
            // The job's own property wins, in any letter case.
            properties.put(StatsConstants.HISTOGRAM_COLLECT_MODE, "SORT");
            Assertions.assertEquals(StatsConstants.HistogramCollectMode.SORT,
                    HistogramCollectParams.resolveCollectMode(properties));
            // An unusable configuration value counts as sort mode.
            properties.clear();
            Config.histogram_collect_mode = "bogus";
            Assertions.assertEquals(StatsConstants.HistogramCollectMode.SORT,
                    HistogramCollectParams.resolveCollectMode(properties));
        } finally {
            Config.histogram_collect_mode = configured;
        }
    }

    @Test
    public void testSketchableTypes() {
        Assertions.assertTrue(SketchHistogramTraits.isSketchable(IntegerType.BIGINT));
        Assertions.assertTrue(SketchHistogramTraits.isSketchable(IntegerType.LARGEINT));
        Assertions.assertTrue(SketchHistogramTraits.isSketchable(com.starrocks.type.BooleanType.BOOLEAN));
        Assertions.assertTrue(SketchHistogramTraits.isSketchable(FloatType.DOUBLE));
        Assertions.assertTrue(SketchHistogramTraits.isSketchable(DateType.DATETIME));
        Assertions.assertTrue(SketchHistogramTraits.isSketchable(VarcharType.VARCHAR));
        Assertions.assertTrue(SketchHistogramTraits.isSketchable(com.starrocks.type.DecimalType.DECIMAL128));
        Assertions.assertFalse(SketchHistogramTraits.isSketchable(com.starrocks.type.DateType.TIME));
        Assertions.assertFalse(SketchHistogramTraits.isSketchable(com.starrocks.type.JsonType.JSON));
    }

    /** Runs a sketch-mode job against scripted query results and captures what it sends and writes. */
    private static class Fixture implements AutoCloseable {
        private final ConnectContext context;
        private final Database db;
        private final OlapTable table;
        private final HistogramStatisticsCollectJob job;
        private final Deque<JsonArray> replies = new ArrayDeque<>();
        private final List<String> queries = new ArrayList<>();
        private final List<String> insertSql = new ArrayList<>();
        private final Map<String, Double> ndv = new HashMap<>();
        private final int aggStage;
        private int sortModeQueries = 0;

        private Fixture(ConnectContext context, List<String> columns, List<Type> types, String sampleRatio,
                        String mcvSize, String ndvMode) {
            this.context = context;
            this.aggStage = context.getSessionVariable().getNewPlannerAggStage();
            db = GlobalStateMgr.getCurrentState().getLocalMetastore().getDb("test");
            table = (OlapTable) GlobalStateMgr.getCurrentState().getLocalMetastore()
                    .getTable(db.getFullName(), "t0_stats");

            Map<String, String> properties = new HashMap<>();
            properties.put(StatsConstants.HISTOGRAM_SAMPLE_RATIO, sampleRatio);
            properties.put(StatsConstants.HISTOGRAM_BUCKET_NUM, "64");
            properties.put(StatsConstants.HISTOGRAM_MCV_SIZE, mcvSize);
            properties.put(StatsConstants.HISTOGRAM_COLLECT_BUCKET_NDV_MODE, ndvMode);
            properties.put(StatsConstants.HISTOGRAM_COLLECT_MODE, "sketch");
            job = new HistogramStatisticsCollectJob(db, table, Lists.newArrayList(columns), Lists.newArrayList(types),
                    StatsConstants.ScheduleType.ONCE, properties);

            new MockUp<StatisticExecutor>() {
                @Mock
                public List<JsonArray> executeStatisticRowsDQL(ConnectContext ctx, String sql) {
                    queries.add(sql);
                    Assertions.assertFalse(replies.isEmpty(), "unexpected query: " + sql);
                    return List.of(replies.remove());
                }

                @Mock
                public List<TStatisticData> executeStatisticDQL(ConnectContext ctx, String sql) {
                    sortModeQueries++;
                    TStatisticData data = new TStatisticData();
                    data.columnName = "1";
                    data.histogram = sql.toLowerCase().contains("group by") ? "10" : "[[\"1\",\"2\",\"3\",\"4\"]]";
                    return Lists.newArrayList(data);
                }
            };

            new MockUp<HistogramStatisticsCollectJob>() {
                @Mock
                public void collectStatisticSync(Supplier<StatementBase> statementSupplier,
                                                 ConnectContext ctx, AnalyzeStatus status) {
                    insertSql.add(statementSupplier.get().getOrigStmt().getOrigStmt());
                }
            };

            new MockUp<CachedStatisticStorage>() {
                @Mock
                public List<ColumnStatistic> getColumnStatistics(Table table, List<String> columnNames) {
                    return columnNames.stream().map(name -> ndv.containsKey(name) ?
                            ColumnStatistic.builder().setMinValue(0).setMaxValue(1000).setNullsFraction(0)
                                    .setAverageRowSize(8).setDistinctValuesCount(ndv.get(name)).build() :
                            ColumnStatistic.unknown()).collect(Collectors.toList());
                }
            };
        }

        /** The single row the next query returns; a null value is SQL NULL. */
        private void reply(Object... values) {
            JsonArray row = new JsonArray();
            for (Object value : values == null ? new Object[] {null} : values) {
                if (value == null) {
                    row.add(JsonNull.INSTANCE);
                } else if (value instanceof Number) {
                    row.add((Number) value);
                } else {
                    row.add((String) value);
                }
            }
            replies.add(row);
        }

        private void ndv(String column, double distinctValues) {
            ndv.put(column, distinctValues);
        }

        private NativeAnalyzeStatus collect() throws Exception {
            NativeAnalyzeStatus status = new NativeAnalyzeStatus();
            job.collect(context, status);
            Assertions.assertTrue(replies.isEmpty(), "queries that were expected but not run: " + replies.size());
            return status;
        }

        /** The queries, with the random seed of the block sample blanked. */
        private List<String> queries() {
            return queries.stream().map(sql -> sql.replaceAll("'seed'='\\d+'", "'seed'='?'"))
                    .collect(Collectors.toList());
        }

        private List<String> rawQueries() {
            return queries;
        }

        private List<String> insertSql() {
            return insertSql;
        }

        @Override
        public void close() {
            context.getSessionVariable().setNewPlanerAggStage(aggStage);
        }
    }

    @Test
    public void testEveryPassReadsTheSameSample() throws Exception {
        try (Fixture fixture = new Fixture(connectContext, List.of("v1"), List.of(IntegerType.BIGINT),
                "0.1", "100", "sample")) {
            fixture.ndv("v1", 1000);
            fixture.reply(frequentItems(1000, 0, "5", 1));
            fixture.reply("{\"count\":1000,\"min\":\"1\",\"max\":\"9\",\"buckets\":[[\"1\",\"9\",1]]}");
            fixture.reply("[[\"1\",\"9\",\"10000\",\"1250\",\"8\"]]");

            fixture.collect();

            List<String> seeds = fixture.rawQueries().stream()
                    .map(sql -> sql.replaceAll(".*'seed'='(\\d+)'.*", "$1")).distinct().collect(Collectors.toList());
            Assertions.assertEquals(3, fixture.rawQueries().size());
            Assertions.assertEquals(1, seeds.size(), Arrays.toString(seeds.toArray()));
        }
    }
}

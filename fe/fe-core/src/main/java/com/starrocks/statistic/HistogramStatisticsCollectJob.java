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

import com.google.common.base.Stopwatch;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.Table;
import com.starrocks.qe.ConnectContext;
import com.starrocks.type.Type;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.TimeUnit;

public class HistogramStatisticsCollectJob extends StatisticsCollectJob {
    private static final Logger LOG = LogManager.getLogger(HistogramStatisticsCollectJob.class);

    public HistogramStatisticsCollectJob(Database db, Table table, List<String> columnNames, List<Type> columnTypes,
                                         StatsConstants.ScheduleType scheduleType, Map<String, String> properties) {
        super(db, table, columnNames, columnTypes, StatsConstants.AnalyzeType.HISTOGRAM, scheduleType, properties);
    }

    @Override
    public void collect(ConnectContext context, AnalyzeStatus analyzeStatus) throws Exception {
        if (table.isTemporaryTable()) {
            context.setSessionId(((OlapTable) table).getSessionId());
        }

        HistogramCollectParams params = new HistogramCollectParams(properties);
        Stopwatch watch = Stopwatch.createStarted();
        try {
            if (params.collectMode() == StatsConstants.HistogramCollectMode.SKETCH) {
                new HistogramCollector(new SketchHistogramTraits(this, params)).collect(context, analyzeStatus);
            } else {
                // histogram() takes one globally sorted stream, so it only runs as a single-stage aggregation.
                context.getSessionVariable().setNewPlanerAggStage(1);
                new HistogramCollector(new NativeHistogramTraits(this, params)).collect(context, analyzeStatus);
            }
        } finally {
            reportCollection(analyzeStatus, params, watch.elapsed(TimeUnit.MILLISECONDS));
        }
    }

    /**
     * Records how the collection ran, whether or not it succeeded: the mode it used, which a job that names
     * none takes from the configuration, how many statistics queries it ran and how long it took. The numbers
     * go next to the job's properties so that they show up in the Properties column of SHOW ANALYZE STATUS
     * without a new column, and in the log.
     */
    private void reportCollection(AnalyzeStatus analyzeStatus, HistogramCollectParams params, long elapsedMs) {
        String mode = params.collectMode().name().toLowerCase(Locale.ROOT);
        // A copy: the status may share its map with this job, whose properties must stay what they were.
        Map<String, String> reported = new LinkedHashMap<>();
        if (analyzeStatus.getProperties() != null) {
            reported.putAll(analyzeStatus.getProperties());
        }
        reported.put(StatsConstants.HISTOGRAM_COLLECT_MODE, mode);
        reported.put(StatsConstants.HISTOGRAM_COLLECT_QUERIES, String.valueOf(getStatisticQueryCount()));
        reported.put(StatsConstants.HISTOGRAM_COLLECT_ELAPSED_MS, String.valueOf(elapsedMs));
        analyzeStatus.setProperties(reported);

        LOG.info("histogram collection | table={}.{} mode={} columns={} sampleRatio={} queries={} elapsedMs={}",
                db.getOriginName(), table.getName(), mode, columnNames.size(), params.sampleRatio(),
                getStatisticQueryCount(), elapsedMs);
    }

    @Override
    public String getName() {
        return "Histogram";
    }
}

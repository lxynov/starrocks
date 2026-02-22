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

package com.starrocks.sql.plan;

import com.starrocks.common.profile.Tracers;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

public class NestedCaseWhenPlanTest extends PlanTestBase {

    /**
     * Use a very long optimizer timeout so that stepping through the planner with breakpoints
     * does not hit the wall-clock timeout (e.g. new_planner_optimize_timeout).
     */
    @BeforeAll
    public static void beforeClassWithLongTimeout() throws Exception {
        connectContext.getSessionVariable().setOptimizerExecuteTimeout(-1);
    }

    /**
     * Trace the optimizer with TRACE ALL OPTIMIZER (times + values + logs in chronological order).
     */
    @Test
    public void testNestedCaseWhenWithCTETraceAllOptimizer() throws Exception {
        Tracers.register(connectContext);
        Tracers.init(connectContext, "TIMING", "Optimizer");

        String sql = "WITH cte1 AS (\n"
                + "  SELECT v1, v2,\n"
                + "    CASE\n"
                + "      WHEN v1 < 10 THEN 1\n"
                + "      WHEN v1 < 20 THEN 2\n"
                + "      ELSE 3\n"
                + "    END AS bucket\n"
                + "  FROM t0\n"
                + "),\n"
                + "cte2 AS (\n"
                + "  SELECT v1, v2,\n"
                + "    CASE\n"
                + "      WHEN bucket = 1 AND v2 > 0 THEN 'a'\n"
                + "      WHEN bucket = 2 AND v2 > 0 THEN 'b'\n"
                + "      ELSE NULL\n"
                + "    END AS label\n"
                + "  FROM cte1\n"
                + ")\n"
                + "SELECT * FROM cte2";

        String plan = getFragmentPlan(sql);

        String traceAll = Tracers.printTiming();
        System.out.println(traceAll);
        System.out.println("\n\n");
        System.out.println(plan);
        System.out.println("\n\n");
        System.out.println(sql);
        Tracers.close();

        // TRACE ALL = times + values + logs in chronological order
        assertContains(traceAll, "watchScope:");
        assertContains(traceAll, "Optimizer");
        assertContains(traceAll, "Tracer Cost:");
        // ScalarOperatorsReuseRule (uses ScalarOperatorsReuse) is relevant for nested CASE/CTE
        assertContains(traceAll, "ScalarOperatorsReuseRule");
    }

    @Test
    public void testNestedCaseWhenWithCTE() throws Exception {
        String sql = "WITH cte1 AS (\n"
                + "  SELECT v1, v2,\n"
                + "    CASE\n"
                + "      WHEN v1 < 10 THEN 1\n"
                + "      WHEN v1 < 20 THEN 2\n"
                + "      ELSE 3\n"
                + "    END AS bucket\n"
                + "  FROM t0\n"
                + "),\n"
                + "cte2 AS (\n"
                + "  SELECT v1, v2, bucket,\n"
                + "    CASE\n"
                + "      WHEN bucket = 1 AND v2 > 0 THEN 'a'\n"
                + "      WHEN bucket = 2 AND v2 > 0 THEN 'b'\n"
                + "      ELSE NULL\n"
                + "    END AS label\n"
                + "  FROM cte1\n"
                + ")\n"
                + "SELECT * FROM cte2";

        String plan = getFragmentPlan(sql);

        // Plan must scan t0 and contain case/if expressions
        assertContains(plan, "OlapScanNode");
        assertContains(plan, "TABLE: t0");
    }

    /**
     * CTE with inner CASE (bucket) referenced multiple times in outer CASE (label).
     * MergeTwoProjectRule skips merging here to avoid duplicating the inner expression
     * in the plan and blowing up expression tree size.
     */
    @Test
    public void testNestedCaseWhenCTEInnerExprReferencedMultipleTimes() throws Exception {
        String sql = "WITH cte1 AS (\n"
                + "  SELECT v1, v2,\n"
                + "    CASE\n"
                + "      WHEN v1 < 10 THEN 1\n"
                + "      WHEN v1 < 20 THEN 2\n"
                + "      ELSE 3\n"
                + "    END AS bucket\n"
                + "  FROM t0\n"
                + "),\n"
                + "cte2 AS (\n"
                + "  SELECT v1, v2,\n"
                + "    CASE\n"
                + "      WHEN bucket = 1 AND v2 > 0 THEN 'a'\n"
                + "      WHEN bucket = 2 AND v2 > 0 THEN 'b'\n"
                + "      ELSE NULL\n"
                + "    END AS label\n"
                + "  FROM cte1\n"
                + ")\n"
                + "SELECT * FROM cte2";

        String plan = getFragmentPlan(sql);

        assertContains(plan, "OlapScanNode");
        assertContains(plan, "TABLE: t0");
        assertContains(plan, "if(");
    }

    @Test
    public void testNestedCaseWhenInSubquery() throws Exception {
        // Single subquery with two levels of CASE: inner bucket, outer priority.
        String sql = "SELECT id, region, priority FROM (\n"
                + "  SELECT v1 AS id, v2 AS region,\n"
                + "    CASE\n"
                + "      WHEN (CASE WHEN v1 < 5 THEN 1 WHEN v1 < 10 THEN 2 ELSE 3 END) = 1 AND v2 > 0 THEN 'p1'\n"
                + "      WHEN (CASE WHEN v1 < 5 THEN 1 WHEN v1 < 10 THEN 2 ELSE 3 END) = 2 AND v2 > 0 THEN 'p2'\n"
                + "      ELSE NULL\n"
                + "    END AS priority\n"
                + "  FROM t0\n"
                + ") t WHERE priority IS NOT NULL";

        String plan = getFragmentPlan(sql);

        assertContains(plan, "OlapScanNode");
        assertContains(plan, "TABLE: t0");
        assertContains(plan, "if(");
    }
}

---
title: StarRocks Query Feedback
---

## 1. Overview: Learning from Execution to Improve Query Plans

Query Feedback is a critical component of StarRocks' Cost-Based Optimizer (CBO) that enables the system to learn from query execution and automatically improve query plans over time. When statistics are outdated or inaccurate, CBO may generate suboptimal query plans that lead to slow queries, excessive resource consumption, or even system instability. Query Feedback addresses this by recording execution statistics during query execution and using this information to dynamically optimize subsequent queries with similar plans.

**The Problem Query Feedback Solves:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    The Statistics Problem                                │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Scenario: Outdated or Missing Statistics                          │  │
│  │                                                                     │  │
│  │  1. CBO estimates table sizes based on statistics                  │  │
│  │     ├─> Statistics: table1 = 1M rows, table2 = 10M rows           │  │
│  │     └─> Reality: table1 = 100M rows, table2 = 1M rows             │  │
│  │                                                                     │  │
│  │  2. CBO generates plan based on wrong estimates                    │  │
│  │     ├─> Chooses: table1 (small) JOIN table2 (large)               │  │
│  │     ├─> Uses: Broadcast Join (broadcasts large table)              │  │
│  │     └─> Result: Slow query, memory pressure, timeouts             │  │
│  │                                                                     │  │
│  │  3. Query Feedback Solution:                                       │  │
│  │     ├─> Records actual execution statistics                         │  │
│  │     ├─> Detects discrepancy between estimates and reality           │  │
│  │     ├─> Generates tuning guide for future queries                  │  │
│  │     └─> CBO applies guide to correct the plan                      │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Key Capabilities:**

- **Automatic Learning**: Records execution statistics (input rows, output rows) for each PlanNode during query execution
- **Intelligent Analysis**: Analyzes slow queries and queries with execution anomalies to identify optimization opportunities
- **Dynamic Optimization**: Applies tuning guides to correct query plans before execution, eliminating the need for manual intervention
- **Performance Evaluation**: Compares optimized plan execution time with original plan to measure improvement

**Version Support:**

Query Feedback is available from StarRocks v3.4.0 onwards.

## 2. Query Feedback Workflow: Observation, Analysis, and Optimization

The Query Feedback mechanism operates in three distinct stages, creating a continuous learning and optimization cycle.

**Three-Stage Workflow:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Stage 1: Observation                                  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  During Query Execution:                                           │  │
│  │                                                                     │  │
│  │  BE/CN Nodes:                                                      │  │
│  │  ├─> Execute query plan fragments                                  │  │
│  │  ├─> Record metrics for each PlanNode:                             │  │
│  │  │   ├─> InputRows: actual input row count                         │  │
│  │  │   ├─> OutputRows: actual output row count                      │  │
│  │  │   ├─> Execution time                                            │  │
│  │  │   └─> Resource consumption                                     │  │
│  │  └─> Send statistics back to FE                                    │  │
│  │                                                                     │  │
│  │  FE:                                                               │  │
│  │  └─> Collects execution statistics from all nodes                 │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Stage 2: Analysis                                     │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  After Query Execution (Before Result Return):                    │  │
│  │                                                                     │  │
│  │  FE Analysis:                                                      │  │
│  │  ├─> Identifies slow queries (> slow_query_analyze_threshold)     │  │
│  │  ├─> Compares execution statistics with CBO estimates             │  │
│  │  ├─> Detects statistical discrepancies:                            │  │
│  │  │   ├─> Overestimated row counts                                 │  │
│  │  │   ├─> Underestimated row counts                                │  │
│  │  │   └─> Incorrect join order assumptions                         │  │
│  │  ├─> Identifies optimization opportunities:                       │  │
│  │  │   ├─> Wrong join order (small table on wrong side)             │  │
│  │  │   ├─> Wrong join method (broadcast vs shuffle)                │  │
│  │  │   └─> Inefficient aggregation mode                             │  │
│  │  └─> Generates tuning guide with optimization strategies           │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Stage 3: Optimization                                 │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  During Next Query Planning:                                       │  │
│  │                                                                     │  │
│  │  CBO Planning:                                                    │  │
│  │  ├─> Generates physical plan as usual                             │  │
│  │  ├─> Searches for applicable tuning guide                          │  │
│  │  ├─> If guide found:                                              │  │
│  │  │   ├─> Applies guide strategies to plan:                         │  │
│  │  │   │   ├─> Adjusts join order                                  │  │
│  │  │   │   ├─> Changes join method (broadcast → shuffle)            │  │
│  │  │   │   └─> Enforces pre-aggregation mode                        │  │
│  │  │   └─> Corrects problematic plan sections                       │  │
│  │  └─> Executes optimized plan                                      │  │
│  │                                                                     │  │
│  │  Performance Evaluation:                                           │  │
│  │  └─> Compares execution time: optimized vs original               │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Key Metrics Collected:**

- **InputRows**: Actual number of rows processed by each PlanNode
- **OutputRows**: Actual number of rows produced by each PlanNode
- **Execution Time**: Time spent in each operator
- **Resource Consumption**: Memory and CPU usage per operator

**Analysis Criteria:**

The system analyzes queries based on:

1. **Slow Query Threshold**: Queries exceeding `slow_query_analyze_threshold` (default: 5 seconds) are automatically analyzed
2. **Statistical Discrepancy**: Large differences between estimated and actual row counts trigger analysis
3. **Manual Marking**: Queries can be manually marked for analysis using `ALTER PLAN ADVISOR ADD`

**Tuning Guide Generation:**

Tuning guides contain:

- **Query ID**: Unique identifier for the query
- **Problem Identification**: Description of the detected issue (e.g., "left child statistics overestimated")
- **Optimization Strategy**: Specific actions to take (e.g., "switch from broadcast to shuffle join")
- **Applicability**: Conditions under which the guide applies (exact query match required)

## 3. Query Plan Advisor: Configuration and Usage

Query Plan Advisor is the user-facing component of Query Feedback that provides control over the analysis and optimization process.

**Configuration:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Query Plan Advisor Configuration                      │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  System Variables:                                                 │  │
│  │                                                                     │  │
│  │  enable_plan_advisor (Default: true)                              │  │
│  │  ├─> Controls whether Query Plan Advisor is enabled               │  │
│  │  └─> When enabled: automatically analyzes slow queries             │  │
│  │                                                                     │  │
│  │  enable_plan_analyzer (Default: false)                             │  │
│  │  ├─> Controls automatic analysis for ALL queries                  │  │
│  │  └─> When true: analyzes every query (not just slow ones)         │  │
│  │                                                                     │  │
│  │  FE Configuration:                                                 │  │
│  │                                                                     │  │
│  │  slow_query_analyze_threshold (Default: 5 seconds)               │  │
│  │  ├─> Execution time threshold for slow query detection            │  │
│  │  └─> Queries exceeding this are automatically analyzed            │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Usage Modes:**

### 3.1. Automatic Analysis for Slow Queries (Default)

By default, Query Plan Advisor automatically analyzes queries that exceed the slow query threshold:

```sql
-- No action needed - automatic for slow queries
-- System variable enable_plan_advisor = true (default)
```

### 3.2. Manual Analysis for Specific Queries

You can manually request analysis for a specific query, even if it doesn't exceed the slow query threshold:

```sql
ALTER PLAN ADVISOR ADD <query_statement>
```

**Example:**

```sql
ALTER PLAN ADVISOR ADD 
SELECT COUNT(*) FROM (
    SELECT * FROM c1_skew_left_over t1 
    JOIN (SELECT * FROM c1_skew_left_over WHERE c1 = 'c') t2 
    ON t1.c2 = t2.c2 
    WHERE t1.c1 > 'c'
) t;
```

### 3.3. Automatic Analysis for All Queries

To enable analysis for all queries (not just slow ones), set:

```sql
SET enable_plan_analyzer = true;
```

**Note**: This may increase analysis overhead for high-frequency queries.

### 3.4. Viewing Tuning Guides

Each FE node maintains its own cache of tuning guides. To view tuning guides on the current FE:

```sql
SHOW PLAN ADVISOR
```

**Output Format:**

The output includes:
- Query ID
- Query statement
- Tuning guide details
- Optimization strategies

### 3.5. Verifying Tuning Guide Application

To check if a tuning guide is being applied to a query, use `EXPLAIN`:

```sql
EXPLAIN SELECT COUNT(*) FROM (
    SELECT * FROM c1_skew_left_over t1 
    JOIN (SELECT * FROM c1_skew_left_over WHERE c1 = 'c') t2 
    ON t1.c2 = t2.c2 
    WHERE t1.c1 > 'c'
) t;
```

**Expected Output:**

```
+-----------------------------------------------------------------------------------------------+
| Explain String                                                                                |
+-----------------------------------------------------------------------------------------------+
| Plan had been tuned by Plan Advisor.                                                          |
| Original query id:8e010cf4-b178-11ef-8aa4-8a5075cec65e                                        |
| Original time cost: 148 ms                                                                    |
| 1: LeftChildEstimationErrorTuningGuide                                                        |
| Reason: left child statistics of JoinNode 5 had been overestimated.                           |
| Advice: Adjust the distribution join execution type and join plan to improve the performance. |
|                                                                                               |
| PLAN FRAGMENT 0                                                                               |
|  OUTPUT EXPRS:9: count                                                                        |
|   PARTITION: UNPARTITIONED                                                                    |
+-----------------------------------------------------------------------------------------------+
```

The message `Plan had been tuned by Plan Advisor` indicates that a tuning guide was applied.

### 3.6. Managing Tuning Guides

**Delete a Specific Tuning Guide:**

```sql
ALTER PLAN ADVISOR DROP <query_id>
```

**Example:**

```sql
ALTER PLAN ADVISOR DROP "8e010cf4-b178-11ef-8aa4-8a5075cec65e";
```

**Clear All Tuning Guides:**

```sql
TRUNCATE PLAN ADVISOR
```

This clears all tuning guides on the current FE node.

## 4. Use Cases: Common Optimization Scenarios

Query Feedback is particularly effective for optimizing three common scenarios where statistics inaccuracy leads to poor query plans.

### 4.1. Case 1: Incorrect Join Order

**Problem:**

When statistics are outdated, CBO may incorrectly estimate table sizes and choose the wrong join order.

**Original Bad Plan:**

```sql
-- CBO incorrectly estimates: small_table (1M rows) JOIN large_table (10M rows)
-- Plan: small_table (left) INNER JOIN large_table (right) [BROADCAST]
-- Reality: small_table (100M rows), large_table (1M rows)
```

**Optimized Plan:**

```sql
-- After Query Feedback correction:
-- Plan: large_table (left) INNER JOIN small_table (right) [BROADCAST]
-- Correctly broadcasts the smaller table
```

**How Query Feedback Fixes It:**

1. **Observation**: Records actual input rows for both join children
2. **Analysis**: Detects that left child (small_table) has 100M rows, not 1M
3. **Optimization**: Generates tuning guide to swap join order
4. **Application**: CBO applies guide, placing larger table on left, smaller on right

**Key Files to Reference:**
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/rule/join/` - Join reordering logic
- `fe/fe-core/src/main/java/com/starrocks/planner/PlanAdvisor.java` - Plan advisor implementation

### 4.2. Case 2: Incorrect Join Execution Method

**Problem:**

When data is skewed or statistics are inaccurate, CBO may choose broadcast join when shuffle join would be more efficient.

**Original Bad Plan:**

```sql
-- CBO estimates: table1 (10M rows) JOIN table2 (10M rows)
-- Plan: table1 INNER JOIN table2 [BROADCAST]
-- Reality: Both tables are 100M+ rows, broadcast causes memory pressure
```

**Optimized Plan:**

```sql
-- After Query Feedback correction:
-- Plan: table1 [SHUFFLE] INNER JOIN table2 [SHUFFLE]
-- Uses shuffle join to distribute load across nodes
```

**How Query Feedback Fixes It:**

1. **Observation**: Records actual row counts and execution metrics
2. **Analysis**: Detects that broadcast join causes memory pressure and slow execution
3. **Optimization**: Generates tuning guide to switch from broadcast to shuffle join
4. **Application**: CBO applies guide, using shuffle join for large tables

**Key Files to Reference:**
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/operator/physical/PhysicalHashJoinOperator.java` - Join operator
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/cost/JoinCostModel.java` - Join cost estimation

### 4.3. Case 3: Inefficient First-Phase Pre-aggregation Mode

**Problem:**

For data with good aggregation potential, the `auto` mode may not aggregate enough data in the first phase, missing performance opportunities.

**Symptom:**

- Local aggregation only processes small amount of data
- Global aggregation processes most of the data
- Missed opportunity for early data reduction

**Solution:**

Query Feedback analyzes aggregation effectiveness:

1. **Observation**: Records input/output rows for both local and global aggregations
2. **Analysis**: Detects high aggregation potential (large reduction in row count)
3. **Optimization**: Generates tuning guide to enforce `pre_aggregation` mode
4. **Application**: CBO applies guide, maximizing aggregation in first phase

**Key Files to Reference:**
- `be/src/exec/pipeline/aggregate/aggregate_operator.cpp` - Aggregation operator
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/operator/physical/PhysicalHashAggregateOperator.java` - Aggregation planning

## 5. Architecture: How Query Feedback Works Internally

Query Feedback involves coordination between FE (Frontend) and BE/CN (Backend/Compute Node) components.

**System Architecture:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Query Feedback Architecture                            │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │                    Frontend (FE)                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Query Plan Advisor                                          │  │  │
│  │  │  ├─> PlanAdvisor: Main coordinator                          │  │  │
│  │  │  │   ├─> Receives execution statistics from BE/CN           │  │  │
│  │  │  │   ├─> Analyzes queries for optimization opportunities   │  │  │
│  │  │  │   ├─> Generates tuning guides                            │  │  │
│  │  │  │   └─> Stores guides in memory cache                       │  │  │
│  │  │  │                                                           │  │  │
│  │  │  ├─> TuningGuide: Optimization strategies                   │  │  │
│  │  │  │   ├─> JoinOrderTuningGuide                               │  │  │
│  │  │  │   ├─> JoinMethodTuningGuide                               │  │  │
│  │  │  │   └─> AggregationTuningGuide                              │  │  │
│  │  │  │                                                           │  │  │
│  │  │  └─> CBO Integration:                                        │  │  │
│  │  │      ├─> QueryOptimizer checks for applicable guides        │  │  │
│  │  │      └─> Applies guides during plan generation              │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                     │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Statistics Collection                                        │  │  │
│  │  │  ├─> Receives QueryStatistics from BE/CN                     │  │  │
│  │  │  ├─> Aggregates statistics across nodes                      │  │  │
│  │  │  └─> Stores statistics for analysis                          │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                          │                                                 │
│                          │ Query Statistics (bRPC)                        │
│                          │                                                 │
│  ┌────────────────────────┴─────────────────────────────────────────────┐  │
│  │                    Backend/Compute Node (BE/CN)                      │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Execution Statistics Collection                              │  │  │
│  │  │  ├─> PlanFragmentExecutor tracks execution                    │  │  │
│  │  │  ├─> Records metrics per PlanNode:                            │  │  │
│  │  │  │   ├─> InputRows: actual input row count                   │  │  │
│  │  │  │   ├─> OutputRows: actual output row count                 │  │  │
│  │  │  │   ├─> ExecutionTime: operator execution time             │  │  │
│  │  │  │   └─> MemoryUsage: operator memory consumption           │  │  │
│  │  │  │                                                           │  │  │
│  │  │  └─> Sends QueryStatistics to FE                            │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Data Flow:**

1. **Query Execution**: BE/CN nodes execute query plan fragments
2. **Statistics Collection**: Each node records execution statistics for PlanNodes
3. **Statistics Reporting**: Statistics are sent to FE via bRPC
4. **Analysis**: FE analyzes statistics and generates tuning guides
5. **Guide Storage**: Tuning guides are stored in FE's in-memory cache
6. **Guide Application**: CBO checks for applicable guides during planning
7. **Plan Optimization**: CBO applies guides to optimize query plans

**Tuning Guide Matching:**

Tuning guides are matched to queries based on:

- **Exact Query Match**: The query statement must match exactly (including whitespace)
- **Query Fingerprint**: A hash of the normalized query is used for matching
- **Plan Structure**: The plan structure must match for the guide to be applicable

**Memory Management:**

- **In-Memory Cache**: Tuning guides are stored in an in-memory cache structure
- **Cache Limit**: Default limit is 300 tuning guides per FE node
- **Eviction Policy**: When the limit is exceeded, expired guides are automatically evicted
- **No Persistence**: Historical tuning guides are not persisted across FE restarts

**Key Files to Reference:**
- `fe/fe-core/src/main/java/com/starrocks/planner/PlanAdvisor.java` - Main plan advisor implementation
- `fe/fe-core/src/main/java/com/starrocks/planner/TuningGuide.java` - Tuning guide definitions
- `fe/fe-core/src/main/java/com/starrocks/qe/QueryStatistics.java` - Statistics data structures
- `be/src/runtime/plan_fragment_executor.cpp` - Statistics collection in BE

## 6. Limitations and Considerations

While Query Feedback is a powerful optimization mechanism, it has several limitations that users should be aware of.

**Limitations:**

1. **Exact Query Matching Required**
   - Tuning guides only apply to queries that match exactly
   - Queries with the same pattern but different parameters require separate guides
   - Example: `SELECT * FROM table WHERE id = 1` and `SELECT * FROM table WHERE id = 2` are treated as different queries

2. **No Cross-FE Synchronization**
   - Each FE node manages its Query Plan Advisor independently
   - Tuning guides are not synchronized across FE nodes
   - If the same query is submitted to different FE nodes, tuning results may vary
   - Recommendation: Use a load balancer that routes queries to the same FE node for consistency

3. **In-Memory Cache Only**
   - Tuning guides are stored in memory and not persisted
   - Guides are lost when FE restarts
   - Cache limit: 300 guides per FE (default)
   - Expired guides are automatically evicted when limit is exceeded

4. **Conservative Tuning Thresholds**
   - Current tuning thresholds are relatively conservative
   - May not catch all optimization opportunities
   - Users are encouraged to manually analyze queries if issues are observed

5. **Query-Specific Optimization**
   - Guides are specific to individual queries
   - Cannot generalize optimizations across query patterns
   - Each query variant needs its own guide

**Best Practices:**

1. **Monitor Query Performance**
   - Use Query Profile to identify slow queries
   - Check EXPLAIN output for plan issues
   - Manually analyze queries that show performance problems

2. **Consistent FE Routing**
   - Use session affinity in load balancers
   - Route queries from the same session to the same FE node
   - Ensures tuning guides are consistently applied

3. **Regular Statistics Updates**
   - Keep table statistics up to date
   - Reduces the need for Query Feedback corrections
   - Improves CBO plan quality from the start

4. **Manual Analysis for Critical Queries**
   - Use `ALTER PLAN ADVISOR ADD` for important queries
   - Ensures analysis even if query doesn't exceed slow query threshold
   - Useful for queries that run frequently

5. **Monitor Tuning Guide Effectiveness**
   - Use EXPLAIN to verify guide application
   - Compare execution times before and after tuning
   - Remove ineffective guides if needed

**Key Files to Reference:**
- `fe/fe-core/src/main/java/com/starrocks/planner/PlanAdvisor.java` - Implementation details
- `fe/fe-core/src/main/java/com/starrocks/qe/DefaultCoordinator.java` - Query coordination
- `docs/en/using_starrocks/query_feedback.md` - User documentation

## 7. Integration with Cost-Based Optimizer

Query Feedback integrates seamlessly with StarRocks' Cost-Based Optimizer, providing a feedback loop that continuously improves plan quality.

**CBO Integration Points:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    CBO with Query Feedback Integration                   │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Query Planning Phase                                              │  │
│  │                                                                     │  │
│  │  1. Logical Plan Generation                                       │  │
│  │     └─> CBO generates logical plan as usual                       │  │
│  │                                                                     │  │
│  │  2. Cost Estimation                                                │  │
│  │     ├─> CBO estimates costs using statistics                       │  │
│  │     └─> May be inaccurate if statistics are outdated              │  │
│  │                                                                     │  │
│  │  3. Physical Plan Generation                                       │  │
│  │     ├─> CBO generates physical plan                               │  │
│  │     └─> Plan may be suboptimal due to wrong estimates             │  │
│  │                                                                     │  │
│  │  4. Tuning Guide Lookup                                            │  │
│  │     ├─> CBO searches for applicable tuning guide                 │  │
│  │     ├─> Matches query fingerprint                                 │  │
│  │     └─> If found: applies guide strategies                        │  │
│  │                                                                     │  │
│  │  5. Plan Correction                                                │  │
│  │     ├─> Adjusts join order based on guide                        │  │
│  │     ├─> Changes join method based on guide                        │  │
│  │     └─> Enforces aggregation mode based on guide                  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                          │                                                 │
│                          ▼                                                 │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Query Execution Phase                                             │  │
│  │                                                                     │  │
│  │  1. Execute Plan                                                   │  │
│  │     └─> BE/CN executes optimized plan                             │  │
│  │                                                                     │  │
│  │  2. Collect Statistics                                             │  │
│  │     ├─> Records actual execution metrics                          │  │
│  │     └─> Sends statistics to FE                                    │  │
│  │                                                                     │  │
│  │  3. Analysis (if needed)                                           │  │
│  │     ├─> FE analyzes execution statistics                           │  │
│  │     ├─> Compares with CBO estimates                               │  │
│  │     └─> Generates new tuning guide if needed                      │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**How CBO Uses Tuning Guides:**

1. **Plan Generation**: CBO generates a physical plan using standard cost-based optimization
2. **Guide Matching**: After plan generation, CBO checks if a tuning guide exists for the query
3. **Guide Application**: If a guide is found, CBO applies the guide's strategies:
   - **Join Order Correction**: Swaps join children if guide indicates wrong order
   - **Join Method Correction**: Changes from broadcast to shuffle (or vice versa) if needed
   - **Aggregation Mode Enforcement**: Enforces pre-aggregation mode if guide indicates high aggregation potential
4. **Plan Execution**: The corrected plan is executed
5. **Feedback Loop**: Execution statistics are collected and may generate new guides

**Statistics Comparison:**

Query Feedback compares:

- **Estimated Rows** (from CBO) vs **Actual Rows** (from execution)
- **Estimated Costs** (from CBO) vs **Actual Costs** (from execution)
- **Plan Choices** (from CBO) vs **Optimal Choices** (inferred from execution)

When discrepancies are detected, tuning guides are generated to correct future plans.

**Key Files to Reference:**
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/QueryOptimizer.java` - CBO main entry point
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/cost/CostModel.java` - Cost estimation
- `fe/fe-core/src/main/java/com/starrocks/planner/PlanAdvisor.java` - Plan advisor integration
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/statistics/Statistics.java` - Statistics management

## 8. Performance Impact and Overhead

Query Feedback is designed to have minimal overhead while providing significant performance benefits for problematic queries.

**Overhead Components:**

1. **Statistics Collection Overhead**
   - Minimal: Statistics are collected as part of normal execution
   - No additional I/O or network overhead
   - Small memory overhead for storing statistics

2. **Analysis Overhead**
   - Only analyzes slow queries (by default) or manually marked queries
   - Analysis happens asynchronously after query execution
   - Does not block query result return

3. **Guide Storage Overhead**
   - In-memory cache with limited size (300 guides default)
   - Small memory footprint per guide
   - Automatic eviction prevents unbounded growth

4. **Guide Lookup Overhead**
   - Fast hash-based lookup during planning
   - Negligible impact on planning time

**Performance Benefits:**

1. **Query Speedup**
   - Can improve query performance by 2-10x for queries with bad plans
   - Eliminates need for manual query tuning
   - Reduces query timeouts and failures

2. **Resource Efficiency**
   - Prevents excessive memory usage from wrong join methods
   - Reduces network overhead from inefficient data distribution
   - Improves cluster resource utilization

3. **System Stability**
   - Prevents system crashes from memory pressure
   - Reduces query failures from timeouts
   - Improves overall system reliability

**Configuration Tuning:**

To balance overhead and benefits:

- **slow_query_analyze_threshold**: Increase to reduce analysis frequency (less overhead, fewer optimizations)
- **enable_plan_analyzer**: Set to `false` to only analyze slow queries (default behavior)
- **Cache Size**: Adjust if needed (currently fixed at 300)

**Key Files to Reference:**
- `fe/fe-core/src/main/java/com/starrocks/planner/PlanAdvisor.java` - Overhead considerations
- `fe/fe-core/src/main/java/com/starrocks/qe/QueryStatistics.java` - Statistics collection efficiency


# StarRocks Query Optimizers

This document describes how query optimization works in StarRocks, the optimizer frameworks, rule types, scalar expression rewriting, and provides a summary of all optimizers.


## 1. How Query Optimization Works

Query optimization in StarRocks runs in the Frontend (FE) after semantic analysis. The planner turns the analyzed AST into a **logical plan** (`OptExpression` tree), then the **optimizer** rewrites and implements it into a **physical plan** that the Backend (BE) executes.

### 1.1 High-Level Flow

1. **Entry**: `StatementPlanner` builds a logical plan and calls `OptimizerFactory.create(optimizerContext).optimize(logicalPlan.getRoot(), requiredProperty, requiredColumns)`.

2. **Optimizer selection**: `OptimizerFactory` returns one of:
    - **QueryOptimizer** (default): full cost-based optimization.
    - **ShortCircuitOptimizer**: when short-circuit is enabled.
    - **SPMOptimizer**: when using a baseline (SPM) plan.

3. **QueryOptimizer phases** (when not rule-only):
    - **Prepare**: Create `Memo`, collect logical OlapScan operators, prepare MV rewrite.
    - **MV text rewrite**: Apply `TextMatchBasedRewriteRule` for text-match–based materialized view rewrite; optionally `SeparateProjectRule`.
    - **Rule-based rewrite**: `rewriteAndValidatePlan()` applies a fixed sequence of logical rewrite rules (see below).
    - **Memo init**: Copy the rewritten logical tree into the Memo (groups and group expressions).
    - **Derive logical properties**: Re-derive logical properties for all groups (e.g. after column pruning).
    - **Cost-based optimize**: Run tasks (e.g. `OptimizeGroupTask`) that apply transformation and implementation rules inside the Memo, compute costs, and pick best physical plans.
    - **Extract best plan**: Walk the Memo to get the lowest-cost physical tree for the root group.
    - **Physical rewrite**: Apply tree rules (e.g. `PreAggregateTurnOnRule`, `ExchangeSortToMergeRule`, `ScalarOperatorsReuseRule`, etc.).
    - **Dynamic rewrite**: Optional skew join elimination, tuning guides, etc.
    - **Validation**: Plan validator and MV rewrite validator run on the final plan.

4. **Rule-only mode**: If the optimizer is configured as rule-based (e.g. for MV rewrite), only `rewriteAndValidatePlan()` is run and no Memo or cost-based search is used.

### 1.2 Logical Rule Rewrite Sequence (Summary)

Inside `logicalRuleRewrite()`, the optimizer runs a long, ordered sequence of steps, including:

- CTE: eliminate constant CTE, inline single-use CTE, collect CTE.
- Iceberg / external: `IcebergPartitionsTableRewriteRule`.
- Aggregates: `AGGREGATE_REWRITE_RULES`, then later `EliminateAggRule`, `EliminateAggFunctionRule`, `MergeTwoAggRule`, etc.
- Subqueries: `PUSH_DOWN_SUBQUERY_RULES`, `SUBQUERY_EXTRACT_CORRELATION_PREDICATE_RULES`, `SUBQUERY_REWRITE_TO_WINDOW_RULES`, `ExtractRangePredicateFromScalarApplyRule`, `SUBQUERY_REWRITE_TO_JOIN_RULES`, `ApplyExceptionRule`.
- TVR: `TVR_REWRITE_RULES` (when IVM refresh is enabled).
- Range predicates: `FINE_GRAINED_RANGE_PREDICATE_RULES` (optional).
- Transparent MV: `MaterializedViewTransparentRewriteRule`.
- Predicates: `PUSH_DOWN_PREDICATE_RULES`, `SchemaTableEvaluateRule`, `ELIMINATE_OP_WITH_CONSTANT_RULES`, `ConvertToEqualForNullRule`.
- Columns: `PRUNE_COLUMNS_RULES` (multiple times), `PRUNE_UKFK_JOIN_RULES`.
- Windows: `PushDownPredicateRankingWindowRule`, `PruneEmptyWindowRule`, `PushDownLimitRankingWindowRule`, etc.
- Join on expressions: `PushDownJoinOnExpressionToChildProject`, `PushDownAsofJoinTemporalExpressionToChildProject`.
- Table pruning: partition/distribution prune, uniqueness-based and RBO table prune, `ReorderJoinRule` (when RBO table prune is on), `PrimaryKeyUpdateTableRule` (optional), `PRUNE_COLUMNS_RULES`, `PUSH_DOWN_PREDICATE_RULES`.
- Limits: `MERGE_LIMIT_RULES`, `PushDownProjectLimitRule`, `HoistHeavyCostExprsUponTopnRule`.
- Grouping sets, meta scan prep, skew join (v1), Iceberg equality delete, subfield pruning, `PRUNE_PROJECT_RULES`.
- CTE: collect/inline/push limit and predicate, `ForceCTEReuseRule`.
- Materialized views: `SplitScanORToUnionRule` (optional), `MaterializedViewRule`, then more predicate/partition/empty/MV compensation rules.
- Join predicate: `OnPredicateMoveAroundRule`, again `PUSH_DOWN_PREDICATE_RULES`, `PartitionColumnMinMaxRewriteRule`, `PARTITION_PRUNE_RULES`, `RewriteMultiDistinctRule`, `PRUNE_EMPTY_OPERATOR_RULES`, `CTEProduceAddProjectionRule`, `PRUNE_PROJECT_RULES`.
- Aggregates: `ArrayDistinctAfterAggRule` (optional), distinct/inner-join to semi, then `pushDownAggregation()` (distinct below window, push-down aggregate, semi-join deduplicate), `MERGE_LIMIT_RULES`.
- More rewrites: `JsonPathRewriteRule` (meta scan), `META_SCAN_REWRITE_RULES`, `PartitionColumnValueOnlyOnScanRule`, `RewriteUnnestBitmapRule`, `MergeProjectWithChildRule`, `EliminateSortColumnWithEqualityPredicateRule`, `PushDownTopNBelowOuterJoinRule`, `INTERSECT_REWRITE_RULES`, `RemoveAggregationFromAggTable`, `SplitScanORToUnionRule`, `PushDownTopNBelowUnionRule`, `SplitTopNAggregateRule`.
- MV rule-based stage 2 (partition prune again, etc.).
- Final logical rewrites: `JsonPathRewriteRule` (OlapScan), `RewriteMinMaxByMonotonicFunctionRule`, `RewriteSimpleAggToHDFSScanRule`, `GroupByCountDistinctRewriteRule`, `DeriveRangeJoinPredicateRule`, `UnionToValuesRule`, `VECTOR_REWRITE_RULES`, `SplitJoinORToUnionRule`, `PullUpScanPredicateRule` (optional), `SimplifyCaseWhenPredicateRule`.

The result of this sequence is a single logical tree that is then put into the Memo for cost-based optimization (or returned as-is in rule-only mode).


## 2. Query Optimizer Frameworks

### 2.1 Optimizer Entry and Context

- **Optimizer** (abstract): Holds `OptimizerContext`, exposes `optimize(tree, requiredProperty, requiredColumns)` and helpers like `deriveLogicalProperty(root)`.
- **OptimizerFactory**: Creates the appropriate optimizer (`QueryOptimizer`, `ShortCircuitOptimizer`, or `SPMOptimizer`) and initializes `OptimizerContext` (connect context, column ref factory, optimizer options). Options can disable specific rules via session variables.
- **OptimizerContext**: Holds connect context, column ref factory, optimizer options, Memo, task scheduler, statement, MV transformer context, query materialization context, CTE context, and rule set. Shared across the whole optimization run.
- **OptimizerOptions**: Controls rule-based vs cost-based, short-circuit, baseline plan, and per-rule disable flags.

### 2.2 Rule Framework

- **Rule** (abstract): Describes a transformation from one expression to a logically equivalent one. Each rule has a **RuleType**, a **Pattern** (structure of the expression it applies to), and implements `transform(OptExpression, OptimizerContext) -> List<OptExpression>`. It can define `promise()`, `predecessorRules()`, `successorRules()`, and `check()`.
- **TransformationRule**: Logical → logical; default `promise() = 1`.
- **ImplementationRule**: Logical → physical; `promise() = 2` so they are preferred when scheduling.
- **RuleSet**: Holds the list of **implementation rules** and **transformation rules** used in the Memo phase. It also defines many **combination rules** (see below) and provides methods to add join rules, CBO table prune, MV rewrite, and join implementation (hash/merge/nestloop/auto).

### 2.3 Memo and Group

- **Memo**: Encodes the search space of plan alternatives. It stores **Groups** and **GroupExpressions**, provides memoization and duplicate detection, and is used for property and cost management. The root of the optimized tree is the root group.
- **Group**: Contains multiple **GroupExpressions** that are logically equivalent. It stores the best physical plan (and cost) per required physical property.
- **GroupExpression**: One operator (logical or physical) in a group, with child groups. Used for applying rules and cost computation.

The Memo is initialized from the logical tree after `rewriteAndValidatePlan()`. Logical properties are derived for all groups; then tasks run to apply transformation and implementation rules, compute costs, and select the best physical plan for the root.

### 2.4 Task Scheduler and Tasks

- **TaskScheduler**: Maintains a **stack** of **OptimizerTask** and runs them with `executeTasks(context)`. There is no thread pool; execution is single-threaded and synchronous.
- **Rewrite tasks** (used during logical rewrite, outside Memo):
    - **rewriteIterative(tree, context, rule)**: Apply rule until fixpoint.
    - **rewriteOnce(tree, context, rule)**: Apply rule one pass.
    - **rewriteAtMostOnce(tree, context, rule)**: Apply at most once.
    - **rewriteDownTop(tree, context, rule)**: Apply rule top-down.
- **Memo tasks** (used in cost-based phase): e.g. **OptimizeGroupTask** (apply rules for a group, enforce properties, cost), **EnforceAndCostTask**, **ApplyRuleTask**. The scheduler pushes and executes these to explore the search space and fill in best plans per group.

#### 2.4.1 Why a stack? (Execution model)

The scheduler uses a **stack** (LIFO) as the single pending-work list. The execution loop is:

1. Push initial task(s) (e.g. one `RewriteTreeTask` or one `OptimizeGroupTask`).
2. **While** the stack is not empty: **pop** one task, **execute** it; during execution the task may **push** more tasks.
3. Repeat until the stack is empty.

**Why not a simple for loop over a fixed list?** The full set of work is not known up front. When a task runs, it can push **zero, one, or many** new tasks. For example:

- **OptimizeGroupTask** pushes one `OptimizeExpressionTask` per logical expression and one `EnforceAndCostTask` per physical expression for that group.
- **OptimizeExpressionTask** pushes multiple `ApplyRuleTask`s, then `DeriveStatsTask`, then multiple `ExploreGroupTask`s for child groups.

So the “list” of work grows as tasks execute. The stack is that growing list; the only loop is “pop → execute (may push more) → repeat.”

**Why a stack (LIFO) instead of a queue (FIFO)?** LIFO gives **depth-first** order. Tasks push children in **reverse** order (e.g. last index to first); when the scheduler pops, the first-pushed child runs last and the last-pushed runs first. That way the optimizer goes deep into one branch before siblings—e.g. optimize the first child group fully before the next. So the stack + push order encodes the intended traversal without extra control logic.

#### 2.4.2 Iterative (fixpoint) rewrites

**rewriteIterative(tree, context, rule)** applies a rule repeatedly until the tree stops changing (fixpoint). It works by having the **same loop** as above: the running task can push another task for the whole tree.

1. **rewriteIterative** pushes a single **RewriteTreeTask** (whole tree, `onlyOnce = false`) and calls **executeTasks**.
2. The loop pops that task and runs it. **RewriteTreeTask.execute()** does one full top-down pass over the tree, applying the rule where it matches. If the tree changed in that pass (`change > 0`) and the task is not “only once,” it **pushes** a new **RewriteTreeTask** for the whole tree.
3. The loop continues: it pops the new task, runs another full pass, and again maybe pushes another task. When a pass makes **no** changes, no task is pushed, the stack becomes empty, and the loop exits.

So “iterative” is implemented by **re-enqueueing a full-tree task** after each changing pass, and the **same stack + single loop** handles both the first pass and all subsequent passes. Example with a predicate-pushdown rule:

| Step | Action | Stack (top on right) |
|------|--------|----------------------|
| 1 | rewriteIterative pushes T1, executeTasks runs | `[T1]` |
| 2 | Pop T1; one pass, tree changed → push T2 | `[T2]` |
| 3 | Pop T2; another pass, tree changed → push T3 | `[T3]` |
| 4 | Pop T3; pass finds nothing to do, no push | `[]` |
| 5 | Stack empty → exit | — |

**rewriteOnce** uses the same mechanism but creates a **RewriteTreeTask** with `onlyOnce = true`, so that task never pushes a follow-up; exactly one pass runs.

#### 2.4.3 OptimizerTask implementations

All task classes that extend **OptimizerTask** (or **RewriteTreeTask**), and what they do:

| Task class | Phase | Description |
|------------|--------|--------------|
| **RewriteTreeTask** | Logical rewrite (outside Memo) | Applies rule(s) top-down to an `OptExpression` tree. One full pass; if tree changed and not `onlyOnce`, pushes another `RewriteTreeTask` for fixpoint. |
| **RewriteAtMostOnceTask** | Logical rewrite | Extends `RewriteTreeTask` with `onlyOnce = true` and stops descending once any node is rewritten (at most one rewrite per root). |
| **RewriteDownTopTask** | Logical rewrite | Extends `RewriteTreeTask`; applies rules in bottom-up order (rewrite children first, then apply rules at current node). Used by `rewriteDownTop()`. |
| **OptimizeGroupTask** | Memo (cost-based) | Optimizes one **Group**: pushes one **OptimizeExpressionTask** per logical expression and one **EnforceAndCostTask** per physical expression. Prunes when group cost LB ≥ context UB or group already optimized. |
| **OptimizeExpressionTask** | Memo | Optimizes one **GroupExpression**: pushes one **ApplyRuleTask** per applicable rule, then **DeriveStatsTask**, then **ExploreGroupTask** per child group (reverse order for depth-first). |
| **ApplyRuleTask** | Memo | Applies a single **Rule** to one GroupExpression (pattern bind, transform). For each new expression: if logical → push **OptimizeExpressionTask**; if physical → push **EnforceAndCostTask**. Marks rule as explored on the expression. |
| **EnforceAndCostTask** | Memo | Costs one **physical** GroupExpression, enforces required properties, adds enforcers if needed. Pushes **OptimizeGroupTask** for child groups (with required properties); updates group best plan/cost. Supports multiple child property sets; can clone and re-push itself for enumeration. |
| **DeriveStatsTask** | Memo | Derives statistics for a GroupExpression (via **StatisticsCalculator**) so it can be costed. Requires children’s stats already derived. Pushes no further tasks. |
| **ExploreGroupTask** | Memo | Explores a Group (logical transformations only): pushes **OptimizeExpressionTask** with `isExplore = true` for each logical expression. Sets group explored when done. |
| **PrepareCollectMetaTask** | Prepare (before Memo) | Pre-collects metadata for logical scan operators that support it (e.g. external tables). Collects scans from the plan tree and may use a thread pool for parallel metadata fetch. Pushes no optimizer tasks. |

### 2.5 Combination Rules (RuleSet)

Many “rules” are actually **CombinationRule**: a named group of sub-rules applied together. Examples:

- **MERGE_LIMIT_RULES**: Merge/split/push down limit (e.g. `PushDownProjectLimitRule`, `EliminateLimitZeroRule`, `MergeLimitWithSortRule`, `SplitLimitRule`, `PushDownLimitJoinRule`, …).
- **PARTITION_PRUNE_RULES**: `PartitionPruneRule`, `DistributionPruneRule`, `ExternalScanPartitionPruneRule`, `LimitPruneTabletsRule`.
- **PRUNE_COLUMNS_RULES**: Prune columns at scan, project, filter, agg, topn, join, window, union, intersect, except, repeat, values, table function, CTE consume, and UKFK group-by.
- **PUSH_DOWN_PREDICATE_RULES**: Cast to empty, prune true filter, push predicate to CTE anchor, scan, agg, window, join, join-on clause, project, union, except, intersect, table function, repeat, agg fun predicate, external scan, merge two filters, CTE consume.
- **PUSH_DOWN_SUBQUERY_RULES**, **SUBQUERY_EXTRACT_CORRELATION_PREDICATE_RULES**, **SUBQUERY_REWRITE_TO_WINDOW_RULES**, **SUBQUERY_REWRITE_TO_JOIN_RULES**.
- **AGGREGATE_REWRITE_RULES**: Bitmap/HLL count distinct, duplicate agg fn, sum-by-associative, count-if.
- **PRUNE_PROJECT_RULES**, **COLLECT_CTE_RULES**, **INLINE_CTE_RULES**, **INTERSECT_REWRITE_RULES**.
- **SINGLE_TABLE_MV_REWRITE_RULES** / **MULTI_TABLE_MV_REWRITE_RULES** (and **ALL_MV_REWRITE_RULES**).
- **PRUNE_EMPTY_OPERATOR_RULES**: Empty scan, join (left/right), direct, CTE anchor, union, intersect, except, window.
- **FINE_GRAINED_RANGE_PREDICATE_RULES**, **ELIMINATE_OP_WITH_CONSTANT_RULES**, **META_SCAN_REWRITE_RULES**, **TVR_REWRITE_RULES**, **VECTOR_REWRITE_RULES**, **PRUNE_UKFK_JOIN_RULES**, **SHORT_CIRCUIT_SET_RULES**.

These are invoked as a single “rule” from `QueryOptimizer` (e.g. `scheduler.rewriteOnce(tree, rootTaskContext, RuleSet.PARTITION_PRUNE_RULES)`).


## 3. Types of Optimizers and Rules

### 3.1 By Role

| Type | Role | When / Where |
|------|------|----------------|
| **Transformation rules** | Logical → logical | Applied in logical rewrite and inside Memo during cost-based search. |
| **Implementation rules** | Logical → physical | Applied inside Memo; replace logical operators with physical ones (scan, join, agg, project, topn, etc.). |
| **Tree rewrite rules** | Rewrite full tree (logical or physical) | Applied outside Memo: e.g. `TreeRewriteRule` (logical), or in physical rewrite phase (e.g. `ScalarOperatorsReuseRule`, `PreAggregateTurnOnRule`). |
| **Scalar rewrite rules** | Rewrite scalar expressions only | Applied inside `ScalarOperatorRewriter` or by tree rules that use it; do not change operator tree shape. |
| **Combination rules** | Group of rules applied together | Used as a single unit in the fixed logical sequence (e.g. `RuleSet.PRUNE_COLUMNS_RULES`). |

### 3.2 Rule Base Classes

- **Rule**: Base for all rule types; pattern + `transform()`.
- **TransformationRule**: For logical-to-logical rules (pattern + type).
- **ImplementationRule**: For logical-to-physical rules; higher promise.
- **TreeRewriteRule**: Interface for rules that rewrite an `OptExpression` tree (e.g. `SeparateProjectRule`, `ReorderJoinRule` when used outside Memo, `ScalarOperatorsReuseRule`, `PreAggregateTurnOnRule`). Some transformation rules are also used in a “rewrite” style via `RewriteTreeTask` (e.g. many rules in the logical sequence).
- **ScalarOperatorRewriteRule**: Interface for rewriting a single `ScalarOperator` (see below).

### 3.3 Implementation Rules (Examples)

Implementation rules turn logical operators into physical ones. They are registered in **RuleSet** (and optionally added for join strategy or MV):

- Scan: OlapScan, HiveScan, FileScan, IcebergScan, IcebergEqualityDeleteScan, HudiScan, DeltaLakeScan, PaimonScan, OdpsScan, IcebergMetadataScan, KuduScan, SchemaScan, MysqlScan, EsScan, MetaScan, JDBCScan, BenchmarkScan, TableFunctionTableScan.
- Aggregation: HashAgg.
- Project, TopN, AssertOneRow, Window, Union, Except, Intersect, Values, RawValues, Repeat, Filter, TableFunction, Limit.
- CTE: CTEAnchor, CTEAnchorToNoCTE, CTEConsumerReuse, CTEConsumeInline, CTEProduce.

Join implementation is added by option: HashJoin, MergeJoin, NestLoopJoin, or “auto” (hash + nestloop). Realtime MV adds StreamAgg, StreamJoin, StreamScan.


## 4. Scalar Expression Rewriting

Scalar expression rewriting operates on **ScalarOperator** trees (predicates, projections, etc.) and does not change the shape of the operator tree—only the expressions attached to operators.

### 4.1 Framework

- **ScalarOperatorRewriteRule**: Interface with `isBottomUp()`, `isTopDown()`, `isOnlyOnce()`, and `apply(operator, context)`.
- **ScalarOperatorRewriter**: Applies a list of rules to a root `ScalarOperator`; supports bottom-up, top-down, or only-once application; iterates until fixpoint (with a limit to avoid infinite rewrite).
- **ScalarOperatorRewriteContext**: Tracks rewrite state (e.g. change count) during rewriting.
- Base helpers: **BottomUpScalarOperatorRewriteRule**, **TopDownScalarOperatorRewriteRule**, **OnlyOnceScalarOperatorRewriteRule**, **BaseScalarOperatorRewriteRule**.

### 4.2 Standard Scalar Rewrite Rules

Used in **ScalarOperatorRewriter** (e.g. `DEFAULT_REWRITE_RULES`, `DEFAULT_REWRITE_SCAN_PREDICATE_RULES`, `FOLD_CONSTANT_RULES`):

| Rule | Purpose |
|------|--------|
| **ImplicitCastRule** | Add implicit casts for type compatibility. |
| **ReduceCastRule** | Reduce redundant or unnecessary casts. |
| **NormalizePredicateRule** | Normalize predicate form (e.g. for indexing and comparison). |
| **FoldConstantsRule** | Evaluate constant expressions at plan time. |
| **SimplifiedPredicateRule** | Simplify predicate expressions. |
| **SimplifiedDateColumnPredicateRule** | Simplify date column predicates. |
| **SimplifiedScanColumnRule** | Simplify scan column expressions (in scan-predicate rule set). |
| **ExtractCommonPredicateRule** | Factor common subexpressions in predicates. |
| **ArithmeticCommutativeRule** | Apply commutativity for arithmetic. |
| **ConsolidateLikesRule** | Consolidate LIKE patterns. |
| **SimplifiedCaseWhenRule** | Simplify CASE WHEN expressions. |
| **PruneTediousPredicateRule** | Remove redundant or trivial predicate parts. |
| **MvNormalizePredicateRule** | MV-specific predicate normalization (replaces NormalizePredicateRule in MV scalar rewrite). |
| **ReplaceScalarOperatorRule** | Replace scalar operators by map. |
| **ReplaceSubqueryRewriteRule** | Replace subquery with a scalar (e.g. for MV). |

These are used when rewriting predicates or projections (e.g. in push-down or in materialized view matching). **ScalarOperatorsReuseRule** (a tree rule) also uses scalar rewriting internally: it finds common subexpressions, introduces new column refs for them, and uses `ScalarOperatorRewriter` with rules such as **NormalizePredicateRule** and **ReduceCastRule** to normalize before deduplication.

### 4.3 Application Points

- Scalar rewriter is invoked from many transformation/tree rules that manipulate predicates or project expressions (e.g. predicate push-down, partition pruning, MV compensation).
- **ScalarOperatorsReuseRule** runs in the **physical rewrite** phase: it rewrites the physical plan so that repeated scalar expressions are computed once and reused via new project columns, reducing redundant computation.


## 5. Representative Optimizers (Rules)

### 5.1 Predicate and Partition

- **PushDownPredicateScanRule**: Pushes filter predicates down to scan when the scan can apply them (e.g. partition/key conditions), reducing data read.
- **PartitionPruneRule**: Uses partition predicates to prune partitions at scan.
- **DistributionPruneRule**: Prunes tablets by distribution.
- **ExtractRangePredicateFromScalarApplyRule**: Extracts range predicates from scalar subquery apply to enable partition pruning.

### 5.2 Join

- **ReorderJoinRule**: Join reordering (with strategies such as DP); can run outside Memo (RBO table prune path) or inside Memo (cost-based).
- **JoinCommutativityRule** / **JoinAssociativityRule** / **JoinLeftAsscomRule**: Change join order for cost-based search.
- **OnPredicateMoveAroundRule**: Moves ON-clause predicates to improve push-down and join placement.
- **OuterJoinEliminationRule**: Removes unnecessary outer join when semantics allow.
- **PruneUKFKJoinRule**: Prunes join based on unique/foreign key.

### 5.3 Aggregate

- **SplitTwoPhaseAggRule** / **SplitMultiPhaseAggRule**: Split aggregation for distributed execution.
- **PushDownTopNToPreAggRule**: Pushes TopN below agg when safe to reduce work.
- **EliminateAggRule** / **EliminateAggFunctionRule**: Remove redundant aggregations.
- **RewriteBitmapCountDistinctRule** / **RewriteHllCountDistinctRule**: Rewrite count distinct to use bitmap/HLL.
- **GroupByCountDistinctRewriteRule**: Rewrites group-by count distinct for better execution.

### 5.4 Column and Project

- **PruneScanColumnRule**: Prunes unused columns from scan.
- **PruneProjectColumnsRule**, **PruneFilterColumnsRule**, **PruneAggregateColumnsRule**, etc.: Column pruning at each operator (combined in **PRUNE_COLUMNS_RULES**).
- **MergeTwoProjectRule**: Merges adjacent projections.
- **MergeProjectWithChildRule**: Merges project into child operator when possible.

### 5.5 Subquery and Apply

- **ScalarApply2JoinRule**: Converts scalar subquery apply to join.
- **ExistentialApply2JoinRule** / **QuantifiedApply2JoinRule**: Existential/quantified apply to join.
- **ScalarApply2AnalyticRule**: Converts scalar apply to analytic when beneficial.

### 5.6 Materialized View

- **TextMatchBasedRewriteRule**: Text-match–based MV rewrite (before main logical rewrite).
- **MaterializedViewRule**: Main CBO-based MV rewrite (logical tree → tree with MV scans).
- **MaterializedViewTransparentRewriteRule**: Transparent rewrite to use MV when query matches.
- **AggregateScanRule**, **OnlyScanRule**, **AggregateTimeSeriesRule** (single-table); **AggregateJoinRule**, **OnlyJoinRule**, **AggregateJoinPushDownRule** (multi-table).

### 5.7 Physical and Tree (Post-Memo)

- **PreAggregateTurnOnRule**: Enables pre-aggregation when profitable.
- **ExchangeSortToMergeRule**: Rewrites exchange + sort to merge sort.
- **ScalarOperatorsReuseRule**: Reuses common scalar expressions via new project columns.
- **AddDecodeNodeForDictStringRule**: Adds decode for dictionary-encoded string.
- **LowCardinalityRewriteRule**: Rewrites for low-cardinality optimization.
- **CloneDuplicateColRefRule**: Clones duplicate column refs for correctness after reuse.


## 6. Summary Table of Optimizers and Rules

The following table lists optimizer **rule types** and **named rule sets / representative rules** as used in the FE. “Rule set” means a combination or group of rules; “Rule” is a single class. Implementation rules are summarized as “Implementation (Scan/Join/Agg/…)” where applicable.

| Category | Name | Type | Brief description |
|----------|------|------|--------------------|
| **Entry** | QueryOptimizer | Optimizer | Main cost-based optimizer; runs MV text rewrite, logical rewrite, Memo, cost-based search, physical/dynamic rewrite. |
| | ShortCircuitOptimizer | Optimizer | Short-circuit path. |
| | SPMOptimizer | Optimizer | Baseline (SPM) plan path. |
| **Logical – Combination** | MERGE_LIMIT_RULES | Rule set | Merge/split/push limit. |
| | PARTITION_PRUNE_RULES | Rule set | Partition, distribution, external partition, limit prune tablets. |
| | PRUNE_COLUMNS_RULES | Rule set | Prune columns at scan, project, filter, agg, topn, join, window, set ops, repeat, values, table function, CTE. |
| | PUSH_DOWN_PREDICATE_RULES | Rule set | Push predicate to scan/agg/window/join/project/union/except/intersect/table function/repeat/CTE; merge filters; cast to empty; prune true filter. |
| | PUSH_DOWN_SUBQUERY_RULES | Rule set | Merge apply with table function; push apply left project/left. |
| | SUBQUERY_EXTRACT_CORRELATION_PREDICATE_RULES | Rule set | Push apply project/filter/agg filter/agg project filter. |
| | SUBQUERY_REWRITE_TO_WINDOW_RULES | Rule set | Scalar apply to analytic. |
| | SUBQUERY_REWRITE_TO_JOIN_RULES | Rule set | Quantified/existential/scalar apply to join or outer join. |
| | AGGREGATE_REWRITE_RULES | Rule set | Bitmap/HLL count distinct, duplicate agg fn, sum-by-associative, count-if. |
| | PRUNE_PROJECT_RULES | Rule set | Prune project, merge two project, push project to CTE, defer project after TopN. |
| | PRUNE_UKFK_JOIN_RULES | Rule set | Prune UK/FK join. |
| | COLLECT_CTE_RULES | Rule set | Collect CTE produce/consume. |
| | INLINE_CTE_RULES | Rule set | Inline one CTE consume; prune CTE produce. |
| | INTERSECT_REWRITE_RULES | Rule set | Intersect add distinct; reorder intersect. |
| | SINGLE_TABLE_MV_REWRITE_RULES | Rule set | AggregateScan, AggregateTimeSeries, OnlyScan. |
| | MULTI_TABLE_MV_REWRITE_RULES | Rule set | AggregateJoin, OnlyJoin, AggregateJoinPushDown. |
| | PRUNE_EMPTY_OPERATOR_RULES | Rule set | Prune empty scan/join/direct/CTE anchor/union/intersect/except/window. |
| | FINE_GRAINED_RANGE_PREDICATE_RULES | Rule set | Fine-grained range predicate (and projection variant). |
| | ELIMINATE_OP_WITH_CONSTANT_RULES | Rule set | Eliminate group-by constant; eliminate join with constant (left/right single value). |
| | META_SCAN_REWRITE_RULES | Rule set | Push agg/flat JSON to meta scan; rewrite simple agg to meta/HDFS scan; min/max on scan. |
| | TVR_REWRITE_RULES | Rule set | TVR table scan, project, filter, join, aggregate, union all. |
| | VECTOR_REWRITE_RULES | Rule set | Rewrite to vector plan. |
| **Logical – Standalone** | TextMatchBasedRewriteRule | Rule | Text-match MV rewrite (early). |
| | SeparateProjectRule | Rule | Separate project from operator. |
| | IcebergPartitionsTableRewriteRule | Rule | Iceberg partitions table rewrite. |
| | ExtractRangePredicateFromScalarApplyRule | Rule | Extract range predicate from scalar apply. |
| | ApplyExceptionRule | Rule | Apply subquery exception handling. |
| | MaterializedViewTransparentRewriteRule | Rule | Transparent MV rewrite. |
| | SplitWindowSkewToUnionRule | Rule | Split window skew to union. |
| | LargeInPredicateToJoinRule | Rule | Large IN to join. |
| | SchemaTableEvaluateRule | Rule | Schema table evaluate. |
| | MergeTwoProjectRule | Rule | Merge two projects. |
| | ConvertToEqualForNullRule | Rule | Convert to equal for NULL. |
| | PushDownPredicateRankingWindowRule | Rule | Push predicate below ranking window. |
| | EliminateAggRule | Rule | Eliminate redundant agg. |
| | EliminateAggFunctionRule | Rule | Eliminate redundant agg function. |
| | PushDownJoinOnExpressionToChildProject | Rule | Push join ON expr to child project. |
| | PushDownAsofJoinTemporalExpressionToChildProject | Rule | Push asof join temporal expr to child project. |
| | PruneEmptyWindowRule | Rule | Prune empty window. |
| | MergeTwoAggRule | Rule | Merge two aggs. |
| | PushDownProjectLimitRule | Rule | Push project below limit. |
| | HoistHeavyCostExprsUponTopnRule | Rule | Hoist heavy cost exprs above TopN. |
| | PushDownLimitRankingWindowRule | Rule | Push limit below ranking window. |
| | RewriteGroupingSetsByCTERule | Rule | Rewrite grouping sets by CTE. |
| | PushDownAggregateGroupingSetsRule | Rule | Push down agg grouping sets. |
| | SkewJoinOptimizeRule | Rule | Skew join optimization. |
| | IcebergEqualityDeleteRewriteRule | Rule | Iceberg equality delete rewrite. |
| | PushDownSubfieldRule | Rule | Push down subfield. |
| | PruneSubfieldRule | Rule | Prune subfield. |
| | PushLimitAndFilterToCTEProduceRule | Rule | Push limit and filter to CTE produce. |
| | ForceCTEReuseRule | Rule | Force CTE reuse. |
| | SplitScanORToUnionRule | Rule | Split scan OR to union. |
| | MaterializedViewRule | Rule | CBO MV rewrite. |
| | OnPredicateMoveAroundRule | Rule | Move ON predicates. |
| | PartitionColumnMinMaxRewriteRule | Rule | Partition column min/max rewrite. |
| | RewriteMultiDistinctRule | Rule | Rewrite multi distinct. |
| | CTEProduceAddProjectionRule | Rule | Add projection on CTE produce. |
| | MVCompensationPruneUnionRule | Rule | MV compensation prune union. |
| | ArrayDistinctAfterAggRule | Rule | Array distinct after agg. |
| | EliminateConstantCTERule | Rule | Eliminate constant CTE. |
| | InnerToSemiRule | Rule | Inner join to semi. |
| | PushDownDistinctAggregateRule | Rule | Push distinct agg below window. |
| | PushDownAggregateRule | Rule | Push down aggregation. |
| | SemiJoinDeduplicateRule | Rule | Semi-join deduplicate. |
| | ReorderJoinRule | Rule | Join reorder (DP / adaptive). |
| | UniquenessBasedTablePruneRule | Rule | Table prune by uniqueness. |
| | PrimaryKeyUpdateTableRule | Rule | Primary key update table prune. |
| | RboTablePruneRule | Rule | RBO table prune. |
| | JsonPathRewriteRule | Rule | JSON path rewrite (meta scan / OlapScan). |
| | MergeProjectWithChildRule | Rule | Merge project with child. |
| | PartitionColumnValueOnlyOnScanRule | Rule | Partition column value only on scan. |
| | RewriteUnnestBitmapRule | Rule | Rewrite unnest bitmap. |
| | EliminateSortColumnWithEqualityPredicateRule | Rule | Eliminate sort column with equality predicate. |
| | PushDownTopNBelowOuterJoinRule | Rule | Push TopN below outer join. |
| | RemoveAggregationFromAggTable | Rule | Remove agg when reading agg table. |
| | PushDownTopNBelowUnionRule | Rule | Push TopN below union. |
| | SplitTopNAggregateRule | Rule | Split TopN aggregate. |
| | RewriteMinMaxByMonotonicFunctionRule | Rule | Rewrite min/max by monotonic function. |
| | RewriteSimpleAggToHDFSScanRule | Rule | Rewrite simple agg to HDFS scan. |
| | GroupByCountDistinctRewriteRule | Rule | Group-by count distinct rewrite. |
| | DeriveRangeJoinPredicateRule | Rule | Derive range join predicate. |
| | UnionToValuesRule | Rule | Union to values. |
| | SplitJoinORToUnionRule | Rule | Split join OR to union. |
| | PullUpScanPredicateRule | Rule | Pull up scan predicate. |
| | SimplifyCaseWhenPredicateRule | Rule | Simplify CASE WHEN predicate. |
| | CboTablePruneRule | Rule | CBO table prune (in Memo). |
| | JoinCommutativityRule | Rule | Join commutativity. |
| | JoinAssociativityRule | Rule | Join associativity (inner/outer). |
| | JoinLeftAsscomRule | Rule | Join left-asscom (inner/outer). |
| | JoinCommutativityWithoutInnerRule | Rule | Join commutativity without inner. |
| | OuterJoinEliminationRule | Rule | Outer join elimination. |
| **Scalar rewriting** | ImplicitCastRule | Scalar rule | Implicit cast. |
| | ReduceCastRule | Scalar rule | Reduce cast. |
| | NormalizePredicateRule | Scalar rule | Normalize predicate. |
| | FoldConstantsRule | Scalar rule | Fold constants. |
| | SimplifiedPredicateRule | Scalar rule | Simplify predicate. |
| | SimplifiedDateColumnPredicateRule | Scalar rule | Simplify date column predicate. |
| | SimplifiedScanColumnRule | Scalar rule | Simplify scan column. |
| | ExtractCommonPredicateRule | Scalar rule | Extract common predicate. |
| | ArithmeticCommutativeRule | Scalar rule | Arithmetic commutativity. |
| | ConsolidateLikesRule | Scalar rule | Consolidate LIKE. |
| | SimplifiedCaseWhenRule | Scalar rule | Simplify CASE WHEN. |
| | PruneTediousPredicateRule | Scalar rule | Prune tedious predicate. |
| | MvNormalizePredicateRule | Scalar rule | MV predicate normalization. |
| **Physical / tree** | PreAggregateTurnOnRule | Tree rule | Turn on pre-aggregation. |
| | ExchangeSortToMergeRule | Tree rule | Exchange+sort to merge. |
| | PruneAggregateNodeRule | Tree rule | Prune agg node. |
| | PruneShuffleDistributionNodeRule | Tree rule | Prune shuffle distribution node. |
| | PruneShuffleColumnRule | Tree rule | Prune shuffle column. |
| | PhysicalDistributionAggOptRule | Tree rule | Physical distribution agg opt. |
| | AddDecodeNodeForDictStringRule | Tree rule | Add decode for dict string. |
| | LowCardinalityRewriteRule | Tree rule | Low-cardinality rewrite. |
| | ApplyMinMaxStatisticRule | Tree rule | Apply min/max statistic. |
| | PruneSubfieldsForComplexType | Tree rule | Prune subfields for complex type. |
| | InlineCteProjectPruneRule | Tree rule | Inline CTE project prune. |
| | ScalarOperatorsReuseRule | Tree rule | Reuse scalar expressions. |
| | PredicateReorderRule | Tree rule | Reorder predicates. |
| | ExtractAggregateColumn | Tree rule | Extract aggregate column. |
| | JoinLocalShuffleRule | Tree rule | Join local shuffle. |
| | CloneDuplicateColRefRule | Tree rule | Clone duplicate column ref. |
| | SubfieldExprNoCopyRule | Tree rule | Subfield expr no-copy. |
| | AddIndexOnlyPredicateRule | Tree rule | Add index-only predicate. |
| | DataCachePopulateRewriteRule | Tree rule | Data cache populate rewrite. |
| | EliminateOveruseColumnAccessPathRule | Tree rule | Eliminate overuse column access path. |
| | RemoveUselessScanOutputPropertyRule | Tree rule | Remove useless scan output property. |
| | GlobalLateMaterializationRewriter | Tree rule | Global late materialization. |
| | MarkParentRequiredDistributionRule | Tree rule | Mark parent required distribution. |
| | SkewShuffleJoinEliminationRule | Tree rule | Skew shuffle join elimination. |
| | ApplyTuningGuideRule | Tree rule | Apply tuning guide. |
| **Implementation** | OlapScanImplementationRule | Implementation | Logical Olap scan → physical. |
| | HiveScanImplementationRule | Implementation | Hive scan. |
| | FileScanImplementationRule | Implementation | File scan. |
| | IcebergScanImplementationRule | Implementation | Iceberg scan. |
| | (Other scan implementations) | Implementation | Iceberg equality delete, Hudi, Delta, Paimon, Odps, Iceberg metadata, Kudu, Schema, Mysql, ES, Meta, JDBC, Benchmark, TableFunctionTableScan. |
| | HashAggImplementationRule | Implementation | Hash agg. |
| | ProjectImplementationRule | Implementation | Project. |
| | TopNImplementationRule | Implementation | TopN. |
| | (Other operator implementations) | Implementation | AssertOneRow, Window, Union, Except, Intersect, Values, RawValues, Repeat, Filter, TableFunction, Limit, CTE anchor / no-CTE / consume reuse / consume inline / produce. |
| | HashJoinImplementationRule | Implementation | Hash join. |
| | MergeJoinImplementationRule | Implementation | Merge join. |
| | NestLoopJoinImplementationRule | Implementation | Nested loop join. |

This table is a snapshot of the optimizer and rule set as of the codebase structure described; new rules or rule sets may be added over time.

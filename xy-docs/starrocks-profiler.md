---
title: StarRocks Profiler and Profiles
---

## 1. Overview

StarRocks has a profiling system that collects execution statistics for performance analysis. It tracks metrics from individual operators to entire queries, covering resource consumption and timings. Profiles are collected on Backends (BE) and aggregated on Frontends (FE).

## 2. Profiling Architecture

The profiling system operates across both FE and BE components, with BE nodes collecting execution statistics and FE nodes aggregating and managing profiles.

**Profiling Flow:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Profile Aggregation (FE)                             │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  QueryRuntimeProfile                                              │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Merges profiles from multiple BE nodes                   │  │  │
│  │  │  - Aggregates fragment profiles                             │  │  │
│  │  │  - Computes time percentages                                │  │  │
│  │  │  - Builds query-level profile                               │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  ProfileManager                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Stores profiles in memory (LinkedHashMap)                │  │  │
│  │  │  - Compresses profile content (GZIP)                        │  │  │
│  │  │  - Manages profile lifecycle                                │  │  │
│  │  │  - Exposes via HTTP API                                     │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ Thrift RPC (report_exec_status)
                          │ - BE → FE: TReportExecStatusParams
                          │ - Includes: TRuntimeProfileTree (Thrift)
                          │ - Primary mechanism for profile reporting
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Query Execution (BE)                                 │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  RuntimeProfile (Per Operator/Fragment)                           │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Operator execution timings                               │  │  │
│  │  │  - Resource usage (CPU, memory, I/O)                        │  │  │
│  │  │  - Row counts and data sizes                                │  │  │
│  │  │  - Custom counters and metrics                              │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  Profile Collection                                               │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Periodic collection (ProfileReportWorker)                │  │  │
│  │  │  - Execution state reporting (report_exec_status RPC)       │  │  │
│  │  │  - Serialization to TRuntimeProfileTree (Thrift)            │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

## 3. RuntimeProfile: The Core Profiling Data Structure

`RuntimeProfile` is the fundamental data structure for collecting and organizing execution statistics. It provides a hierarchical tree structure where each node represents a profiled component (operator, fragment, query) and contains counters, info strings, and child profiles.

### 3.1. Profile Structure

**Hierarchical Organization:**

The profile hierarchy follows the execution model with five main layers: Query → Fragment → FragmentInstance → Pipeline → PipelineDriver → Operator. The actual profile structure depends on the `pipeline_profile_level` setting (see Section 7), which can simplify the hierarchy by merging layers.

```text
RuntimeProfile (Query)
├── RuntimeProfile (Fragment 0)
│   ├── RuntimeProfile (FragmentInstance)
│   │   ├── RuntimeProfile (Pipeline (id=0))
│   │   │   ├── RuntimeProfile (PipelineDriver (id=0))
│   │   │   │   ├── RuntimeProfile (OlapScanOperator)
│   │   │   │   │   ├── CommonMetrics
│   │   │   │   │   │   ├── OperatorTotalTime
│   │   │   │   │   │   ├── PushChunkNum
│   │   │   │   │   │   └── PullChunkNum
│   │   │   │   │   └── UniqueMetrics
│   │   │   │   │       ├── RowsRead
│   │   │   │   │       ├── BytesRead
│   │   │   │   │       └── ScanTime
│   │   │   │   └── RuntimeProfile (HashJoinOperator)
│   │   │   │       ├── CommonMetrics
│   │   │   │       └── UniqueMetrics
│   │   │   │           ├── BuildRows
│   │   │   │           ├── ProbeRows
│   │   │   │           └── HashTableSize
│   │   │   └── RuntimeProfile (PipelineDriver (id=1))
│   │   │       └── ... (parallel driver instances)
│   │   └── RuntimeProfile (Pipeline (id=1))
│   │       └── ... (additional pipelines in fragment)
│   └── RuntimeProfile (FragmentInstance)
│       └── ... (additional fragment instances)
└── RuntimeProfile (Fragment 1)
    └── ...
```

**Note:** With default `pipeline_profile_level=1`, the profile is simplified and may merge Pipeline and PipelineDriver layers, showing operators directly under FragmentInstance. With `pipeline_profile_level=2`, all layers are preserved.

### 3.2. Counter Types

Counters track numeric metrics with different aggregation strategies:

- **Regular Counters**: Track cumulative values (e.g., rows processed, bytes scanned). Support sum, average, min, and max aggregation strategies.

- **Timer Counters**: Track elapsed time in nanoseconds. Used for measuring execution durations (operator total time, I/O wait time, etc.).

- **High Water Mark Counters**: Track peak values (e.g., peak memory usage). Maintain both current value and maximum value seen.

- **Low Water Mark Counters**: Track minimum values seen (e.g., minimum buffer size).

- **Derived Counters**: Compute values from other counters (e.g., throughput = bytes / time).

**Counter Strategy:**

Each counter has a `TCounterStrategy` that defines how it should be aggregated:

- **Aggregate Type**: `SUM`, `AVG`, `SUM_AVG` (sum at BE, average at FE), `AVG_SUM` (average at BE, sum at FE)
- **Merge Type**: `MERGE_ALL`, `SKIP_ALL`, `SKIP_FIRST_MERGE`, `SKIP_SECOND_MERGE`
- **Display Threshold**: Minimum value required for display (filters out insignificant counters)
- **Min/Max Type**: Whether to track min/max values across instances

### 3.3. Info Strings

Info strings store metadata and configuration information as key-value pairs:

- **Query Metadata**: Query ID, SQL statement, user, database, start time, end time
- **Execution Options**: Query hints, session variables, execution mode
- **Operator Configuration**: Scan ranges, join types, aggregation modes

### 3.4. Profile Versioning

Profiles use version numbers to prevent stale updates. When a profile is updated from a BE node, the version is checked to ensure the update is newer than the current version. This prevents race conditions when multiple BE nodes report profiles concurrently.

### 3.5. Implementation Perspective: Data Structures and Representations

This subsection describes how the runtime profile is represented in code: the in-memory structures on BE, the counter hierarchy, the string-based format used for display and parsing, and the Thrift (flattened) format used for transport. Terminology below follows the implementation; some names may differ from user-facing wording.

#### 3.5.1. In-Memory Structure on BE (C++)

On the backend, `RuntimeProfile` is the core type. Each instance represents one node in the profile tree and holds:

**Per-node identity and metadata:**

- `_name`: Display name of the node (e.g. fragment name, operator name).
- `_metadata`: Opaque int64 (e.g. plan node id), not interpreted by the profile layer.
- `_indent`: Not stored on the node itself; each parent stores per-child whether that child is printed with extra indentation (`ChildVector` stores `(RuntimeProfile*, bool indent)`).

**Counters:**

- `_counter_map`: `map<string, pair<Counter*, string>>` — from counter **name** to (counter object, **parent counter name**). The parent counter name is the empty string `""` for top-level counters (see `ROOT_COUNTER`).
- `_child_counter_map`: `map<string, set<string>>` — from **parent counter name** to the set of **child counter names**. This defines the counter hierarchy within this node (which counters are grouped under which).
- Each `Counter` holds: an atomic `int64_t` value, a `TUnit::type` (unit for display), a `TCounterStrategy` (aggregation/merge/display rules), and optional min/max. Specializations include `HighWaterMarkCounter`, `LowWaterMarkCounter`, `DerivedCounter`, etc.
- Counters are owned by the profile via an `ObjectPool`; the profile does not own child **profiles** (only references them).

**Profile tree (children):**

- `_children`: `vector<pair<RuntimeProfile*, bool>>` — ordered list of child profiles and their indent flag (order is insertion order and is preserved for output).
- `_child_map`: `map<string, RuntimeProfile*>` — from child **name** to child profile pointer, for lookup and merge-by-name.
- `_parent`: Pointer to the parent profile (null for the root).

**Other per-node data:**

- `_info_strings`: `map<string, string>` — key-value metadata (e.g. "Query ID", "SQL").
- `_info_strings_display_order`: `vector<string>` — order in which info strings are printed.
- `_event_sequence_map`: Named event sequences (timeline events) for this node.
- `_counter_total_time`: Built-in counter for total time of this node.
- `_local_time_percent`: Fraction of total time spent in this node (excluding children), computed later.
- `_version`: Used to reject stale Thrift updates.

Locks: `_counter_lock` (counters and counter hierarchy), `_children_lock` (child list/map), `_info_strings_lock`, `_event_sequences_lock`, `_version_lock`.

#### 3.5.2. Counter Hierarchy Within a Node

Counters inside a single `RuntimeProfile` can form a **tree**, not just a flat list:

- **Root**: All top-level counters have parent name `ROOT_COUNTER` (the empty string `""`). They are the roots of the counter tree for this node.
- **Child counters**: `add_child_counter(name, type, strategy, parent_name)` registers a counter and records `parent_name` in `_counter_map` and adds `name` to `_child_counter_map[parent_name]`.
- **Display**: When printing (e.g. `pretty_print`), the backend walks the counter tree recursively using `_child_counter_map`: for each counter name it prints the counter, then recurses on its children. So the string output can show a nested “counter hierarchy” under each profile node (e.g. under an operator, “TotalTime” with child “ScanTime”, “WaitTime”, etc.).

This is separate from the **profile** tree: the profile tree is “profile nodes containing counters and child profile nodes”; within one node, counters themselves can be organized in a parent-child tree.

#### 3.5.3. Profile Tree Structure

The **profile tree** is a tree of `RuntimeProfile` nodes:

- **Root**: Usually the query or fragment root.
- **Edges**: Each node has a list of children (`_children`); each child has a parent pointer (`_parent`). The same child is not shared across parents; merging is done by merging contents of nodes with the same name.
- **Order**: Child order is the order of insertion (e.g. Fragment 0, Fragment 1, or operators in pipeline order). This order is preserved when serializing to Thrift and when printing.
- **Indent**: Each child is stored with an `indent` flag; when producing the string profile, children with `indent == true` are printed with additional indentation (e.g. two more spaces per level).

So the “tree” you see in the text profile (nested lines with indentation) is exactly this profile tree, with optional extra indent per child and with counters (and their own hierarchy) printed under each node.

#### 3.5.4. String-Based Profile Structure

The human-readable profile is produced by `RuntimeProfile::pretty_print()`. The format is line-based and indentation encodes the tree:

**Per node, in order:**

1. **One line for the node**: `prefix + _name + ":"` and, if total time is non-zero, `"(Active: <time>, non-child: <local_time_percent>%)"`.
2. **Info strings**: For each key in `_info_strings_display_order`, a line `prefix + "   - " + key + ": " + value`.
3. **Event sequences**: For each named event sequence, a line with elapsed time and then indented lines for each event (timestamp and delta from previous).
4. **Counters**: Recursive print using `_child_counter_map`: start from `ROOT_COUNTER`, for each child counter name print a line `prefix + "   - " + name + ": " + value` (with value formatted by `PrettyPrinter`), then recurse with increased prefix for that counter’s children.
5. **Child profiles**: For each entry in `_children`, call `pretty_print` on the child with `prefix + (indent ? "  " : "")`.

So the string form is a **lossy, display-oriented encoding** of the profile tree plus counter trees: structure is expressed by indentation and by the order of lines. The FE can reconstruct a profile from this string using `RuntimeProfileParser.parseFrom(content)`, which parses the same line patterns (profile names, `- key: value` for info strings and counters, indent to infer hierarchy). That allows profiles that were stored or transmitted as text (e.g. from logs or APIs that return a single string) to be turned back into a `RuntimeProfile` object on the FE for merging or analysis.

#### 3.5.5. Thrift Representation: Flattened Tree

For transport (BE → FE) and storage, the profile is serialized to Thrift, not to the string format above.

**Types (see `gensrc/thrift/RuntimeProfile.thrift` and `Metrics.thrift`):**

- `TRuntimeProfileTree`: contains a single field `nodes` of type `list<TRuntimeProfileNode>`.
- `TRuntimeProfileNode`: one node in the tree. Fields include: `name`, `num_children`, `counters` (list of `TCounter`), `metadata`, `indent`, `info_strings`, `info_strings_display_order`, `child_counters_map` (map from parent counter name to set of child counter names), and optional `version`.
- `TCounter`: `name`, `type` (TUnit), `value`, optional `strategy`, `min_value`, `max_value`.

**Flattening (BE → Thrift):**

- The in-memory profile tree is written out in **depth-first preorder**: the root node is appended to `nodes`, then each child is serialized in order (each child appending itself and its subtree). So the list order is a preorder traversal of the profile tree.
- For each node we write: its counters (respecting display thresholds and child_counter_map for structure), `child_counters_map`, info_strings, info_strings_display_order, and `num_children` = number of direct children. The `indent` flag for each child is stored on the **child** node in the list.

**Reconstructing the tree (FE):**

- FE’s `RuntimeProfile.update(TRuntimeProfileTree)` (and the internal `update(nodes, idx, ...)`) interprets the list as the same preorder traversal: at each step it reads the current node, updates the corresponding in-memory node (or creates one by name if missing), then advances `idx` and, for `num_children` times, recursively processes the next node in the list as a child. After processing those `num_children` nodes, the index points to the next sibling (or parent’s next sibling). So the **tree structure is encoded by list order + num_children**; there are no explicit parent pointers in the Thrift format.

**Counter hierarchy in Thrift:** Each `TRuntimeProfileNode` carries a flat `counters` list plus `child_counters_map`. The FE (and BE when updating from Thrift) uses `child_counters_map` to rebuild the same counter hierarchy (which counter is under which parent) when updating the in-memory profile. So both the profile tree and the per-node counter trees are reconstructable from the flattened representation.

## 4. Profile Collection on Backend

BE nodes collect profiles during query execution, with each operator maintaining its own `RuntimeProfile` instance.

### 4.1. Operator-Level Profiling

Each operator in the pipeline execution engine maintains a `RuntimeProfile` with two child profiles:

- **CommonMetrics**: Metrics common to all operators (e.g., `OperatorTotalTime`, `PushChunkNum`, `PullChunkNum`)
- **UniqueMetrics**: Operator-specific metrics (e.g., `RowsRead` for scan operators, `HashTableSize` for join operators)

**Profile Creation:**

```cpp
Operator::Operator(...) {
    _runtime_profile = std::make_shared<RuntimeProfile>(profile_name);
    _common_metrics = std::make_shared<RuntimeProfile>("CommonMetrics");
    _runtime_profile->add_child(_common_metrics.get(), true, nullptr);
    _unique_metrics = std::make_shared<RuntimeProfile>("UniqueMetrics");
    _runtime_profile->add_child(_unique_metrics.get(), true, nullptr);
}
```

**Profile Enablement Check:**

Before updating counters, operators check if profiling is enabled via `QueryContext::enable_profile()`. This method implements the `big_query_profile_threshold` logic:

- If `enable_profile=true` is explicitly set, profiling is always enabled from query start, and the threshold is never checked
- If `enable_profile=false` and `big_query_profile_threshold > 0`, profiling is enabled only when query execution time exceeds the threshold (dynamic enablement)
- If both are disabled (`enable_profile=false` and `big_query_profile_threshold=0`), profiling is never enabled
- The check is performed dynamically during query execution, allowing profiling to be enabled mid-query when using threshold-based profiling

**Counter Updates:**

Operators update counters during execution using macros:

- `SCOPED_TIMER(counter)`: Measures elapsed time for a code block
- `COUNTER_UPDATE(counter, value)`: Increments a counter by a value
- `COUNTER_SET(counter, value)`: Sets a counter to a value
- `COUNTER_ADD(counter, value)`: Adds to a high water mark counter

**Mid-Query Enablement and Stat Loss:**

When `big_query_profile_threshold` is used, profiling is enabled dynamically during query execution. This has important implications:

1. **Counter Creation**: Counters are always created during operator initialization (`Operator::prepare_local_state()`), regardless of whether profiling is enabled. This ensures counters exist when profiling is enabled mid-query.

2. **Counter Updates**: Counters are only updated when `enable_profile()` returns `true`. Before the threshold is exceeded, counters exist but remain at their initial values (typically 0).

3. **Stat Loss**: Statistics from the portion of query execution before the threshold is exceeded are **not captured**. For example:
    - If a query runs for 60 seconds with a 30-second threshold, operator-level counters (e.g., `OperatorTotalTime`, `RowsRead`) will only reflect the last 30 seconds of execution
    - Time-based counters will show only the time spent after profiling was enabled
    - Row/byte counters will show only data processed after profiling was enabled

4. **Query-Level Metrics Exception**: Some query-level metrics (e.g., `QueryExecutionWallTime`) are set using `query_ctx->lifetime()`, which captures the full query execution time regardless of when profiling was enabled. These metrics provide the complete picture even when profiling starts mid-query.

**Example:**

```text
Query execution timeline (60 seconds total, threshold = 30s):
┌─────────────────────────────────────────────────────────────┐
│ 0s ──────────── 30s ──────────── 60s                      │
│ │                │                │                        │
│ Profiling        Profiling        Query                    │
│ DISABLED         ENABLED          Complete                 │
│                  │                                        │
│                  └─> Counters start updating here         │
└─────────────────────────────────────────────────────────────┘

Result:
- OperatorTotalTime: ~30s (only time after threshold)
- RowsRead: Only rows processed after threshold
- QueryExecutionWallTime: 60s (full query time)
```

**Implications:**

- **Partial Profiles**: Profiles for queries that exceed the threshold will show partial statistics, making it difficult to analyze the full query execution
- **Bottleneck Identification**: If a bottleneck occurs before the threshold, it may not be visible in the profile
- **Use Case**: Threshold-based profiling is best suited for identifying issues in long-running queries where the problem persists throughout execution, not for analyzing query startup or early execution phases

### 4.2. Fragment-Level Profiling

Each fragment maintains a `RuntimeProfile` that aggregates profiles from all operators in the fragment. The fragment profile is created in `FragmentContext` and updated as operators report their execution state.

**Profile Merging:**

When multiple instances of the same fragment execute in parallel (e.g., multiple drivers processing different data partitions), their profiles are merged:

- **Isomorphic Profiles**: Profiles with identical structure (same operators, same counters) are merged by combining counter values according to their aggregation strategies
- **Non-Isomorphic Profiles**: Profiles with different structures are kept separate as child profiles

### 4.3. Profile Reporting Mechanisms

BE nodes report profiles to FE via the primary mechanism `report_exec_status` (Thrift RPC):

**Primary Mechanism: `report_exec_status` (Thrift RPC)**

- **Periodic Reporting (ProfileReportWorker)**: Background thread (`ProfileReportWorker`) periodically collects profiles for active LOAD queries and reports them at configurable intervals (`profile_report_interval`, default: 30 seconds). This mechanism is specifically for LOAD queries (both pipeline and non-pipeline) to provide incremental progress updates for long-running load operations.

- **Execution State Reporting**: BE proactively sends profiles to FE during query execution:
    - When fragment execution state changes
    - When execution completes (done=true)
    - Periodically based on `runtime_profile_report_interval` session variable
    - Profile data is serialized as `TRuntimeProfileTree` and sent as part of `TReportExecStatusParams`

**Optional Mechanism: `trigger_profile_report` (bRPC, Currently Unused)**

- FE can theoretically request profiles on-demand via `trigger_profile_report` bRPC call
- When BE receives this request, it internally triggers `report_exec_status` to send the profile back to FE
- However, this mechanism is not currently used in the FE codebase - all profile collection relies on BE-initiated `report_exec_status` calls
- The API exists for potential future use cases requiring synchronous profile retrieval, but the current implementation uses only BE-initiated reporting

**Profile Serialization:**

Profiles are serialized to Thrift format (`TRuntimeProfileTree`) for transmission:

```cpp
void RuntimeProfile::to_thrift(TRuntimeProfileTree* tree) {
    std::vector<TRuntimeProfileNode> nodes;
    to_thrift(&nodes);
    tree->nodes = nodes;
}
```

The serialization performs an in-order traversal of the profile tree, flattening it into a list of nodes with parent-child relationships encoded via node ordering and metadata.

### 4.4. Query-Level Metrics

Query-level metrics are added to fragment profiles during execution state reporting:

- **QueryPeakMemoryUsage**: Peak memory consumption across all fragments
- **QueryCumulativeCpuTime**: Total CPU time consumed by the query
- **QuerySpillBytes**: Total bytes spilled to disk
- **QueryExecutionWallTime**: Total wall-clock time for query execution

These metrics use `SKIP_FIRST_MERGE` strategy to avoid double-counting when merging fragment profiles.

**Threshold-Based Profiling Impact:**

When `big_query_profile_threshold` is used, most query-level metrics reflect only the portion of execution after the threshold is exceeded. However, `QueryExecutionWallTime` is an exception—it uses `query_ctx->lifetime()` which captures the full query execution time from start to finish, regardless of when profiling was enabled.

For example, if a query runs for 60 seconds with a 30-second threshold:

- `QueryExecutionWallTime`: 60 seconds (full query time, captured via `lifetime()`)
- `QueryCumulativeCpuTime`: Only CPU time after threshold (approximately 30 seconds worth)
- `QueryPeakMemoryUsage`: Peak memory after threshold (may miss early peaks)
- `QuerySpillBytes`: Only spill bytes after threshold

This mixed behavior means that while `QueryExecutionWallTime` provides the complete picture, other metrics may be incomplete when profiling starts mid-query.

## 5. Profile Aggregation on Frontend

FE nodes aggregate profiles from multiple BE nodes, merge fragment profiles, and compute query-level statistics.

### 5.1. Profile Reception

FE receives profiles via `report_exec_status` RPC calls from BE nodes. Each call includes:

- **Fragment Instance ID**: Identifies the fragment instance reporting the profile
- **Profile Data**: `TRuntimeProfileTree` serialized as Thrift attachment
- **Execution Status**: Query completion status, error information
- **Done Flag**: Indicates whether the fragment has completed execution

**Profile Update Strategy:**

FE updates profiles incrementally as BE nodes report execution state:

- **Incremental Updates**: Profiles are updated as fragments report progress, enabling real-time monitoring
- **Version Checking**: Profile versions prevent stale updates from overwriting newer data
- **Interval-Based Export**: Profiles are exported to `ProfileManager` at configurable intervals to avoid excessive memory usage

**FE Finalization vs. Incomplete Reports:**

FE **only processes and finalizes** the profile (merge all fragments, store in `ProfileManager` as the authoritative profile, optionally write to `fe.profile.log`) **after** the query execution completes—i.e., after all fragment instances have reported with `done=true` and the finalization task runs (see Section 5.4). Until then, FE only merges incoming updates into the in-memory `QueryRuntimeProfile` and may push a **running** snapshot to `ProfileManager` at intervals.

BE nevertheless sends **incomplete** (mid-execution) profiles in `report_exec_status` for two reasons:

1. **Real-time monitoring**: BE reports periodically with `done=false` (see `runtime_profile_report_interval` and `FragmentContext::report_exec_state_if_necessary()`). FE merges these into the query profile and, at interval boundaries, may call `saveRunningProfile()` so that the profile API (e.g. `/api/v2/profile?query_id=...`) can return a current snapshot while the query is still running.
2. **Final report**: Each fragment also sends a final `report_exec_status` with `done=true` when it completes. The **stored** profile is built only after FE has received `done=true` from every fragment instance; that finalization uses the latest merged state (including those final reports), so the persisted profile is complete.

So incomplete reports are intentional: they support live progress and running-profile retrieval. The authoritative, finalized profile is still produced only once the query has completed and all fragments have reported done.

### 5.2. Profile Merging

FE merges profiles from multiple sources:

**Fragment Instance Merging:**

- Multiple instances of the same fragment (executing on different BE nodes or with different data partitions) have their profiles merged
- Isomorphic profiles (identical structure) are merged by combining counter values
- Non-isomorphic profiles are kept as separate children

**Fragment Merging:**

- Fragment profiles are merged into the query profile
- Time percentages are computed to show relative execution time spent in each fragment
- Fragment profiles are organized by fragment index

**Query-Level Aggregation:**

- Query-level metrics (peak memory, CPU time, spill bytes) are aggregated across all fragments
- Summary information (query ID, SQL statement, execution time) is added to the profile

### 5.3. Profile Storage (ProfileManager)

`ProfileManager` manages profile lifecycle and storage:

**Storage Structure:**

- **In-Memory Storage**: Profiles are stored in a `LinkedHashMap` keyed by query ID
- **Compression**: Profile content is compressed using GZIP to reduce memory usage
- **Retention**: Profiles are retained up to a configurable limit (`profile_info_reserved_num`, default: 100)

**Profile Element:**

Each stored profile contains:

- **Info Strings**: Query metadata (ID, SQL, user, execution time, etc.)
- **Profile Content**: Serialized profile tree (compressed)
- **Profiling Plan**: Execution plan information for display

**Profile Retrieval:**

- Profiles can be retrieved by query ID via HTTP API (`/api/v2/profile?query_id=...`)
- Profiles are decompressed on retrieval
- FE nodes can query profiles from other FE nodes for distributed query profiles

### 5.4. Profile Processing and Finalization

After query execution completes, FE processes and finalizes profiles. This happens in `StmtExecutor` after the query finishes executing.

**Profile Processing Flow:**

```text
Query Execution Completes
    │
    ▼
StmtExecutor.tryProcessProfileAsync()
    │
    ├─> Checks if async processing is enabled (enable_async_profile)
    │
    ├─> Builds top-level profile (buildTopLevelProfile)
    │   └─> Creates "Query" profile with "Summary" child
    │       └─> Adds metadata: query ID, SQL, user, start/end time, etc.
    │
    ├─> Creates async task (Consumer<Boolean>)
    │   └─> This task will execute when all fragment instances finish
    │
    └─> Coordinator.tryProcessProfileAsync(task)
        │
        ├─> If async processing succeeds:
        │   └─> Registers task as listener via QueryRuntimeProfile.addListener()
        │       └─> Task executes when profileDoneSignal counts down to zero
        │           (i.e., when all fragment instances have reported completion)
        │
        └─> If async processing fails or is disabled:
            └─> Falls back to synchronous processing (collectProfileSync)
                └─> Waits for all fragment instances to finish
                └─> Finalizes profile immediately
```

**`tryProcessProfileAsync` Method:**

The `tryProcessProfileAsync` method in `DefaultCoordinator` attempts to process profiles asynchronously:

1. **Validation Checks:**
    - Verifies that fragment executions exist (or query is short-circuit)
    - Checks if profile reporting is needed (`jobSpec.isNeedReport()`)
    - Checks if async profile processing is enabled (`enable_async_profile` session variable)

2. **Async Task Registration:**
    - If validation passes, registers the provided task as a listener via `QueryRuntimeProfile.addListener(task)`
    - The task is executed asynchronously when all fragment instances finish reporting (when `profileDoneSignal` counts down to zero)
    - Returns `true` if async processing was successfully scheduled

3. **Fallback to Synchronous Processing:**
    - If async processing fails (queue full, disabled, etc.), calls `collectProfileSync()`
    - `collectProfileSync()` waits for all fragment instances to finish reporting (with timeout)
    - Then finalizes the profile synchronously
    - Executes the task immediately with `isAsync=false`

**Async Task Execution:**

When the async task executes (after all fragment instances finish), it:

1. **Builds Complete Profile:**
    - Adds profile collection time to summary
    - Merges fragment profiles via `coord.buildQueryProfile(needMerge)`
    - Updates total execution time
    - Adds retry information if applicable

2. **Stores Profile:**
    - Calls `ProfileManager.getInstance().pushProfile(profilingPlan, profile)`
    - Compresses and stores profile in memory
    - Updates `QueryDetail` with profile content

3. **Cleanup:**
    - Unmonitors query from `QeProcessorImpl`
    - Unregisters query from query registry
    - Optionally logs profile to disk (`fe.profile.log`) if enabled

**Benefits of Async Processing:**

- **Non-Blocking**: Query execution can return results to client immediately without waiting for profile finalization
- **Resource Efficiency**: Profile processing happens in background thread pool, avoiding blocking query execution threads
- **Timeout Protection**: Profile processing has its own timeout (`profile_timeout` session variable) separate from query execution timeout

**Synchronous Fallback:**

When async processing is not available or fails:

- Profile processing blocks until all fragment instances finish reporting
- This ensures profile is available immediately after query completion
- Used when async processing is disabled or when async task queue is full

## 6. Profile Display and Analysis

Profiles are displayed in human-readable format and can be analyzed for performance optimization.

### 6.1. Profile Format

Profiles are formatted as text with hierarchical indentation:

```
Query (query_id=xxx)
├── Summary
│   ├── Query ID: xxx
│   ├── Start Time: 2024-01-01 10:00:00
│   ├── End Time: 2024-01-01 10:00:05
│   ├── Total Time: 5.000s
│   └── SQL Statement: SELECT ...
├── Fragment 0
│   ├── TotalTime: 4.500s
│   ├── OlapScanOperator (plan_node_id=1)
│   │   ├── CommonMetrics
│   │   │   ├── OperatorTotalTime: 2.000s
│   │   │   ├── PushChunkNum: 100
│   │   │   └── PullChunkNum: 100
│   │   └── UniqueMetrics
│   │       ├── RowsRead: 1,000,000
│   │       ├── BytesRead: 100 MB
│   │       └── ScanTime: 1.800s
│   └── HashJoinOperator (plan_node_id=2)
│       └── ...
└── Fragment 1
    └── ...
```

### 6.2. Key Metrics

**Execution Time Metrics:**

- **TotalTime**: Total execution time for a component
- **OperatorTotalTime**: Time spent in an operator
- **LocalTimePercent**: Percentage of time spent in a component (excluding children)

**Resource Usage Metrics:**

- **QueryPeakMemoryUsage**: Peak memory consumption
- **QueryCumulativeCpuTime**: Total CPU time
- **QuerySpillBytes**: Bytes spilled to disk

**Data Flow Metrics:**

- **RowsRead/RowsWritten**: Number of rows processed
- **BytesRead/BytesWritten**: Data volume processed
- **ChunksTransmitted**: Number of chunks sent between nodes

**Operator-Specific Metrics:**

- **Scan Operators**: RowsRead, BytesRead, ScanTime, IndexPruneTime
- **Join Operators**: BuildRows, ProbeRows, HashTableSize, JoinTime
- **Aggregate Operators**: InputRows, OutputRows, HashTableSize, AggregateTime
- **Exchange Operators**: ChunksTransmitted, BytesTransmitted, NetworkTime

### 6.3. Profile Analysis

Profiles enable performance analysis by identifying:

- **Bottlenecks**: Components with high `LocalTimePercent` indicate performance bottlenecks
- **Resource Consumption**: High memory or CPU usage indicates resource-intensive operations
- **Data Skew**: Uneven row counts across fragment instances indicate data skew
- **I/O Performance**: High scan time relative to total time indicates I/O bottlenecks
- **Network Overhead**: High network transmission time indicates network bottlenecks

## 7. Profile Collection Configuration

Profile collection can be configured via session variables and system configuration:

**Session Variables:**

- `enable_profile`: Enable/disable profile collection (default: true)
- `runtime_profile_report_interval`: Interval for periodic profile reporting in seconds (default: 10)
- `is_report_success`: Whether to report profiles for successful queries (default: true)
- `big_query_profile_threshold`: Time threshold for selective profiling (default: 0, meaning disabled)
- `pipeline_profile_level`: Controls the level of detail in query profiles. A query profile has five layers: Fragment, FragmentInstance, Pipeline, PipelineDriver, and Operator. Level 0 shows only core metrics, level 1 (default) simplifies the profile by merging layers, and level 2 retains all layers (default: 1)

**System Configuration:**

- `profile_report_interval`: Interval for `ProfileReportWorker` periodic reporting in seconds (default: 1)
- `profile_info_reserved_num`: Maximum number of profiles to retain in `ProfileManager` (default: 100)
- `default_big_load_profile_threshold_second`: Default threshold for load operations when `big_query_profile_threshold` is 0 (default: 300 seconds)

**Profile Collection Modes:**

- **Full Profiling**: Collects detailed profiles for all operators (default for queries with `enable_profile=true`)
- **Minimal Profiling**: Collects only query-level metrics (for performance-sensitive scenarios)
- **Selective Profiling**: Collects profiles only for specific query types (e.g., data loading)

### 7.1. Selective Profiling with `big_query_profile_threshold`

`big_query_profile_threshold` enables selective profiling to reduce overhead in production environments by profiling only queries that exceed a specified execution time threshold. This is particularly useful for high-throughput systems where profiling all queries would impose significant overhead.

**How It Works:**

When `big_query_profile_threshold` is set to a value greater than 0 and `enable_profile=false`, profiling is dynamically enabled during query execution:

1. **Initial State**: If `enable_profile=false`, profiling is disabled when the query starts
2. **Threshold Check**: During query execution, the system periodically checks if the query's execution time exceeds the threshold
3. **Dynamic Enablement**: Once the threshold is exceeded, profiling is automatically enabled for the remainder of the query execution
4. **Profile Collection**: Profile data is collected from the point of enablement onward

**Important**: If `enable_profile=true` is explicitly set, profiling is enabled from the start regardless of `big_query_profile_threshold`. The threshold only applies when `enable_profile=false`.

**Configuration:**

```sql
-- Enable profiling for queries exceeding 30 seconds
SET GLOBAL big_query_profile_threshold = '30s';

-- Enable profiling for queries exceeding 500 milliseconds
SET big_query_profile_threshold = '500ms';

-- Enable profiling for queries exceeding 60 minutes
SET big_query_profile_threshold = '60m';
```

**Implementation Details:**

The threshold check is performed in `QueryContext::enable_profile()`:

```cpp
bool enable_profile() {
    if (_enable_profile) {
        return true;  // Explicitly enabled
    }
    if (_big_query_profile_threshold_ns <= 0) {
        return false;  // Threshold not set
    }
    // Check if query execution time exceeds threshold
    return MonotonicNanos() - _query_begin_time > _big_query_profile_threshold_ns;
}
```

**Time Unit Support:**

The threshold supports multiple time units:

- `NANOSECOND`: Nanoseconds
- `MICROSECOND`: Microseconds  
- `MILLISECOND`: Milliseconds
- `SECOND`: Seconds (default)
- `MINUTE`: Minutes

**Interaction with `enable_profile`:**

The `enable_profile()` method checks `_enable_profile` first, then falls back to threshold checking:

```cpp
bool enable_profile() {
    if (_enable_profile) {
        return true;  // Explicitly enabled - threshold is ignored
    }
    // Threshold check only applies when enable_profile=false
    if (_big_query_profile_threshold_ns <= 0) {
        return false;
    }
    return MonotonicNanos() - _query_begin_time > _big_query_profile_threshold_ns;
}
```

Behavior:

- If `enable_profile=true`: Profiling is always enabled from query start, regardless of threshold
- If `enable_profile=false` and `big_query_profile_threshold > 0`: Profiling is enabled only when threshold is exceeded (dynamic enablement)
- If both are disabled (`enable_profile=false` and `big_query_profile_threshold=0`): No profiling

**Load Operation Handling:**

For load operations (INSERT, Broker Load, Stream Load), special handling applies:

- **Broker Load**: If `big_query_profile_threshold` is 0, the system uses `default_big_load_profile_threshold_second` (default: 300 seconds) as the threshold
- **Stream Load / Routine Load**: Uses `load_profile_collect_second` from query options if available, otherwise follows the same threshold logic as regular queries
- **Load Channel Profiling**: Load channel profiles respect the same threshold configuration

**Benefits:**

- **Reduced Overhead**: Fast queries (< threshold) avoid profiling overhead entirely
- **Focused Analysis**: Only slow queries (likely to have performance issues) are profiled
- **Production-Friendly**: Enables profiling in production without impacting high-throughput workloads
- **Automatic Selection**: No manual intervention required—slow queries are automatically profiled

**Use Cases:**

- **Production Monitoring**: Profile only slow queries to identify performance issues without impacting fast queries
- **Performance Analysis**: Focus profiling resources on queries that need optimization
- **Resource Conservation**: Reduce memory and CPU overhead from profiling in high-throughput environments

## 8. Profile Logging to Disk (`fe.profile.log`)

StarRocks can write query profile information to disk in JSON format for persistent storage and analysis. This is separate from the in-memory profile storage in `ProfileManager` and provides a durable record of query execution details.

### 8.1. Profile Log Population

The `fe.profile.log` file is populated after query completion in `StmtExecutor`:

**Population Flow:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Query Completion                                      │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  StmtExecutor.finishProfile()                                     │  │
│  │  ├─> Builds query profile                                          │  │
│  │  ├─> Pushes profile to ProfileManager                             │  │
│  │  ├─> Sets profile content in QueryDetail                          │  │
│  │  └─> Checks logging conditions                                    │  │
│  │      └─> if (enable_collect_query_detail_info &&                  │  │
│  │              enable_profile_log)                                   │  │
│  │          └─> Serialize QueryDetail to JSON                         │  │
│  │          └─> PROFILE_LOG.info(jsonString)                          │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└───────────────────────┼───────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Log4j Profile Logger                                  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Logger: "profile"                                                 │  │
│  │  Appender: ProfileFile (RollingFile)                              │  │
│  │  File: ${profile_log_dir}/fe.profile.log                          │  │
│  │  Format: JSON (one QueryDetail object per line)                   │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Code Location:**

```java
// fe/fe-core/src/main/java/com/starrocks/qe/StmtExecutor.java
if (Config.enable_collect_query_detail_info && Config.enable_profile_log) {
    String jsonString = GSON.toJson(queryDetail);
    PROFILE_LOG.info(jsonString);
}
```

### 8.2. Logging Conditions

Profile logging requires both conditions to be enabled:

- **`enable_collect_query_detail_info`**: Must be `true` to collect `QueryDetail` objects during query execution
- **`enable_profile_log`**: Must be `true` to write profiles to the log file

If either is disabled, no profile log entries are written.

### 8.3. QueryDetail Content

Each log entry contains a serialized `QueryDetail` object with:

- **Query Metadata**: Query ID, user, database, SQL statement, query type
- **Execution Timing**: Start time, end time, total execution time
- **Execution Status**: RUNNING, FINISHED, FAILED, CANCELLED
- **Resource Metrics**:
    - `scanRows`: Number of rows scanned
    - `scanBytes`: Bytes of data scanned
    - `returnRows`: Number of result rows returned
    - `cpuCostNs`: CPU time consumed (nanoseconds)
    - `memCostBytes`: Memory usage (bytes)
    - `spillBytes`: Data spilled to disk (bytes)
- **Profile Content**: Full query profile string (if profile was collected)
- **Execution Plan**: Profiling execution plan (if available)
- **Error Information**: Error messages and stack traces (for failed queries)
- **Resource Group**: Resource group information (if applicable)

### 8.4. Log File Management

The profile log file is managed by Log4j with automatic rotation and retention:

**Configuration Parameters:**

- **`profile_log_dir`**: Directory where `fe.profile.log` is written (default: `$STARROCKS_HOME/log`)
- **`profile_log_roll_size_mb`**: Size threshold for rotation in MB (default: 1024 MB = 1 GB)
- **`profile_log_roll_interval`**: Time-based rotation interval (default: `DAY`, also supports `HOUR`)
- **`profile_log_roll_num`**: Maximum number of rotated files to retain (default: 5)
- **`profile_log_delete_age`**: Age threshold for deleting old files (default: `1d`, supports formats like `7d`, `10h`, `60m`, `120s`)

**Rotation Behavior:**

- **Size-Based**: When `fe.profile.log` exceeds `profile_log_roll_size_mb`, it is rotated
- **Time-Based**: When `profile_log_roll_interval` is reached, it is rotated
- **File Naming**: Rotated files are named `fe.profile.log.${pattern}-%i` where:
    - `${pattern}` is based on `profile_log_roll_interval` (e.g., `yyyyMMdd` for DAY, `yyyyMMddHH` for HOUR)
    - `%i` is the file index (0, 1, 2, ...)
- **Retention**: Log4j retains up to `profile_log_roll_num` files and deletes older files based on `profile_log_delete_age`

**Example File Names:**

- `fe.profile.log` (current active file)
- `fe.profile.log.20240101-0` (rotated file from January 1, 2024, index 0)
- `fe.profile.log.20240101-1` (rotated file from January 1, 2024, index 1)
- `fe.profile.log.20240102-0` (rotated file from January 2, 2024, index 0)

### 8.5. Log Format

Each log entry is a single line containing a JSON object:

```json
{
  "queryId": "abc123...",
  "isQuery": true,
  "connectionId": 12345,
  "database": "test_db",
  "sql": "SELECT * FROM table1",
  "user": "admin",
  "startTime": 1704067200000,
  "endTime": 1704067205000,
  "latency": 5000,
  "state": "FINISHED",
  "scanRows": 1000000,
  "scanBytes": 104857600,
  "returnRows": 100,
  "cpuCostNs": 4500000000,
  "memCostBytes": 52428800,
  "spillBytes": 0,
  "profile": "...",
  "plan": "..."
}
```

### 8.6. Use Cases

Profile logging enables:

- **Historical Analysis**: Analyze query performance trends over time
- **Audit Trail**: Maintain a record of all query executions with detailed metrics
- **Offline Analysis**: Process profile logs with external tools (e.g., log aggregation systems, analytics platforms)
- **Troubleshooting**: Review query execution details for past queries without requiring in-memory profile retention
- **Performance Monitoring**: Track resource usage patterns and identify performance regressions

### 8.7. Performance Considerations

- **Disk I/O**: Writing profiles to disk adds I/O overhead, especially for high-throughput workloads
- **Disk Space**: Profile logs can consume significant disk space, especially with detailed profiles
- **Compression**: Consider enabling `enable_profile_log_compress` to reduce disk usage (if supported)
- **Selective Logging**: Use `big_query_profile_threshold` to log only slow queries, reducing log volume

## 9. Profile Transport

Profiles are transmitted from BE to FE nodes using **`report_exec_status` (Thrift RPC)** as the primary transport mechanism:

**Primary Transport: `report_exec_status` (Thrift RPC)**

- BE calls `FrontendService.reportExecStatus()` to send profiles to FE
- Profile data is serialized as `TRuntimeProfileTree` (Thrift) and included in `TReportExecStatusParams`
- This is the same Thrift RPC mechanism used for reporting execution status, errors, and other execution metadata
- FE receives profiles in `QeProcessorImpl.reportExecStatus()` and processes them in `DefaultCoordinator.updateFragmentExecStatus()`

**Optional Transport: `trigger_profile_report` (bRPC, Currently Unused)**

- FE can theoretically request profiles on-demand via `trigger_profile_report` bRPC call
- When BE receives this request, it internally triggers `report_exec_status` to send the profile back
- This uses bRPC with Thrift attachment (Protobuf envelope, Thrift payload), similar to `exec_plan_fragment`
- However, this mechanism is not currently used in the FE codebase - all profile collection relies on BE-initiated `report_exec_status` calls
- The API exists for potential future use cases, but the current implementation uses only BE-initiated reporting

**Profile Size Considerations:**

- Large profiles (many operators, many counters) can be several megabytes when serialized
- Compression (GZIP) reduces profile size by 5-10x for storage
- Profile transmission uses bRPC's attachment mechanism to avoid Protobuf message size limits

## 10. Profile Use Cases

Profiles serve multiple purposes in StarRocks:

**Performance Optimization:**

- Identify slow operators and optimize query plans
- Analyze resource consumption and adjust resource limits
- Detect data skew and adjust data distribution

**Troubleshooting:**

- Diagnose query failures by examining execution state
- Identify memory leaks by tracking memory usage over time
- Analyze network issues by examining data transmission metrics

**Query Monitoring:**

- Monitor query progress for long-running queries
- Track resource usage across the cluster
- Generate query performance reports

**Query Plan Analysis:**

- Understand query execution flow through profile hierarchy
- Analyze operator efficiency and selectivity
- Validate query optimization decisions

## 11. Implementation Details

### 11.1. Thread Safety

`RuntimeProfile` is designed for concurrent access:

- **Counters**: Use atomic operations for thread-safe updates
- **Child Profiles**: Protected by mutexes for safe addition/removal
- **Info Strings**: Use synchronized maps for thread-safe updates
- **Profile Merging**: Uses version numbers to prevent race conditions

### 11.2. Memory Management

Profile memory is managed carefully to avoid excessive overhead:

- **Profile Compression**: Profiles are compressed before storage to reduce memory usage
- **Profile Retention**: Old profiles are evicted when retention limit is reached
- **Profile Cleanup**: Profiles are cleaned up when queries complete or fail

### 11.3. Performance Impact

Profile collection has minimal performance impact:

- **Counter Updates**: Use atomic operations with relaxed memory ordering for low overhead
- **Profile Serialization**: Performed asynchronously to avoid blocking query execution
- **Profile Reporting**: Uses background threads to avoid impacting query latency

**Key Files to Reference:**

- `be/src/util/runtime_profile.h` / `be/src/util/runtime_profile.cpp` - RuntimeProfile implementation (C++)
- `fe/fe-core/src/main/java/com/starrocks/common/util/RuntimeProfile.java` - RuntimeProfile implementation (Java)
- `gensrc/thrift/RuntimeProfile.thrift` - Profile data structure definitions
- `be/src/runtime/profile_report_worker.cpp` - Periodic profile reporting
- `be/src/service/internal_service.cpp` - Profile report RPC handler
- `fe/fe-core/src/main/java/com/starrocks/common/util/ProfileManager.java` - Profile storage and management
- `fe/fe-core/src/main/java/com/starrocks/qe/scheduler/QueryRuntimeProfile.java` - Profile aggregation logic

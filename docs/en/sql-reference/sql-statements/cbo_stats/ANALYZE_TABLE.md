---
displayed_sidebar: docs
description: "ANALYZE TABLE creates a manual collection task for collecting CBO statistics."
products: [oss, enterprise, cloud, anywhere]
---

# ANALYZE TABLE

ANALYZE TABLE creates a manual collection task for collecting CBO statistics. By default, manual collection is a synchronous operation. You can also set it to an asynchronous operation. In asynchronous mode, after you run ANALYZE TABLE, the system immediately returns whether this statement is successful. However, the collection task will be running in the background and you do not have to wait for the result. You can check the status of the task by running SHOW ANALYZE STATUS. Asynchronous collection is suitable for tables with large data volume, whereas synchronous collection is suitable for tables with small data volume.

**Manual collection tasks are run only once after creation. You do not need to delete manual collection tasks.**

This statement is supported from v2.4.

### Manually collect basic statistics

For more information about basic statistics, see [Gather statistics for CBO](../../../using_starrocks/Cost_based_optimizer.md#basic-statistics).

#### Syntax

```SQL
ANALYZE [FULL|SAMPLE] TABLE tbl_name (col_name [,col_name])
[WITH SYNC | ASYNC MODE]
PROPERTIES (property [,property])
```

#### Parameter description

- Collection type
  - FULL: indicates full collection.
  - SAMPLE: indicates sampled collection.
  - If no collection type is specified, full collection is used by default.

- `col_name`: columns from which to collect statistics. Separate multiple columns with commas (`,`). If this parameter is not specified, the entire table is collected.

- [WITH SYNC | ASYNC MODE]: whether to run the manual collection task in synchronous or asynchronous mode. Synchronous collection is used by default if you do not specify this parameter.

- `PROPERTIES`: custom parameters. If `PROPERTIES` is not specified, the default settings in the `fe.conf` file are used. The properties that are actually used can be viewed via the `Properties` column in the output of SHOW ANALYZE STATUS.

| **PROPERTIES**                | **Type** | **Default value** | **Description**                                              |
| ----------------------------- | -------- | ----------------- | ------------------------------------------------------------ |
| statistic_sample_collect_rows | INT      | 200000            | The minimum number of rows to collect for sampled collection.If the parameter value exceeds the actual number of rows in your table, full collection is performed. |

#### Examples

Example 1: Manual full collection

```SQL
-- Manually collect full stats of a table using default settings.
ANALYZE TABLE tbl_name;

-- Manually collect full stats of a table using default settings.
ANALYZE FULL TABLE tbl_name;

-- Manually collect stats of specified columns in a table using default settings.
ANALYZE TABLE tbl_name(c1, c2, c3);
```

Example 2: Manual sampled collection

```SQL
-- Manually collect partial stats of a table using default settings.
ANALYZE SAMPLE TABLE tbl_name;

-- Manually collect stats of specified columns in a table, with the number of rows to collect specified.
ANALYZE SAMPLE TABLE tbl_name (v1, v2, v3) PROPERTIES(
    "statistic_sample_collect_rows" = "1000000"
);
```

### Manually collect histograms

For more information about histograms, see [Gather statistics for CBO](../../../using_starrocks/Cost_based_optimizer.md#histogram).

#### Syntax

```SQL
ANALYZE [FULL | SAMPLE] TABLE tbl_name UPDATE HISTOGRAM ON col_name [, col_name]
[WITH SYNC | ASYNC MODE]
[WITH N BUCKETS]
PROPERTIES (property [,property]);
```

#### Parameter description

- `FULL | SAMPLE`: how much data is read.
  - `SAMPLE` (default if neither keyword is specified): collects the histogram from a sample of the table. The number of sampled rows is controlled by the properties described below.
  - `FULL`: reads every row. `histogram_sample_ratio` is ignored and the number of rows is not limited by `histogram_max_sample_row_count`, so the counts of the most common values and the per-bucket distinct counts have no sampling error. `FULL` is supported only for StarRocks native tables and requires `histogram_collect_mode` to be `sketch`. Otherwise, an error is returned.

- `col_name`: columns from which to collect statistics. Separate multiple columns with commas (`,`). If this parameter is not specified, the entire table is collected. This parameter is required for histograms.

- [WITH SYNC | ASYNC MODE]: whether to run the manual collection task in synchronous or asynchronous mode. Synchronous collection is used by default if you do not specify this parameter.

- `WITH N BUCKETS`: `N` is the number of buckets for histogram collection. If not specified, the default value in `fe.conf` is used.

- PROPERTIES: custom parameters. If `PROPERTIES` is not specified, the default settings in `fe.conf` are used. The properties that are actually used can be viewed via the `Properties` column in the output of SHOW ANALYZE STATUS.

| **PROPERTIES**                 | **Type** | **Default value** | **Description**                                              |
| ------------------------------ | -------- | ----------------- | ------------------------------------------------------------ |
| statistic_sample_collect_rows  | INT      | 200000            | The minimum number of rows to collect. If the parameter value exceeds the actual number of rows in your table, full collection is performed. |
| histogram_buckets_size         | LONG     | 64                | The default bucket number for a histogram.                   |
| histogram_mcv_size             | INT      | 100               | The number of most common values (MCV) for a histogram.      |
| histogram_sample_ratio         | FLOAT    | 0.1               | The sampling ratio for a histogram.                          |
| histogram_max_sample_row_count | LONG     | 10000000          | The maximum number of rows to collect for a histogram.       |
| histogram_collect_mode         | STRING   | sort              | The mode in which a histogram of a native table is collected. Valid values: `sort` and `sketch`. See [Collection modes](#collection-modes). If this property is not specified, the FE configuration item `histogram_collect_mode` is used. |

The number of rows to collect for a histogram is controlled by multiple parameters. It is the larger value between `statistic_sample_collect_rows` and table row count * `histogram_sample_ratio`. The number cannot exceed the value specified by `histogram_max_sample_row_count`. If the value is exceeded, `histogram_max_sample_row_count` takes precedence. These parameters do not apply when `FULL` is specified.

#### Collection modes

A histogram of a StarRocks native table can be collected in one of two modes, which is set by the property `histogram_collect_mode` or, if the property is not specified, by the FE configuration item of the same name. Both modes store the histogram in the same format.

- `sort` (default): StarRocks runs two queries for each column. The first query finds the most common values. The second query sorts the remaining sampled values and builds the buckets on a single thread.
- `sketch`: StarRocks builds mergeable sketches by using [`ds_frequent`](../../sql-functions/aggregate-functions/ds_frequent.md) for the most common values and [`ds_kll`](../../sql-functions/aggregate-functions/ds_kll.md) for the buckets. A sketch does not need sorted input, so one query collects many columns and runs as an ordinary multi-stage aggregation. Columns of fixed-size types are collected in batches of up to `histogram_sketch_max_columns_per_query` (FE configuration item, default: 128) columns per query, and each string column is collected by a query of its own. The bounds of the buckets are approximate: the rank of each bound is within about 0.7% of the rows from the exact one.

The `Properties` column in the output of SHOW ANALYZE STATUS shows how a collection task of a native table ran: the mode that was used (`histogram_collect_mode`), the number of statistics queries (`histogram_collect_queries`), and the elapsed time in milliseconds (`histogram_collect_elapsed_ms`).

#### Examples

```SQL
-- Manually collect histograms on v1 using the default settings.
ANALYZE TABLE tbl_name UPDATE HISTOGRAM ON v1;

-- Manually collect histograms on v1 and v2, with 32 buckets, 32 MCVs, and 50% sampling ratio.
ANALYZE TABLE tbl_name UPDATE HISTOGRAM ON v1,v2 WITH 32 BUCKETS 
PROPERTIES(
   "histogram_mcv_size" = "32",
   "histogram_sample_ratio" = "0.5"
);

-- Collect histograms on v1 and v2 from sketches.
ANALYZE TABLE tbl_name UPDATE HISTOGRAM ON v1,v2
PROPERTIES(
   "histogram_collect_mode" = "sketch"
);

-- Collect histograms on v1 and v2 from every row of the table.
ANALYZE FULL TABLE tbl_name UPDATE HISTOGRAM ON v1,v2
PROPERTIES(
   "histogram_collect_mode" = "sketch"
);
```

## References

[SHOW ANALYZE STATUS](SHOW_ANALYZE_STATUS.md): view the status of a manual collection task.

[KILL ANALYZE](KILL_ANALYZE.md): cancel a manual collection task that is running.

For more information about collecting statistics for CBO, see [Gather statistics for CBO](../../../using_starrocks/Cost_based_optimizer.md).

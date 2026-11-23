---
displayed_sidebar: docs
description: "ANALYZE TABLE 创建手动采集任务进行 CBO 统计信息采集。"
products: [oss, enterprise, cloud, anywhere]
---

# ANALYZE TABLE

## 功能

创建手动采集任务，进行 CBO 统计信息采集。**手动采集默认为同步操作。您也可以将手动任务设置为异步，执行命令后，会立即返回命令的状态，但是统计信息采集任务会在后台运行，运行的状态可以使用 SHOW ANALYZE STATUS 查看。手动任务创建后仅会执行一次，无需手动删除。**

### 手动采集基础统计信息

关于基础统计信息，请参见[CBO 统计信息](../../../using_starrocks/Cost_based_optimizer.md#统计信息数据类型)。

#### 语法

```SQL
ANALYZE [FULL|SAMPLE] TABLE tbl_name (col_name [,col_name])
[WITH SYNC | ASYNC MODE]
PROPERTIES (property [,property])
```

#### 参数说明

- 采集类型
  - FULL：全量采集。
  - SAMPLE：抽样采集。
  - 如果不指定采集类型，默认为全量采集。

- `WITH SYNC | ASYNC MODE`: 如果不指定，默认为同步操作。

- `col_name`: 要采集统计信息的列，多列使用逗号分隔。如果不指定，表示采集整张表的信息。

- PROPERTIES: 采集任务的自定义参数。如果不配置，则采用`fe.conf`中的默认配置。任务实际执行中使用的 PROPERTIES，可以通过 SHOW ANALYZE STATUS 返回结果中的 `Properties` 列查看。

| **PROPERTIES**                | **类型** | **默认值** | **说明**                                                     |
| ----------------------------- | -------- | ---------- | ------------------------------------------------------------ |
| statistic_sample_collect_rows | INT      | 200000     | 最小采样行数。如果参数取值超过了实际的表行数，默认进行全量采集。 |

#### 示例

示例1：手动全量采集

```SQL
-- 手动全量采集指定表的统计信息，使用默认配置。
ANALYZE TABLE tbl_name;

-- 手动全量采集指定表的统计信息，使用默认配置。
ANALYZE FULL TABLE tbl_name;

-- 手动全量采集指定表指定列的统计信息，使用默认配置。
ANALYZE TABLE tbl_name(c1, c2, c3);
```

示例2：手动抽样采集

```SQL
-- 手动抽样采集指定表的统计信息，使用默认配置。
ANALYZE SAMPLE TABLE tbl_name;

-- 手动抽样采集指定表指定列的统计信息，设置抽样行数。
ANALYZE SAMPLE TABLE tbl_name (v1, v2, v3) PROPERTIES(
    "statistic_sample_collect_rows" = "1000000"
);
```

### 手动采集直方图统计信息

关于直方图的说明，请参见[CBO 统计信息](../../../using_starrocks/Cost_based_optimizer.md#统计信息数据类型)。

#### 语法

```SQL
ANALYZE [FULL | SAMPLE] TABLE tbl_name UPDATE HISTOGRAM ON col_name [, col_name]
[WITH SYNC | ASYNC MODE]
[WITH N BUCKETS]
PROPERTIES (property [,property]);
```

#### 参数说明

- `FULL | SAMPLE`: 读取多少数据。
  - `SAMPLE`（未指定关键字时的默认值）：基于表的抽样数据采集直方图。抽样行数由下文的属性控制。
  - `FULL`：读取所有行。此时忽略 `histogram_sample_ratio`，行数也不受 `histogram_max_sample_row_count` 的限制，因此 MCV 的计数和每个分桶的不同值数量没有抽样误差。`FULL` 仅支持 StarRocks 内表，并且要求 `histogram_collect_mode` 为 `sketch`，否则返回错误。

- `col_name`: 要采集统计信息的列，多列使用逗号分隔。该参数必填。

- `WITH SYNC | ASYNC MODE`: 如果不指定，默认为同步采集。

- `WITH ``N`` BUCKETS`: `N`为直方图的分桶数。如果不指定，则使用`fe.conf`中的默认值。

- PROPERTIES: 采集任务的自定义参数。如果不指定，则使用`fe.conf`中的默认配置。

| **PROPERTIES**                 | **类型** | **默认值** | **说明**                                                     |
| ------------------------------ | -------- | ---------- | ------------------------------------------------------------ |
| statistic_sample_collect_rows  | INT      | 200000     | 最小采样行数。如果参数取值超过了实际的表行数，默认进行全量采集。 |
| histogram_mcv_size             | INT      | 100        | 直方图 most common value (MCV) 的数量。                      |
| histogram_sample_ratio         | FLOAT    | 0.1        | 直方图采样比例。                                             |
| histogram_max_sample_row_count | LONG     | 10000000   | 直方图最大采样行数。                                         |
| histogram_collect_mode | STRING | sort | 内表直方图的采集模式。有效值：`sort` 和 `sketch`。参见[采集模式](#采集模式)。如果不指定该属性，则使用 FE 配置项 `histogram_collect_mode`。 |

直方图的采样行数由多个参数共同控制，采样行数取 `statistic_sample_collect_rows` 和表总行数 `histogram_sample_ratio` 两者中的最大值。最多不超过 `histogram_max_sample_row_count` 指定的行数。如果超过，则按照该参数定义的上限行数进行采集。指定 `FULL` 时，这些参数不生效。

直方图任务实际执行中使用的**PROPERTIES**，可以通过 SHOW ANALYZE STATUS 返回结果中的**PROPERTIES**列查看。

#### 采集模式

StarRocks 内表的直方图可以通过两种模式采集。采集模式由属性 `histogram_collect_mode` 指定；如果未指定该属性，则由同名的 FE 配置项决定。两种模式存储的直方图格式相同。

- `sort`（默认）：StarRocks 为每一列执行两条查询。第一条查询找出 MCV。第二条查询对其余的采样值排序，并在单个线程上构建分桶。
- `sketch`：StarRocks 构建可合并的草图，使用 [`ds_frequent`](../../sql-functions/aggregate-functions/ds_frequent.md) 获取 MCV，使用 [`ds_kll`](../../sql-functions/aggregate-functions/ds_kll.md) 获取分桶。草图不要求输入有序，因此一条查询可以采集多列，并作为普通的多阶段聚合执行。定长类型的列分批采集，每条查询最多采集 `histogram_sketch_max_columns_per_query`（FE 配置项，默认值：128）列；每个字符串列由单独的一条查询采集。分桶的边界是近似值：每个边界的排名与精确排名之差约在总行数的 0.7% 以内。

对于内表的采集任务，SHOW ANALYZE STATUS 返回结果中的 `Properties` 列会显示任务的执行情况：使用的模式（`histogram_collect_mode`）、执行的统计信息查询数量（`histogram_collect_queries`）以及耗时（`histogram_collect_elapsed_ms`，单位：毫秒）。

#### 示例

```SQL
-- 手动采集v1列的直方图信息，使用默认配置。
ANALYZE TABLE tbl_name UPDATE HISTOGRAM ON v1;

-- 手动采集 v1 列的直方图信息，指定 32 个分桶，MCV 指定为 32 个，采样比例为 50%。
ANALYZE TABLE tbl_name UPDATE HISTOGRAM ON v1,v2 WITH 32 BUCKETS 
PROPERTIES(
   "histogram_mcv_size" = "32",
   "histogram_sample_ratio" = "0.5"
);

-- 基于草图采集 v1 和 v2 列的直方图。
ANALYZE TABLE tbl_name UPDATE HISTOGRAM ON v1,v2
PROPERTIES(
   "histogram_collect_mode" = "sketch"
);

-- 读取表的所有行，采集 v1 和 v2 列的直方图。
ANALYZE FULL TABLE tbl_name UPDATE HISTOGRAM ON v1,v2
PROPERTIES(
   "histogram_collect_mode" = "sketch"
);
```

## 相关文档

[SHOW ANALYZE STATUS](SHOW_ANALYZE_STATUS.md)：查看当前所有**手动采集任务**的状态。

[KILL ANALYZE](KILL_ANALYZE.md)：取消**正在运行中（Running）**的统计信息收集任务。

想了解更多 CBO 统计信息采集的内容，参见[CBO 统计信息](../../../using_starrocks/Cost_based_optimizer.md)。

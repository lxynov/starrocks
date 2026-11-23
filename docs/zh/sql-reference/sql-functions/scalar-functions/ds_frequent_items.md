---
displayed_sidebar: docs
description: "从序列化的 Apache DataSketches 频繁项草图中返回出现最频繁的值及其计数的上下界，结果为 JSON 字符串。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent_items

标量函数。读取序列化的频繁项草图（`VARBINARY`），以 JSON 字符串返回该草图所跟踪的值中出现最频繁的值，以及每个值的计数所在的上下界。

草图使用 [`ds_frequent`](../aggregate-functions/ds_frequent.md) 构建，或使用 [`ds_frequent_union`](../aggregate-functions/ds_frequent_union.md) 合并得到。

## 语法

```Haskell
VARCHAR ds_frequent_items(sketch, n)
```

### 参数

- `sketch`: 包含使用 `ds_frequent` 构建的草图的 VARBINARY 值。
- `n`: 最多返回的值的数量，取值为非负的 INT。

## 返回值

返回 VARCHAR 值，其内容为如下格式的 JSON 对象：

```json
{"total":N,"max_error":E,"items":[["value",estimate,lower_bound,upper_bound],...]}
```

- `total`: 草图所基于的值的数量。
- `max_error`: 每个值的下界与上界之差。所有计数都精确时为 `0`。
- `items`: 最多 `n` 个值，出现最频繁的排在最前。值的真实出现次数介于 `lower_bound` 和 `upper_bound` 之间。与 Apache DataSketches 一致，`estimate` 等于 `upper_bound`。计数相同的值按升序排列。

值以字符串形式输出，格式与 `CAST(value AS VARCHAR)` 的返回结果相同。

草图未跟踪的值，其出现次数最多为 `max_error`。

如果 `sketch` 或 `n` 为 `NULL`，则返回 `NULL`。

## 示例

```sql
-- page_views 表有 1000 行数据：国家为 US 的有 600 行，DE 有 300 行，JP 有 100 行。参见 ds_frequent。
SELECT ds_frequent_items(ds_frequent(country), 2) FROM page_views;
```

```plaintext
+------------------------------------------------------------------------------+
| ds_frequent_items(ds_frequent(country), 2)                                   |
+------------------------------------------------------------------------------+
| {"total":1000,"max_error":0,"items":[["US",600,600,600],["DE",300,300,300]]} |
+------------------------------------------------------------------------------+
```

该列的不同值很少，因此每个计数都是精确的。

## 关键词

DS_FREQUENT_ITEMS, DS_FREQUENT, DS_FREQUENT_UNION

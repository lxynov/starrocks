---
displayed_sidebar: docs
description: "跨行合并序列化的 Apache DataSketches KLL 分位数草图，以 VARBINARY 返回单个草图。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_union

聚合函数。跨行合并序列化的 KLL 分位数草图（`VARBINARY`），返回单个序列化的草图。

输入是使用 [`ds_kll`](./ds_kll.md) 构建的草图列，例如按天或按地区存储的草图。合并后的草图描述了所有输入草图所基于的全部值，其排名误差与直接基于这些值构建的草图相同。

## 语法

```Haskell
VARBINARY ds_kll_union(sketch)
```

### 参数

- `sketch`: 使用 `ds_kll` 构建的草图的 VARBINARY 列。同一分组内所有草图的值必须是相同的数据类型，否则返回错误。

## 返回值

返回包含合并后草图的 VARBINARY 值。`NULL` 值会被忽略。如果某个分组中没有可合并的草图，则返回 `NULL`。

## 示例

```sql
-- amount_sketches 中每个地区保存了一个订单金额的草图。参见 ds_kll。
SELECT ds_kll_histogram(ds_kll_union(sketch), 4) FROM amount_sketches;
```

## 关键词

DS_KLL_UNION, DS_KLL, DS_KLL_HISTOGRAM

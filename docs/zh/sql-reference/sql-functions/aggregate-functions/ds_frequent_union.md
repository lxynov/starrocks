---
displayed_sidebar: docs
description: "跨行合并序列化的 Apache DataSketches 频繁项草图，以 VARBINARY 返回单个草图。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent_union

聚合函数。跨行合并序列化的频繁项草图（`VARBINARY`），返回单个序列化的草图。

输入是使用 [`ds_frequent`](./ds_frequent.md) 构建的草图列，例如按天或按地区存储的草图。合并后的草图描述了所有输入草图所基于的全部值，其计数的误差范围与直接基于这些值构建的草图相同。

## 语法

```Haskell
VARBINARY ds_frequent_union(sketch)
```

### 参数

- `sketch`: 使用 `ds_frequent` 构建的草图的 VARBINARY 列。同一分组内所有草图的值必须是相同的数据类型，否则返回错误。

## 返回值

返回包含合并后草图的 VARBINARY 值。`NULL` 值会被忽略。如果某个分组中没有可合并的草图，则返回 `NULL`。

## 示例

```sql
-- country_sketches 中每种浏览器保存了一个国家的草图。参见 ds_frequent。
SELECT ds_frequent_items(ds_frequent_union(sketch), 2) FROM country_sketches;
```

## 关键词

DS_FREQUENT_UNION, DS_FREQUENT, DS_FREQUENT_ITEMS

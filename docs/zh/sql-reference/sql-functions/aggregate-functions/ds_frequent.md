---
displayed_sidebar: docs
description: "为列的值构建 Apache DataSketches 频繁项草图，并以 VARBINARY 返回序列化的草图。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent

聚合函数。为表达式的值构建频繁项（Frequent Items）草图，并以 `VARBINARY` 返回序列化的草图。

频繁项草图只需扫描一遍数据、占用有限的内存，即可找出一列中出现最频繁的值。它在一个有 2048 个槽位的哈希表中统计每个值出现的次数，该哈希表最多保存 1536 个值。哈希表存满后，草图会将所有计数器减去相同的数值，并丢弃减到零的计数器。因此，对于草图仍在跟踪的值，其真实出现次数介于一个下界和一个上界之间，并且所有值的上下界之差都相同：

- 只要该列的不同值不超过 1536 个，所有计数都是精确的。
- 否则，上下界之差不超过构建草图所用行数的 0.17%。

草图合并后仍然满足上述保证，因此 `ds_frequent` 作为普通的多阶段聚合执行：每个实例为自己的数据行构建草图，然后合并这些部分草图。已存储的草图之后可以使用 [`ds_frequent_union`](./ds_frequent_union.md) 与其他草图合并，并使用 [`ds_frequent_items`](../scalar-functions/ds_frequent_items.md) 读取。

该函数基于 Apache DataSketches。更多信息，请参见 [Frequent Items Sketches](https://datasketches.apache.org/docs/Frequency/FrequentItemsOverview.html)。

## 语法

```Haskell
VARBINARY ds_frequent(expr)
```

### 参数

- `expr`: 要构建草图的表达式。支持的类型为 BOOLEAN、TINYINT、SMALLINT、INT、BIGINT、LARGEINT、FLOAT、DOUBLE、DECIMAL、DATE、DATETIME、CHAR 和 VARCHAR。

## 返回值

返回包含序列化草图的 VARBINARY 值。草图中记录了其值的数据类型。

`NULL` 值会被忽略。如果某个分组中没有可用于构建草图的值，则返回 `NULL`。

## 示例

```sql
CREATE TABLE page_views (
  id BIGINT,
  country VARCHAR(16),
  browser VARCHAR(16)
)
DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 3;

INSERT INTO page_views
SELECT generate_series,
       CASE WHEN generate_series % 10 < 6 THEN 'US' WHEN generate_series % 10 < 9 THEN 'DE' ELSE 'JP' END,
       concat('b', generate_series % 4)
FROM TABLE(generate_series(1, 1000));

-- 出现最频繁的两个国家。
SELECT ds_frequent_items(ds_frequent(country), 2) FROM page_views;

-- 每种浏览器一个草图，存储起来供以后使用。
CREATE TABLE country_sketches AS
SELECT browser, ds_frequent(country) AS sketch FROM page_views GROUP BY browser;
```

## 关键词

DS_FREQUENT, DS_FREQUENT_UNION, DS_FREQUENT_ITEMS

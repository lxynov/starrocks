---
displayed_sidebar: docs
description: "为列的值构建 Apache DataSketches KLL 分位数草图，并以 VARBINARY 返回序列化的草图。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_accumulate

聚合函数。为表达式的值构建 KLL 分位数草图，并以 `VARBINARY` 返回序列化的草图。

KLL 草图只需扫描一遍数据、占用很少的内存，即可概括一列数据的分布。它根据输入数据的一小部分样本回答“位于某个排名的值是多少”。误差体现在排名上，而不是值上：针对某个排名返回的值是输入中真实存在的值，其真实排名与所查询排名之差不超过排名误差。草图默认使用 `k = 400`，在 99% 置信度下排名误差约为 0.68%。`k` 越大，排名误差越小，草图也越大。

草图合并后仍然满足上述保证，因此 `ds_kll_accumulate` 作为普通的多阶段聚合执行：每个实例为自己的数据行构建草图，然后合并这些部分草图。已存储的草图之后可以使用 [`ds_kll_combine`](./ds_kll_combine.md) 与其他草图合并，并使用 [`ds_kll_estimate`](../scalar-functions/ds_kll_estimate.md) 读取。如果不需要保留草图，可以使用 [`ds_kll_histogram`](./ds_kll_histogram.md) 直接得到列的直方图。

该函数基于 Apache DataSketches。更多信息，请参见 [KLL Sketch](https://datasketches.apache.org/docs/KLL/KLLSketch.html)。

## 语法

```Haskell
VARBINARY ds_kll_accumulate(expr)
VARBINARY ds_kll_accumulate(expr, k)
```

### 参数

- `expr`: 要构建草图的表达式。支持的类型为 BOOLEAN、TINYINT、SMALLINT、INT、BIGINT、LARGEINT、FLOAT、DOUBLE、DECIMAL、DATE 和 DATETIME。
- `k`: INT 类型的字面量，取值范围为 8 到 65535，默认值为 400。控制草图的精度和大小。

## 返回值

返回包含序列化草图的 VARBINARY 值。草图中记录了其值的数据类型。

`NULL` 值会被忽略；FLOAT 或 DOUBLE 列中的非数值（NaN）没有排名，也会被忽略。如果某个分组中没有可用于构建草图的值，则返回 `NULL`。

## 示例

```sql
CREATE TABLE orders (
  id BIGINT,
  region VARCHAR(16),
  amount DECIMAL(10, 2),
  order_date DATE
)
DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 3;

INSERT INTO orders
SELECT generate_series, concat('r', generate_series % 3), generate_series / 4,
       date_add('2024-01-01', INTERVAL generate_series % 200 DAY)
FROM TABLE(generate_series(1, 1000));

-- 金额的 4 个等高分桶。
SELECT ds_kll_estimate(ds_kll_accumulate(amount), 4) FROM orders;

-- 使用更大的 k，以获得更小的排名误差。
SELECT ds_kll_estimate(ds_kll_accumulate(amount, 1000), 4) FROM orders;

-- 每个地区一个草图，存储起来供以后使用。
CREATE TABLE amount_sketches AS
SELECT region, ds_kll_accumulate(amount) AS sketch FROM orders GROUP BY region;
```

## 关键词

DS_KLL_ACCUMULATE, DS_KLL_COMBINE, DS_KLL_ESTIMATE, DS_KLL_HISTOGRAM

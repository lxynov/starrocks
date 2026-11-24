---
displayed_sidebar: docs
description: "使用 Apache DataSketches KLL 分位数草图，以 JSON 字符串返回列值的等高分桶的边界和排名。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_histogram

聚合函数。以 JSON 字符串返回表达式的值的等高分桶的边界和排名。该函数为这些值构建 KLL 分位数草图，并从草图中读取分桶。

`ds_kll_histogram(expr, n)` 是 `ds_kll_estimate(ds_kll_accumulate(expr), n)` 的简写。草图的构建方式参见 [`ds_kll_accumulate`](./ds_kll_accumulate.md)，分桶的读取方式参见 [`ds_kll_estimate`](../scalar-functions/ds_kll_estimate.md)。如果需要存储草图并在之后合并，请改用这些函数以及 [`ds_kll_combine`](./ds_kll_combine.md)。

## 语法

```Haskell
VARCHAR ds_kll_histogram(expr, n)
VARCHAR ds_kll_histogram(expr, n, k)
```

### 参数

- `expr`: 要汇总的表达式。支持的类型为 BOOLEAN、TINYINT、SMALLINT、INT、BIGINT、LARGEINT、FLOAT、DOUBLE、DECIMAL、DATE 和 DATETIME。
- `n`: 分桶数量，取值为 1 到 65536 之间的 INT。
- `k`: INT 类型的字面量，取值范围为 8 到 65535，默认值为 400。与 `ds_kll_accumulate` 中相同，控制草图的精度和大小。

## 返回值

返回 VARCHAR 值，其内容为 `ds_kll_estimate` 所述格式的 JSON 对象：

```json
{"count":N,"min":"v","max":"v","buckets":[["lower","upper",rank],...]}
```

`NULL` 值会被忽略；FLOAT 或 DOUBLE 列中的非数值（NaN）没有排名，也会被忽略。如果某个分组中没有可汇总的值，则返回 `NULL`。

## 示例

```sql
-- orders 表有 1000 行数据，amount 的取值从 0.25 到 250.00。参见 ds_kll_accumulate。
SELECT ds_kll_histogram(amount, 4) FROM orders;
```

```plaintext
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| ds_kll_histogram(amount, 4)                                                                                                                         |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| {"count":1000,"min":"0.25","max":"250.00","buckets":[["0.25","62.50",0.251],["62.50","125.00",0.5],["125.00","187.25",0.75],["187.25","250.00",1]]} |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
```

表中的值多于草图保留的值，因此草图会对其采样，每次运行得到的边界和排名可能略有不同。

```sql
-- 每个地区一个直方图，并使用更大的 k，以获得更小的排名误差。
SELECT region, ds_kll_histogram(amount, 4, 1000) FROM orders GROUP BY region;
```

## 关键词

DS_KLL_HISTOGRAM, DS_KLL_ACCUMULATE, DS_KLL_COMBINE, DS_KLL_ESTIMATE

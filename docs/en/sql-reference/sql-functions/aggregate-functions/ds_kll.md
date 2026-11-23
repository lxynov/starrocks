---
displayed_sidebar: docs
description: "Builds an Apache DataSketches KLL quantiles sketch of the values of a column and returns the serialized sketch as VARBINARY."
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll

Aggregate function that builds a KLL quantiles sketch of the values of an expression and returns the serialized sketch as `VARBINARY`.

A KLL sketch summarizes the distribution of a column in one pass and in little memory. It answers which value sits at a given rank from a small sample of the input. The error is on the rank, not on the value: the value returned for a rank is a real input value whose true rank is within the rank error of the rank that was asked for. The sketch uses `k = 400`, which gives a rank error of about 0.68% at 99% confidence.

Sketches merge without losing that guarantee, so `ds_kll` runs as an ordinary multi-stage aggregation: every instance sketches its own rows and the partial sketches are merged. A sketch that was stored can be merged with others later by using [`ds_kll_union`](./ds_kll_union.md), and read by using [`ds_kll_histogram`](../scalar-functions/ds_kll_histogram.md).

It is based on Apache DataSketches. For more information, see [KLL Sketch](https://datasketches.apache.org/docs/KLL/KLLSketch.html).

## Syntax

```Haskell
VARBINARY ds_kll(expr)
```

### Parameters

- `expr`: the expression to sketch. Supported types are BOOLEAN, TINYINT, SMALLINT, INT, BIGINT, LARGEINT, FLOAT, DOUBLE, DECIMAL, DATE, and DATETIME.

## Return value

Returns a VARBINARY value that contains the serialized sketch. The sketch records the data type of its values.

`NULL` values are ignored, and so is the not-a-number value of a FLOAT or DOUBLE column, which has no rank. If a group has no value to sketch, `NULL` is returned.

## Examples

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

-- Four equal-height buckets over the amounts.
SELECT ds_kll_histogram(ds_kll(amount), 4) FROM orders;

-- One sketch per region, stored for later.
CREATE TABLE amount_sketches AS
SELECT region, ds_kll(amount) AS sketch FROM orders GROUP BY region;
```

## Keywords

DS_KLL, DS_KLL_UNION, DS_KLL_HISTOGRAM

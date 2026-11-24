---
displayed_sidebar: docs
description: "Returns the bounds and ranks of equal-height buckets over the values of a column as a JSON string, by using an Apache DataSketches KLL quantiles sketch."
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_histogram

Aggregate function that returns the bounds and ranks of equal-height buckets over the values of an expression, as a JSON string. It builds a KLL quantiles sketch of the values and reads the buckets from it.

`ds_kll_histogram(expr, n)` is shorthand for `ds_kll_estimate(ds_kll_accumulate(expr), n)`. See [`ds_kll_accumulate`](./ds_kll_accumulate.md) for how the sketch is built and [`ds_kll_estimate`](../scalar-functions/ds_kll_estimate.md) for how the buckets are read. To store sketches and merge them later, use those functions and [`ds_kll_combine`](./ds_kll_combine.md) instead.

## Syntax

```Haskell
VARCHAR ds_kll_histogram(expr, n)
VARCHAR ds_kll_histogram(expr, n, k)
```

### Parameters

- `expr`: the expression to summarize. Supported types are BOOLEAN, TINYINT, SMALLINT, INT, BIGINT, LARGEINT, FLOAT, DOUBLE, DECIMAL, DATE, and DATETIME.
- `n`: the number of buckets, an INT between 1 and 65536.
- `k`: an INT literal between 8 and 65535. Default: 400. Controls the accuracy and the size of the sketch, as in `ds_kll_accumulate`.

## Return value

Returns a VARCHAR value that contains a JSON object in the format that `ds_kll_estimate` describes:

```json
{"count":N,"min":"v","max":"v","buckets":[["lower","upper",rank],...]}
```

`NULL` values are ignored, and so is the not-a-number value of a FLOAT or DOUBLE column, which has no rank. If a group has no value to summarize, `NULL` is returned.

## Examples

```sql
-- orders holds 1000 rows whose amount runs from 0.25 to 250.00. See ds_kll_accumulate.
SELECT ds_kll_histogram(amount, 4) FROM orders;
```

```plaintext
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| ds_kll_histogram(amount, 4)                                                                                                                         |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| {"count":1000,"min":"0.25","max":"250.00","buckets":[["0.25","62.50",0.251],["62.50","125.00",0.5],["125.00","187.25",0.75],["187.25","250.00",1]]} |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
```

The table holds more values than the sketch keeps, so the sketch samples them, and the bounds and ranks can differ slightly from one run to the next.

```sql
-- One histogram per region, with a larger k for a smaller rank error.
SELECT region, ds_kll_histogram(amount, 4, 1000) FROM orders GROUP BY region;
```

## Keywords

DS_KLL_HISTOGRAM, DS_KLL_ACCUMULATE, DS_KLL_COMBINE, DS_KLL_ESTIMATE

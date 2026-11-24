---
displayed_sidebar: docs
description: "Returns the bounds and ranks of equal-height buckets from a serialized Apache DataSketches KLL quantiles sketch, as a JSON string."
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_estimate

Scalar function that reads a serialized KLL quantiles sketch (`VARBINARY`) and returns the bounds and ranks of equal-height buckets over the values the sketch was built from, as a JSON string.

The sketch is built by using [`ds_kll_accumulate`](../aggregate-functions/ds_kll_accumulate.md) or merged by using [`ds_kll_combine`](../aggregate-functions/ds_kll_combine.md). To get the same result from a column directly, use [`ds_kll_histogram`](../aggregate-functions/ds_kll_histogram.md).

## Syntax

```Haskell
VARCHAR ds_kll_estimate(sketch, n)
```

### Parameters

- `sketch`: a VARBINARY value that contains a sketch built by using `ds_kll_accumulate`.
- `n`: the number of buckets, an INT between 1 and 65536.

## Return value

Returns a VARCHAR value that contains a JSON object in the following format:

```json
{"count":N,"min":"v","max":"v","buckets":[["lower","upper",rank],...]}
```

- `count`: the number of values the sketch was built from.
- `min` and `max`: the minimum and the maximum of those values, which are exact.
- `buckets`: the buckets in ascending order. The first bucket starts at the minimum and the last bucket ends at the maximum. Every other bucket starts at the upper bound of the bucket before it, which it does not contain. `rank` is the fraction of all the values that are less than or equal to the upper bound of the bucket.

Values are written as strings, in the same form as `CAST(value AS VARCHAR)` returns.

The upper bounds are the values at the ranks 1/n, 2/n, and so on. When a value occurs so often that several of those ranks fall on it, it becomes the upper bound of a single bucket, so fewer than `n` buckets can be returned.

Returns `NULL` if `sketch` or `n` is `NULL`.

## Examples

```sql
-- orders holds 1000 rows whose amount runs from 0.25 to 250.00. See ds_kll_accumulate.
SELECT ds_kll_estimate(ds_kll_accumulate(amount), 4) FROM orders;
```

```plaintext
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| ds_kll_estimate(ds_kll_accumulate(amount), 4)                                                                                                       |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| {"count":1000,"min":"0.25","max":"250.00","buckets":[["0.25","62.50",0.251],["62.50","125.00",0.5],["125.00","187.25",0.75],["187.25","250.00",1]]} |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
```

The table holds more values than the sketch keeps, so the sketch samples them, and the bounds and ranks can differ slightly from one run to the next.

## Keywords

DS_KLL_ESTIMATE, DS_KLL_ACCUMULATE, DS_KLL_COMBINE, DS_KLL_HISTOGRAM

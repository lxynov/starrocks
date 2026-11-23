---
displayed_sidebar: docs
description: "Returns the bounds and ranks of equal-height buckets from a serialized Apache DataSketches KLL quantiles sketch, as a JSON string."
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_histogram

Scalar function that reads a serialized KLL quantiles sketch (`VARBINARY`) and returns the bounds and ranks of equal-height buckets over the values the sketch was built from, as a JSON string.

The sketch is built by using [`ds_kll`](../aggregate-functions/ds_kll.md) or merged by using [`ds_kll_union`](../aggregate-functions/ds_kll_union.md).

## Syntax

```Haskell
VARCHAR ds_kll_histogram(sketch, n)
```

### Parameters

- `sketch`: a VARBINARY value that contains a sketch built by using `ds_kll`.
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
-- orders holds 1000 rows whose amount runs from 0.25 to 250.00. See ds_kll.
SELECT ds_kll_histogram(ds_kll(amount), 4) FROM orders;
```

```plaintext
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| ds_kll_histogram(ds_kll(amount), 4)                                                                                                                 |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| {"count":1000,"min":"0.25","max":"250.00","buckets":[["0.25","62.50",0.251],["62.50","125.00",0.5],["125.00","187.25",0.75],["187.25","250.00",1]]} |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
```

The table holds more values than the sketch keeps, so the sketch samples them, and the bounds and ranks can differ slightly from one run to the next.

## Keywords

DS_KLL_HISTOGRAM, DS_KLL, DS_KLL_UNION

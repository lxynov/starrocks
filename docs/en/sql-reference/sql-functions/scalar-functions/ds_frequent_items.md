---
displayed_sidebar: docs
description: "Returns the most frequent values from a serialized Apache DataSketches frequent items sketch, with the bounds of their counts, as a JSON string."
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent_items

Scalar function that reads a serialized frequent items sketch (`VARBINARY`) and returns the most frequent of the values the sketch tracks, with the bounds between which the count of each lies, as a JSON string.

The sketch is built by using [`ds_frequent`](../aggregate-functions/ds_frequent.md) or merged by using [`ds_frequent_union`](../aggregate-functions/ds_frequent_union.md).

## Syntax

```Haskell
VARCHAR ds_frequent_items(sketch, n)
```

### Parameters

- `sketch`: a VARBINARY value that contains a sketch built by using `ds_frequent`.
- `n`: the maximum number of values to return, a non-negative INT.

## Return value

Returns a VARCHAR value that contains a JSON object in the following format:

```json
{"total":N,"max_error":E,"items":[["value",estimate,lower_bound,upper_bound],...]}
```

- `total`: the number of values the sketch was built from.
- `max_error`: the gap between the lower bound and the upper bound of every value. It is `0` when every count is exact.
- `items`: up to `n` values, the most frequent first. The true count of a value lies between `lower_bound` and `upper_bound`. As in Apache DataSketches, `estimate` is equal to `upper_bound`. Values with equal counts are listed in ascending order.

Values are written as strings, in the same form as `CAST(value AS VARCHAR)` returns.

A value that the sketch does not track occurs at most `max_error` times.

Returns `NULL` if `sketch` or `n` is `NULL`.

## Examples

```sql
-- page_views holds 1000 rows: 600 for the country US, 300 for DE, and 100 for JP. See ds_frequent.
SELECT ds_frequent_items(ds_frequent(country), 2) FROM page_views;
```

```plaintext
+------------------------------------------------------------------------------+
| ds_frequent_items(ds_frequent(country), 2)                                   |
+------------------------------------------------------------------------------+
| {"total":1000,"max_error":0,"items":[["US",600,600,600],["DE",300,300,300]]} |
+------------------------------------------------------------------------------+
```

The column has few distinct values, so every count is exact.

## Keywords

DS_FREQUENT_ITEMS, DS_FREQUENT, DS_FREQUENT_UNION

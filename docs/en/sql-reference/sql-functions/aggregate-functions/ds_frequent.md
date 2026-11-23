---
displayed_sidebar: docs
description: "Builds an Apache DataSketches frequent items sketch of the values of a column and returns the serialized sketch as VARBINARY."
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent

Aggregate function that builds a frequent items sketch of the values of an expression and returns the serialized sketch as `VARBINARY`.

A frequent items sketch finds the most frequent values of a column in one pass and in bounded memory. It counts how often each value occurs in a hash map of 2048 entries, which holds up to 1536 values. When the map is full, the sketch lowers every counter by the same amount and drops the counters that reach zero. So for a value that the sketch still tracks, the true count lies between a lower bound and an upper bound, and the gap between the two is the same for every value:

- As long as the column has no more than 1536 distinct values, every count is exact.
- Otherwise, the gap is at most 0.17% of the rows the sketch was built from.

Sketches merge without losing that guarantee, so `ds_frequent` runs as an ordinary multi-stage aggregation: every instance sketches its own rows and the partial sketches are merged. A sketch that was stored can be merged with others later by using [`ds_frequent_union`](./ds_frequent_union.md), and read by using [`ds_frequent_items`](../scalar-functions/ds_frequent_items.md).

It is based on Apache DataSketches. For more information, see [Frequent Items Sketches](https://datasketches.apache.org/docs/Frequency/FrequentItemsOverview.html).

## Syntax

```Haskell
VARBINARY ds_frequent(expr)
```

### Parameters

- `expr`: the expression to sketch. Supported types are BOOLEAN, TINYINT, SMALLINT, INT, BIGINT, LARGEINT, FLOAT, DOUBLE, DECIMAL, DATE, DATETIME, CHAR, and VARCHAR.

## Return value

Returns a VARBINARY value that contains the serialized sketch. The sketch records the data type of its values.

`NULL` values are ignored. If a group has no value to sketch, `NULL` is returned.

## Examples

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

-- The two most frequent countries.
SELECT ds_frequent_items(ds_frequent(country), 2) FROM page_views;

-- One sketch per browser, stored for later.
CREATE TABLE country_sketches AS
SELECT browser, ds_frequent(country) AS sketch FROM page_views GROUP BY browser;
```

## Keywords

DS_FREQUENT, DS_FREQUENT_UNION, DS_FREQUENT_ITEMS

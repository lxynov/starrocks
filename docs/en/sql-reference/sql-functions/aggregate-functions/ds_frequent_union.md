---
displayed_sidebar: docs
description: "Merges serialized Apache DataSketches frequent items sketches across rows into a single sketch returned as VARBINARY."
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent_union

Aggregate function that merges serialized frequent items sketches (`VARBINARY`) across rows and returns a single serialized sketch.

The input is a column of sketches built by using [`ds_frequent`](./ds_frequent.md), for example sketches that were stored per day or per region. The merged sketch describes all the values the input sketches were built from, and its counts stay within the same error as those of a sketch built over those values directly.

## Syntax

```Haskell
VARBINARY ds_frequent_union(sketch)
```

### Parameters

- `sketch`: a VARBINARY column of sketches built by using `ds_frequent`. All the sketches of a group must hold values of the same data type. Otherwise, an error is returned.

## Return value

Returns a VARBINARY value that contains the merged sketch. `NULL` values are ignored. If a group has no sketch to merge, `NULL` is returned.

## Examples

```sql
-- country_sketches holds one sketch of the countries per browser. See ds_frequent.
SELECT ds_frequent_items(ds_frequent_union(sketch), 2) FROM country_sketches;
```

## Keywords

DS_FREQUENT_UNION, DS_FREQUENT, DS_FREQUENT_ITEMS

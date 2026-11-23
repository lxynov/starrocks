---
displayed_sidebar: docs
description: "Merges serialized Apache DataSketches KLL quantiles sketches across rows into a single sketch returned as VARBINARY."
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_union

Aggregate function that merges serialized KLL quantiles sketches (`VARBINARY`) across rows and returns a single serialized sketch.

The input is a column of sketches built by using [`ds_kll`](./ds_kll.md), for example sketches that were stored per day or per region. The merged sketch describes all the values the input sketches were built from, with the same rank error as a sketch built over those values directly.

## Syntax

```Haskell
VARBINARY ds_kll_union(sketch)
```

### Parameters

- `sketch`: a VARBINARY column of sketches built by using `ds_kll`. All the sketches of a group must hold values of the same data type. Otherwise, an error is returned.

## Return value

Returns a VARBINARY value that contains the merged sketch. `NULL` values are ignored. If a group has no sketch to merge, `NULL` is returned.

## Examples

```sql
-- amount_sketches holds one sketch of the order amounts per region. See ds_kll.
SELECT ds_kll_histogram(ds_kll_union(sketch), 4) FROM amount_sketches;
```

## Keywords

DS_KLL_UNION, DS_KLL, DS_KLL_HISTOGRAM

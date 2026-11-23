---
displayed_sidebar: docs
description: "シリアライズされた Apache DataSketches 頻出アイテムスケッチから、最も頻出する値とそのカウントの上下限を JSON 文字列として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent_items

スカラー関数。シリアライズされた頻出アイテムスケッチ（`VARBINARY`）を読み取り、スケッチが追跡している値のうち最も頻出する値と、それぞれのカウントが収まる上下限を JSON 文字列として返します。

スケッチは [`ds_frequent`](../aggregate-functions/ds_frequent.md) で構築するか、[`ds_frequent_union`](../aggregate-functions/ds_frequent_union.md) でマージして得られます。

## 構文

```Haskell
VARCHAR ds_frequent_items(sketch, n)
```

### パラメータ

- `sketch`: `ds_frequent` で構築したスケッチを含む VARBINARY 値。
- `n`: 返す値の最大数。0 以上の INT。

## 戻り値

次の形式の JSON オブジェクトを含む VARCHAR 値を返します。

```json
{"total":N,"max_error":E,"items":[["value",estimate,lower_bound,upper_bound],...]}
```

- `total`: スケッチの元になった値の数。
- `max_error`: 各値の下限と上限の差。すべてのカウントが正確な場合は `0` です。
- `items`: 最大 `n` 個の値。最も頻出する値が先頭です。値の真の出現回数は `lower_bound` と `upper_bound` の間にあります。Apache DataSketches と同様に、`estimate` は `upper_bound` と等しくなります。カウントが同じ値は昇順に並びます。

値は文字列として出力され、その形式は `CAST(value AS VARCHAR)` の結果と同じです。

スケッチが追跡していない値の出現回数は、最大でも `max_error` です。

`sketch` または `n` が `NULL` の場合は `NULL` を返します。

## 例

```sql
-- page_views テーブルには 1000 行があります。国が US の行が 600 行、DE が 300 行、JP が 100 行です。ds_frequent を参照してください。
SELECT ds_frequent_items(ds_frequent(country), 2) FROM page_views;
```

```plaintext
+------------------------------------------------------------------------------+
| ds_frequent_items(ds_frequent(country), 2)                                   |
+------------------------------------------------------------------------------+
| {"total":1000,"max_error":0,"items":[["US",600,600,600],["DE",300,300,300]]} |
+------------------------------------------------------------------------------+
```

この列は異なる値が少ないため、すべてのカウントは正確です。

## キーワード

DS_FREQUENT_ITEMS, DS_FREQUENT, DS_FREQUENT_UNION

---
displayed_sidebar: docs
description: "複数行のシリアライズされた Apache DataSketches 頻出アイテムスケッチを単一のスケッチにマージし、VARBINARY として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent_union

集計関数。行をまたいでシリアライズされた頻出アイテムスケッチ（`VARBINARY`）をマージし、単一のシリアライズされたスケッチを返します。

入力は [`ds_frequent`](./ds_frequent.md) で構築したスケッチの列です。たとえば、日ごとや地域ごとに保存したスケッチです。マージ後のスケッチは、入力スケッチの元になったすべての値を表し、そのカウントの誤差は、それらの値から直接構築したスケッチと同じ範囲に収まります。

## 構文

```Haskell
VARBINARY ds_frequent_union(sketch)
```

### パラメータ

- `sketch`: `ds_frequent` で構築したスケッチの VARBINARY 列。同じグループ内のすべてのスケッチは、同じデータ型の値を保持している必要があります。そうでない場合はエラーが返されます。

## 戻り値

マージされたスケッチを含む VARBINARY 値を返します。`NULL` 値は無視されます。グループにマージ対象のスケッチがない場合は `NULL` を返します。

## 例

```sql
-- country_sketches には、ブラウザーごとに国のスケッチが 1 つ保存されています。ds_frequent を参照してください。
SELECT ds_frequent_items(ds_frequent_union(sketch), 2) FROM country_sketches;
```

## キーワード

DS_FREQUENT_UNION, DS_FREQUENT, DS_FREQUENT_ITEMS

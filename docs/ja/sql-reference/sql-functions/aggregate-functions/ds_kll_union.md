---
displayed_sidebar: docs
description: "複数行のシリアライズされた Apache DataSketches KLL 分位数スケッチを単一のスケッチにマージし、VARBINARY として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_union

集計関数。行をまたいでシリアライズされた KLL 分位数スケッチ（`VARBINARY`）をマージし、単一のシリアライズされたスケッチを返します。

入力は [`ds_kll`](./ds_kll.md) で構築したスケッチの列です。たとえば、日ごとや地域ごとに保存したスケッチです。マージ後のスケッチは、入力スケッチの元になったすべての値を表し、それらの値から直接構築したスケッチと同じ順位誤差を持ちます。

## 構文

```Haskell
VARBINARY ds_kll_union(sketch)
```

### パラメータ

- `sketch`: `ds_kll` で構築したスケッチの VARBINARY 列。同じグループ内のすべてのスケッチは、同じデータ型の値を保持している必要があります。そうでない場合はエラーが返されます。

## 戻り値

マージされたスケッチを含む VARBINARY 値を返します。`NULL` 値は無視されます。グループにマージ対象のスケッチがない場合は `NULL` を返します。

## 例

```sql
-- amount_sketches には、地域ごとに注文金額のスケッチが 1 つ保存されています。ds_kll を参照してください。
SELECT ds_kll_histogram(ds_kll_union(sketch), 4) FROM amount_sketches;
```

## キーワード

DS_KLL_UNION, DS_KLL, DS_KLL_HISTOGRAM

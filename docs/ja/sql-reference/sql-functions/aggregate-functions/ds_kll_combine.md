---
displayed_sidebar: docs
description: "複数行のシリアライズされた Apache DataSketches KLL 分位数スケッチを単一のスケッチにマージし、VARBINARY として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_combine

集計関数。行をまたいでシリアライズされた KLL 分位数スケッチ（`VARBINARY`）をマージし、単一のシリアライズされたスケッチを返します。

入力は [`ds_kll_accumulate`](./ds_kll_accumulate.md) で構築したスケッチの列です。たとえば、日ごとや地域ごとに保存したスケッチです。マージ後のスケッチは、入力スケッチの元になったすべての値を表し、それらの値から直接構築したスケッチと同じ順位誤差を持ちます。異なる `k` の値で構築したスケッチもマージできます。マージ後のスケッチの順位誤差は、その中で最も小さい `k` によって決まります。

## 構文

```Haskell
VARBINARY ds_kll_combine(sketch)
```

### パラメータ

- `sketch`: `ds_kll_accumulate` で構築したスケッチの VARBINARY 列。同じグループ内のすべてのスケッチは、同じデータ型の値を保持している必要があります。そうでない場合はエラーが返されます。

## 戻り値

マージされたスケッチを含む VARBINARY 値を返します。`NULL` 値は無視されます。グループにマージ対象のスケッチがない場合は `NULL` を返します。

## 例

```sql
-- amount_sketches には、地域ごとに注文金額のスケッチが 1 つ保存されています。ds_kll_accumulate を参照してください。
SELECT ds_kll_estimate(ds_kll_combine(sketch), 4) FROM amount_sketches;
```

## キーワード

DS_KLL_COMBINE, DS_KLL_ACCUMULATE, DS_KLL_ESTIMATE, DS_KLL_HISTOGRAM

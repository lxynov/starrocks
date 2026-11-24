---
displayed_sidebar: docs
description: "シリアライズされた Apache DataSketches KLL 分位数スケッチから、等高バケットの境界と順位を JSON 文字列として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_estimate

スカラー関数。シリアライズされた KLL 分位数スケッチ（`VARBINARY`）を読み取り、スケッチの元になった値に対する等高バケットの境界と順位を JSON 文字列として返します。

スケッチは [`ds_kll_accumulate`](../aggregate-functions/ds_kll_accumulate.md) で構築するか、[`ds_kll_combine`](../aggregate-functions/ds_kll_combine.md) でマージして得られます。列から直接同じ結果を得るには、[`ds_kll_histogram`](../aggregate-functions/ds_kll_histogram.md) を使用してください。

## 構文

```Haskell
VARCHAR ds_kll_estimate(sketch, n)
```

### パラメータ

- `sketch`: `ds_kll_accumulate` で構築したスケッチを含む VARBINARY 値。
- `n`: バケット数。1 から 65536 までの INT。

## 戻り値

次の形式の JSON オブジェクトを含む VARCHAR 値を返します。

```json
{"count":N,"min":"v","max":"v","buckets":[["lower","upper",rank],...]}
```

- `count`: スケッチの元になった値の数。
- `min` と `max`: それらの値の最小値と最大値。どちらも正確な値です。
- `buckets`: 昇順に並んだバケット。最初のバケットは最小値から始まり、最後のバケットは最大値で終わります。それ以外のバケットは、直前のバケットの上限から始まり、その上限自体は含みません。`rank` は、そのバケットの上限以下の値が全体に占める割合です。

値は文字列として出力され、その形式は `CAST(value AS VARCHAR)` の結果と同じです。

各バケットの上限は、順位 1/n、2/n、… にある値です。ある値が非常に頻繁に出現し、これらの順位のうち複数がその値に当たる場合、その値は 1 つのバケットの上限にしかならないため、返されるバケット数が `n` より少なくなることがあります。

`sketch` または `n` が `NULL` の場合は `NULL` を返します。

## 例

```sql
-- orders テーブルには 1000 行があり、amount は 0.25 から 250.00 までの値を取ります。ds_kll_accumulate を参照してください。
SELECT ds_kll_estimate(ds_kll_accumulate(amount), 4) FROM orders;
```

```plaintext
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| ds_kll_estimate(ds_kll_accumulate(amount), 4)                                                                                                       |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| {"count":1000,"min":"0.25","max":"250.00","buckets":[["0.25","62.50",0.251],["62.50","125.00",0.5],["125.00","187.25",0.75],["187.25","250.00",1]]} |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
```

テーブルにはスケッチが保持できる数より多くの値があるため、スケッチは値をサンプリングします。そのため、上限と順位は実行ごとにわずかに異なることがあります。

## キーワード

DS_KLL_ESTIMATE, DS_KLL_ACCUMULATE, DS_KLL_COMBINE, DS_KLL_HISTOGRAM

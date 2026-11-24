---
displayed_sidebar: docs
description: "Apache DataSketches KLL 分位数スケッチを使用して、列の値に対する等高バケットの境界と順位を JSON 文字列として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_histogram

集計関数。式の値に対する等高バケットの境界と順位を JSON 文字列として返します。値から KLL 分位数スケッチを構築し、そのスケッチからバケットを読み取ります。

`ds_kll_histogram(expr, n)` は `ds_kll_estimate(ds_kll_accumulate(expr), n)` の省略形です。スケッチの構築方法は [`ds_kll_accumulate`](./ds_kll_accumulate.md)、バケットの読み取り方法は [`ds_kll_estimate`](../scalar-functions/ds_kll_estimate.md) を参照してください。スケッチを保存して後でマージする場合は、これらの関数と [`ds_kll_combine`](./ds_kll_combine.md) を使用してください。

## 構文

```Haskell
VARCHAR ds_kll_histogram(expr, n)
VARCHAR ds_kll_histogram(expr, n, k)
```

### パラメータ

- `expr`: 要約する式。サポートされる型は BOOLEAN、TINYINT、SMALLINT、INT、BIGINT、LARGEINT、FLOAT、DOUBLE、DECIMAL、DATE、DATETIME です。
- `n`: バケット数。1 から 65536 までの INT。
- `k`: 8 から 65535 までの INT 型のリテラル。デフォルトは 400 です。`ds_kll_accumulate` と同様に、スケッチの精度とサイズを制御します。

## 戻り値

`ds_kll_estimate` で説明している形式の JSON オブジェクトを含む VARCHAR 値を返します。

```json
{"count":N,"min":"v","max":"v","buckets":[["lower","upper",rank],...]}
```

`NULL` 値は無視されます。FLOAT 列または DOUBLE 列の非数（NaN）は順位を持たないため、同様に無視されます。グループに要約対象の値がない場合は `NULL` を返します。

## 例

```sql
-- orders テーブルには 1000 行があり、amount は 0.25 から 250.00 までの値を取ります。ds_kll_accumulate を参照してください。
SELECT ds_kll_histogram(amount, 4) FROM orders;
```

```plaintext
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| ds_kll_histogram(amount, 4)                                                                                                                         |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
| {"count":1000,"min":"0.25","max":"250.00","buckets":[["0.25","62.50",0.251],["62.50","125.00",0.5],["125.00","187.25",0.75],["187.25","250.00",1]]} |
+-----------------------------------------------------------------------------------------------------------------------------------------------------+
```

テーブルにはスケッチが保持できる数より多くの値があるため、スケッチは値をサンプリングします。そのため、上限と順位は実行ごとにわずかに異なることがあります。

```sql
-- 地域ごとに 1 つのヒストグラムを作成し、順位誤差を小さくするために、より大きな k を使う。
SELECT region, ds_kll_histogram(amount, 4, 1000) FROM orders GROUP BY region;
```

## キーワード

DS_KLL_HISTOGRAM, DS_KLL_ACCUMULATE, DS_KLL_COMBINE, DS_KLL_ESTIMATE

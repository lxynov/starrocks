---
displayed_sidebar: docs
description: "列の値から Apache DataSketches KLL 分位数スケッチを構築し、シリアライズされたスケッチを VARBINARY として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_kll_accumulate

集計関数。式の値から KLL 分位数スケッチを構築し、シリアライズされたスケッチを `VARBINARY` として返します。

KLL スケッチは、データを 1 回走査するだけで、少ないメモリで列の分布を要約します。入力の小さなサンプルから「ある順位にある値は何か」に答えます。誤差は値ではなく順位に現れます。ある順位に対して返される値は入力に実在する値であり、その真の順位と要求した順位との差は順位誤差以内に収まります。スケッチはデフォルトで `k = 400` を使用し、99% の信頼度で順位誤差は約 0.68% です。`k` を大きくすると、順位誤差は小さくなり、スケッチは大きくなります。

スケッチはこの保証を保ったままマージできるため、`ds_kll_accumulate` は通常の多段階集計として実行されます。各インスタンスが自分の行のスケッチを構築し、部分スケッチがマージされます。保存したスケッチは、後から [`ds_kll_combine`](./ds_kll_combine.md) で他のスケッチとマージでき、[`ds_kll_estimate`](../scalar-functions/ds_kll_estimate.md) で読み取れます。スケッチを保持する必要がない場合は、[`ds_kll_histogram`](./ds_kll_histogram.md) で列のヒストグラムを直接取得できます。

この関数は Apache DataSketches に基づいています。詳細は [KLL Sketch](https://datasketches.apache.org/docs/KLL/KLLSketch.html) を参照してください。

## 構文

```Haskell
VARBINARY ds_kll_accumulate(expr)
VARBINARY ds_kll_accumulate(expr, k)
```

### パラメータ

- `expr`: スケッチを構築する式。サポートされる型は BOOLEAN、TINYINT、SMALLINT、INT、BIGINT、LARGEINT、FLOAT、DOUBLE、DECIMAL、DATE、DATETIME です。
- `k`: 8 から 65535 までの INT 型のリテラル。デフォルトは 400 です。スケッチの精度とサイズを制御します。

## 戻り値

シリアライズされたスケッチを含む VARBINARY 値を返します。スケッチには値のデータ型が記録されます。

`NULL` 値は無視されます。FLOAT 列または DOUBLE 列の非数（NaN）は順位を持たないため、同様に無視されます。グループにスケッチ対象の値がない場合は `NULL` を返します。

## 例

```sql
CREATE TABLE orders (
  id BIGINT,
  region VARCHAR(16),
  amount DECIMAL(10, 2),
  order_date DATE
)
DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 3;

INSERT INTO orders
SELECT generate_series, concat('r', generate_series % 3), generate_series / 4,
       date_add('2024-01-01', INTERVAL generate_series % 200 DAY)
FROM TABLE(generate_series(1, 1000));

-- 金額に対する 4 つの等高バケット。
SELECT ds_kll_estimate(ds_kll_accumulate(amount), 4) FROM orders;

-- 順位誤差を小さくするために、より大きな k を使う。
SELECT ds_kll_estimate(ds_kll_accumulate(amount, 1000), 4) FROM orders;

-- 地域ごとに 1 つのスケッチを作成し、後で使えるように保存する。
CREATE TABLE amount_sketches AS
SELECT region, ds_kll_accumulate(amount) AS sketch FROM orders GROUP BY region;
```

## キーワード

DS_KLL_ACCUMULATE, DS_KLL_COMBINE, DS_KLL_ESTIMATE, DS_KLL_HISTOGRAM

---
displayed_sidebar: docs
description: "列の値から Apache DataSketches 頻出アイテムスケッチを構築し、シリアライズされたスケッチを VARBINARY として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ds_frequent

集計関数。式の値から頻出アイテム（Frequent Items）スケッチを構築し、シリアライズされたスケッチを `VARBINARY` として返します。

頻出アイテムスケッチは、データを 1 回走査するだけで、限られたメモリで列の中で最も頻繁に出現する値を見つけます。2048 エントリのハッシュマップで各値の出現回数を数え、このハッシュマップは最大 1536 個の値を保持します。ハッシュマップがいっぱいになると、スケッチはすべてのカウンターから同じ数を引き、ゼロになったカウンターを破棄します。そのため、スケッチがまだ追跡している値について、真の出現回数は下限と上限の間にあり、その差はすべての値で同じです。

- 列の異なる値が 1536 個以下であれば、すべてのカウントは正確です。
- それ以外の場合、下限と上限の差は、スケッチの構築に使われた行数の 0.17% 以下です。

スケッチはこの保証を保ったままマージできるため、`ds_frequent` は通常の多段階集計として実行されます。各インスタンスが自分の行のスケッチを構築し、部分スケッチがマージされます。保存したスケッチは、後から [`ds_frequent_union`](./ds_frequent_union.md) で他のスケッチとマージでき、[`ds_frequent_items`](../scalar-functions/ds_frequent_items.md) で読み取れます。

この関数は Apache DataSketches に基づいています。詳細は [Frequent Items Sketches](https://datasketches.apache.org/docs/Frequency/FrequentItemsOverview.html) を参照してください。

## 構文

```Haskell
VARBINARY ds_frequent(expr)
```

### パラメータ

- `expr`: スケッチを構築する式。サポートされる型は BOOLEAN、TINYINT、SMALLINT、INT、BIGINT、LARGEINT、FLOAT、DOUBLE、DECIMAL、DATE、DATETIME、CHAR、VARCHAR です。

## 戻り値

シリアライズされたスケッチを含む VARBINARY 値を返します。スケッチには値のデータ型が記録されます。

`NULL` 値は無視されます。グループにスケッチ対象の値がない場合は `NULL` を返します。

## 例

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

-- 最も頻繁に出現する 2 つの国。
SELECT ds_frequent_items(ds_frequent(country), 2) FROM page_views;

-- ブラウザーごとに 1 つのスケッチを作成し、後で使えるように保存する。
CREATE TABLE country_sketches AS
SELECT browser, ds_frequent(country) AS sketch FROM page_views GROUP BY browser;
```

## キーワード

DS_FREQUENT, DS_FREQUENT_UNION, DS_FREQUENT_ITEMS

# Anomaly Detection

> Find unusual rows in a table, explain them, and stop a pipeline when they appear.

**Use this guide to:**
- Score one column with an Isolation Forest and pick the anomalies
- Join the results back to your table by key
- Find rows that are unusual only in a combination of columns
- Find noise points with DBSCAN
- Explain an outlier with OutlierTree
- Turn a detector into a pass/fail gate

Every example builds on the table created in the first step. Run them in one DuckDB session.
The full parameter lists are in the [anomaly detection reference](../api/08-anomaly-detection.md).

## 1. Create example data

The `orders` table has 300 orders from two segments. Retail orders are small (up to 79, with
1 to 5 items). Wholesale orders are large (500 to 900, with 20 to 59 items). Three orders are
planted as anomalies:

- order 1077: a retail order of 5000, far above every other order;
- order 1041: a retail order of 850, a normal amount for wholesale but not for retail;
- order 1200: a retail order with 55 items, a normal count for wholesale but not for retail.

```sql
CREATE TABLE orders AS
SELECT 1000 + i AS order_id,
       CASE WHEN i % 3 = 0 THEN 'wholesale' ELSE 'retail' END AS segment,
       CASE WHEN i = 77 THEN 5000.0
            WHEN i = 41 THEN 850.0
            WHEN i % 3 = 0 THEN 500 + (i * 37 % 400)
            ELSE 20 + (i * 13 % 60) END::DOUBLE AS amount,
       CASE WHEN i = 200 THEN 55
            WHEN i % 3 = 0 THEN 20 + (i * 7 % 40)
            ELSE 1 + (i % 5) END AS items
FROM range(1, 301) t(i);
```

The anomaly functions read the table through a second connection. Use a regular table or a
view, not a TEMP table.

## 2. Score one column

`isolation_forest` scores every value of one column. Higher scores are more unusual.
`contamination` is the share of rows you expect to be anomalous; `is_anomaly` flags that share
of the highest scores. A fixed `seed` makes the scores reproducible.

```sql
SELECT order_id, value AS amount, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest('orders', 'amount', 100, 256, 0.01, 'scores', 7,
                      id_column := 'order_id')
ORDER BY anomaly_score DESC, order_id
LIMIT 4;
```

```text
┌──────────┬────────┬────────┬────────────┐
│ order_id │ amount │ score  │ is_anomaly │
│  int64   │ double │ double │  boolean   │
├──────────┼────────┼────────┼────────────┤
│     1077 │ 5000.0 │  0.871 │ true       │
│     1054 │  898.0 │  0.635 │ true       │
│     1249 │  513.0 │  0.618 │ true       │
│     1108 │  896.0 │  0.608 │ false      │
└──────────┴────────┴────────┴────────────┘
```

Order 1077 stands out with a score of 0.871. With 300 rows and `contamination` 0.01, three rows
are flagged, so orders 1054 and 1249, at the edges of the wholesale range, are flagged too.
Orders 1041 and 1200 are not: an amount of 850 is normal for the table as a whole. Steps 4 and
6 find them.

## 3. Join the anomalies back to the table

`row_id` is a position among the analyzed rows. Join on a key instead: `id_column` copies the
key into the result.

```sql
SELECT o.order_id, o.segment, o.amount, o.items
FROM isolation_forest('orders', 'amount', 100, 256, 0.01, 'scores', 7,
                      id_column := 'order_id') s
JOIN orders o USING (order_id)
WHERE s.is_anomaly
ORDER BY o.order_id;
```

```text
┌──────────┬───────────┬────────┬───────┐
│ order_id │  segment  │ amount │ items │
│  int64   │  varchar  │ double │ int64 │
├──────────┼───────────┼────────┼───────┤
│     1054 │ wholesale │  898.0 │    38 │
│     1077 │ retail    │ 5000.0 │     3 │
│     1249 │ wholesale │  513.0 │    43 │
└──────────┴───────────┴────────┴───────┘
```

## 4. Score several columns together

Order 1200 has a normal amount for retail and a normal item count for wholesale. Only the
combination is unusual. `isolation_forest_mv` scores several columns at once:

```sql
SELECT order_id, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest_mv('orders', 'amount, items', 100, 256, 0.01, 'scores', 7,
                         id_column := 'order_id')
ORDER BY anomaly_score DESC, order_id
LIMIT 4;
```

```text
┌──────────┬────────┬────────────┐
│ order_id │ score  │ is_anomaly │
│  int64   │ double │  boolean   │
├──────────┼────────┼────────────┤
│     1077 │  0.833 │ true       │
│     1200 │  0.717 │ true       │
│     1041 │  0.662 │ true       │
│     1108 │  0.625 │ false      │
└──────────┴────────┴────────────┘
```

All three planted orders are flagged. `isolation_forest_mv` also accepts `VARCHAR` columns,
such as `segment`, as categorical features.

## 5. Find noise points with DBSCAN

DBSCAN groups values that lie within `eps` of each other. Values without enough neighbors are
noise. Here an order counts as noise when fewer than 5 orders lie within 60 of its amount:

```sql
SELECT order_id, value AS amount, point_type
FROM dbscan('orders', 'amount', 60, 5, 'clusters', id_column := 'order_id')
WHERE is_anomaly;
```

```text
┌──────────┬────────┬────────────┐
│ order_id │ amount │ point_type │
│  int64   │ double │  varchar   │
├──────────┼────────┼────────────┤
│     1077 │ 5000.0 │ NOISE      │
└──────────┴────────┴────────────┘
```

DBSCAN measures distances on the raw values. When you combine columns with `dbscan_mv`, bring
them to comparable scales first.

## 6. Explain an outlier

OutlierTree compares each value with a group of similar rows, for example the retail orders,
and writes down why a value is unusual:

```sql
SELECT order_id, column_name, outlier_value, explanation
FROM outlier_tree('orders', 'segment, amount', 'outliers', id_column := 'order_id')
ORDER BY order_id;
```

```text
┌──────────┬─────────────┬───────────────┬────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│ order_id │ column_name │ outlier_value │                                                      explanation                                                       │
│  int64   │   varchar   │    varchar    │                                                        varchar                                                         │
├──────────┼─────────────┼───────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│     1041 │ amount      │ 850           │ Value 850 for column 'amount' is unusually high (mean: 78.58, SD: 354.71, z-score: 35.93) when segment IN ('retail')   │
│     1054 │ amount      │ 898           │ Value 898 for column 'amount' is unusually high (mean: 287.55, SD: 419.22, z-score: 17.54)                             │
│     1077 │ amount      │ 5000          │ Value 5000 for column 'amount' is unusually high (mean: 78.58, SD: 354.71, z-score: 222.54) when segment IN ('retail') │
│     1108 │ amount      │ 896           │ Value 896 for column 'amount' is unusually high (mean: 287.55, SD: 419.22, z-score: 17.49)                             │
└──────────┴─────────────┴───────────────┴────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Order 1041 is found now: 850 is normal overall, but not when `segment IN ('retail')`. The rows
without a `when ...` condition (1054, 1108) are compared with the whole table, where the two
segments mix; read the explanation before you act on such a row.

## 7. Use a detector as a gate

Every function has a `summary` mode that returns one row with `status = 'pass'` or `'fail'`.
DBSCAN fails when it finds a noise point:

```sql
SELECT status, noise_count, message
FROM dbscan('orders', 'amount', 60, 5, 'summary');
```

```text
┌─────────┬─────────────┬───────────────────────────┐
│ status  │ noise_count │          message          │
│ varchar │    int64    │          varchar          │
├─────────┼─────────────┼───────────────────────────┤
│ fail    │           1 │ 1 noise point(s) detected │
└─────────┴─────────────┴───────────────────────────┘
```

To stop a SQL script on a failure, raise an error with DuckDB's `error()` function. The DuckDB
CLI with `-bail`, or any client, then stops at this statement:

```sql
SELECT CASE WHEN status = 'fail' THEN error('anomaly gate: ' || message) ELSE 'ok' END AS gate
FROM dbscan('orders', 'amount', 60, 5, 'summary');
```

```text
Invalid Input Error: anomaly gate: 1 noise point(s) detected
```

The Isolation Forest `summary` mode is a poor gate on its own: it flags about the
`contamination` share of rows on every run, so it fails on almost any data. Use DBSCAN or
OutlierTree for pass/fail decisions, and the Isolation Forest for ranking rows by score.

## Next steps

- [Anomaly detection reference](../api/08-anomaly-detection.md): every parameter and output column
- [Data quality checks](02-data-quality-checks.md): rule-based checks such as `zscore`, `iqr` and `metric_anomaly_iqr`
- [Conventions](../api/00-conventions.md): positional and named parameters

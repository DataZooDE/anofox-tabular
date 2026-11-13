---
name: anofox-tabular-anomaly
description: >
  Unsupervised anomaly detection in the anofox_tabular DuckDB extension: Isolation Forest
  (`isolation_forest`, `isolation_forest_mv`), DBSCAN noise points (`dbscan`, `dbscan_mv`) and
  explained group outliers (`outlier_tree`) — signatures, output modes, `id_column`, row_id
  numbering, contamination semantics and pass/fail gating. Use when a user wants to find,
  score, explain or join back unusual rows of a table, or stop a pipeline when outlier rows
  appear. Row-level detectors only: for a daily count or metric against its own history use
  anofox-tabular-data-quality (`rel_count_change`, `metric_anomaly_iqr`).
version: 2026.11.02
user-invocable: false
---

# Anofox Tabular — Anomaly Detection Cheat Sheet

**Extension:** `anofox_tabular` v2026.11.02 | **DuckDB:** v2.0.0 / v1.5.6 | **Naming:** short alias (`isolation_forest`) = canonical `anofox_tab_isolation_forest`

Five table functions that flag unusual rows without labels. Each has a one-row `'summary'` mode
(`status` = `'pass'`/`'fail'`) and a row-level mode.

Load with `LOAD anofox_tabular;` (install: `INSTALL anofox_tabular FROM community;`).

```sql no-run
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;
```

## Critical gotchas

1. **Table and columns are string literals.** `isolation_forest('readings', 'temperature')`,
   not `isolation_forest(readings, temperature)`.
2. **Multi-column args are ONE comma-separated VARCHAR**, not a LIST:
   `isolation_forest_mv('t', 'a, b')`, `dbscan_mv('t', 'a, b')`, `outlier_tree('t', 'a, b', 'summary')`.
   `['a','b']` is a "No function matches" binder error. Names match case-insensitively; a
   column listed twice (any spelling) is a binder error.
3. **Parameters are positional.** The only named parameter is `id_column := '<col>'`;
   `n_trees := 50` is `Invalid named parameter "n_trees"`. Use `NULL` to skip a positional
   optional and keep its default (`isolation_forest('t','x', NULL, NULL, 0.01, 'scores', 42)`).
4. **output_mode position differs:** 6th for `isolation_forest(_mv)` (after contamination),
   5th for `dbscan(_mv)` (after min_pts), **3rd and required** for `outlier_tree`.
   Row-level mode names differ: `'scores'` (IF), `'clusters'` (DBSCAN), `'outliers'` (OutlierTree).
   A wrong mode is a binder error (`output_mode must be 'summary' or 'scores'`).
5. **`outlier_tree` has exactly two forms:** 3 args, or all 9 args. 4–8 args → binder error.
6. **Second connection:** all five read the table through a separate connection.
   - TEMP tables are rejected: `TEMP tables are not supported by isolation_forest; use a regular table (CREATE TABLE ... AS SELECT * FROM <name>)`.
     Regular tables and views work.
   - Only committed data is visible. Inside `BEGIN` with uncommitted writes they refuse:
     `... reads the last committed data, but this transaction has uncommitted changes`.
     On DuckDB 2.0 that error aborts the transaction — roll back, reload, call after `COMMIT`.
7. **Contamination always flags ~that share.** Isolation Forest flags the top `contamination`
   share of scores (default `0.1` → ~20 of 200 rows) even on clean data. Its `'summary'` mode
   therefore almost always says `'fail'` — do not use it as a gate. Set `contamination` to the
   expected outlier rate, rank by `anomaly_score`, and gate with DBSCAN / OutlierTree instead.
8. **Without `seed` the forest is random** — scores change per run. Pass a seed (7th arg).
   Seeds reproduce per platform only (Linux/macOS/Windows differ on borderline rows).
9. **`id_column` works only in the row-level mode** (`'scores'`/`'clusters'`/`'outliers'`);
   in `'summary'` it is `id_column requires output_mode '...'`. It must not collide with an
   output column name (`value`, `anomaly_score`, `row_id`, ...) — alias it in a view first.
10. **`row_id` is the 1-based scan position in the table/view**, counting skipped rows (NULL /
    NaN), so output row_ids can have gaps. Prefer `id_column` for joins, especially with
    `SET preserve_insertion_order = false`.
11. **Type rules:** `isolation_forest`, `dbscan`, `dbscan_mv` need numeric columns (BOOLEAN = 0/1).
    `isolation_forest_mv` also takes VARCHAR (categorical). `outlier_tree` takes numeric,
    BOOLEAN, VARCHAR, ENUM. DATE/TIMESTAMP → binder error; pass `epoch(ts)` through a view.
12. **DBSCAN distances are Euclidean on raw values** — scale columns before `dbscan_mv`.
    `dbscan_mv` is O(n²): sample tables above ~50k rows. `dbscan` (one column) is O(n log n) only
    while every value is finite: a single NaN/±Inf falls back to the O(n²) all-pairs search (40k
    rows: 0.05 s → 12 s). Filter them in a view (`WHERE isfinite(x)`); they are NOISE anyway.

## Sample data (used below)

```sql
CREATE TABLE readings AS
SELECT i AS reading_id,
       'S' || lpad((i % 4 + 1)::VARCHAR, 2, '0') AS sensor,
       CASE i WHEN 50 THEN 95.0 WHEN 120 THEN -30.0 ELSE 20 + (i % 10) * 0.5 END AS temperature,
       CASE i WHEN 150 THEN 5.0 ELSE 40 + (i % 7) * 1.0 END AS humidity
FROM range(1, 201) t(i);

CREATE TABLE staff AS
SELECT i AS staff_id,
       CASE WHEN i <= 100 THEN 'junior' ELSE 'senior' END AS level,
       CASE WHEN i = 7 THEN 92000
            WHEN i <= 100 THEN 48000 + (i % 10) * 500
            ELSE 88000 + (i % 10) * 600 END AS salary
FROM range(1, 201) t(i);
```

Readings 50 and 120 have extreme temperatures; reading 150 has an unusual humidity only.
Staff member 7 is a junior with a senior salary.

## Which function

| Need | Use |
|---|---|
| Rank rows of one numeric column by unusualness | `isolation_forest` |
| Unusual *combination* of columns, incl. categorical | `isolation_forest_mv` |
| Isolated values (noise) with a distance radius; good pass/fail gate | `dbscan` / `dbscan_mv` |
| Outlier *within a group* (e.g. per segment) + a human-readable reason | `outlier_tree` |
| Unusual *for a key you name* (per customer, per store) | No detector partitions by a given key: `outlier_tree` picks its own groups (≥ 25 rows each) and may score against the whole table. Use a robust z in SQL: `median`/`mad` per key with `GROUP BY`, `abs(x - median) / (1.4826 * mad) > 3.5` |
| Fixed-rule single column (z-score / IQR) | `zscore` / `iqr` (data-quality skill) |
| Daily volume / metric vs. its own history | `rel_count_change` / `metric_anomaly_iqr` (data-quality skill) |

## `isolation_forest`

```text
isolation_forest(table_name VARCHAR, column_name VARCHAR
                 [, n_trees BIGINT [, sample_size BIGINT [, contamination DOUBLE
                 [, output_mode VARCHAR [, seed BIGINT]]]]]
                 [, id_column := VARCHAR]) -> TABLE
```

| # | Param | Type | Default | Meaning |
|---|---|---|---|---|
| 3 | `n_trees` | BIGINT | 100 | 1..500 |
| 4 | `sample_size` | BIGINT | 256 | rows per tree, 1..10000 |
| 5 | `contamination` | DOUBLE | 0.1 | share flagged, 0.0..0.5 (0.0 flags only ±Inf) |
| 6 | `output_mode` | VARCHAR | `'summary'` | `'summary'` / `'scores'` (case-insensitive) |
| 7 | `seed` | BIGINT | NULL = random | reproducible scores |
| named | `id_column` | VARCHAR | — | `'scores'` mode only |

`NULL` in positions 3–7 keeps the default.

- **summary:** `status VARCHAR, outlier_count BIGINT, total_count BIGINT, avg_value DOUBLE,
  contamination DOUBLE, n_trees INTEGER, message VARCHAR` (`'20 anomalies detected'`).
- **scores:** `row_id BIGINT, [<id_column>], value DOUBLE, anomaly_score DOUBLE, is_anomaly BOOLEAN`.

Scoring rules: score near 1 = anomalous, ~0.5 or lower = typical. NULL and NaN rows are skipped
(not in `total_count`); ±Inf rows get score 1.0 and `is_anomaly = true`. Equal values get equal
scores; a tie group at the very top is flagged as a whole (can exceed contamination), other ties
at the cut are not flagged; a constant column flags nothing (`'pass'`). The extended options
(ndim, SCiForest, weights) exist only on `isolation_forest_mv`.

```sql
SELECT reading_id, value, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest('readings', 'temperature', 100, 256, 0.01, 'scores', 42,
                      id_column := 'reading_id')
ORDER BY anomaly_score DESC, reading_id
LIMIT 3;
```

```text
┌────────────┬────────┬────────┬────────────┐
│ reading_id │ value  │ score  │ is_anomaly │
├────────────┼────────┼────────┼────────────┤
│         50 │   95.0 │  0.903 │ true       │
│        120 │  -30.0 │  0.892 │ true       │
│         10 │   20.0 │  0.497 │ false      │
└────────────┴────────┴────────┴────────────┘
```

Default contamination flags 10% regardless of the data:

```sql
SELECT status, outlier_count, total_count, message
FROM isolation_forest('readings', 'temperature', 100, 256, 0.1, 'summary', 42);
-- fail | 20 | 200 | 20 anomalies detected   (only 2 real outliers)
```

## `isolation_forest_mv`

```text
isolation_forest_mv(table_name VARCHAR, column_names VARCHAR
                    [, n_trees BIGINT [, sample_size BIGINT [, contamination DOUBLE
                    [, output_mode VARCHAR [, seed BIGINT [, ndim BIGINT
                    [, coef_type VARCHAR [, scoring_metric VARCHAR [, weight_column VARCHAR
                    [, ntry BIGINT [, prob_pick_avg_gain DOUBLE]]]]]]]]]]]
                    [, id_column := VARCHAR]) -> TABLE
```

Positions 3–7 as `isolation_forest`. `column_names` = `'a, b, c'` (numeric or VARCHAR categorical).

| # | Param | Type | Default | Meaning |
|---|---|---|---|---|
| 8 | `ndim` | BIGINT | 1 | columns per split; >1 = Extended IF (hyperplanes); ≤ number of columns |
| 9 | `coef_type` | VARCHAR | `'uniform'` | `'uniform'` / `'normal'` hyperplane coefficients |
| 10 | `scoring_metric` | VARCHAR | `'depth'` | `'depth'`, `'density'` (scores ≤ 0, higher still worse), `'adj_depth'` |
| 11 | `weight_column` | VARCHAR | NULL | numeric sample-weight column (NULL weight → row skipped) |
| 12 | `ntry` | BIGINT | 1 | split candidates per node; >1 = SCiForest |
| 13 | `prob_pick_avg_gain` | DOUBLE | 0.0 | 0..1, probability of choosing split by gain |

Every optional accepts `NULL` = default, so later options can be set without the earlier ones.
Rows with NULL in any selected column (or NaN in a numeric one) are skipped.

- **summary:** `status, outlier_count, total_count, n_columns INTEGER, contamination, n_trees, message`.
- **scores:** `row_id, [<id_column>], anomaly_score, is_anomaly` — **no `value` column**.

Reading 150 is only unusual in the temperature/humidity combination:

```sql
SELECT reading_id, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest_mv('readings', 'temperature, humidity', 100, 256, 0.02, 'scores', 42,
                         id_column := 'reading_id')
WHERE is_anomaly
ORDER BY reading_id;
-- 50, 120, 150 flagged
```

Extended IF, and SCiForest via NULL placeholders; a categorical column mixed in:

```sql
SELECT outlier_count FROM isolation_forest_mv('readings', 'temperature, humidity',
       100, 256, 0.02, 'summary', 42, 2, 'normal');
SELECT outlier_count FROM isolation_forest_mv('readings', 'temperature, humidity',
       100, 256, 0.02, 'summary', 42, NULL, NULL, NULL, NULL, 10, 0.5);
SELECT row_id, round(anomaly_score, 3) AS score
FROM isolation_forest_mv('readings', 'sensor, temperature', 100, 256, 0.01, 'scores', 42)
ORDER BY anomaly_score DESC, row_id LIMIT 3;
```

Duplicate column (case-insensitive) and too-large `ndim` are binder errors:

```sql
SELECT * FROM isolation_forest_mv('readings', 'temperature, humidity, Temperature');
```

```text
Binder Error: isolation_forest_mv: column 'temperature' is listed more than once
```

```sql
SELECT * FROM isolation_forest_mv('readings', 'temperature, humidity', 100, 256, 0.1, 'summary', 42, 3);
```

```text
Binder Error: isolation_forest_mv: ndim (3) must not exceed the number of columns (2)
```

## `dbscan` / `dbscan_mv`

```text
dbscan(table_name VARCHAR, column_name VARCHAR
       [, eps DOUBLE [, min_pts BIGINT [, output_mode VARCHAR]]]
       [, id_column := VARCHAR]) -> TABLE

dbscan_mv(table_name VARCHAR, column_names VARCHAR
          [, eps DOUBLE [, min_pts BIGINT [, output_mode VARCHAR]]]
          [, id_column := VARCHAR]) -> TABLE
```

| # | Param | Type | Default | Meaning |
|---|---|---|---|---|
| 3 | `eps` | DOUBLE | 0.5 | neighbourhood radius, finite > 0 (in raw units!) |
| 4 | `min_pts` | BIGINT | 5 | neighbours for a CORE point, ≥ 1 |
| 5 | `output_mode` | VARCHAR | `'summary'` | `'summary'` / `'clusters'` |
| named | `id_column` | VARCHAR | — | `'clusters'` mode only |

`NULL` keeps the default. Point types: `CORE` (≥ `min_pts` neighbours within `eps`), `BORDER`
(near a core point), `NOISE` (`cluster_id = -1`, `anomaly_score = 1.0`, `is_anomaly = true`).
Rows with NULL in a selected column are skipped; NaN and ±Inf are kept and come out as `NOISE`
(and make `dbscan` quadratic, see gotcha 12). Numeric columns only.

- **summary:** `status VARCHAR` (`'fail'` if any noise), `cluster_count BIGINT, noise_count BIGINT,
  total_count BIGINT, noise_rate DOUBLE, largest_cluster_size BIGINT, eps DOUBLE, min_pts BIGINT,`
  [`n_columns INTEGER` — `_mv` only,] `message VARCHAR` (`'2 noise point(s) detected'`).
- **clusters:** `row_id BIGINT, [<id_column>], value DOUBLE` (**not in `_mv`**)`, cluster_id INTEGER,
  point_type VARCHAR, neighbor_count BIGINT, anomaly_score DOUBLE, is_anomaly BOOLEAN`.

```sql
SELECT status, cluster_count, noise_count, noise_rate, message
FROM dbscan('readings', 'temperature', 1.0, 5);
-- fail | 1 | 2 | 0.01 | 2 noise point(s) detected

SELECT reading_id, value, point_type
FROM dbscan('readings', 'temperature', 1.0, 5, 'clusters', id_column := 'reading_id')
WHERE is_anomaly ORDER BY reading_id;
-- 50 / 95.0 / NOISE, 120 / -30.0 / NOISE

SELECT row_id, cluster_id, point_type
FROM dbscan_mv('readings', 'temperature, humidity', 2.0, 5, 'clusters')
WHERE is_anomaly ORDER BY row_id;
-- 50, 120, 150
```

Choosing `eps`: roughly the typical gap between normal neighbouring values; for `dbscan_mv`
standardise first, e.g. a view with `(x - avg(x) OVER ()) / stddev(x) OVER ()`.

## `outlier_tree`

```text
outlier_tree(table_name VARCHAR, columns VARCHAR, output_mode VARCHAR
             [, id_column := VARCHAR]) -> TABLE

outlier_tree(table_name VARCHAR, columns VARCHAR, output_mode VARCHAR,
             max_depth INTEGER, max_perc_outliers DOUBLE,
             min_size_numeric INTEGER, min_size_categ INTEGER,
             z_norm DOUBLE, z_outlier DOUBLE
             [, id_column := VARCHAR]) -> TABLE
```

| # | Param | Type | Default | Meaning |
|---|---|---|---|---|
| 3 | `output_mode` | VARCHAR | required (`NULL` = `'summary'`) | `'summary'` / `'outliers'` |
| 4 | `max_depth` | INTEGER | 4 | tree depth, 1..1024 |
| 5 | `max_perc_outliers` | DOUBLE | 0.01 | max share of a group flagged, (0, 1] |
| 6 | `min_size_numeric` | INTEGER | 25 | smallest group for a numeric target |
| 7 | `min_size_categ` | INTEGER | 75 | smallest group for a categorical target |
| 8 | `z_norm` | DOUBLE | 2.67 | half-width of the normal range (in spreads) |
| 9 | `z_outlier` | DOUBLE | 8.0 | \|z\| needed to flag |
| named | `id_column` | VARCHAR | — | `'outliers'` mode only |

In the 9-arg form `NULL` keeps a default. Lower `min_size_*` and `z_outlier` for small tables.
Rows with NULL in any selected column are excluded. Splits groups by tree conditions
(`level IN ('junior')`) and scores each numeric value in its group with a robust z:
`z = (value - median) / spread`, spread = MAD × 1.4826 (fallbacks: mean abs. deviation × 1.2533
when > half the group shares one value; then std dev). A value occurring more often than
`max_perc_outliers` of the group is never flagged. Categorical values can be outliers too.

- **summary:** `status VARCHAR, total_rows BIGINT, outlier_count BIGINT, columns_analyzed INTEGER,
  clusters_evaluated BIGINT, max_depth_reached INTEGER, message VARCHAR` (`'2 outlier(s) detected'`).
- **outliers** (one row per outlier **value** — a row can appear once per column):
  `row_id BIGINT, [<id_column>], column_name VARCHAR, outlier_value VARCHAR,
  outlier_value_num DOUBLE` (NULL for categorical)`, cluster_mean DOUBLE, cluster_sd DOUBLE,
  cluster_median DOUBLE, cluster_mad DOUBLE` (the spread used)`, cluster_size BIGINT,
  z_score DOUBLE` (NULL for ±Inf; categorical: 1/sqrt(share of the category))`, lower_bound DOUBLE, upper_bound DOUBLE,
  conditions VARCHAR` (JSON array; `[]` = whole table)`, explanation VARCHAR,
  outlier_score DOUBLE` (lower = more anomalous).

```sql
SELECT staff_id, column_name, outlier_value, round(z_score, 2) AS z, explanation
FROM outlier_tree('staff', 'level, salary', 'outliers', id_column := 'staff_id')
ORDER BY column_name;
```

```text
staff_id|column_name|outlier_value|z|explanation
7|level|junior|9.54|Value junior for column 'level' is unusually rare (cluster size: 91) when salary > 88000
7|salary|92000|18.66|Value 92000 for column 'salary' is unusually high (median: 50500.00, MAD: 2223.90, z-score: 18.66) when level IN ('junior')
```

Categorical outliers read "unusually rare"; numeric ones "unusually high/low" with median, MAD, z.

`outlier_value_num` compares directly with the bounds; the 9-arg form loosens thresholds:

```sql
SELECT outlier_value_num, round(lower_bound) AS lo, round(upper_bound) AS hi
FROM outlier_tree('staff', 'level, salary', 'outliers')
WHERE outlier_value_num IS NOT NULL;

SELECT status, outlier_count
FROM outlier_tree('staff', 'level, salary', 'summary', 4, 0.05, 10, 10, 2.67, 4.0);
-- fail | 12
```

Read the explanation before acting: an outlier without a `when ...` condition (`conditions = []`)
was compared against the whole table, where different groups may mix.

## Row numbering and `id_column`

- `row_id` = `row_number() OVER ()` of the scanned table/view; identical across all five
  functions; skipped rows (NULL, NaN, NULL weight) keep their number and leave gaps.
- `id_column := 'key'` inserts that column (own name and type) right after `row_id`. NULL ids
  are passed through. Works with canonical names and aliases.

```sql
CREATE VIEW readings_gaps AS
SELECT reading_id, CASE WHEN reading_id IN (10, 20, 30) THEN NULL ELSE temperature END AS temperature
FROM readings;

SELECT s.row_id, n.reading_id, n.temperature
FROM isolation_forest('readings_gaps', 'temperature', 100, 256, 0.01, 'scores', 42) s
JOIN (SELECT row_number() OVER () AS rn, * FROM readings_gaps) n ON n.rn = s.row_id
WHERE s.is_anomaly ORDER BY s.row_id;
-- row_id 50 / 120 still match reading 50 / 120 despite the skipped NULL rows
```

`id_column` errors:

```sql
SELECT * FROM dbscan('readings', 'temperature', 1.0, 5, 'summary', id_column := 'reading_id');
```

```text
Binder Error: dbscan: id_column requires output_mode 'clusters'
```

```sql
CREATE TABLE scored AS SELECT reading_id AS anomaly_score, temperature FROM readings;
SELECT * FROM isolation_forest('scored', 'temperature', 100, 256, 0.1, 'scores', 42,
                               id_column := 'anomaly_score');
```

```text
Binder Error: isolation_forest: id_column 'anomaly_score' collides with output column 'anomaly_score'; rename or alias it first
```

## Second-connection limits

```sql
CREATE TEMP TABLE readings_tmp AS SELECT * FROM readings;
SELECT * FROM isolation_forest('readings_tmp', 'temperature');
```

```text
Binder Error: TEMP tables are not supported by isolation_forest; use a regular table (CREATE TABLE ... AS SELECT * FROM readings_tmp)
```

```sql
CREATE TABLE tx_demo AS SELECT i AS id, (i % 10)::DOUBLE AS x FROM range(100) t(i);
BEGIN;
INSERT INTO tx_demo VALUES (1000, 500.0);
SELECT * FROM dbscan('tx_demo', 'x');
```

```text
Binder Error: dbscan reads the last committed data, but this transaction has uncommitted changes; call it after COMMIT, or outside a transaction
```

Fix: materialise TEMP data with `CREATE TABLE ... AS`, and run the detector after `COMMIT`
(a transaction that has only read data, or written only TEMP tables, is fine).

## Canonical workflow

Score, explain, store and gate a table in one script:

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

-- 1. rank by combined unusualness (contamination = expected rate, fixed seed)
CREATE TABLE order_scores AS
SELECT * FROM isolation_forest_mv('orders', 'amount, items', 100, 256, 0.01, 'scores', 7,
                                  id_column := 'order_id');

-- 2. explain outliers within segments
CREATE TABLE order_explanations AS
SELECT order_id, column_name, outlier_value, explanation
FROM outlier_tree('orders', 'segment, amount, items', 'outliers', id_column := 'order_id');

-- 3. review IF-flagged orders next to independent OutlierTree findings. OutlierTree explains its
--    own detections, not the IF scores: a flagged order can have zero or several findings, so
--    aggregate them to keep one row per order.
SELECT o.order_id, o.segment, o.amount, o.items, round(s.anomaly_score, 3) AS score,
       (SELECT string_agg(e.column_name || ': ' || e.explanation, '; ' ORDER BY e.column_name)
        FROM order_explanations e WHERE e.order_id = s.order_id) AS outlier_tree_findings
FROM order_scores s
JOIN orders o USING (order_id)
WHERE s.is_anomaly
ORDER BY s.anomaly_score DESC;
```

Gate: raise an error to stop the script (`duckdb -bail`, or any client) when DBSCAN finds noise:

```sql
SELECT CASE WHEN status = 'fail' THEN error('anomaly gate: ' || message) ELSE 'ok' END AS gate
FROM dbscan('orders', 'amount', 60, 5, 'summary');
```

```text
Invalid Input Error: anomaly gate: 1 noise point(s) detected
```

## Common patterns

```sql
-- Gate on an Isolation Forest score threshold instead of its contamination-based status
SELECT CASE WHEN count(*) FILTER (WHERE anomaly_score > 0.75) > 5
            THEN error('too many high-score rows') ELSE 'ok' END AS gate
FROM isolation_forest('orders', 'amount', 100, 256, 0.01, 'scores', 7);

-- Per-group outlier counts from OutlierTree
SELECT column_name, count(*) AS n_outliers
FROM outlier_tree('orders', 'segment, amount', 'outliers')
GROUP BY column_name;

-- Date columns: expose a numeric derivative through a view
CREATE VIEW orders_num AS SELECT order_id, amount, epoch(DATE '2024-01-01' + order_id::INT % 30) AS day_epoch FROM orders;
SELECT status, outlier_count FROM outlier_tree('orders_num', 'amount, day_epoch', 'summary');

-- Canonical name works identically
SELECT status FROM anofox_tab_dbscan('orders', 'amount', 60, 5);
```

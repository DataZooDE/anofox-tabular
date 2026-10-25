# Anomaly Detection

> Find unusual rows with Isolation Forest, DBSCAN and OutlierTree.

**Use this page to:**
- Score rows of one column or several columns with an Isolation Forest
- Find noise points with DBSCAN
- Get a written explanation for an outlier with OutlierTree
- Keep a key column next to every result with `id_column`

All functions here are table functions that run unsupervised: they need no labels. Each has a
`summary` mode that returns one pass/fail row, and a row-level mode that returns one row per
input row (or per outlier).

These functions read the table through a second connection. Pass a regular table or a view,
not a TEMP table: a TEMP table fails with `Table with name ... does not exist`.

## Functions

| Function | Canonical name | Kind | Purpose |
|----------|----------------|------|---------|
| [`isolation_forest`](#isolation_forest) | `anofox_tab_isolation_forest` | table | Isolation Forest over one numeric column |
| [`isolation_forest_mv`](#isolation_forest_mv) | `anofox_tab_isolation_forest_mv` | table | Isolation Forest over one or more columns, numeric or categorical |
| [`dbscan`](#dbscan) | `anofox_tab_dbscan` | table | DBSCAN density clustering over one numeric column |
| [`dbscan_mv`](#dbscan_mv) | `anofox_tab_dbscan_mv` | table | DBSCAN over several numeric columns |
| [`outlier_tree`](#outlier_tree) | `anofox_tab_outlier_tree` | table | Outliers within groups of similar rows, with an explanation |

For single-column checks with a fixed rule (z-score, IQR) see [`zscore` and `iqr`](07-data-quality.md).

## Example data

The examples on this page use two tables. `readings` has 200 sensor readings with two extreme
temperatures (readings 50 and 120) and one reading with an unusual humidity (reading 150).
`staff` has 100 junior and 100 senior salaries; staff member 7 is a junior with a senior salary.

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

## How the Isolation Forest flags rows

An Isolation Forest isolates each row with random splits. Rows that are isolated with few
splits get a high `anomaly_score` (close to 1); typical rows score around 0.5 or lower.
`is_anomaly` is `true` for the rows whose score lies above the `(1 - contamination)` quantile of
all scores. `contamination` is therefore the share of rows you expect to be anomalous: with the
default `0.1`, about 10% of the rows are flagged even when the data has fewer real outliers.
Set it to your expected outlier rate.

Without a `seed` the trees are random and scores change between runs. Pass a `seed` for
reproducible results.

---

## `isolation_forest`

Canonical name: `anofox_tab_isolation_forest`. Table function.

Isolation Forest over a single numeric column. Rows where the column is `NULL` are skipped.

```text
isolation_forest(table_name VARCHAR, column_name VARCHAR
                 [, n_trees BIGINT [, sample_size BIGINT [, contamination DOUBLE
                 [, output_mode VARCHAR [, seed BIGINT]]]]]
                 [, id_column := VARCHAR]) -> TABLE
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view to read |
| `column_name` | `VARCHAR` | required | Numeric column to score |
| `n_trees` | `BIGINT` | `100` | Number of trees, 1 to 500 |
| `sample_size` | `BIGINT` | `256` | Rows sampled per tree, 1 to 10000 |
| `contamination` | `DOUBLE` | `0.1` | Expected share of anomalies, 0.0 to 0.5 |
| `output_mode` | `VARCHAR` | `'summary'` | `'summary'` or `'scores'` (lowercase) |
| `seed` | `BIGINT` | `NULL` (random) | Fixed seed for reproducible scores; `NULL` keeps it random |
| `id_column` | `VARCHAR` | none | Named only; `'scores'` mode only. See [Identifier pass-through](#identifier-pass-through-id_column) |

Do not pass `NULL` for `n_trees`, `sample_size` or `contamination`: it raises an internal error
instead of using the default. Pass the default value instead.

**Output, `'summary'` mode** (one row):

| Column | Type | Description |
|--------|------|-------------|
| `status` | `VARCHAR` | `'fail'` when at least one row is flagged, else `'pass'` |
| `outlier_count` | `BIGINT` | Rows with `is_anomaly = true` |
| `total_count` | `BIGINT` | Rows analyzed (non-NULL values) |
| `avg_value` | `DOUBLE` | Mean of the column |
| `contamination` | `DOUBLE` | The contamination used |
| `n_trees` | `INTEGER` | The number of trees used |
| `message` | `VARCHAR` | For example `20 anomalies detected` |

**Output, `'scores'` mode** (one row per analyzed row):

| Column | Type | Description |
|--------|------|-------------|
| `row_id` | `BIGINT` | 1-based position among the analyzed rows (rows with a `NULL` value are not counted) |
| `<id_column>` | the column's type | Only with `id_column` |
| `value` | `DOUBLE` | The column value |
| `anomaly_score` | `DOUBLE` | Higher is more anomalous |
| `is_anomaly` | `BOOLEAN` | Score above the contamination threshold |

With the default contamination of 0.1, 20 of the 200 rows are flagged:

```sql
SELECT * FROM isolation_forest('readings', 'temperature');
```

```text
┌─────────┬───────────────┬─────────────┬───────────┬───────────────┬─────────┬───────────────────────┐
│ status  │ outlier_count │ total_count │ avg_value │ contamination │ n_trees │        message        │
│ varchar │     int64     │    int64    │  double   │    double     │  int32  │        varchar        │
├─────────┼───────────────┼─────────────┼───────────┼───────────────┼─────────┼───────────────────────┤
│ fail    │            20 │         200 │    22.375 │           0.1 │     100 │ 20 anomalies detected │
└─────────┴───────────────┴─────────────┴───────────┴───────────────┴─────────┴───────────────────────┘
```

Score every row with 1% contamination and a fixed seed, and keep the reading id:

```sql
SELECT reading_id, value, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest('readings', 'temperature', 100, 256, 0.01, 'scores', 42,
                      id_column := 'reading_id')
ORDER BY anomaly_score DESC, reading_id
LIMIT 4;
```

```text
┌────────────┬────────┬────────┬────────────┐
│ reading_id │ value  │ score  │ is_anomaly │
│   int64    │ double │ double │  boolean   │
├────────────┼────────┼────────┼────────────┤
│         50 │   95.0 │  0.903 │ true       │
│        120 │  -30.0 │  0.892 │ true       │
│         10 │   20.0 │  0.497 │ false      │
│         20 │   20.0 │  0.497 │ false      │
└────────────┴────────┴────────┴────────────┘
```

The extended options (Extended IF, SCiForest, weights, scoring metrics) exist only on
[`isolation_forest_mv`](#isolation_forest_mv), which also accepts a single column.

---

## `isolation_forest_mv`

Canonical name: `anofox_tab_isolation_forest_mv`. Table function.

Isolation Forest over one or more columns. Numeric columns are split on values. `VARCHAR`
columns are treated as categorical and split on random subsets of their categories. Rows with a
`NULL` in any selected column are skipped.

```text
isolation_forest_mv(table_name VARCHAR, column_names VARCHAR
                    [, n_trees BIGINT [, sample_size BIGINT [, contamination DOUBLE
                    [, output_mode VARCHAR [, seed BIGINT [, ndim BIGINT
                    [, coef_type VARCHAR [, scoring_metric VARCHAR [, weight_column VARCHAR
                    [, ntry BIGINT [, prob_pick_avg_gain DOUBLE]]]]]]]]]]]
                    [, id_column := VARCHAR]) -> TABLE
```

| # | Parameter | Type | Default | Description |
|---|-----------|------|---------|-------------|
| 1 | `table_name` | `VARCHAR` | required | Table or view to read |
| 2 | `column_names` | `VARCHAR` | required | Comma-separated columns, e.g. `'temperature, humidity'` |
| 3 | `n_trees` | `BIGINT` | `100` | Number of trees, 1 to 500 |
| 4 | `sample_size` | `BIGINT` | `256` | Rows sampled per tree, 1 to 10000 |
| 5 | `contamination` | `DOUBLE` | `0.1` | Expected share of anomalies, 0.0 to 0.5 |
| 6 | `output_mode` | `VARCHAR` | `'summary'` | `'summary'` or `'scores'` (lowercase) |
| 7 | `seed` | `BIGINT` | `NULL` (random) | Fixed seed; `NULL` keeps it random |
| 8 | `ndim` | `BIGINT` | `1` | Columns combined per split, at least 1. Above 1 the trees split on random hyperplanes (Extended Isolation Forest) |
| 9 | `coef_type` | `VARCHAR` | `'uniform'` | Distribution of the hyperplane coefficients: `'uniform'` or `'normal'` |
| 10 | `scoring_metric` | `VARCHAR` | `'depth'` | `'depth'`, `'density'` or `'adj_depth'` |
| 11 | `weight_column` | `VARCHAR` | `NULL` (equal weights) | Numeric column with sample weights |
| 12 | `ntry` | `BIGINT` | `1` | Split candidates tried per node, at least 1. Above 1 the best candidate by gain is used (SCiForest) |
| 13 | `prob_pick_avg_gain` | `DOUBLE` | `0.0` | Probability, 0.0 to 1.0, of choosing a split by average gain instead of at random |
| | `id_column` | `VARCHAR` | none | Named only; `'scores'` mode only |

Parameters 7 to 13 accept `NULL` to keep their default, so you can set a later parameter
without spelling out the earlier ones. Parameters 3 to 5 do not: a `NULL` there raises an
internal error. `coef_type` and `scoring_metric` are case-insensitive.

**Output, `'summary'` mode** (one row): `status`, `outlier_count`, `total_count` as for
`isolation_forest`, then:

| Column | Type | Description |
|--------|------|-------------|
| `n_columns` | `INTEGER` | Number of columns analyzed |
| `contamination` | `DOUBLE` | The contamination used |
| `n_trees` | `INTEGER` | The number of trees used |
| `message` | `VARCHAR` | For example `3 anomalies detected` |

**Output, `'scores'` mode**: `row_id BIGINT`, [`<id_column>`,] `anomaly_score DOUBLE`,
`is_anomaly BOOLEAN`. There is no `value` column.

Reading 150 has a normal temperature but an unusual humidity. Only the two-column forest finds
it:

```sql
SELECT row_id, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest_mv('readings', 'temperature, humidity', 100, 256, 0.02, 'scores', 42)
ORDER BY anomaly_score DESC, row_id
LIMIT 4;
```

```text
┌────────┬────────┬────────────┐
│ row_id │ score  │ is_anomaly │
│ int64  │ double │  boolean   │
├────────┼────────┼────────────┤
│    150 │  0.848 │ true       │
│     50 │  0.792 │ true       │
│    120 │   0.78 │ true       │
│     69 │  0.536 │ false      │
└────────┴────────┴────────────┘
```

```sql
SELECT status, outlier_count, total_count, n_columns, message
FROM isolation_forest_mv('readings', 'temperature, humidity', 100, 256, 0.02, 'summary', 42);
```

```text
┌─────────┬───────────────┬─────────────┬───────────┬──────────────────────┐
│ status  │ outlier_count │ total_count │ n_columns │       message        │
│ varchar │     int64     │    int64    │   int32   │       varchar        │
├─────────┼───────────────┼─────────────┼───────────┼──────────────────────┤
│ fail    │             3 │         200 │         2 │ 3 anomalies detected │
└─────────┴───────────────┴─────────────┴───────────┴──────────────────────┘
```

Extended Isolation Forest with 2-dimensional hyperplanes and normally distributed
coefficients:

```sql
SELECT row_id, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest_mv('readings', 'temperature, humidity', 100, 256, 0.02, 'scores', 42,
                         2, 'normal')
ORDER BY anomaly_score DESC, row_id
LIMIT 4;
```

```text
┌────────┬────────┬────────────┐
│ row_id │ score  │ is_anomaly │
│ int64  │ double │  boolean   │
├────────┼────────┼────────────┤
│     50 │  0.874 │ true       │
│    120 │  0.852 │ true       │
│    150 │  0.828 │ true       │
│     69 │  0.537 │ false      │
└────────┴────────┴────────────┘
```

The `'density'` metric produces scores on another scale than `'depth'` and `'adj_depth'`: zero and
below instead of between 0 and 1. Higher is still more anomalous, and `is_anomaly` still flags
the highest `contamination` share of scores.

SCiForest: try 10 split candidates per node and pick by gain half of the time:

```sql
SELECT status, outlier_count, message
FROM isolation_forest_mv('readings', 'temperature, humidity', 100, 256, 0.02, 'summary', 42,
                         NULL, NULL, NULL, NULL, 10, 0.5);
```

```text
┌─────────┬───────────────┬──────────────────────┐
│ status  │ outlier_count │       message        │
│ varchar │     int64     │       varchar        │
├─────────┼───────────────┼──────────────────────┤
│ fail    │             3 │ 3 anomalies detected │
└─────────┴───────────────┴──────────────────────┘
```

A categorical column next to a numeric one:

```sql
SELECT row_id, round(anomaly_score, 3) AS score
FROM isolation_forest_mv('readings', 'sensor, temperature', 100, 256, 0.01, 'scores', 42)
ORDER BY anomaly_score DESC, row_id
LIMIT 3;
```

```text
┌────────┬────────┐
│ row_id │ score  │
│ int64  │ double │
├────────┼────────┤
│     50 │  0.798 │
│    120 │  0.776 │
│      9 │  0.518 │
└────────┴────────┘
```

---

## How DBSCAN labels points

DBSCAN groups points that lie close together. A point with at least `min_pts` neighbors within
distance `eps` is a `CORE` point. A point within `eps` of a core point but with fewer neighbors
is a `BORDER` point. Every other point is `NOISE`, with `cluster_id = -1`. Noise points are the
anomalies: they have `anomaly_score = 1.0` and `is_anomaly = true`. Distances are Euclidean on
the raw values, so put columns on comparable scales before you combine them.

---

## `dbscan`

Canonical name: `anofox_tab_dbscan`. Table function.

DBSCAN over a single numeric column. Rows where the column is `NULL` are skipped.

```text
dbscan(table_name VARCHAR, column_name VARCHAR
       [, eps DOUBLE [, min_pts BIGINT [, output_mode VARCHAR]]]
       [, id_column := VARCHAR]) -> TABLE
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view to read |
| `column_name` | `VARCHAR` | required | Numeric column |
| `eps` | `DOUBLE` | `0.5` | Neighborhood radius, a finite number above 0 |
| `min_pts` | `BIGINT` | `5` | Neighbors needed for a core point, at least 1 |
| `output_mode` | `VARCHAR` | `'summary'` | `'summary'` or `'clusters'` (lowercase) |
| `id_column` | `VARCHAR` | none | Named only; `'clusters'` mode only |

A `NULL` `eps` or `min_pts` raises an internal error instead of using the default.

**Output, `'summary'` mode** (one row):

| Column | Type | Description |
|--------|------|-------------|
| `status` | `VARCHAR` | `'fail'` when there is at least one noise point, else `'pass'` |
| `cluster_count` | `BIGINT` | Number of clusters |
| `noise_count` | `BIGINT` | Number of noise points |
| `total_count` | `BIGINT` | Rows analyzed |
| `noise_rate` | `DOUBLE` | `noise_count / total_count` |
| `largest_cluster_size` | `BIGINT` | Points in the largest cluster |
| `eps` | `DOUBLE` | The radius used |
| `min_pts` | `BIGINT` | The `min_pts` used |
| `message` | `VARCHAR` | For example `2 noise point(s) detected` |

**Output, `'clusters'` mode** (one row per analyzed row):

| Column | Type | Description |
|--------|------|-------------|
| `row_id` | `BIGINT` | 1-based position among the analyzed rows |
| `<id_column>` | the column's type | Only with `id_column` |
| `value` | `DOUBLE` | The column value |
| `cluster_id` | `INTEGER` | Cluster number, `-1` for noise |
| `point_type` | `VARCHAR` | `'CORE'`, `'BORDER'` or `'NOISE'` |
| `neighbor_count` | `BIGINT` | Neighbors within `eps` |
| `anomaly_score` | `DOUBLE` | `1.0` for noise points |
| `is_anomaly` | `BOOLEAN` | `true` for noise points |

```sql
SELECT * FROM dbscan('readings', 'temperature', 1.0, 5);
```

```text
┌─────────┬───────────────┬─────────────┬─────────────┬────────────┬──────────────────────┬────────┬─────────┬───────────────────────────┐
│ status  │ cluster_count │ noise_count │ total_count │ noise_rate │ largest_cluster_size │  eps   │ min_pts │          message          │
│ varchar │     int64     │    int64    │    int64    │   double   │        int64         │ double │  int64  │          varchar          │
├─────────┼───────────────┼─────────────┼─────────────┼────────────┼──────────────────────┼────────┼─────────┼───────────────────────────┤
│ fail    │             1 │           2 │         200 │       0.01 │                  198 │    1.0 │       5 │ 2 noise point(s) detected │
└─────────┴───────────────┴─────────────┴─────────────┴────────────┴──────────────────────┴────────┴─────────┴───────────────────────────┘
```

```sql
SELECT row_id, value, cluster_id, point_type, anomaly_score
FROM dbscan('readings', 'temperature', 1.0, 5, 'clusters')
WHERE point_type = 'NOISE'
ORDER BY row_id;
```

```text
┌────────┬────────┬────────────┬────────────┬───────────────┐
│ row_id │ value  │ cluster_id │ point_type │ anomaly_score │
│ int64  │ double │   int32    │  varchar   │    double     │
├────────┼────────┼────────────┼────────────┼───────────────┤
│     50 │   95.0 │         -1 │ NOISE      │           1.0 │
│    120 │  -30.0 │         -1 │ NOISE      │           1.0 │
└────────┴────────┴────────────┴────────────┴───────────────┘
```

---

## `dbscan_mv`

Canonical name: `anofox_tab_dbscan_mv`. Table function.

DBSCAN over several numeric columns. Rows with a `NULL` in any selected column are skipped.

```text
dbscan_mv(table_name VARCHAR, column_names VARCHAR
          [, eps DOUBLE [, min_pts BIGINT [, output_mode VARCHAR]]]
          [, id_column := VARCHAR]) -> TABLE
```

The parameters are those of [`dbscan`](#dbscan), with `column_names` as a comma-separated list.

**Output, `'summary'` mode**: the columns of `dbscan`, plus `n_columns INTEGER` before
`message`.

**Output, `'clusters'` mode**: the columns of `dbscan` without `value`: `row_id`,
[`<id_column>`,] `cluster_id`, `point_type`, `neighbor_count`, `anomaly_score`, `is_anomaly`.

```sql
SELECT row_id, cluster_id, point_type
FROM dbscan_mv('readings', 'temperature, humidity', 2.0, 5, 'clusters')
WHERE is_anomaly
ORDER BY row_id;
```

```text
┌────────┬────────────┬────────────┐
│ row_id │ cluster_id │ point_type │
│ int64  │   int32    │  varchar   │
├────────┼────────────┼────────────┤
│     50 │         -1 │ NOISE      │
│    120 │         -1 │ NOISE      │
│    150 │         -1 │ NOISE      │
└────────┴────────────┴────────────┘
```

---

## `outlier_tree`

Canonical name: `anofox_tab_outlier_tree`. Table function.

OutlierTree splits the rows into groups with decision-tree conditions (for example
`level IN ('junior')`) and flags values that are far from the rest of their group. A salary
that is normal across the table can be an outlier for a junior. Each outlier comes with the
group's statistics and a sentence that explains it. Numeric and categorical (`VARCHAR`)
columns are both supported. Rows with a `NULL` in any selected column are excluded.

There are two forms: 3 arguments, or all 9. Other argument counts are a binder error.

```text
outlier_tree(table_name VARCHAR, columns VARCHAR, output_mode VARCHAR
             [, id_column := VARCHAR]) -> TABLE

outlier_tree(table_name VARCHAR, columns VARCHAR, output_mode VARCHAR,
             max_depth INTEGER, max_perc_outliers DOUBLE,
             min_size_numeric INTEGER, min_size_categ INTEGER,
             z_norm DOUBLE, z_outlier DOUBLE
             [, id_column := VARCHAR]) -> TABLE
```

| Parameter | Type | Default (3-argument form) | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view to read |
| `columns` | `VARCHAR` | required | Comma-separated columns to analyze |
| `output_mode` | `VARCHAR` | required | `'outliers'` for one row per outlier; any other value returns the summary row |
| `max_depth` | `INTEGER` | `4` | Maximum tree depth, 1 to 1024 |
| `max_perc_outliers` | `DOUBLE` | `0.01` | Largest share of a group that may be flagged, above 0 and at most 1 |
| `min_size_numeric` | `INTEGER` | `25` | Smallest group for a numeric target, at least 1 |
| `min_size_categ` | `INTEGER` | `75` | Smallest group for a categorical target, at least 1 |
| `z_norm` | `DOUBLE` | `2.67` | Z threshold for the group's confidence interval, above 0 |
| `z_outlier` | `DOUBLE` | `8.0` | Z threshold to flag a value, above 0 |
| `id_column` | `VARCHAR` | none | Named only; `'outliers'` mode only |

In the 9-argument form all values are required; `NULL` raises an internal error. Lower the
size and z thresholds for small tables.

**Output, summary mode** (one row):

| Column | Type | Description |
|--------|------|-------------|
| `status` | `VARCHAR` | `'fail'` when at least one outlier is found, else `'pass'` |
| `total_rows` | `BIGINT` | Rows analyzed (non-NULL in every selected column) |
| `outlier_count` | `BIGINT` | Outliers found |
| `columns_analyzed` | `INTEGER` | Number of columns |
| `clusters_evaluated` | `BIGINT` | Groups evaluated |
| `max_depth_reached` | `INTEGER` | Deepest tree level used |
| `message` | `VARCHAR` | For example `2 outlier(s) detected` |

**Output, `'outliers'` mode** (one row per outlier value):

| Column | Type | Description |
|--------|------|-------------|
| `row_id` | `BIGINT` | 1-based position of the row in the table; excluded `NULL` rows still count |
| `<id_column>` | the column's type | Only with `id_column` |
| `column_name` | `VARCHAR` | Column with the outlier |
| `outlier_value` | `VARCHAR` | The value as text |
| `outlier_value_num` | `DOUBLE` | The value as a number for numeric columns, `NULL` for categorical ones |
| `cluster_mean` | `DOUBLE` | Mean of the group |
| `cluster_sd` | `DOUBLE` | Standard deviation of the group |
| `cluster_size` | `BIGINT` | Rows in the group |
| `z_score` | `DOUBLE` | Distance from the group in standard deviations |
| `lower_bound` | `DOUBLE` | Lower end of the group's normal range |
| `upper_bound` | `DOUBLE` | Upper end of the group's normal range |
| `conditions` | `VARCHAR` | JSON array of the conditions that define the group |
| `explanation` | `VARCHAR` | The outlier in one sentence |
| `outlier_score` | `DOUBLE` | Rarity score; lower is more anomalous |

```sql
SELECT status, total_rows, outlier_count, message
FROM outlier_tree('staff', 'level, salary', 'summary');
```

```text
┌─────────┬────────────┬───────────────┬───────────────────────┐
│ status  │ total_rows │ outlier_count │        message        │
│ varchar │   int64    │     int64     │        varchar        │
├─────────┼────────────┼───────────────┼───────────────────────┤
│ fail    │        200 │             2 │ 2 outlier(s) detected │
└─────────┴────────────┴───────────────┴───────────────────────┘
```

```sql
SELECT row_id, column_name, outlier_value, round(z_score, 2) AS z
FROM outlier_tree('staff', 'level, salary', 'outliers')
ORDER BY column_name;
```

```text
┌────────┬─────────────┬───────────────┬────────┐
│ row_id │ column_name │ outlier_value │   z    │
│ int64  │   varchar   │    varchar    │ double │
├────────┼─────────────┼───────────────┼────────┤
│      7 │ level       │ junior        │   9.54 │
│      7 │ salary      │ 92000         │  18.66 │
└────────┴─────────────┴───────────────┴────────┘
```

```sql
SELECT explanation
FROM outlier_tree('staff', 'level, salary', 'outliers')
WHERE column_name = 'salary';
```

```text
┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                       explanation                                                        │
│                                                         varchar                                                          │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Value 92000 for column 'salary' is unusually high (mean: 50655.00, SD: 4416.85, z-score: 18.66) when level IN ('junior') │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

`outlier_value_num` compares directly with the bounds, with no cast:

```sql
SELECT outlier_value_num, round(lower_bound) AS lower_bound, round(upper_bound) AS upper_bound
FROM outlier_tree('staff', 'level, salary', 'outliers')
WHERE outlier_value_num IS NOT NULL;
```

```text
┌───────────────────┬─────────────┬─────────────┐
│ outlier_value_num │ lower_bound │ upper_bound │
│      double       │   double    │   double    │
├───────────────────┼─────────────┼─────────────┤
│           92000.0 │     38862.0 │     62448.0 │
└───────────────────┴─────────────┴─────────────┘
```

The 9-argument form, with smaller groups and a lower flag threshold:

```sql
SELECT status, outlier_count
FROM outlier_tree('staff', 'level, salary', 'summary', 4, 0.05, 10, 10, 2.67, 4.0);
```

```text
┌─────────┬───────────────┐
│ status  │ outlier_count │
│ varchar │     int64     │
├─────────┼───────────────┤
│ fail    │            12 │
└─────────┴───────────────┘
```

---

## Identifier pass-through (`id_column`)

The row-level outputs identify rows by `row_id`, a position. For `isolation_forest*` and
`dbscan*` it counts only the analyzed rows, so after a skipped `NULL` row it no longer matches
the table position. The named parameter `id_column := '<column>'` adds the value of an input
column to every output row, so results join back to the input by key:

| Function | Row-level mode |
|----------|----------------|
| `isolation_forest`, `isolation_forest_mv` | `'scores'` |
| `dbscan`, `dbscan_mv` | `'clusters'` |
| `outlier_tree` | `'outliers'` |
| [`pii_audit_table`](06-pii.md#pii_audit_table) | always |

- The column comes directly after `row_id`, under the input column's name and with its type.
  Without `id_column` the output is unchanged.
- The id is read in the same pass as the values, so it always belongs to the row it is shown
  with. Rows skipped because a value is `NULL` are skipped with their id.
- `NULL` ids are passed through; they never exclude a row.
- The canonical names and the short aliases both accept it.

```sql
SELECT s.reading_id, r.sensor, r.temperature, round(s.anomaly_score, 3) AS score
FROM isolation_forest_mv('readings', 'temperature, humidity', 100, 256, 0.02, 'scores', 42,
                         id_column := 'reading_id') s
JOIN readings r USING (reading_id)
WHERE s.is_anomaly
ORDER BY s.reading_id;
```

```text
┌────────────┬─────────┬───────────────┬────────┐
│ reading_id │ sensor  │  temperature  │ score  │
│   int64    │ varchar │ decimal(22,1) │ double │
├────────────┼─────────┼───────────────┼────────┤
│         50 │ S03     │          95.0 │  0.792 │
│        120 │ S01     │         -30.0 │   0.78 │
│        150 │ S03     │          20.0 │  0.848 │
└────────────┴─────────┴───────────────┴────────┘
```

Bind errors: a column the table does not have,

```sql
SELECT * FROM isolation_forest('readings', 'temperature', 100, 256, 0.1, 'scores', 42,
                               id_column := 'no_such_column');
```

```text
Binder Error: isolation_forest: id_column 'no_such_column' not found in table 'readings'

LINE 1: SELECT * FROM isolation_forest('readings', 'temperature', 100, 256, 0...
                      ^
```

a mode that is not row-level,

```sql
SELECT * FROM dbscan('readings', 'temperature', 1.0, 5, 'summary', id_column := 'reading_id');
```

```text
Binder Error: dbscan: id_column requires output_mode 'clusters'
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM dbscan('readings', 'temperature', 1.0, 5, 'summary', id_column...
                      ^
```

and a name that collides with an output column. Rename the column in a view first.

```sql
CREATE TABLE scored AS SELECT reading_id AS anomaly_score, temperature FROM readings;
SELECT * FROM isolation_forest('scored', 'temperature', 100, 256, 0.1, 'scores', 42,
                               id_column := 'anomaly_score');
```

```text
Binder Error: isolation_forest: id_column 'anomaly_score' collides with output column 'anomaly_score'; rename or alias it first

LINE 1: SELECT * FROM isolation_forest('scored', 'temperature', 100, 256, 0.1...
                      ^
```

## See also

- [Guide: anomaly detection](../guides/03-anomaly-detection.md)
- [Data quality checks: `zscore`, `iqr`, `metric_anomaly_iqr`](07-data-quality.md)
- [Conventions](00-conventions.md)

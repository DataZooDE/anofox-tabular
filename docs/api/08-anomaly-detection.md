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
not a TEMP table: a TEMP table fails with
`TEMP tables are not supported by isolation_forest; use a regular table`. The second connection
sees only committed data, so inside a transaction with uncommitted changes the functions refuse
to run; call them after `COMMIT` (see
[Conventions](00-conventions.md#table-and-column-arguments)).

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
`contamination` is the share of rows you expect to be anomalous. The cut is the score at the
`(1 - contamination)` quantile of the `n` scored rows, and the rows that score above it are
flagged: `k = (n - 1) - floor((1 - contamination) * (n - 1))` rows when no scores are equal.
With the default `0.1`, about 10% of the rows are flagged even when the data has fewer real
outliers. Set it to your expected outlier rate.

Rows with equal values get equal scores. Ties at the cut are handled like this:

- When the cut lies inside the group of rows that share the **highest** score (identical
  outliers at the top), the whole group is flagged, and `outlier_count` can be larger than
  `contamination` times the rows. Thirty identical `1000.0` readings among 200 rows are all
  flagged at `contamination` 0.01, not none of them.
- Any other tie at the cut is not flagged. When the cut falls inside a group of equal ordinary
  readings, only the rows above that group are flagged, so fewer than `k` rows can be flagged.
- When all scores are equal, as for a constant column, no row is flagged and `status` is
  `'pass'`.

Scores that differ by at most a relative `1e-9` count as equal.

### NaN and infinite values

The trees are built from the finite values only:

- A row with `+Inf` or `-Inf` (in any selected column for `isolation_forest_mv`) gets
  `anomaly_score` 1.0 and `is_anomaly = true`. It counts in `outlier_count` and `total_count`,
  but not in the contamination share, which applies to the finite rows.
- A row with `NaN` (in any selected numeric column) is skipped like a `NULL`: it is not in the
  `'scores'` output or in `total_count`, and it keeps its row number.
- `avg_value` is the mean of the finite values, and `NULL` when no scored row has a finite value.

Without a `seed` the trees are random and scores change between runs. Pass a `seed` for
reproducible results. A seed reproduces the scores on the same platform; Linux, macOS and Windows
builds draw different random numbers from the same seed, so scores (and which ordinary rows fall
inside the contamination share) can differ between them, while clear outliers are flagged on all.

---

## `isolation_forest`

Canonical name: `anofox_tab_isolation_forest`. Table function.

Isolation Forest over a single numeric column. Rows where the column is `NULL` or `NaN` are
skipped; `±Inf` values are anomalies with score 1.0 (see
[NaN and infinite values](#nan-and-infinite-values)). A
column that is not numeric (`BOOLEAN` counts as 0/1) is a binder error, for example
`isolation_forest: column 'segment' must be numeric, got VARCHAR`; score categorical columns with
[`isolation_forest_mv`](#isolation_forest_mv).

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
| `output_mode` | `VARCHAR` | `'summary'` | `'summary'` or `'scores'` (case-insensitive) |
| `seed` | `BIGINT` | `NULL` (random) | Fixed seed for reproducible scores; `NULL` keeps it random |
| `id_column` | `VARCHAR` | none | Named only; `'scores'` mode only. See [Identifier pass-through](#identifier-pass-through-id_column) |

A `NULL` for `n_trees`, `sample_size`, `contamination` or `output_mode` keeps the default.

**Output, `'summary'` mode** (one row):

| Column | Type | Description |
|--------|------|-------------|
| `status` | `VARCHAR` | `'fail'` when at least one row is flagged, else `'pass'` |
| `outlier_count` | `BIGINT` | Rows with `is_anomaly = true` |
| `total_count` | `BIGINT` | Rows analyzed (values that are not `NULL` or `NaN`) |
| `avg_value` | `DOUBLE` | Mean of the finite values (`NULL` if there are none) |
| `contamination` | `DOUBLE` | The contamination used |
| `n_trees` | `INTEGER` | The number of trees used |
| `message` | `VARCHAR` | For example `20 anomalies detected` |

**Output, `'scores'` mode** (one row per analyzed row):

| Column | Type | Description |
|--------|------|-------------|
| `row_id` | `BIGINT` | 1-based row number in the table; skipped `NULL` and `NaN` rows keep their number. See [Row numbering](#row-numbering-row_id) |
| `<id_column>` | the column's type | Only with `id_column` |
| `value` | `DOUBLE` | The column value |
| `anomaly_score` | `DOUBLE` | Higher is more anomalous |
| `is_anomaly` | `BOOLEAN` | Score at or above the contamination threshold, or a `±Inf` value. See [How the Isolation Forest flags rows](#how-the-isolation-forest-flags-rows) |

With the default contamination of 0.1, 20 of the 200 rows are flagged (the seed `42` makes the
example reproducible):

```sql
SELECT * FROM isolation_forest('readings', 'temperature', 100, 256, 0.1, 'summary', 42);
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
columns are treated as categorical and split on random subsets of their categories. Columns of
other types (`DATE`, `TIMESTAMP`, ...) are a binder error. Rows with a
`NULL` in any selected column, or a `NaN` in any selected numeric column, are skipped. A row with
`±Inf` in any selected column is an anomaly with score 1.0 (see
[NaN and infinite values](#nan-and-infinite-values)).

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
| 6 | `output_mode` | `VARCHAR` | `'summary'` | `'summary'` or `'scores'` (case-insensitive) |
| 7 | `seed` | `BIGINT` | `NULL` (random) | Fixed seed; `NULL` keeps it random |
| 8 | `ndim` | `BIGINT` | `1` | Columns combined per split, at least 1. Above 1 the trees split on random hyperplanes (Extended Isolation Forest) |
| 9 | `coef_type` | `VARCHAR` | `'uniform'` | Distribution of the hyperplane coefficients: `'uniform'` or `'normal'` |
| 10 | `scoring_metric` | `VARCHAR` | `'depth'` | `'depth'`, `'density'` or `'adj_depth'` |
| 11 | `weight_column` | `VARCHAR` | `NULL` (equal weights) | Numeric column with sample weights |
| 12 | `ntry` | `BIGINT` | `1` | Split candidates tried per node, at least 1. Above 1 the best candidate by gain is used (SCiForest) |
| 13 | `prob_pick_avg_gain` | `DOUBLE` | `0.0` | Probability, 0.0 to 1.0, of choosing a split by average gain instead of at random |
| | `id_column` | `VARCHAR` | none | Named only; `'scores'` mode only |

Every optional parameter accepts `NULL` to keep its default, so you can set a later parameter
without spelling out the earlier ones. `coef_type` and `scoring_metric` are case-insensitive.

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

DBSCAN over a single numeric column. Rows where the column is `NULL` are skipped. A non-numeric
column is a binder error (`dbscan: column 'segment' must be numeric, got VARCHAR`).

The neighbour search sorts the values once and only visits the points within `eps` of each point,
so the run time grows with the number of rows times the typical neighbourhood size, not with the
square of the row count. A large `eps` that puts most rows into each other's neighbourhood is
still quadratic. A column holding `NaN` or infinite values falls back to comparing every pair of
points.

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
| `output_mode` | `VARCHAR` | `'summary'` | `'summary'` or `'clusters'` (case-insensitive) |
| `id_column` | `VARCHAR` | none | Named only; `'clusters'` mode only |

A `NULL` for `eps`, `min_pts` or `output_mode` keeps the default.

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
| `row_id` | `BIGINT` | 1-based row number in the table; skipped `NULL` rows keep their number. See [Row numbering](#row-numbering-row_id) |
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

DBSCAN over several numeric columns. Rows with a `NULL` in any selected column are skipped. A
non-numeric column is a binder error.

With two or more columns every point is compared with every other point, so the run time grows
with the square of the row count: tens of thousands of rows take seconds, 100,000 rows take
tens of seconds or more. Sample larger tables first.

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
group's statistics and a sentence that explains it. Rows with a `NULL` in any selected column
are excluded.

Column names match case-insensitively. Supported column types:

| Column type | Treated as |
|-------------|-----------|
| Numeric types (`INTEGER`, `BIGINT`, `DECIMAL`, `DOUBLE`, ...) | numeric |
| `BOOLEAN` | numeric, `false` = 0 and `true` = 1 |
| `VARCHAR`, `ENUM` | categorical |

Any other type (`DATE`, `TIMESTAMP`, `LIST`, ...) is a binder error, for example
`outlier_tree: column 'shipped_at' must be numeric or categorical (VARCHAR), got TIMESTAMP`. An
unknown column is a binder error too. To analyze a date, pass a numeric derivative such as
`epoch(shipped_at)` through a view.

### How a numeric value is scored

Within its group, a numeric value is scored by the robust z-score
`z = (value - median) / spread`, and flagged when `|z| >= z_outlier`. The spread is:

1. the median absolute deviation (MAD) × 1.4826;
2. when more than half of the group shares one value, the MAD is 0 (order quantities that are
   mostly 1, discounts that are mostly 0). The spread is then the mean absolute deviation from
   the median × 1.2533 (√(π/2));
3. the standard deviation, if both are 0.

Both factors make the spread comparable to a standard deviation for normally distributed
values. A single deviating value in an otherwise constant group is therefore flagged. When
every finite value of the group equals the median, the spread is 0 and nothing in the group is
flagged, except an infinite value. The group's normal range is `median ± z_norm × spread`, so
a flagged value is always outside its reported range.

A value that occurs more often in a group than the group may have outliers
(`max_perc_outliers` of its rows, at least 1) is a regular value of the group and is never
flagged: with most invoices at discount 0 and 6 % at 5, the 5 % discounts stay unflagged.

99 receipts of 1.10 and one of 999.99 (MAD = 0):

```sql
CREATE TABLE pos_receipts AS
SELECT i AS receipt_id, 'shop' AS seg, CASE WHEN i = 50 THEN 999.99 ELSE 1.10 END AS amount
FROM range(1, 101) t(i);

SELECT row_id, outlier_value, cluster_median, round(cluster_mad, 2) AS cluster_mad,
       round(z_score, 2) AS z, round(upper_bound, 2) AS upper_bound
FROM outlier_tree('pos_receipts', 'seg, amount', 'outliers');
```

```text
┌────────┬───────────────┬────────────────┬─────────────┬────────┬─────────────┐
│ row_id │ outlier_value │ cluster_median │ cluster_mad │   z    │ upper_bound │
│ int64  │    varchar    │     double     │   double    │ double │   double    │
├────────┼───────────────┼────────────────┼─────────────┼────────┼─────────────┤
│     50 │ 999.99        │            1.1 │       12.52 │  79.79 │       34.53 │
└────────┴───────────────┴────────────────┴─────────────┴────────┴─────────────┘
```

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
| `output_mode` | `VARCHAR` | required | `'summary'` for one summary row, `'outliers'` for one row per outlier value, so a row with outliers in two columns gives two rows (case-insensitive; any other value is an error, `NULL` means `'summary'`) |
| `max_depth` | `INTEGER` | `4` | Maximum tree depth, 1 to 1024 |
| `max_perc_outliers` | `DOUBLE` | `0.01` | Largest share of a group that may be flagged, above 0 and at most 1 |
| `min_size_numeric` | `INTEGER` | `25` | Smallest group for a numeric target, at least 1 |
| `min_size_categ` | `INTEGER` | `75` | Smallest group for a categorical target, at least 1 |
| `z_norm` | `DOUBLE` | `2.67` | Z threshold for the group's confidence interval, above 0 |
| `z_outlier` | `DOUBLE` | `8.0` | Z threshold to flag a value, above 0 |
| `id_column` | `VARCHAR` | none | Named only; `'outliers'` mode only |

In the 9-argument form a `NULL` keeps that parameter's default. Lower the size and z
thresholds for small tables.

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

NaN and `±Inf` values of a numeric target are left out of the group statistics, which are
computed over the finite values. An infinite value is still reported as an outlier, with
`z_score` `NULL` and `z-score: n/a` in the explanation. A statistic that does not fit in a
`DOUBLE` (values near ±1.8e308) is `NULL` too, so the output never contains `inf` or `NaN`
statistics.

| Column | Type | Description |
|--------|------|-------------|
| `row_id` | `BIGINT` | 1-based row number in the table; skipped `NULL` rows keep their number. See [Row numbering](#row-numbering-row_id) |
| `<id_column>` | the column's type | Only with `id_column` |
| `column_name` | `VARCHAR` | Column with the outlier |
| `outlier_value` | `VARCHAR` | The value as text |
| `outlier_value_num` | `DOUBLE` | The value as a number for numeric columns, `NULL` for categorical ones |
| `cluster_mean` | `DOUBLE` | Mean of the group's finite values (descriptive; not used for scoring). `NULL` for categorical columns |
| `cluster_sd` | `DOUBLE` | Standard deviation of the group's finite values (descriptive; not used for scoring). `NULL` for categorical columns |
| `cluster_median` | `DOUBLE` | Median of the group's finite values; the centre of `z_score` and the bounds. `NULL` for categorical columns |
| `cluster_mad` | `DOUBLE` | The spread `z_score` and the bounds are scaled by: MAD × 1.4826, or the fallback described in [How a numeric value is scored](#how-a-numeric-value-is-scored). `NULL` for categorical columns |
| `cluster_size` | `BIGINT` | Rows in the group with a finite value |
| `z_score` | `DOUBLE` | `(value - cluster_median) / cluster_mad`; `NULL` for an infinite value |
| `lower_bound` | `DOUBLE` | Lower end of the group's normal range, `cluster_median - z_norm × cluster_mad`. `NULL` for categorical columns |
| `upper_bound` | `DOUBLE` | Upper end of the group's normal range, `cluster_median + z_norm × cluster_mad`. `NULL` for categorical columns |
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
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                         explanation                                                         │
│                                                           varchar                                                           │
├─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Value 92000 for column 'salary' is unusually high (median: 50500.00, MAD: 2223.90, z-score: 18.66) when level IN ('junior') │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
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
│           92000.0 │     44562.0 │     56438.0 │
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

## Row numbering (`row_id`)

The row-level outputs of every anomaly function identify a row by `row_id`:

- `row_id` is the 1-based number of the row in the order the input is scanned, the number
  `SELECT row_number() OVER () AS row_id, * FROM <table_name>` gives the same row.
- Every scanned row is counted, including rows that are skipped because a selected column (or
  the `weight_column`) is `NULL`, or because an Isolation Forest feature is `NaN`. A skipped row keeps its number; it is only absent from the
  output. So the row ids of the output can have gaps.
- All functions number the same way, so the same row has the same `row_id` in
  `isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv` and `outlier_tree`.
- For a view, the row number follows the rows the view returns, not the base table.
- The scan order is the insertion order while `preserve_insertion_order` is `true` (the DuckDB
  default). With `SET preserve_insertion_order = false` DuckDB can scan the rows in any order;
  join on a key with [`id_column`](#identifier-pass-through-id_column) in that case.

> **Changed**: before this release `isolation_forest`, `isolation_forest_mv`, `dbscan` and
> `dbscan_mv` numbered only the analyzed rows, so after a row with a `NULL` value every
> `row_id` was too small and a join on it returned the wrong rows. `outlier_tree` already used
> the table row number. Tables with no `NULL` in the analyzed columns get the same row ids as
> before.

In this view three temperatures are missing. The rows are skipped, but readings 50 and 120 keep
row ids 50 and 120, so the join by row number finds them:

```sql
CREATE VIEW readings_gaps AS
SELECT reading_id,
       CASE WHEN reading_id IN (10, 20, 30) THEN NULL ELSE temperature END AS temperature
FROM readings;

SELECT s.row_id, n.reading_id, n.temperature, round(s.anomaly_score, 3) AS score
FROM isolation_forest('readings_gaps', 'temperature', 100, 256, 0.01, 'scores', 42) s
JOIN (SELECT row_number() OVER () AS rn, * FROM readings_gaps) n ON n.rn = s.row_id
WHERE s.is_anomaly
ORDER BY s.row_id;
```

```text
┌────────┬────────────┬───────────────┬────────┐
│ row_id │ reading_id │  temperature  │ score  │
│ int64  │   int64    │ decimal(22,1) │ double │
├────────┼────────────┼───────────────┼────────┤
│     50 │         50 │          95.0 │  0.902 │
│    120 │        120 │         -30.0 │  0.887 │
└────────┴────────────┴───────────────┴────────┘
```

---

## Identifier pass-through (`id_column`)

The row-level outputs identify rows by `row_id`, a position (see
[Row numbering](#row-numbering-row_id)). The named parameter `id_column := '<column>'` adds the
value of an input column to every output row, so results join back to the input by key:

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
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

a mode that is not row-level,

```sql
SELECT * FROM dbscan('readings', 'temperature', 1.0, 5, 'summary', id_column := 'reading_id');
```

```text
Binder Error: dbscan: id_column requires output_mode 'clusters'
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM dbscan('readings', 'temperature', 1.0, 5, 'summary', id_column...
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
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
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

## See also

- [Guide: anomaly detection](../guides/03-anomaly-detection.md)
- [Data quality checks: `zscore`, `iqr`, `metric_anomaly_iqr`](07-data-quality.md)
- [Conventions](00-conventions.md)

---
name: anofox-tabular-profiling
description: >
  Table profiling with the anofox_tabular DuckDB extension: per-column statistics with
  `profile_table` (nulls, distinct counts, numeric stats, top values, lengths, detected
  patterns, NaN/Inf counts), a one-row overview with `profile_summary` and pairwise
  Pearson/Spearman correlations with `profile_correlations`. Use when a user asks to profile,
  explore or summarise a table, find null-heavy, constant, unique or correlated columns, or
  derive data-quality checks (thresholds, allowed values) from what the data looks like.
version: 2026.11.02
user-invocable: false
---

# Anofox Tabular — Profiling Cheat Sheet

**Extension:** `anofox_tabular` v2026.11.02 | **DuckDB:** v2.0.0 / v1.5.6 | **Naming:** short alias (`profile_table`) = canonical `anofox_tab_profile_table`

Describe a table before you validate it: one row per column, one row per table, one row per
numeric column pair. Load with `LOAD anofox_tabular;` (install: `INSTALL anofox_tabular FROM community;`).

```sql no-run
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;
```

## Critical gotchas

- **Column lists are `VARCHAR[]`, never a CSV string.** `profile_table('t', ['a', 'b'])`.
  `profile_table('t', 'a')` fails with a cast error (`VARCHAR` to `VARCHAR[]`).
- **Positional only.** No named parameters (`columns := ...` is a binder error). To set `exact`
  without changing other values, pass `NULL` for the skipped ones:
  `profile_table('t', NULL, NULL, true)`. `NULL` keeps the default.
- **Second connection.** All three functions read the table through a second connection:
  - TEMP tables are rejected: `TEMP tables are not supported by profile_table; use a regular table (CREATE TABLE ... AS SELECT * FROM <name>)`.
    Regular tables and views work. Copy a TEMP table with `CREATE TABLE x AS SELECT * FROM tmp`.
  - Committed data only: inside `BEGIN ... COMMIT` with uncommitted writes they refuse at bind
    time (`... reads the last committed data, but this transaction has uncommitted changes ...`).
    On DuckDB 2.0 that error aborts the transaction. Call after `COMMIT`.
  - The search path is inherited (`SET schema`, `SET search_path`, `USE`), so unqualified
    names resolve as in your session; `'staging.orders'` also works.
  - Python: a DataFrame registered as a view is not visible; create a real table first.
- **Errors are bind-time:** an unknown table is `profile_table: table 'x' does not exist`; an
  unknown column is `profile_table: column 'x' not found in table 't' (available columns: ...)`.
  Column names match case-insensitively. `sample_size <= 0` is `sample_size must be positive`.
- **Sampling is on by default above 1,000,000 rows.** Then `row_count` is the sample size, not
  the table size (`is_sampled = true`). Use `exact = true` for full-table counts, or
  `profile_summary` for the true `row_count`. The sample is a fixed-seed reservoir, so repeated
  runs give the same profile.
- **`min_val`, `max_val` and `top_values[].value` are `VARCHAR`.** Cast them back for
  arithmetic (`min_val::DOUBLE`). Ordering uses the column's own type, not the text.
  `TIMESTAMP WITH TIME ZONE` is shown in UTC (`2026-03-02 08:15:00+00`) whatever the session
  `TimeZone`; `INTERVAL` carries whole days out of the hours (`5 days`, not `120:00:00`).
- **NaN / ±Inf** are left out of `min_val`, `max_val`, `mean`, `median`, `stddev`, `p25`,
  `p75`, `skewness`, `kurtosis` and `top_values`; `nan_count` / `inf_count` count them. They
  still count in `distinct_count`, `is_unique`, `is_constant`. `profile_correlations` drops a
  row from a pair when either value is NULL, NaN or ±Inf.
- **Numeric-only columns are `NULL` for other types:** `mean`…`kurtosis`, `zero_count`,
  `negative_count`, `nan_count`, `inf_count`. `BOOLEAN` is not numeric (no `mean`). Length
  columns are `NULL` for numeric and temporal columns. `pattern_summary` is `NULL` for numeric,
  boolean, temporal and `BLOB` columns.
- **`is_unique` ignores NULLs.** A column with NULLs can be `is_unique = true`; pair it with
  `null_count = 0` before you call it a key. On an empty table `null_rate` is `NULL` and
  `is_unique` / `is_constant` are `true`.
- **`top_values` holds at most 5 values.** It is the complete value set only when
  `distinct_count <= 5`.
- These functions only describe. To enforce rules, use the check functions or `run_checks`
  (skill `anofox-tabular-data-quality`).

Sample data used below:

```sql
CREATE TABLE customers AS
SELECT i AS customer_id,
       CASE WHEN i % 8 = 0 THEN NULL ELSE 'c' || i || '@example.com' END AS email,
       ['basic', 'pro', 'pro', 'enterprise'][(i % 4) + 1] AS plan,
       CASE WHEN i % 25 = 0 THEN -5.0 ELSE 20.0 + (i % 9) * 3 END AS monthly_fee,
       (i % 4) * 2 + 1 AS seats,
       TIMESTAMPTZ '2026-01-01 00:00:00+00' + i * INTERVAL '7 hours' AS signed_up_at
FROM range(1, 101) t(i);
```

## `profile_table` — one row per column

```text
profile_table(table_name VARCHAR
              [, columns VARCHAR[] [, sample_size BIGINT [, exact BOOLEAN]]])
  -> TABLE(29 columns, one row per profiled column, in table order)
```

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `table_name` | `VARCHAR` | required | Table or view (not TEMP) |
| `columns` | `VARCHAR[]` | all columns | Subset to profile; output stays in table order; unknown name = error |
| `sample_size` | `BIGINT` | `1000000` | Profile a reservoir sample of this many rows when the table is larger; must be > 0 |
| `exact` | `BOOLEAN` | `false` | `true` scans every row regardless of `sample_size` |

Output columns:

| Column | Type | Notes |
|---|---|---|
| `column_name`, `column_type` | `VARCHAR` | `column_type` as DuckDB prints it (`DECIMAL(21,1)`, `TIMESTAMP WITH TIME ZONE`) |
| `row_count` | `BIGINT` | Rows scanned (sample size when sampled) |
| `null_count` / `null_rate` | `BIGINT` / `DOUBLE` | `null_rate = null_count / row_count` |
| `distinct_count` / `distinct_rate` | `BIGINT` / `DOUBLE` | Distinct non-NULL values; rate over `row_count` |
| `min_val`, `max_val` | `VARCHAR` | Text; finite values only for numeric; `NULL` for LIST/STRUCT/MAP/UNION |
| `mean`, `median`, `stddev`, `p25`, `p75`, `skewness`, `kurtosis` | `DOUBLE` | Numeric only, finite values only; exact for integer/HUGEINT/DECIMAL above 2^53 |
| `top_values` | `STRUCT(value VARCHAR, count BIGINT)[]` | Up to 5, most frequent first; ties in ascending order of the column type |
| `avg_length` / `min_length` / `max_length` | `DOUBLE` / `BIGINT` / `BIGINT` | Characters for strings, bytes for BLOB, elements for lists, cardinality for maps |
| `pattern_summary` | `VARCHAR` | See below |
| `is_unique`, `is_constant` | `BOOLEAN` | Over non-NULL values |
| `zero_count`, `negative_count` | `BIGINT` | Numeric only |
| `is_sampled` / `actual_sample_size` | `BOOLEAN` / `BIGINT` | Whether a sample was profiled / rows scanned |
| `nan_count`, `inf_count` | `BIGINT` | Numeric only; `inf_count` counts `Inf` and `-Inf` |

`pattern_summary` for string columns is the first of `email`, `phone` (via
`phonenumber_is_valid`, region `US`), `uuid`, `url`, `ip_address`, `iso_date`,
`numeric_string` that at least 80% of non-NULL values match, else `mixed`. Complex columns get
`list`, `map`, `struct` or `union`.

For a `HUGEINT` / wide `DECIMAL` column whose exact sum would overflow (about 1e38), `mean` (and
for DECIMAL with a huge spread also `stddev`, `skewness`, `kurtosis`) is `NULL` and a warning
trace names the column; the profile does not fail.

```sql
SELECT column_name, column_type, null_rate, distinct_count, min_val, max_val,
       is_unique, negative_count, pattern_summary
FROM profile_table('customers');
```

```text
┌──────────────┬──────────────────────────┬───────────┬────────────────┬────────────────────────┬────────────────────────┬───────────┬────────────────┬─────────────────┐
│ column_name  │       column_type        │ null_rate │ distinct_count │        min_val         │        max_val         │ is_unique │ negative_count │ pattern_summary │
├──────────────┼──────────────────────────┼───────────┼────────────────┼────────────────────────┼────────────────────────┼───────────┼────────────────┼─────────────────┤
│ customer_id  │ BIGINT                   │       0.0 │            100 │ 1                      │ 100                    │ true      │              0 │ NULL            │
│ email        │ VARCHAR                  │      0.12 │             88 │ c100@example.com       │ c9@example.com         │ true      │           NULL │ email           │
│ plan         │ VARCHAR                  │       0.0 │              3 │ basic                  │ pro                    │ false     │           NULL │ mixed           │
│ monthly_fee  │ DECIMAL(21,1)            │       0.0 │             10 │ -5.0                   │ 44.0                   │ false     │              4 │ NULL            │
│ seats        │ BIGINT                   │       0.0 │              4 │ 1                      │ 7                      │ false     │              0 │ NULL            │
│ signed_up_at │ TIMESTAMP WITH TIME ZONE │       0.0 │            100 │ 2026-01-01 07:00:00+00 │ 2026-01-30 04:00:00+00 │ true      │           NULL │ NULL            │
└──────────────┴──────────────────────────┴───────────┴────────────────┴────────────────────────┴────────────────────────┴───────────┴────────────────┴─────────────────┘
```

Selected columns, unnest `top_values`:

```sql
SELECT column_name, tv.value, tv.count
FROM profile_table('customers', ['plan', 'seats']), unnest(top_values) AS u(tv)
ORDER BY column_name, tv.count DESC, tv.value;
```

Full scan versus sample (`row_count` is the scanned rows):

```sql
SELECT column_name, row_count, is_sampled, actual_sample_size
FROM profile_table('customers', ['monthly_fee'], 50);          -- 50 rows, is_sampled = true

SELECT column_name, row_count, is_sampled, actual_sample_size
FROM profile_table('customers', ['monthly_fee'], 50, true);    -- 100 rows, is_sampled = false

SELECT count(*) FROM profile_table('customers', NULL, NULL, true);  -- all columns, exact
```

NaN / Inf are counted, not used in the statistics:

```sql
CREATE TABLE sensor_readings AS
SELECT * FROM (VALUES (21.5::DOUBLE), ('nan'), (22.0), ('inf'), (19.5)) t(temperature);

SELECT column_name, round(mean, 2) AS mean, nan_count, inf_count, min_val, max_val
FROM profile_table('sensor_readings');
```

```text
┌─────────────┬────────┬───────────┬───────────┬─────────┬─────────┐
│ column_name │  mean  │ nan_count │ inf_count │ min_val │ max_val │
├─────────────┼────────┼───────────┼───────────┼─────────┼─────────┤
│ temperature │   21.0 │         1 │         1 │ 19.5    │ 22.0    │
└─────────────┴────────┴───────────┴───────────┴─────────┴─────────┘
```

TIMESTAMPTZ values are shown in UTC whatever the session time zone:

```sql
SET TimeZone = 'America/New_York';
SELECT min_val, max_val FROM profile_table('customers', ['signed_up_at']);
-- 2026-01-01 07:00:00+00 | 2026-01-30 04:00:00+00
SET TimeZone = 'UTC';
```

## `profile_summary` — one row per table

```text
profile_summary(table_name VARCHAR) -> TABLE(12 columns, one row)
```

Only `table_name` (table or view, not TEMP). Always scans the whole table (no sampling).

| Column | Type | Notes |
|---|---|---|
| `row_count`, `column_count` | `BIGINT` | |
| `numeric_columns`, `string_columns`, `temporal_columns`, `boolean_columns`, `complex_columns`, `other_columns` | `BIGINT` | string = VARCHAR, BLOB; temporal = date/time/INTERVAL; complex = LIST/MAP/STRUCT/UNION; other = UUID, BIT, ENUM, JSON, ...; the six add up to `column_count` |
| `total_nulls` | `BIGINT` | NULL cells over all columns |
| `total_null_rate` | `DOUBLE` | `total_nulls / (row_count * column_count)` |
| `duplicate_row_count` | `BIGINT` | Rows that duplicate another full row |
| `estimated_memory_bytes` | `BIGINT` | Rough: `row_count * column_count * 8` |

```sql
SELECT row_count, column_count, numeric_columns, string_columns, temporal_columns,
       total_nulls, total_null_rate, duplicate_row_count
FROM profile_summary('customers');
-- 100 | 6 | 3 | 2 | 1 | 12 | 0.02 | 0
```

## `profile_correlations` — one row per numeric column pair

```text
profile_correlations(table_name VARCHAR [, columns VARCHAR[]])
  -> TABLE(column_a VARCHAR, column_b VARCHAR, pearson DOUBLE, spearman DOUBLE, n BIGINT)
```

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `table_name` | `VARCHAR` | required | Table or view (not TEMP) |
| `columns` | `VARCHAR[]` | all numeric columns | Subset; non-numeric names are skipped silently; unknown name = error |

- Methods: Pearson and Spearman (rank), both always computed. No other methods, no p-values.
- Each unordered pair appears once, `column_a` before `column_b` in table order.
- Only numeric columns take part: string, date, timestamp, `INTERVAL` and `BOOLEAN` are skipped.
  Fewer than two numeric columns gives 0 rows (no error).
- `n` = rows where both values are finite. Coefficients are clamped to [-1, 1]. `NULL` when
  undefined: `n < 2`, or one column constant over the pairs. `n = 2` with varying values gives
  exactly `1.0` or `-1.0`.
- Integer / HUGEINT / DECIMAL columns are correlated exactly (values above 2^53 stay distinct).
- Pair count grows quadratically; on wide tables pass `columns`.

```sql
SELECT column_a, column_b, round(pearson, 3) AS pearson, round(spearman, 3) AS spearman, n
FROM profile_correlations('customers')
ORDER BY abs(pearson) DESC;

SELECT * FROM profile_correlations('customers', ['monthly_fee', 'seats', 'plan']);  -- plan skipped
```

Strongly correlated pairs (candidates for redundant columns or leakage):

```sql
SELECT column_a, column_b, pearson, spearman
FROM profile_correlations('customers')
WHERE abs(pearson) >= 0.9 OR abs(spearman) >= 0.9;
```

## Error cases

```sql
CREATE TEMP TABLE staging_tmp AS SELECT 1 AS a;
SELECT * FROM profile_summary('staging_tmp');
```

```text
Binder Error: TEMP tables are not supported by profile_summary; use a regular table (CREATE TABLE ... AS SELECT * FROM staging_tmp)
```

```sql
SELECT * FROM profile_table('customers', ['email_address']);
```

```text
Binder Error: profile_table: column 'email_address' not found in table 'customers' (available columns: customer_id, email, plan, monthly_fee, seats, signed_up_at)
```

The uncommitted-transaction error is shown at the end of this file.

## Settings

No profiling-specific settings. `SET anofox_tab_trace_level = 'debug'` shows the generated
profiling SQL and warnings (e.g. a skipped `mean` on overflow).

## Canonical workflow — profile, then codify checks

Profile, read the findings, then turn them into a checks table for `run_checks` (details of
check types, `params` and thresholds: skill `anofox-tabular-data-quality`).

| Profile finding | Check to derive |
|---|---|
| `null_rate` (observed) | `null_rate` with `upper_threshold` = observed rate + margin (0 for required columns) |
| `distinct_count <= 5` on a string column | `values_in_set` with `allowed_values` from `top_values` |
| `is_unique AND null_count = 0` | `duplicate_count` with `upper_threshold` 0 (key candidate) |
| `negative_count = 0` on a numeric column | `agg` `min` with `lower_threshold` 0 |
| `min_val` / `max_val` / `p25`, `p75` | `agg` min/max bounds, or the `iqr` / `zscore` functions |
| `pattern_summary = 'email'` | `regex_match`, or `email_is_valid` row-level |
| `duplicate_row_count > 0` (summary) | `duplicate_count` on the natural key |
| `row_count` (summary) | `volume` with a band around it |

```sql
-- 1. Overview and per-column profile
SELECT row_count, total_null_rate, duplicate_row_count FROM profile_summary('customers');

-- 2. Generate checks from the profile
CREATE TABLE dq_checks AS
SELECT column_name || '_null_rate' AS check_name, 'null_rate' AS check_type,
       'customers' AS table_name, column_name, NULL::VARCHAR AS params,
       NULL::DOUBLE AS lower_threshold, round(null_rate + 0.02, 2) AS upper_threshold
FROM profile_table('customers')
UNION ALL
SELECT column_name || '_in_set', 'values_in_set', 'customers', column_name,
       json_object('allowed_values', list_transform(top_values, lambda x: x.value))::VARCHAR,
       1.0, NULL
FROM profile_table('customers')
WHERE column_type = 'VARCHAR' AND distinct_count <= 5
UNION ALL
SELECT column_name || '_unique', 'duplicate_count', 'customers', column_name, NULL, NULL, 0
FROM profile_table('customers')
WHERE is_unique AND null_count = 0
UNION ALL
SELECT column_name || '_non_negative', 'agg', 'customers', column_name, '{"agg": "min"}', 0, NULL
FROM profile_table('customers')
WHERE negative_count IS NOT NULL;               -- every numeric column

-- 3. Run them: monthly_fee_non_negative fails (min -5.0), the rest pass
SELECT check_name, check_type, value, status
FROM run_checks('dq_checks')
ORDER BY status, check_name;
```

Thresholds derived from today's data encode today's problems: review the generated rows
(a non-negative rule that already fails, as here, is a finding, not a bad check) before you
schedule the suite.

## Common patterns

```sql
-- Columns worth a closer look
SELECT column_name, null_rate, distinct_count, is_constant, nan_count, inf_count
FROM profile_table('customers')
WHERE null_rate > 0.1 OR is_constant OR coalesce(nan_count, 0) + coalesce(inf_count, 0) > 0;

-- Key candidates
SELECT column_name FROM profile_table('customers') WHERE is_unique AND null_count = 0;

-- Low-cardinality columns (category / enum candidates) with their values
SELECT column_name, distinct_count, list_transform(top_values, lambda x: x.value) AS vals
FROM profile_table('customers')
WHERE distinct_count BETWEEN 2 AND 5;

-- Persist a profile snapshot for drift comparison across loads
CREATE TABLE profile_history AS
SELECT current_date AS profiled_on, * FROM profile_table('customers');
```

Profiling a TEMP table or a query result: materialise it as a regular table (or a view over
regular tables) first.

```sql
CREATE VIEW pro_customers AS SELECT * FROM customers WHERE plan = 'pro';
SELECT column_name, null_rate, round(mean, 2) AS mean FROM profile_table('pro_customers');
```

## Error: profiling inside a transaction with uncommitted changes

```sql
BEGIN;
INSERT INTO customers VALUES (101, 'c101@example.com', 'pro', 20.0, 1, TIMESTAMPTZ '2026-02-01 00:00:00+00');
SELECT * FROM profile_summary('customers');
ROLLBACK;
```

```text
Binder Error: profile_summary reads the last committed data, but this transaction has uncommitted changes; call it after COMMIT, or outside a transaction
```

---
name: anofox-tabular-data-quality
description: >
  Data quality checks for the anofox_tabular DuckDB extension: one-row checks (`volume`,
  `null_rate`, `distinct_count`, `schema_check`, `regex_match`, `values_in_set`, `agg_check`,
  `duplicate_count`, `occurrence`, `match_rate`, `compliance`, `freshness`, `zscore`, `iqr`),
  the time-aware checks (`rel_count_change`, `metric_anomaly_iqr`, `rolling_values_in_set`) with
  their `_events` companions, the table-driven suite runner `run_checks` and the integration
  contract (`anofox_contract_version`, output tags). Use when asserting row counts, NULL rates,
  value sets, keys, foreign keys, freshness or daily volumes in SQL, building a checks table,
  storing check history, or failing a nightly pipeline on bad data.
version: 2026.11.02
user-invocable: false
---

# Anofox Tabular — Data Quality Checks Cheat Sheet

**Extension:** `anofox_tabular` v2026.11.02 | **DuckDB:** v2.0.0 / v1.5.6 | **Naming:** short alias (`volume`) = canonical `anofox_tab_volume`

Table functions that assert properties of a table and return a `status` row, plus a suite runner
that executes many checks from a checks table. Load with `LOAD anofox_tabular;` (install:
`INSTALL anofox_tabular FROM community;`).

## Critical gotchas

- **Every check is a table function taking the table name as a string**: `FROM null_rate('orders', 'email', 0.05)`,
  never `null_rate(email)`. Column names are strings too, matched case-insensitively.
- **One row per call**, also for an empty table: columns `status` (`'pass'` / `'fail'`) and
  `message`, plus check-specific measures. Only the time-aware checks with
  `start_date`/`end_date` return one row per date. Direct checks never return `warn`/`error`;
  those come from `run_checks` (`monitor_only`, checks that cannot run).
- **Empty input passes for the rate/statistics checks** (`null_rate`, `regex_match`,
  `values_in_set`, `match_rate`, `agg_check`, `occurrence`, time-aware checks, ...), with a
  message such as `Table is empty (0 rows); null rate check passed trivially`. Exceptions:
  `volume` with `min_rows >= 1` and `distinct_count` with `min_distinct >= 1` fail on 0 rows,
  and `freshness` fails
  (`No timestamp values found ...`). In `run_checks`, `"fail_on_empty": true` makes an empty
  check fail.
- **Thresholds come back with a fixed type**, whatever literal you passed: `DOUBLE` for rates,
  multipliers and value bounds, `BIGINT` for count bounds (`min_rows`, `max_distinct`,
  `max_duplicates`, `occurrence` bounds). `null_rate(..., 0.015)` returns `threshold DOUBLE 0.015`,
  so results of several checks `UNION ALL` cleanly (select common columns).
- **Bound conventions**: `NULL` = unbounded where the parameter table says so; `agg_check`,
  `rel_count_change` and `run_checks` also accept `-inf` lower / `+inf` upper (returned as `NULL`).
  `NaN`, `+inf` lower, `-inf` upper, lower > upper, negative counts, rates outside 0..1 are binder
  errors. A `NULL` required argument is a binder error (`null_rate: max_null_rate must not be NULL`).
- **Positional arguments only**, except: `reference_date :=` (or positional), `start_date :=` /
  `end_date :=` (named only, both or neither, not with a non-NULL `reference_date`), and
  `fill_missing_days :=` (named only, `rel_count_change`). Any other `name :=` is
  `Invalid named parameter`. Passing `reference_date` both ways is an error.
- **Time-aware checks evaluate the latest date in the data by default** — a day that never
  arrived is never checked. Anchor a nightly check with `reference_date` (or
  `"reference_date": "${yesterday}"` in `run_checks`). For a backfill use
  `start_date := ..., end_date := ...` (one set-based query, one row per date) instead of one call
  per `reference_date`.
- **Column-list forms differ**: `schema_check`, `values_in_set`, `rolling_values_in_set` take
  `VARCHAR[]` (`['a', 'b']`); `match_rate` keys are comma-separated strings (`'id, region'`);
  `duplicate_count` takes either.
- **All checks read in your transaction** and accept TEMP tables, views and files (`'orders.csv'`).
  `run_checks` reads a checks *table* (TEMP too) in your transaction, but a checks *view* through a
  second connection: TEMP views are rejected, and a view inside a transaction with uncommitted
  changes is refused.
- **`run_checks` never raises for a bad check**: a missing table/column, unknown type, bad
  `params` key or value is an `error` row, and the other checks run. A gate must treat `error`
  like `fail`. `zscore`, `iqr` and the `_events` functions are not suite types; `agg_check` is
  type `agg` in a suite.
- **A suite row without `lower_threshold` and `upper_threshold` always passes** (monitor row,
  message `... no thresholds configured (monitor)`), except `schema_check` (defaults to
  `upper_threshold` 0) and `metric_anomaly_iqr` (IQR bounds). The direct `rel_count_change`
  defaults to -0.5/+0.5; the suite type does not.
- **Units**: direct `freshness` takes an `INTERVAL` and reports `age_seconds`; the suite
  `freshness` value and thresholds are seconds (`86400` = 1 day). All times are UTC
  (`TIMESTAMPTZ` read as its UTC instant / UTC date, session `TimeZone` ignored).
- **NaN/±Inf**: `zscore`/`iqr` ignore them (counted in the message); `agg_check` returns `NaN`
  and fails. `infinity` dates/timestamps (SCD2 `valid_to`) are ignored by `freshness` and the
  time-aware checks.
- **`compliance` expressions run as SQL** with your privileges; they must parse as exactly one
  expression (`;`-separated statements are a binder error). In `run_checks` a runtime error per
  row counts as non-compliant (TRY semantics); in the direct function it raises.

## Sample data (used by every example below)

```sql
CREATE TABLE orders AS
SELECT day * 10 + n + 1                                          AS order_id,
       DATE '2026-01-01' + day::INTEGER                          AS order_date,
       TIMESTAMP '2026-01-01 09:00:00' + to_days(day::INTEGER) + to_hours(n::INTEGER) AS created_at,
       1 + (day * 10 + n) % 12                                   AS customer_id,
       CASE WHEN day * 10 + n + 1 = 55 THEN 5000.0
            ELSE 20.0 + ((n * 7 + day * 3) % 80) END::DOUBLE     AS amount,
       CASE WHEN day = 29 AND n = 1 THEN 'refunded'
            ELSE ['completed', 'completed', 'pending', 'completed', 'failed'][n % 5 + 1] END AS status,
       CASE WHEN n = 9 THEN NULL
            ELSE 'c' || (1 + (day * 10 + n) % 12) || '@example.com' END AS email
FROM range(30) d(day), range(10) k(n)
WHERE n < CASE WHEN day = 29 THEN 2 ELSE 10 END;   -- 292 rows; last day (2026-01-30) has 2

CREATE TABLE customers AS
SELECT i AS id, 'Customer ' || i AS name FROM range(1, 12) t(i);  -- customer 12 missing
```

## One-row checks

All return `status VARCHAR` first and `message VARCHAR` last. Bounds marked † accept `NULL` = unbounded.

| Function | Signature (after `table_name VARCHAR`) | Output columns between `status` and `message` |
|---|---|---|
| `volume` | `min_rows BIGINT†, max_rows BIGINT†` | `row_count BIGINT, min_threshold BIGINT, max_threshold BIGINT` |
| `null_rate` | `column_name, max_null_rate DOUBLE` (0..1, required) | `null_count BIGINT, total_count BIGINT, null_rate DOUBLE, threshold DOUBLE` |
| `distinct_count` | `column_name, min_distinct BIGINT†, max_distinct BIGINT†` (non-NULL values) | `distinct_count BIGINT, min_threshold BIGINT, max_threshold BIGINT` |
| `schema_check` | `required_columns VARCHAR[]` | `missing_columns VARCHAR[]` |
| `regex_match` | `column_name, pattern VARCHAR (RE2), min_match_rate DOUBLE† [, max_match_rate DOUBLE = NULL]` | `match_rate DOUBLE, matched_count BIGINT, total_count BIGINT, min_threshold DOUBLE, max_threshold DOUBLE` |
| `values_in_set` | `column_name, allowed_values VARCHAR[] [, min_rate DOUBLE = 1.0]` | `in_set_rate DOUBLE, in_set_count BIGINT, total_count BIGINT, threshold DOUBLE, sample_violations VARCHAR[]` |
| `agg_check` | `column_name, agg VARCHAR, lower_threshold DOUBLE†, upper_threshold DOUBLE†` | `agg VARCHAR, value DOUBLE, lower_threshold DOUBLE, upper_threshold DOUBLE` |
| `duplicate_count` | `column_names VARCHAR \| VARCHAR[] [, max_duplicates BIGINT = 0]` | `duplicate_count BIGINT, total_count BIGINT, threshold BIGINT` |
| `occurrence` | `column_name, mode VARCHAR, lower_threshold BIGINT†, upper_threshold BIGINT†` | `mode VARCHAR, occurrence BIGINT, extreme_value VARCHAR, distinct_values BIGINT, lower_threshold BIGINT, upper_threshold BIGINT` |
| `compliance` | `expression VARCHAR [, min_rate DOUBLE = 1.0]` | `compliance_rate DOUBLE, compliant_count BIGINT, total_count BIGINT, threshold DOUBLE` |
| `freshness` | `timestamp_column, max_age INTERVAL [, reference_time TIMESTAMP\|TIMESTAMPTZ = now (UTC)]` | `metric_value TIMESTAMP, threshold TIMESTAMP, age_seconds BIGINT` |
| `zscore` | `column_name, threshold DOUBLE` (> 0, e.g. 3.0) | `mean DOUBLE, stddev DOUBLE, outlier_count BIGINT, total_count BIGINT, outlier_rate DOUBLE, threshold DOUBLE` |
| `iqr` | `column_name, iqr_multiplier DOUBLE` (e.g. 1.5) | `q1, q3, iqr, lower_bound, upper_bound DOUBLE, outlier_count BIGINT, total_count BIGINT, multiplier DOUBLE` |

`match_rate` takes two tables:

```text
match_rate(left_table VARCHAR, right_table VARCHAR, left_keys VARCHAR, right_keys VARCHAR [, min_rate DOUBLE = 1.0])
  -> TABLE(status VARCHAR, match_rate DOUBLE, matched_count BIGINT, total_count BIGINT, threshold DOUBLE, message VARCHAR)
```

Semantics worth knowing:

- `null_rate`: share of NULLs over all rows. `regex_match`, `values_in_set`, `occurrence`,
  `distinct_count`: NULLs are not counted. Values are compared as `VARCHAR` (cast first).
- `values_in_set.sample_violations`: up to 5 distinct out-of-set values, sorted.
- `agg_check.agg`: `avg`, `min`, `max`, `sum`, `median`, `stddev` (case-insensitive). The column
  must be numeric or `BOOLEAN`. `min`/`max`/`sum` of integer/`DECIMAL` columns compare exactly.
- `duplicate_count`: duplicates = rows beyond the first per key; NULL keys are equal to each
  other. The failing message lists up to 5 duplicated keys.
- `occurrence.mode`: `'max'` (most common value's frequency) or `'min'` (least common).
- `match_rate`: foreign-key check; right side deduplicated; left rows with a NULL key are
  excluded from `total_count` (message says how many). Failing message lists up to 5 orphan keys.
- `compliance`: rows where the expression is NULL count as non-compliant. Subqueries work.
- `freshness`: column may be `DATE`, `TIMESTAMP`, `TIMESTAMPTZ` or text holding timestamps
  (unparseable text is skipped and counted); a `BIGINT` epoch column is a binder error — wrap it
  in a view with `to_timestamp()`. A future newest timestamp passes with negative `age_seconds`.
- `zscore`/`iqr` are row-level outlier checks on numeric columns; not available in `run_checks`.

```sql
SELECT 'volume' AS chk, status, message FROM volume('orders', 100, 1000)
UNION ALL SELECT 'null_rate', status, message FROM null_rate('orders', 'email', 0.05)
UNION ALL SELECT 'distinct', status, message FROM distinct_count('orders', 'status', 1, 3)
UNION ALL SELECT 'schema', status, message FROM schema_check('orders', ['order_id', 'discount'])
UNION ALL SELECT 'regex', status, message FROM regex_match('orders', 'email', '^[^@]+@[^@]+$', 0.99)
UNION ALL SELECT 'in_set', status, message FROM values_in_set('orders', 'status', ['completed', 'pending', 'failed'])
UNION ALL SELECT 'median', status, message FROM agg_check('orders', 'amount', 'median', 10.0, 100.0)
UNION ALL SELECT 'dupes', status, message FROM duplicate_count('orders', ['customer_id', 'status'], 10)
UNION ALL SELECT 'occurrence', status, message FROM occurrence('orders', 'status', 'max', NULL, 150)
UNION ALL SELECT 'fk', status, message FROM match_rate('orders', 'customers', 'customer_id', 'id')
UNION ALL SELECT 'rule', status, message FROM compliance('orders', 'amount > 0 AND amount < 1000', 0.95)
UNION ALL SELECT 'fresh', status, message FROM freshness('orders', 'created_at', INTERVAL '1 day', TIMESTAMP '2026-01-31 08:00:00')
UNION ALL SELECT 'zscore', status, message FROM zscore('orders', 'amount', 3.0)
UNION ALL SELECT 'iqr', status, message FROM iqr('orders', 'amount', 1.5);
```

Threshold types are fixed, so this combines without casts:

```sql
SELECT 'null_rate' AS chk, threshold FROM null_rate('orders', 'email', 0.015)
UNION ALL
SELECT 'values_in_set', threshold FROM values_in_set('orders', 'status', ['completed'], 0.5);
```

A multi-statement expression is rejected at bind time:

```sql
SELECT * FROM compliance('orders', 'amount > 0; DROP TABLE orders');
```

```text
Binder Error: compliance: expression 'amount > 0; DROP TABLE orders' must be a single expression
```

## Time-aware checks

```text
rel_count_change(table_name, date_column [, count_column VARCHAR = NULL [, window_days BIGINT = 7
                 [, lower_threshold DOUBLE = -0.5 [, upper_threshold DOUBLE = 0.5 [, reference_date DATE = NULL]]]]]
                 [, start_date := DATE, end_date := DATE] [, fill_missing_days := BOOLEAN = false])
  -> TABLE(status, reference_date DATE, reference_count DOUBLE, baseline_avg DOUBLE, baseline_days BIGINT,
           rel_change DOUBLE, lower_threshold DOUBLE, upper_threshold DOUBLE, message)

metric_anomaly_iqr(table_name, date_column [, metric_column VARCHAR = NULL [, window_days BIGINT = 30
                   [, k DOUBLE = 1.5 [, mode VARCHAR = 'both' [, reference_date DATE = NULL]]]]]
                   [, start_date := DATE, end_date := DATE])
  -> TABLE(status, reference_date DATE, metric_value DOUBLE, q1 DOUBLE, q3 DOUBLE, lower_bound DOUBLE,
           upper_bound DOUBLE, baseline_days BIGINT, mode VARCHAR, message)

rolling_values_in_set(table_name, column_name, allowed_values VARCHAR[], date_column
                      [, window_days BIGINT = 7 [, min_rate DOUBLE = 1.0 [, reference_date DATE = NULL]]]
                      [, start_date := DATE, end_date := DATE])
  -> TABLE(status, reference_date DATE, in_set_rate DOUBLE, in_set_count BIGINT, total_count BIGINT,
           threshold DOUBLE, sample_violations VARCHAR[], message)
```

| Check | What it compares |
|---|---|
| `rel_count_change` | daily count on `reference_date` vs average of the `window_days` days strictly before; `rel_change = (count - baseline) / baseline`. `count_column` = `COUNT(DISTINCT col)` per day, else rows. Baseline = days *with rows* only unless `fill_missing_days := true` (missing calendar days count as 0). A reference day without rows counts 0. |
| `metric_anomaly_iqr` | daily metric (row count, or `AVG(metric_column)`) vs `Q1 - k*IQR .. Q3 + k*IQR` of the `window_days` days before. `mode`: `'both'`, `'upper'`, `'lower'`. Row-count mode: a day without rows is 0 and fails; with a `metric_column` it passes trivially. |
| `rolling_values_in_set` | `values_in_set` over the `window_days` days ending on (and including) `reference_date`. An empty window passes. |

Date column: `DATE`, `TIMESTAMP`, `TIMESTAMPTZ` (UTC date) or text holding dates (unparseable
rows skipped and counted). `window_days` 1..36500. Empty input, no baseline and a zero baseline
pass with a message saying so.

```sql
-- latest day (2026-01-30, 2 orders vs ~10) fails
SELECT status, reference_date, reference_count, baseline_avg, rel_change
FROM rel_count_change('orders', 'order_date');

-- anchor a specific day by name, keeping the other defaults
SELECT status, rel_change FROM rel_count_change('orders', 'order_date', reference_date := DATE '2026-01-29');

-- one row per day of an inclusive range
SELECT reference_date, status, rel_change
FROM rel_count_change('orders', 'order_date', start_date := DATE '2026-01-27', end_date := DATE '2026-01-30');

SELECT reference_date, status, metric_value, upper_bound
FROM metric_anomaly_iqr('orders', 'order_date', 'amount', 14, 1.5, 'upper',
                        start_date := DATE '2026-01-04', end_date := DATE '2026-01-07');

SELECT reference_date, status, in_set_rate, sample_violations
FROM rolling_values_in_set('orders', 'status', ['completed', 'pending', 'failed'], 'order_date', 7);
```

Only one end of the range is a binder error:

```sql
SELECT * FROM rel_count_change('orders', 'order_date', start_date := DATE '2026-01-27');
```

```text
Binder Error: rel_count_change: start_date and end_date must be given together
```

## `_events` companions (long-form failing dates)

`rel_count_change_events`, `metric_anomaly_iqr_events`, `rolling_values_in_set_events` take exactly
the arguments of their base check (incl. `reference_date`, `start_date`/`end_date`,
`fill_missing_days`) and return **one row per failing (or warning) date only**, ordered by `ds`.
Passing dates produce no rows, so a passing single-date call returns 0 rows.

```text
-> TABLE(ds DATE, kind VARCHAR, label VARCHAR, severity VARCHAR, value DOUBLE, threshold DOUBLE, series VARCHAR)
```

| Column | Meaning |
|---|---|
| `kind` | `rel_count_change` / `metric_anomaly_iqr` / `rolling_values_in_set` |
| `label` | the base check's `message` |
| `severity` | `'alarm'` (fail) or `'warning'` (warn) |
| `value` | `rel_change` / `metric_value` / `in_set_rate` |
| `threshold` | the bound that was crossed |
| `series` | table name, or `table.column` when a column is measured |

```sql
SELECT ds, kind, severity, value, threshold, series
FROM rel_count_change_events('orders', 'order_date', start_date := DATE '2026-01-20', end_date := DATE '2026-01-30')
UNION ALL
SELECT ds, kind, severity, value, threshold, series
FROM metric_anomaly_iqr_events('orders', 'order_date', 'amount', 14, 1.5, 'upper',
                               start_date := DATE '2026-01-04', end_date := DATE '2026-01-07')
UNION ALL
SELECT ds, kind, severity, round(value, 4), threshold, series
FROM rolling_values_in_set_events('orders', 'status', ['completed', 'pending', 'failed'], 'order_date', 7,
                                  start_date := DATE '2026-01-28', end_date := DATE '2026-01-30')
ORDER BY ds, kind;
```

## `run_checks` — table-driven suite

```text
run_checks(checks_table VARCHAR)
  -> TABLE(run_ts TIMESTAMP, check_name VARCHAR, check_type VARCHAR, table_name VARCHAR, column_name VARCHAR,
           identifier VARCHAR, value DOUBLE, lower_threshold DOUBLE, upper_threshold DOUBLE,
           status VARCHAR, message VARCHAR)
```

One row per check (per partition with `identifier_column`), ordered by `check_name`, then
`identifier` (NULL last). All rows of one call share `run_ts` (UTC). `checks_table` may be a
table (TEMP ok), a regular view, a qualified name, or a file (`'checks.csv'`, Parquet, JSON).

### Checks table columns

Required: `check_name`, `check_type`, `table_name`. Optional (missing = `NULL`,
`monitor_only` = `false`); extra columns are ignored.

| Column | Type | Meaning |
|---|---|---|
| `check_name` | `VARCHAR` | unique label; duplicates or NULL/empty are `error` rows |
| `check_type` | `VARCHAR` | suite type (case/space-insensitive) |
| `table_name` | `VARCHAR` | checked table or view (TEMP ok) |
| `column_name` | `VARCHAR` | checked column; key list (`'a, b'` or `'["a","b"]'`) for `duplicate_count`/`match_rate` |
| `params` | `VARCHAR` | JSON object of type-specific keys |
| `lower_threshold` / `upper_threshold` | `DOUBLE` | fail when `value` is outside; `NULL`/`±inf` = unbounded |
| `monitor_only` | `BOOLEAN` | `true` records `warn` instead of `fail` |
| `identifier_column` | `VARCHAR` | one row per distinct value (NULLs form one partition) |
| `filter_expr` | `VARCHAR` | SQL pre-filter; date tokens `${today}`, `${yesterday}`, `${today-N}`, `${yesterday-N}` (UTC) |

### Suite types (15) and their `value`

| `check_type` | `value` | `params` keys (plus `fail_on_empty` everywhere except `schema_check`) | `identifier_column` |
|---|---|---|---|
| `volume` | row count | — | yes |
| `null_rate` | NULL share | — | yes |
| `distinct_count` | distinct values | — | yes |
| `regex_match` | match rate | `pattern` (req.) | yes |
| `values_in_set` | share in set; set `lower_threshold` (e.g. 1.0) | `allowed_values` (JSON array) | yes |
| `agg` | the aggregate | `agg` (req.): avg/min/max/sum/median/stddev | yes |
| `duplicate_count` | duplicates | — (key in `column_name`) | yes |
| `compliance` | compliance rate | `expression` (req.) | yes |
| `freshness` | age in **seconds** | `reference_time` (default now UTC; accepts tokens) | yes |
| `occurrence` | extreme frequency | `mode`: `max` (default) / `min` | no |
| `match_rate` | match rate | `right_table`, `right_keys` (req.), `left_keys` (default `column_name`) | no |
| `rel_count_change` | relative change; **set `lower_threshold`/`upper_threshold` (e.g. -0.5/0.5)**, without them the row only monitors and a missing day passes | `date_column` (req.), `count_column`, `window_days` (7), `reference_date`, `fill_missing_days` | no |
| `metric_anomaly_iqr` | daily metric; IQR bounds fill the threshold columns | `date_column` (req.), `metric_column`, `window_days` (30), `k` (1.5), `reference_date` (no `mode`) | no |
| `rolling_values_in_set` | share in set; set `lower_threshold` (e.g. 1.0) or it never fails | `date_column` (req.), `allowed_values`, `window_days` (7), `reference_date` | no |
| `schema_check` | missing column count; default `upper_threshold` 0 | `columns` (req.): JSON array or CSV string; no `column_name`/`filter_expr`/`identifier_column` | no |

Unknown `params` keys (typos like `refernce_date`), malformed dates (`reference_date` must be
`YYYY-MM-DD` or a token) and unknown tokens are `error` rows naming the problem.

### Status values

| `status` | Meaning |
|---|---|
| `pass` | within thresholds; or no thresholds (monitor row); or `value` NULL / nothing to evaluate (unless `fail_on_empty`) |
| `fail` | outside a threshold; `fail_on_empty` with no rows; `freshness` with no timestamps; `NaN` value |
| `warn` | as `fail`, with `monitor_only = true` |
| `error` | the check could not run: `value` NULL, message `check '<name>' (<type>) cannot run: <cause>`; other checks still run |

```sql
CREATE TABLE dq_checks (
    check_name VARCHAR, check_type VARCHAR, table_name VARCHAR, column_name VARCHAR,
    params VARCHAR, lower_threshold DOUBLE, upper_threshold DOUBLE,
    monitor_only BOOLEAN, identifier_column VARCHAR, filter_expr VARCHAR);

INSERT INTO dq_checks VALUES
    ('orders_volume',  'volume',        'orders', NULL,          NULL, 100, NULL, false, NULL, NULL),
    ('email_nulls',    'null_rate',     'orders', 'email',       NULL, NULL, 0.05, true,  NULL, NULL),
    ('status_allowed', 'values_in_set', 'orders', 'status',
     '{"allowed_values": ["completed", "pending", "failed"]}', 1.0, NULL, false, NULL, NULL),
    ('customer_fk',    'match_rate',    'orders', 'customer_id',
     '{"right_table": "customers", "right_keys": "id"}', 1.0, NULL, false, NULL, NULL),
    ('fresh',          'freshness',     'orders', 'created_at',
     '{"reference_time": "2026-01-31 08:00:00"}', NULL, 86400, false, NULL, NULL),
    ('amount_sum',     'agg',           'orders', 'amount', '{"agg": "sum"}', 1000, NULL, false, NULL, NULL),
    ('daily_iqr',      'metric_anomaly_iqr', 'orders', NULL, '{"date_column": "order_date"}', NULL, NULL, false, NULL, NULL),
    ('required_cols',  'schema_check',  'orders', NULL, '{"columns": ["order_id", "email"]}', NULL, NULL, false, NULL, NULL),
    ('missing_table',  'volume',        'no_such_table', NULL,   NULL, 1, NULL, false, NULL, NULL),
    ('status_volume',  'volume',        'orders', NULL,          NULL, 50, NULL, false, 'status', NULL);

SELECT check_name, identifier, round(value, 4) AS value, lower_threshold, upper_threshold, status
FROM run_checks('dq_checks');
```

```text
┌────────────────┬────────────┬──────────┬─────────────────┬─────────────────┬─────────┐
│   check_name   │ identifier │  value   │ lower_threshold │ upper_threshold │ status  │
│    varchar     │  varchar   │  double  │     double      │     double      │ varchar │
├────────────────┼────────────┼──────────┼─────────────────┼─────────────────┼─────────┤
│ amount_sum     │ NULL       │  22113.0 │          1000.0 │            NULL │ pass    │
│ customer_fk    │ NULL       │   0.9178 │             1.0 │            NULL │ fail    │
│ daily_iqr      │ NULL       │      2.0 │            10.0 │            10.0 │ fail    │
│ email_nulls    │ NULL       │   0.0993 │            NULL │            0.05 │ warn    │
│ fresh          │ NULL       │  79200.0 │            NULL │         86400.0 │ pass    │
│ missing_table  │ NULL       │     NULL │            NULL │            NULL │ error   │
│ orders_volume  │ NULL       │    292.0 │           100.0 │            NULL │ pass    │
│ required_cols  │ NULL       │      0.0 │            NULL │             0.0 │ pass    │
│ status_allowed │ NULL       │   0.9966 │             1.0 │            NULL │ fail    │
│ status_volume  │ completed  │    175.0 │            50.0 │            NULL │ pass    │
│ status_volume  │ failed     │     58.0 │            50.0 │            NULL │ pass    │
│ status_volume  │ pending    │     58.0 │            50.0 │            NULL │ pass    │
│ status_volume  │ refunded   │      1.0 │            50.0 │            NULL │ fail    │
└────────────────┴────────────┴──────────┴─────────────────┴─────────────────┴─────────┘
```

Read the reasons from `message` (it names the violated bound and, for `values_in_set`,
`duplicate_count`, `match_rate`, up to 5 offending samples):

```sql
SELECT check_name, status, message FROM run_checks('dq_checks') WHERE status <> 'pass';
```

A minimal checks table (three required columns) works; unsupported types become `error` rows:

```sql
CREATE TABLE mini_checks (check_name VARCHAR, check_type VARCHAR, table_name VARCHAR);
INSERT INTO mini_checks VALUES ('rows', 'volume', 'orders'), ('outliers', 'zscore', 'orders');
SELECT check_name, status, message FROM run_checks('mini_checks');
-- rows: pass "volume value 292.0: no thresholds configured (monitor)"
-- outliers: error "... check type 'zscore' is not supported in run_checks (use the zscore() function)"
```

## Result history and pipeline gate

Run the suite once into a TEMP table, append it to the history, and gate on that run (a second
`run_checks` call would re-evaluate at a later `run_ts`). The script is safe to run every night:
`CREATE TABLE IF NOT EXISTS`, and the gate fails when no check ran (`run_checks` itself raises
on an empty checks table; the `count(*) = 0` branch catches a run that returned nothing). Treat
`error` like `fail`:

```sql
CREATE OR REPLACE TEMP TABLE dq_run AS SELECT * FROM run_checks('dq_checks');
CREATE TABLE IF NOT EXISTS dq_results AS SELECT * FROM dq_run LIMIT 0;
INSERT INTO dq_results SELECT * FROM dq_run;

SELECT CASE WHEN count(*) = 0 THEN error('dq gate failed: no checks ran')
            WHEN count(*) FILTER (WHERE status IN ('fail', 'error')) > 0
            THEN error('dq gate failed: ' ||
                       string_agg(check_name || coalesce('[' || identifier || ']', '') || '=' || status,
                                  ', ' ORDER BY check_name, identifier)
                           FILTER (WHERE status IN ('fail', 'error')))
            ELSE 'dq gate passed' END AS gate
FROM dq_run;
```

```text
Invalid Input Error: dq gate failed: customer_fk=fail, daily_iqr=fail, missing_table=error, status_allowed=fail, status_volume[refunded]=fail
```

Run the job with `duckdb -bail warehouse.duckdb < gate.sql` so the CLI stops at the gate with a
non-zero exit code. Watch a stored check value over time with
`metric_anomaly_iqr('dq_results_one_check', 'run_ts', 'value', 30, 1.5, 'both')` on a view that
keeps a single `check_name`.

## Integration contract

`anofox_contract_version()` (scalar, no alias, shared by all anofox extensions) returns the
contract version `'1'`. Producers carry `duckdb_functions().tags`: `run_checks` →
`{'anofox.output': 'check', 'anofox.contract': '1'}`; the three `_events` functions →
`'events'`. The one-row direct checks are not tagged: put them in a checks table to get the
`check` schema.

```sql
SELECT anofox_contract_version() AS contract_version;

SELECT DISTINCT function_name, tags['anofox.output'] AS output
FROM duckdb_functions()
WHERE tags['anofox.contract'] IS NOT NULL AND function_name NOT LIKE 'anofox_tab_%'
ORDER BY function_name;
```

## Canonical workflow — nightly suite with anchored dates

```sql
CREATE TABLE nightly_checks AS SELECT * FROM dq_checks LIMIT 0;
INSERT INTO nightly_checks VALUES
    -- expected day: anchor it, otherwise a missing day is never evaluated
    ('orders_arrived', 'rel_count_change', 'orders', NULL,
     '{"date_column": "order_date", "reference_date": "2026-01-31", "fill_missing_days": true}',
     -0.5, 0.5, false, NULL, NULL),
    ('orders_fresh', 'freshness', 'orders', 'created_at',
     '{"reference_time": "2026-02-01 06:00:00"}', NULL, 129600, false, NULL, NULL),
    ('order_id_unique', 'duplicate_count', 'orders', 'order_id', NULL, NULL, 0, false, NULL, NULL),
    ('recent_emails', 'null_rate', 'orders', 'email', '{"fail_on_empty": true}',
     NULL, 0.2, false, NULL, 'order_date >= DATE ''2026-01-25''');

CREATE OR REPLACE TEMP TABLE nightly_run AS SELECT * FROM run_checks('nightly_checks');
CREATE TABLE IF NOT EXISTS nightly_results AS SELECT * FROM nightly_run LIMIT 0;
INSERT INTO nightly_results SELECT * FROM nightly_run;

SELECT check_name, value, status, message FROM nightly_run ORDER BY check_name;
```

In production replace the fixed dates with tokens so the table needs no daily `UPDATE`:
`"reference_date": "${yesterday}"`, `"reference_time": "${today}"`, `filter_expr`
`order_date >= ${today-7}`. Finish with the gate above.

## Common patterns

- **Expected partitions that sent nothing** have no row at all: `LEFT JOIN` a list of expected
  identifiers to `run_checks` results and treat a missing row as failing.
- **Exact sums of money ledgers**: `agg_check(..., 'sum', x, x)` compares `DECIMAL` sums exactly.
- **Foreign key with NULL references**: `match_rate` ignores NULL keys; add `null_rate` on the key.
- **Epoch-second timestamps**: `CREATE VIEW v AS SELECT to_timestamp(epoch_col) AS ts FROM t` before `freshness`.
- **Checks in version control**: `run_checks('nightly_checks.csv')` reads a CSV with the checks-table columns.
- **Type drift**: `schema_check` asserts required column names only. To catch retyped, added or
  removed columns, compare with a reference table or view using `diff_schema` (skill
  anofox-tabular-diff): `SELECT * FROM diff_schema('orders_expected', 'orders') WHERE drift <> 'same'`;
  for a gate wrap it in `CASE WHEN count(*) > 0 THEN error(...) END`.
- **Qualified/quoted names**: `'sales.orders'` (schema or attached DB); a table named with a dot is `'"orders.archive"'`.

# Data Quality Checks

> Check a table once, then run a whole suite of checks from a table and keep the results.

**Use this guide to:**
- Run single checks against a table
- Define checks as rows of a checks table and run them with `run_checks`
- Report a check as a warning instead of a failure, and split a check by a column
- Store the results of each run and watch them over time
- Check daily volumes over a date range and list the failing days as events
- Gate a nightly job: fail it when the expected load is missing

Every example creates its own data. Run the blocks in order in one DuckDB session with the
extension loaded (`LOAD anofox_tabular;`).

## The example data

An `orders` table with 14 days of orders from three regions. Each day has 30 orders, except
the last day, when the load failed and only 6 orders arrived. Region `south` sends a status
that is not on the allowed list, and some emails are missing.

```sql
CREATE TABLE orders AS
SELECT
    day * 100 + n                                    AS order_id,
    DATE '2026-03-01' + day::INTEGER                 AS order_date,
    ['north', 'south', 'west'][n % 3 + 1]            AS region,
    CASE WHEN n % 3 = 1 AND n % 7 = 0 THEN 'on_hold'
         ELSE ['paid', 'paid', 'shipped', 'cancelled'][n % 4 + 1] END AS status,
    (10 + (n * 13 + day * 7) % 90)::DOUBLE           AS amount,
    CASE WHEN n % 10 = 0 THEN NULL
         ELSE 'buyer' || n || '@example.com' END     AS email
FROM range(14) d(day), range(30) k(n)
WHERE n < CASE WHEN day = 13 THEN 6 ELSE 30 END;

SELECT order_date, count(*) AS orders
FROM orders GROUP BY order_date ORDER BY order_date DESC LIMIT 3;
```

```text
┌────────────┬────────┐
│ order_date │ orders │
│    date    │ int64  │
├────────────┼────────┤
│ 2026-03-14 │      6 │
│ 2026-03-13 │     30 │
│ 2026-03-12 │     30 │
└────────────┴────────┘
```

## 1. Run single checks

Each check is a table function. It takes the table name as a string and returns one row with a
`status` and a `message`.

```sql
SELECT status, row_count, message FROM volume('orders', 100, NULL);
```

```text
┌─────────┬───────────┬──────────────────────────────────────────┐
│ status  │ row_count │                 message                  │
│ varchar │   int64   │                 varchar                  │
├─────────┼───────────┼──────────────────────────────────────────┤
│ pass    │       396 │ Row count 396 is within acceptable range │
└─────────┴───────────┴──────────────────────────────────────────┘
```

```sql
SELECT status, round(null_rate, 3) AS null_rate, message
FROM null_rate('orders', 'email', 0.05);
```

```text
┌─────────┬───────────┬───────────────────────────────────────────────┐
│ status  │ null_rate │                    message                    │
│ varchar │  double   │                    varchar                    │
├─────────┼───────────┼───────────────────────────────────────────────┤
│ fail    │     0.101 │ Null rate 0.101 (40/396) exceeds maximum 0.05 │
└─────────┴───────────┴───────────────────────────────────────────────┘
```

`values_in_set` also returns a few of the values that broke the rule:

```sql
SELECT status, in_set_count, total_count, sample_violations
FROM values_in_set('orders', 'status', ['paid', 'shipped', 'cancelled']);
```

```text
┌─────────┬──────────────┬─────────────┬───────────────────┐
│ status  │ in_set_count │ total_count │ sample_violations │
│ varchar │    int64     │    int64    │     varchar[]     │
├─────────┼──────────────┼─────────────┼───────────────────┤
│ fail    │          370 │         396 │ [on_hold]         │
└─────────┴──────────────┴─────────────┴───────────────────┘
```

To see several checks side by side, combine them with `UNION ALL`. The threshold columns
have a fixed type (`DOUBLE` for rates and value bounds, `BIGINT` for counts), so they line up
whatever literals you pass:

```sql
SELECT 'volume' AS check_name, status FROM volume('orders', 100, NULL)
UNION ALL
SELECT 'email nulls', status FROM null_rate('orders', 'email', 0.05)
UNION ALL
SELECT 'unique ids', status FROM duplicate_count('orders', 'order_id')
UNION ALL
SELECT 'avg amount', status FROM agg_check('orders', 'amount', 'avg', 20, 80);
```

```text
┌─────────────┬─────────┐
│ check_name  │ status  │
│   varchar   │ varchar │
├─────────────┼─────────┤
│ volume      │ pass    │
│ email nulls │ fail    │
│ unique ids  │ pass    │
│ avg amount  │ pass    │
└─────────────┴─────────┘
```

## 2. Define a suite of checks

When the list of checks grows, keep it in a table. Each row is one check; `run_checks` runs
them all and returns the same columns for every check. The checks table must be a regular
table, not a TEMP table. `check_name`, `check_type` and `table_name` are required; the other
seven columns are optional and read as `NULL` when the table leaves them out.

```sql
CREATE TABLE dq_checks (
    check_name        VARCHAR,
    check_type        VARCHAR,
    table_name        VARCHAR,
    column_name       VARCHAR,
    params            VARCHAR,   -- JSON object with type-specific parameters
    lower_threshold   DOUBLE,
    upper_threshold   DOUBLE,
    monitor_only      BOOLEAN,   -- true: report 'warn' instead of 'fail'
    identifier_column VARCHAR,   -- one result row per value of this column
    filter_expr       VARCHAR    -- optional SQL filter on the checked table
);

INSERT INTO dq_checks VALUES
    ('orders_volume',  'volume',        'orders', NULL,     NULL,
     100, NULL, false, NULL, NULL),
    ('email_nulls',    'null_rate',     'orders', 'email',  NULL,
     NULL, 0.05, false, NULL, NULL),
    ('status_allowed', 'values_in_set', 'orders', 'status',
     '{"allowed_values": ["paid", "shipped", "cancelled"]}', 1.0, NULL, false, NULL, NULL),
    ('order_id_dupes', 'duplicate_count', 'orders', 'order_id', NULL,
     NULL, 0, false, NULL, NULL);

SELECT check_name, value, lower_threshold, upper_threshold, status
FROM run_checks('dq_checks')
ORDER BY check_name;
```

```text
┌────────────────┬─────────────────────┬─────────────────┬─────────────────┬─────────┐
│   check_name   │        value        │ lower_threshold │ upper_threshold │ status  │
│    varchar     │       double        │     double      │     double      │ varchar │
├────────────────┼─────────────────────┼─────────────────┼─────────────────┼─────────┤
│ email_nulls    │ 0.10101010101010101 │            NULL │            0.05 │ fail    │
│ order_id_dupes │                 0.0 │            NULL │             0.0 │ pass    │
│ orders_volume  │               396.0 │           100.0 │            NULL │ pass    │
│ status_allowed │  0.9343434343434344 │             1.0 │            NULL │ fail    │
└────────────────┴─────────────────────┴─────────────────┴─────────────────┴─────────┘
```

The `value` column holds the measure of each check type: the row count for `volume`, the
share of NULLs for `null_rate`, and so on. The thresholds come from the checks table. See
[Check types](../api/07-data-quality.md#check-types) for the full list.

## 3. Warnings and partitions

Missing emails are known and not urgent. Set `monitor_only` to report them as `warn`:

```sql
UPDATE dq_checks SET monitor_only = true WHERE check_name = 'email_nulls';

SELECT check_name, status FROM run_checks('dq_checks') ORDER BY check_name;
```

```text
┌────────────────┬─────────┐
│   check_name   │ status  │
│    varchar     │ varchar │
├────────────────┼─────────┤
│ email_nulls    │ warn    │
│ order_id_dupes │ pass    │
│ orders_volume  │ pass    │
│ status_allowed │ fail    │
└────────────────┴─────────┘
```

To find out which region sends the bad status, add a check with `identifier_column`. It
returns one row per region:

```sql
INSERT INTO dq_checks VALUES
    ('status_by_region', 'values_in_set', 'orders', 'status',
     '{"allowed_values": ["paid", "shipped", "cancelled"]}', 1.0, NULL, false, 'region', NULL);

SELECT identifier AS region, round(value, 3) AS in_set_rate, status
FROM run_checks('dq_checks')
WHERE check_name = 'status_by_region'
ORDER BY region;
```

```text
┌─────────┬─────────────┬─────────┐
│ region  │ in_set_rate │ status  │
│ varchar │   double    │ varchar │
├─────────┼─────────────┼─────────┤
│ north   │         1.0 │ pass    │
│ south   │       0.803 │ fail    │
│ west    │         1.0 │ pass    │
└─────────┴─────────────┴─────────┘
```

The message of a failing check names the violated bound. For `values_in_set`,
`duplicate_count` and `match_rate` it also shows up to 5 offending values, so you see the bad
status without a second query:

```sql
SELECT identifier AS region, message
FROM run_checks('dq_checks')
WHERE check_name = 'status_by_region' AND status = 'fail';
```

```text
┌─────────┬───────────────────────────────────────────────────────────────────────────────────────┐
│ region  │                                        message                                        │
│ varchar │                                        varchar                                        │
├─────────┼───────────────────────────────────────────────────────────────────────────────────────┤
│ south   │ values_in_set value 0.803 violates thresholds (0.803 < lower 1.0); samples: 'on_hold' │
└─────────┴───────────────────────────────────────────────────────────────────────────────────────┘
```

A check that cannot run does not stop the others. It returns a row with status `error`. This
also covers a misspelled `params` key (`refernce_date`) and a malformed date in `params`
(`"reference_date": "15.03.2026"`):

```sql
INSERT INTO dq_checks VALUES
    ('old_orders_volume', 'volume', 'orders_archive', NULL, NULL, 1, NULL, false, NULL, NULL);

SELECT check_name, status, message
FROM run_checks('dq_checks')
WHERE status = 'error';
```

```text
┌───────────────────┬─────────┬─────────────────────────────────────────────────────────────────────────────────────────────┐
│    check_name     │ status  │                                           message                                           │
│      varchar      │ varchar │                                           varchar                                           │
├───────────────────┼─────────┼─────────────────────────────────────────────────────────────────────────────────────────────┤
│ old_orders_volume │ error   │ check 'old_orders_volume' (volume) cannot run: target table 'orders_archive' does not exist │
└───────────────────┴─────────┴─────────────────────────────────────────────────────────────────────────────────────────────┘
```

```sql
DELETE FROM dq_checks WHERE check_name = 'old_orders_volume';
```

## 4. Keep the results

Store each run in a results table. `run_ts` records when the check ran. Create the table
with the result schema once, then insert after every run:

```sql
CREATE TABLE dq_results AS SELECT * FROM run_checks('dq_checks') LIMIT 0;
```

In production you would run one `INSERT INTO dq_results SELECT * FROM run_checks('dq_checks')`
per day. Here we simulate six daily runs against a `daily_load` table that holds one day of
orders. The `REPLACE` clause sets `run_ts` to the simulated day.

```sql
CREATE TABLE load_checks AS SELECT * FROM dq_checks LIMIT 0;
INSERT INTO load_checks VALUES
    ('load_volume', 'volume', 'daily_load', NULL, NULL, 20, NULL, false, NULL, NULL);

CREATE TABLE daily_load AS SELECT * FROM orders WHERE order_date = DATE '2026-03-09';
INSERT INTO dq_results SELECT * REPLACE (TIMESTAMP '2026-03-09 06:00' AS run_ts) FROM run_checks('load_checks');
CREATE OR REPLACE TABLE daily_load AS SELECT * FROM orders WHERE order_date = DATE '2026-03-10';
INSERT INTO dq_results SELECT * REPLACE (TIMESTAMP '2026-03-10 06:00' AS run_ts) FROM run_checks('load_checks');
CREATE OR REPLACE TABLE daily_load AS SELECT * FROM orders WHERE order_date = DATE '2026-03-11';
INSERT INTO dq_results SELECT * REPLACE (TIMESTAMP '2026-03-11 06:00' AS run_ts) FROM run_checks('load_checks');
CREATE OR REPLACE TABLE daily_load AS SELECT * FROM orders WHERE order_date = DATE '2026-03-12';
INSERT INTO dq_results SELECT * REPLACE (TIMESTAMP '2026-03-12 06:00' AS run_ts) FROM run_checks('load_checks');
CREATE OR REPLACE TABLE daily_load AS SELECT * FROM orders WHERE order_date = DATE '2026-03-13';
INSERT INTO dq_results SELECT * REPLACE (TIMESTAMP '2026-03-13 06:00' AS run_ts) FROM run_checks('load_checks');
CREATE OR REPLACE TABLE daily_load AS SELECT * FROM orders WHERE order_date = DATE '2026-03-14';
INSERT INTO dq_results SELECT * REPLACE (TIMESTAMP '2026-03-14 06:00' AS run_ts) FROM run_checks('load_checks');

SELECT run_ts, check_name, value, status FROM dq_results ORDER BY run_ts;
```

```text
┌─────────────────────┬─────────────┬────────┬─────────┐
│       run_ts        │ check_name  │ value  │ status  │
│      timestamp      │   varchar   │ double │ varchar │
├─────────────────────┼─────────────┼────────┼─────────┤
│ 2026-03-09 06:00:00 │ load_volume │   30.0 │ pass    │
│ 2026-03-10 06:00:00 │ load_volume │   30.0 │ pass    │
│ 2026-03-11 06:00:00 │ load_volume │   30.0 │ pass    │
│ 2026-03-12 06:00:00 │ load_volume │   30.0 │ pass    │
│ 2026-03-13 06:00:00 │ load_volume │   30.0 │ pass    │
│ 2026-03-14 06:00:00 │ load_volume │    6.0 │ fail    │
└─────────────────────┴─────────────┴────────┴─────────┘
```

The history is a normal table, so the time-aware checks work on it. Is the last run's value
unusual compared with the runs before it?

```sql
SELECT reference_date, metric_value, lower_bound, upper_bound, status
FROM metric_anomaly_iqr('dq_results', 'run_ts', 'value', 30, 1.5, 'both');
```

```text
┌────────────────┬──────────────┬─────────────┬─────────────┬─────────┐
│ reference_date │ metric_value │ lower_bound │ upper_bound │ status  │
│      date      │    double    │   double    │   double    │ varchar │
├────────────────┼──────────────┼─────────────┼─────────────┼─────────┤
│ 2026-03-14     │          6.0 │        30.0 │        30.0 │ fail    │
└────────────────┴──────────────┴─────────────┴─────────────┴─────────┘
```

With several checks in the history, run this on a view that keeps one `check_name`.

## 5. Check daily volumes over a date range

`rel_count_change` compares the number of rows on one day with the average of the days
before it. By default it checks the latest day in the data:

```sql
SELECT reference_date, reference_count, baseline_avg, rel_change, status
FROM rel_count_change('orders', 'order_date');
```

```text
┌────────────────┬─────────────────┬──────────────┬────────────┬─────────┐
│ reference_date │ reference_count │ baseline_avg │ rel_change │ status  │
│      date      │     double      │    double    │   double   │ varchar │
├────────────────┼─────────────────┼──────────────┼────────────┼─────────┤
│ 2026-03-14     │             6.0 │         30.0 │       -0.8 │ fail    │
└────────────────┴─────────────────┴──────────────┴────────────┴─────────┘
```

The days are calendar days in UTC. A `TIMESTAMP WITH TIME ZONE` column is bucketed by its UTC
date, whatever the session `TimeZone`; a plain `TIMESTAMP` or `DATE` column is used as it is.

The named parameters `start_date` and `end_date` check every day of a range and return one
row per day:

```sql
SELECT reference_date, reference_count, rel_change, status
FROM rel_count_change('orders', 'order_date',
                      start_date := DATE '2026-03-10', end_date := DATE '2026-03-14');
```

```text
┌────────────────┬─────────────────┬────────────┬─────────┐
│ reference_date │ reference_count │ rel_change │ status  │
│      date      │     double      │   double   │ varchar │
├────────────────┼─────────────────┼────────────┼─────────┤
│ 2026-03-10     │            30.0 │        0.0 │ pass    │
│ 2026-03-11     │            30.0 │        0.0 │ pass    │
│ 2026-03-12     │            30.0 │        0.0 │ pass    │
│ 2026-03-13     │            30.0 │        0.0 │ pass    │
│ 2026-03-14     │             6.0 │       -0.8 │ fail    │
└────────────────┴─────────────────┴────────────┴─────────┘
```

## 6. List the failing days as events

Each time-aware check has an `_events` companion with the same arguments. It returns only the
days that fail, in a fixed event shape (`ds`, `kind`, `label`, `severity`, `value`,
`threshold`, `series`). This is the input for an alarm timeline, for example in
anofox-visualization.

```sql
SELECT ds, kind, severity, value, threshold
FROM rel_count_change_events('orders', 'order_date',
                             start_date := DATE '2026-03-02', end_date := DATE '2026-03-14')
UNION ALL
SELECT ds, kind, severity, round(value, 3), threshold
FROM rolling_values_in_set_events('orders', 'status', ['paid', 'shipped', 'cancelled'], 'order_date', 7,
                                  start_date := DATE '2026-03-12', end_date := DATE '2026-03-14')
ORDER BY ds, kind;
```

```text
┌────────────┬───────────────────────┬──────────┬────────┬───────────┐
│     ds     │         kind          │ severity │ value  │ threshold │
│    date    │        varchar        │ varchar  │ double │  double   │
├────────────┼───────────────────────┼──────────┼────────┼───────────┤
│ 2026-03-12 │ rolling_values_in_set │ alarm    │  0.933 │       1.0 │
│ 2026-03-13 │ rolling_values_in_set │ alarm    │  0.933 │       1.0 │
│ 2026-03-14 │ rel_count_change      │ alarm    │   -0.8 │      -0.5 │
│ 2026-03-14 │ rolling_values_in_set │ alarm    │  0.935 │       1.0 │
└────────────┴───────────────────────┴──────────┴────────┴───────────┘
```

## 7. A nightly gate

A nightly job should fail when the expected load did not arrive, and it should fail loudly:
with a non-zero exit code, not only a `fail` row nobody reads.

Anchor the time-aware checks to the date you expect, not to the latest date in the data. By
default `rel_count_change` evaluates the latest day present, so a day that never arrived is
never checked. Here the job runs on the morning of 2026-03-16 and expects the orders of
2026-03-15, which are missing:

```sql
CREATE TABLE nightly_checks AS SELECT * FROM dq_checks LIMIT 0;
INSERT INTO nightly_checks VALUES
    ('orders_arrived', 'rel_count_change', 'orders', NULL,
     '{"date_column": "order_date", "reference_date": "2026-03-15"}', -0.5, 0.5, false, NULL, NULL),
    ('orders_fresh',   'freshness',        'orders', 'order_date',
     '{"reference_time": "2026-03-16 06:00:00"}', NULL, 129600, false, NULL, NULL),
    ('order_id_dupes', 'duplicate_count',  'orders', 'order_id',
     NULL, NULL, 0, false, NULL, NULL);

SELECT check_name, value, status, message FROM run_checks('nightly_checks');
```

```text
┌────────────────┬──────────┬─────────┬──────────────────────────────────────────────────────────────────────────────────────────────────┐
│   check_name   │  value   │ status  │                                             message                                              │
│    varchar     │  double  │ varchar │                                             varchar                                              │
├────────────────┼──────────┼─────────┼──────────────────────────────────────────────────────────────────────────────────────────────────┤
│ order_id_dupes │      0.0 │ pass    │ duplicate_count value 0.0 is within thresholds                                                   │
│ orders_arrived │     -1.0 │ fail    │ rel_count_change value -1.0 violates thresholds (-1.0 < lower -0.5)                              │
│ orders_fresh   │ 194400.0 │ fail    │ freshness value 194400.0 seconds violates thresholds (194400.0 seconds > upper 129600.0 seconds) │
└────────────────┴──────────┴─────────┴──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

`orders_arrived` compares the 0 orders of 2026-03-15 with the average of the 7 days before
(`value` -1.0). `orders_fresh` measures the age of the newest `order_date` (midnight of
2026-03-14) at 06:00 on 2026-03-16: 54 hours, more than the allowed 36 hours (129600 seconds).
In production, write the expected date into the checks table when the job starts (`json_object`
needs the `json` extension). `run_checks` works in UTC (`run_ts`, `${today}`, the default
`freshness` reference), so compute the date in UTC too; `make_timestamp(epoch_us(now()))` is
the current UTC time with or without the ICU extension:

```sql no-run
UPDATE nightly_checks
SET params = json_object('date_column', 'order_date',
                         'reference_date', CAST(make_timestamp(epoch_us(now())) AS DATE) - 1)
WHERE check_name = 'orders_arrived';
```

Then turn any `fail` or `error` into a failed statement with `error()`. The DuckDB CLI exits
with a non-zero code when a statement fails, so a scheduler (cron, Airflow, a CI job) sees the
failure:

```sql
SELECT CASE WHEN count(*) FILTER (WHERE status IN ('fail', 'error')) > 0
            THEN error('nightly gate failed: ' ||
                       string_agg(check_name || '=' || status, ', ' ORDER BY check_name)
                           FILTER (WHERE status IN ('fail', 'error')))
            ELSE 'nightly gate passed' END AS gate
FROM run_checks('nightly_checks');
```

```text
Invalid Input Error: nightly gate failed: orders_arrived=fail, orders_fresh=fail
```

Store the results first (section 4) and run the gate afterwards, so the history also has the
failing run. Keep in mind what the checks do not cover:

- `schema_check` compares column names only, not their types. A column whose type changed from
  `DOUBLE` to `VARCHAR` still passes; add a `compliance` or `agg` check on it if the type
  matters.
- A check with nothing to evaluate passes trivially. For a check whose `filter_expr` matches
  no row the message says so (`...; filter matched 0/399 rows`), but the status is `pass`; set
  `"fail_on_empty": true` in its `params` to make an empty table or an empty filter result
  fail. `rolling_values_in_set` also passes when only its window is empty, so pair it with a
  volume or `rel_count_change` check anchored to
  the expected date, as above.
- A check that cannot run (missing table or column, a misspelled `params` key, a malformed
  `reference_date`) is an `error` row, not a failed query; the gate above treats it like a
  `fail`.
- A `compliance` expression or `filter_expr` with a subquery or `random()` cannot be wrapped in
  `TRY()`. It runs as written, so a runtime error in it (a failing `CAST`) aborts the whole
  `run_checks` call instead of counting the row as non-compliant.

## Next steps

- [Data quality API reference](../api/07-data-quality.md): every check, its parameters and its columns
- [Anomaly detection guide](03-anomaly-detection.md): scores for single rows instead of table-level checks
- [Profiling](../api/10-profiling.md): column statistics to choose thresholds
- [Integration contract](../api/11-integration-contract.md): how `run_checks` and the `_events` functions feed other anofox tools

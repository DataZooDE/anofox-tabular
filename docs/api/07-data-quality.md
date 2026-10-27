# Data Quality Checks

> Assert row counts, null rates, value sets, aggregates, keys, freshness and daily baselines in SQL.

**Use this page to:**
- Look up the signature and output columns of each check
- Run a check once against a table or view
- Define many checks in a table and run them with `run_checks`
- Evaluate the time-aware checks over a date range and get their failing dates as events

The module has 17 checks, a table-driven suite runner (`run_checks`) and three `_events`
companions for the time-aware checks. Every check is a table function that takes the table
name as a string. Each call returns one summary row with a `status` (`'pass'` or `'fail'`) and a
`message`, also for an empty table. The three time-aware checks return one row per date when
you pass a date range.

All checks run locally. The thresholds you pass come back in the output with a fixed type,
whatever literal you passed: `DOUBLE` for rates, multipliers and value bounds, `BIGINT` for
count bounds (`min_rows`, `max_distinct`, `max_duplicates`, the `occurrence` bounds). So
`null_rate(..., 0.015)` returns `threshold` as `DOUBLE` `0.015`, not as `DECIMAL(4,3)`, and the
results of several checks combine with `UNION ALL`.

The arguments are checked when the query is bound:

- A `NULL` required argument is an error such as `null_rate: max_null_rate must not be NULL`,
  also for a prepared statement executed with a `NULL` parameter. Required are the table,
  column and key names, the lists (`allowed_values`, `required_columns`, `column_names`), the
  `pattern`, `expression`, `agg` and `mode` and the thresholds without a `NULL` meaning
  (`max_null_rate`, the `zscore` `threshold`, `iqr_multiplier`, `max_age`). Where a parameter
  table below gives `NULL` a meaning (`NULL` = unbounded, or a default such as `min_rate` `1.0`),
  `NULL` is accepted.
- The table and every named column must exist, else the error names them:
  `null_rate: table 'ordrs' does not exist`, or
  `null_rate: column 'emial' not found in table 'orders' (available columns: ...)`. Column names
  match case-insensitively, like DuckDB identifiers. An empty or blank name is an error such as
  `null_rate: table_name must not be empty`. The table may be a TEMP table, a view or a file
  (`'orders.csv'`); a file that cannot be read is `<function>: cannot read table '<name>': <cause>`.
- A lower bound greater than the upper bound is an error, for example
  `volume: min_rows (100) must not be greater than max_rows (10)`.
- The value bounds of `agg_check` and `rel_count_change` accept `-inf` as lower and `+inf` as
  upper bound, meaning no bound, like `NULL`. `NaN`, a `+inf` lower or a `-inf` upper bound is
  an error, for example
  `agg_check: upper_threshold must not be NaN (NULL or +inf mean no upper bound)`.
- Count bounds (`min_rows`, `max_distinct`, `iqr_multiplier`, ...) must not be negative, and a
  `zscore` threshold must be positive.
- Rates (`max_null_rate`, `min_match_rate`, `min_rate`, ...) must be between 0 and 1, for
  example `null_rate: max_null_rate must be between 0 and 1 (got 1.5)`.
- The statistical checks `zscore`, `iqr`, `agg_check` and the `metric_column` of
  `metric_anomaly_iqr` need a numeric (or `BOOLEAN`) column, for example
  `zscore: column 'supplier' must be numeric, got VARCHAR`.

Rates in the messages are rounded to three decimals (`Null rate 0.333 (2/6) ...`), and so are
the bounds in the `iqr` message (`1 outlier(s) detected outside bounds [-0.217, 5.75]`); the
result columns keep the full precision.

NaN and `±Inf` values in numeric columns do not break a check. The outlier checks `zscore` and
`iqr` ignore them: they leave them out of the statistics and of `total_count`, and the message
ends with `; <n> NaN/Inf value(s) ignored`. `agg_check`, and an `agg` check in `run_checks`,
assert one aggregate value instead: they report a `NaN` value and fail when the column contains
NaN or `±Inf` values, with the message
`<agg> value is NaN: the column contains <n> NaN/Inf value(s)`.

`infinity` and `-infinity` timestamps and dates (the open end of an SCD2 validity range) are
ignored by `freshness` and the time-aware checks, which say in the message how many rows they
ignored.

Timestamps are compared in UTC. A `TIMESTAMP WITH TIME ZONE` column is read as its UTC instant
(in `freshness`) and bucketed by its UTC date (in the time-aware checks), whatever the session
`TimeZone` and with or without the ICU extension. A plain `TIMESTAMP` column is taken as UTC as
it is. Text with a UTC offset (`'2026-10-29 04:00:00+02'`, `'2026-10-29T02:00:00Z'`) is the
instant it names; text without an offset is taken as UTC.

## Functions

| Function | Canonical name | Checks |
|----------|----------------|--------|
| [`volume`](#volume) | `anofox_tab_volume` | row count within bounds |
| [`null_rate`](#null_rate) | `anofox_tab_null_rate` | share of NULLs in a column at most a maximum |
| [`distinct_count`](#distinct_count) | `anofox_tab_distinct_count` | number of distinct values within bounds |
| [`schema_check`](#schema_check) | `anofox_tab_schema_check` | required columns exist |
| [`freshness`](#freshness) | `anofox_tab_freshness` | the newest timestamp is not older than a maximum age |
| [`zscore`](#zscore) | `anofox_tab_zscore` | no values beyond a z-score threshold |
| [`iqr`](#iqr) | `anofox_tab_iqr` | no values outside the IQR fences |
| [`regex_match`](#regex_match) | `anofox_tab_regex_match` | share of values matching a pattern within bounds |
| [`values_in_set`](#values_in_set) | `anofox_tab_values_in_set` | share of values in an allowed set at least a minimum |
| [`agg_check`](#agg_check) | `anofox_tab_agg_check` | `avg`/`min`/`max`/`sum`/`median`/`stddev` within bounds |
| [`duplicate_count`](#duplicate_count) | `anofox_tab_duplicate_count` | duplicate keys at most a maximum |
| [`occurrence`](#occurrence) | `anofox_tab_occurrence` | frequency of the most or least common value within bounds |
| [`match_rate`](#match_rate) | `anofox_tab_match_rate` | share of rows with a partner in another table (foreign key) |
| [`compliance`](#compliance) | `anofox_tab_compliance` | share of rows satisfying a SQL expression |
| [`rel_count_change`](#rel_count_change) | `anofox_tab_rel_count_change` | daily count against a rolling baseline |
| [`metric_anomaly_iqr`](#metric_anomaly_iqr) | `anofox_tab_metric_anomaly_iqr` | daily metric inside the IQR band of a trailing window |
| [`rolling_values_in_set`](#rolling_values_in_set) | `anofox_tab_rolling_values_in_set` | value-set membership over a trailing window of days |
| [`rel_count_change_events`](#events-companions) | `anofox_tab_rel_count_change_events` | failing dates of `rel_count_change` as events |
| [`metric_anomaly_iqr_events`](#events-companions) | `anofox_tab_metric_anomaly_iqr_events` | failing dates of `metric_anomaly_iqr` as events |
| [`rolling_values_in_set_events`](#events-companions) | `anofox_tab_rolling_values_in_set_events` | failing dates of `rolling_values_in_set` as events |
| [`run_checks`](#run_checks) | `anofox_tab_run_checks` | every check defined in a checks table |

## Example data

The examples on this page use these two tables. `orders` has 30 days of orders, ten per day
except the last day, which has two. One order has an amount of 5000, one status is
`refunded`, every tenth email is NULL, and customer 12 is missing from `customers`.

```sql
CREATE TABLE orders AS
SELECT
    day * 10 + n + 1                                         AS order_id,
    DATE '2026-01-01' + day::INTEGER                         AS order_date,
    TIMESTAMP '2026-01-01 09:00:00' + to_days(day::INTEGER) + to_hours(n::INTEGER) AS created_at,
    1 + (day * 10 + n) % 12                                  AS customer_id,
    CASE WHEN day * 10 + n + 1 = 55 THEN 5000.0
         ELSE 20.0 + ((n * 7 + day * 3) % 80) END::DOUBLE    AS amount,
    CASE WHEN day = 29 AND n = 1 THEN 'refunded'
         ELSE ['completed', 'completed', 'pending', 'completed', 'failed'][n % 5 + 1] END AS status,
    CASE WHEN n = 9 THEN NULL
         ELSE 'c' || (1 + (day * 10 + n) % 12) || '@example.com' END AS email
FROM range(30) d(day), range(10) k(n)
WHERE n < CASE WHEN day = 29 THEN 2 ELSE 10 END;

CREATE TABLE customers AS
SELECT i AS id, 'Customer ' || i AS name FROM range(1, 12) t(i);
```

---

## `volume`

Canonical name: `anofox_tab_volume`. Table function.

Passes when the row count is between `min_rows` and `max_rows`.

```text
volume(table_name VARCHAR, min_rows BIGINT, max_rows BIGINT)
  -> TABLE(status VARCHAR, row_count BIGINT, min_threshold BIGINT, max_threshold BIGINT, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `min_rows` | `BIGINT` | required | Lower bound; `NULL` = unbounded |
| `max_rows` | `BIGINT` | required | Upper bound; `NULL` = unbounded |

```sql
SELECT status, row_count, message FROM volume('orders', 100, 1000);
```

```text
┌─────────┬───────────┬──────────────────────────────────────────┐
│ status  │ row_count │                 message                  │
│ varchar │   int64   │                 varchar                  │
├─────────┼───────────┼──────────────────────────────────────────┤
│ pass    │       292 │ Row count 292 is within acceptable range │
└─────────┴───────────┴──────────────────────────────────────────┘
```

---

## `null_rate`

Canonical name: `anofox_tab_null_rate`. Table function.

Passes when the share of NULL values in a column is at most `max_null_rate`.

```text
null_rate(table_name VARCHAR, column_name VARCHAR, max_null_rate DOUBLE)
  -> TABLE(status VARCHAR, null_count BIGINT, total_count BIGINT, null_rate DOUBLE,
           threshold DOUBLE, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check |
| `max_null_rate` | `DOUBLE` | required | Highest allowed share of NULLs, 0.0 to 1.0; other values are a binder error |

```sql
SELECT status, null_count, total_count, round(null_rate, 4) AS null_rate
FROM null_rate('orders', 'email', 0.05);
```

```text
┌─────────┬────────────┬─────────────┬───────────┐
│ status  │ null_count │ total_count │ null_rate │
│ varchar │   int64    │    int64    │  double   │
├─────────┼────────────┼─────────────┼───────────┤
│ fail    │         29 │         292 │    0.0993 │
└─────────┴────────────┴─────────────┴───────────┘
```

---

## `distinct_count`

Canonical name: `anofox_tab_distinct_count`. Table function.

Passes when the number of distinct non-NULL values is between the bounds.

```text
distinct_count(table_name VARCHAR, column_name VARCHAR, min_distinct BIGINT, max_distinct BIGINT)
  -> TABLE(status VARCHAR, distinct_count BIGINT, min_threshold BIGINT, max_threshold BIGINT, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check |
| `min_distinct` | `BIGINT` | required | Lower bound; `NULL` = unbounded |
| `max_distinct` | `BIGINT` | required | Upper bound; `NULL` = unbounded |

```sql
SELECT status, distinct_count, message FROM distinct_count('orders', 'status', 1, 3);
```

```text
┌─────────┬────────────────┬────────────────────────────────────┐
│ status  │ distinct_count │              message               │
│ varchar │     int64      │              varchar               │
├─────────┼────────────────┼────────────────────────────────────┤
│ fail    │              4 │ Distinct count 4 exceeds maximum 3 │
└─────────┴────────────────┴────────────────────────────────────┘
```

---

## `schema_check`

Canonical name: `anofox_tab_schema_check`. Table function.

Passes when the table has every listed column.

```text
schema_check(table_name VARCHAR, required_columns VARCHAR[])
  -> TABLE(status VARCHAR, missing_columns VARCHAR[], message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `required_columns` | `VARCHAR[]` | required | Column names that must exist; matched case-insensitively, like DuckDB identifiers |

```sql
SELECT * FROM schema_check('orders', ['order_id', 'order_date', 'discount']);
```

```text
┌─────────┬─────────────────┬─────────────────────────────────────┐
│ status  │ missing_columns │               message               │
│ varchar │    varchar[]    │               varchar               │
├─────────┼─────────────────┼─────────────────────────────────────┤
│ fail    │ [discount]      │ Table orders is missing 1 column(s) │
└─────────┴─────────────────┴─────────────────────────────────────┘
```

`schema_check` cannot run inside a [`run_checks`](#run_checks) suite.

---

## `freshness`

Canonical name: `anofox_tab_freshness`. Table function.

Passes when the newest value of a timestamp column is not older than `max_age`, measured from
`reference_time`.

```text
freshness(table_name VARCHAR, timestamp_column VARCHAR, max_age INTERVAL
          [, reference_time TIMESTAMP | TIMESTAMP WITH TIME ZONE])
  -> TABLE(status VARCHAR, metric_value TIMESTAMP, threshold TIMESTAMP, age_seconds BIGINT, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `timestamp_column` | `VARCHAR` | required | A `DATE`, `TIMESTAMP` or `TIMESTAMP WITH TIME ZONE` column, or text holding timestamps |
| `max_age` | `INTERVAL` | required | Largest allowed age of the newest row, for example `INTERVAL '1 day'` |
| `reference_time` | `TIMESTAMP` or `TIMESTAMP WITH TIME ZONE` | current time (also for `NULL`) | The time the age is measured from. An instant such as `now()` is taken as its UTC time |

| Column | Type | Description |
|--------|------|-------------|
| `metric_value` | `TIMESTAMP` | The newest timestamp in the column |
| `threshold` | `TIMESTAMP` | `reference_time - max_age`: the cutoff the newest timestamp must not be older than |
| `age_seconds` | `BIGINT` | Seconds between the newest timestamp and `reference_time` |

```sql
SELECT status, metric_value, age_seconds
FROM freshness('orders', 'created_at', INTERVAL '1 day', TIMESTAMP '2026-01-31 08:00:00');
```

```text
┌─────────┬─────────────────────┬─────────────┐
│ status  │    metric_value     │ age_seconds │
│ varchar │      timestamp      │    int64    │
├─────────┼─────────────────────┼─────────────┤
│ pass    │ 2026-01-30 10:00:00 │       79200 │
└─────────┴─────────────────────┴─────────────┘
```

All times are UTC. Without `reference_time` the check compares against the current time in UTC,
also when the ICU extension is loaded and the session `TimeZone` is set. A
`TIMESTAMP WITH TIME ZONE` column is compared as its UTC instant, and `metric_value` is then the
newest value in UTC, the same in every session time zone. A plain `TIMESTAMP` column and
`reference_time` are taken as UTC as they are. A `DATE` column is compared as midnight of that
day. A text column with a UTC offset (`'2026-10-29 04:00:00+02'`) is read as the instant it
names, text without an offset as UTC. Text that is no timestamp (`'n/a'`) is skipped, and the
message says how many rows were:
`Data is fresh. Latest update: 2026-10-27 09:00:00 (age: 5400s). 2 row(s) skipped (seen_at_text is not a timestamp)`.
When no value parses, the check fails with `No timestamp values found` and that note. The check
does not need ICU.

`reference_time` may also be an instant, for example `now()` or `current_timestamp`
(`TIMESTAMP WITH TIME ZONE`): it is converted to its UTC time, so
`freshness('orders', 'created_at', INTERVAL '1 day', now())` equals the default, and
`TIMESTAMPTZ '2026-10-27 12:30:00+02'` measures from `2026-10-27 10:30:00` UTC.

Any other column type (a `BIGINT` of epoch seconds, a `TIME`, a `BOOLEAN`) is a binder error:
`freshness: timestamp_column 'seen_epoch' has type BIGINT; expected DATE, TIMESTAMP, TIMESTAMP WITH TIME ZONE or a VARCHAR holding timestamps`.
Convert epoch seconds in a view first, for example with `to_timestamp(seen_epoch)`.

A newest timestamp after `reference_time` (clock skew, or timestamps loaded in the wrong time
zone) still passes, but `age_seconds` is negative and the message warns about it:
`Data is fresh. Latest update: 2026-10-05 23:30:00 (age: -3600s). Warning: the latest timestamp
is 3600s after the reference time; check for clock skew or a time zone mismatch`.

An empty table, or a column that is all `NULL`, fails with `metric_value` and `age_seconds`
`NULL` and the message `No timestamp values found (table is empty or column is all NULL)`.

`infinity` and `-infinity` values are not update times and are ignored: the `valid_to` column
of a slowly changing dimension (SCD2), which is `infinity` for the current version of each
row, measures the newest closed version, and the message says how many rows were ignored:
`Data is fresh. Latest update: 2026-10-24 18:00:00 (age: 43200s). 5 row(s) with an infinite timestamp ignored`.
A column that holds only infinite values fails like an empty one. A `reference_time` of
`infinity` is an error: `freshness: reference_time must be a finite timestamp`.

Units: the direct function takes `max_age` as an `INTERVAL` and reports the age in seconds
(`age_seconds`, and `age: 90000s, max allowed: 86400s` in the message of a stale table). A
`freshness` check in [`run_checks`](#run_checks) has no `INTERVAL`: its `value` is the age in
seconds and its thresholds are seconds too (`86400` for one day); its messages say so
(`freshness value 79200.0 seconds is within thresholds`).

---

## `zscore`

Canonical name: `anofox_tab_zscore`. Table function.

Fails when any value lies more than `threshold` standard deviations from the mean. The method
assumes roughly normal data. For skewed data use [`iqr`](#iqr).

```text
zscore(table_name VARCHAR, column_name VARCHAR, threshold DOUBLE)
  -> TABLE(status VARCHAR, mean DOUBLE, stddev DOUBLE, outlier_count BIGINT, total_count BIGINT,
           outlier_rate DOUBLE, threshold DOUBLE, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Numeric column |
| `threshold` | `DOUBLE` | required | Z-score limit, typically `3.0`; must be positive |

The column must be numeric (or `BOOLEAN`, read as 0/1); a `VARCHAR` or `DATE` column is a
binder error. The mean, the standard deviation and the outliers use the finite values only. NaN
and `±Inf` values are left out (`total_count` does not count them) and the message ends with
`; <n> NaN/Inf value(s) ignored`.

Integer, `HUGEINT` and `DECIMAL` columns stay exact above 2^53: the values are centred on a
reference value in their own type (the midpoint of the minimum and maximum for integers, the
minimum for `DECIMAL`) before the cast to `DOUBLE`, so `9007199254740992` and `9007199254740993`
are two values, not a constant column. `mean` is reported as a `DOUBLE`. `FLOAT`, `DOUBLE` and
`UHUGEINT` columns are cast to `DOUBLE` directly.

```sql
SELECT status, outlier_count, total_count, message FROM zscore('orders', 'amount', 3.0);
```

```text
┌─────────┬───────────────┬─────────────┬────────────────────────────────────────────┐
│ status  │ outlier_count │ total_count │                  message                   │
│ varchar │     int64     │    int64    │                  varchar                   │
├─────────┼───────────────┼─────────────┼────────────────────────────────────────────┤
│ fail    │             1 │         292 │ 1 outlier(s) detected (0.342% of 292 rows) │
└─────────┴───────────────┴─────────────┴────────────────────────────────────────────┘
```

`zscore` cannot run inside a [`run_checks`](#run_checks) suite. For row-level scores see
[Anomaly detection](08-anomaly-detection.md).

---

## `iqr`

Canonical name: `anofox_tab_iqr`. Table function.

Fails when any value lies outside `[Q1 - k * IQR, Q3 + k * IQR]`, where `k` is `iqr_multiplier`.
This is a row-level outlier check; for a daily metric use
[`metric_anomaly_iqr`](#metric_anomaly_iqr).

```text
iqr(table_name VARCHAR, column_name VARCHAR, iqr_multiplier DOUBLE)
  -> TABLE(status VARCHAR, q1 DOUBLE, q3 DOUBLE, iqr DOUBLE, lower_bound DOUBLE, upper_bound DOUBLE,
           outlier_count BIGINT, total_count BIGINT, multiplier DOUBLE, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Numeric column |
| `iqr_multiplier` | `DOUBLE` | required | Fence multiplier `k`, typically `1.5` |

The quartiles and the outliers use the finite values only. NaN and `±Inf` values are left out
(`total_count` does not count them) and the message ends with `; <n> NaN/Inf value(s) ignored`,
as in [`zscore`](#zscore). The column must be numeric (or `BOOLEAN`). Integer, `HUGEINT` and
`DECIMAL` columns are centred in their own type before the cast to `DOUBLE`, as in
[`zscore`](#zscore), so the quartiles and fences compare exactly above 2^53; `iqr` is computed on
the exact differences, the reported quartiles and bounds are `DOUBLE`s.

```sql
SELECT status, lower_bound, upper_bound, outlier_count FROM iqr('orders', 'amount', 1.5);
```

```text
┌─────────┬─────────────┬─────────────┬───────────────┐
│ status  │ lower_bound │ upper_bound │ outlier_count │
│ varchar │   double    │   double    │     int64     │
├─────────┼─────────────┼─────────────┼───────────────┤
│ fail    │       -21.0 │       139.0 │             1 │
└─────────┴─────────────┴─────────────┴───────────────┘
```

The message names the bounds rounded to three decimals, for example
`1 outlier(s) detected outside bounds [-0.217, 5.75]`; `lower_bound` and `upper_bound` keep the
full precision.

`iqr` cannot run inside a [`run_checks`](#run_checks) suite.

---

## `regex_match`

Canonical name: `anofox_tab_regex_match`. Table function.

Passes when the share of non-NULL values that match a regular expression is within bounds.

```text
regex_match(table_name VARCHAR, column_name VARCHAR, pattern VARCHAR, min_match_rate DOUBLE
            [, max_match_rate DOUBLE])
  -> TABLE(status VARCHAR, match_rate DOUBLE, matched_count BIGINT, total_count BIGINT,
           min_threshold DOUBLE, max_threshold DOUBLE, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check; values are cast to `VARCHAR` |
| `pattern` | `VARCHAR` | required | RE2 pattern, as for `regexp_matches`; an invalid one is a binder error such as `regex_match: pattern '[a-z' is not a valid regular expression: missing ]: [a-z` |
| `min_match_rate` | `DOUBLE` | required | Lower bound; `NULL` = unbounded |
| `max_match_rate` | `DOUBLE` | `NULL` (unbounded) | Upper bound |

NULL values are not counted. An empty table or an all-NULL column passes with `total_count = 0`.

```sql
SELECT status, match_rate, matched_count, total_count
FROM regex_match('orders', 'email', '^[^@]+@[^@]+$', 0.99);
```

```text
┌─────────┬────────────┬───────────────┬─────────────┐
│ status  │ match_rate │ matched_count │ total_count │
│ varchar │   double   │     int64     │    int64    │
├─────────┼────────────┼───────────────┼─────────────┤
│ pass    │        1.0 │           263 │         263 │
└─────────┴────────────┴───────────────┴─────────────┘
```

---

## `values_in_set`

Canonical name: `anofox_tab_values_in_set`. Table function.

Passes when the share of non-NULL values found in `allowed_values` is at least `min_rate`.
Values are compared as strings.

```text
values_in_set(table_name VARCHAR, column_name VARCHAR, allowed_values VARCHAR[] [, min_rate DOUBLE])
  -> TABLE(status VARCHAR, in_set_rate DOUBLE, in_set_count BIGINT, total_count BIGINT,
           threshold DOUBLE, sample_violations VARCHAR[], message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check |
| `allowed_values` | `VARCHAR[]` | required | The allowed values |
| `min_rate` | `DOUBLE` | `1.0` (also for `NULL`) | Lowest allowed share of values in the set |

`sample_violations` holds up to 5 distinct values outside the set, sorted.

```sql
SELECT status, in_set_count, total_count, sample_violations
FROM values_in_set('orders', 'status', ['completed', 'pending', 'failed']);
```

```text
┌─────────┬──────────────┬─────────────┬───────────────────┐
│ status  │ in_set_count │ total_count │ sample_violations │
│ varchar │    int64     │    int64    │     varchar[]     │
├─────────┼──────────────┼─────────────┼───────────────────┤
│ fail    │          291 │         292 │ [refunded]        │
└─────────┴──────────────┴─────────────┴───────────────────┘
```

---

## `agg_check`

Canonical name: `anofox_tab_agg_check`. Table function.

Passes when an aggregate of a numeric column is within bounds.

```text
agg_check(table_name VARCHAR, column_name VARCHAR, agg VARCHAR, lower_threshold DOUBLE, upper_threshold DOUBLE)
  -> TABLE(status VARCHAR, agg VARCHAR, value DOUBLE, lower_threshold DOUBLE, upper_threshold DOUBLE,
           message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Numeric column |
| `agg` | `VARCHAR` | required | `avg`, `min`, `max`, `sum`, `median` or `stddev`; any other value is a binder error |
| `lower_threshold` | `DOUBLE` | required | Lower bound; `NULL` or `-inf` = unbounded |
| `upper_threshold` | `DOUBLE` | required | Upper bound; `NULL` or `+inf` = unbounded |

The column must be numeric (or `BOOLEAN`); another type is a binder error. An empty table or an
all-NULL column gives `value = NULL` and passes. When a `FLOAT` or `DOUBLE` column contains
NaN or `±Inf` values, `value` is `NaN` and the check fails with the message
`<agg> value is NaN: the column contains <n> NaN/Inf value(s)`; an average or a sum over such a
column has no meaningful value.

`min`, `max` and `sum` of an integer or `DECIMAL` column, and `median` of a `DECIMAL` column,
are computed and compared exactly, in the column's type: the sum of a `DECIMAL(10,2)` ledger
is `918624.95`, not the `918624.9499999994` a `DOUBLE` sum gives, so `lower_threshold =
upper_threshold = 918624.95` passes, and a `BIGINT` maximum of `9007199254740993` exceeds an
upper bound of `9007199254740992`. A threshold is compared as the decimal it is written as;
the message prints the exact value (`sum value 918624.95 is within acceptable range`). The
`value` column stays `DOUBLE`. `avg` and `stddev`, and every aggregate of a `FLOAT` or
`DOUBLE` column, are `DOUBLE`.

`sum` and `avg` add up the exact values. On a `HUGEINT` column or a `DECIMAL` column wider than
18 digits that sum can leave the type's range (three balances of `9e37` in a
`DECIMAL(38,0)` column); the check then raises
`agg_check: avg of DECIMAL(38,0) column 'balance_wei' overflows its exact sum` instead of
DuckDB's `Overflow in HUGEINT addition`. In [`run_checks`](#run_checks) such a check is an
`error` row instead, and the other checks of the suite still run.

A `-inf` lower or `+inf` upper threshold means no bound, like `NULL`, and is returned as
`NULL`. A `NaN` threshold, a `+inf` lower or a `-inf` upper threshold is a binder error, for
example `agg_check: upper_threshold must not be NaN (NULL or +inf mean no upper bound)`.

```sql
SELECT status, agg, round(value, 2) AS value, message
FROM agg_check('orders', 'amount', 'median', 10.0, 100.0);
```

```text
┌─────────┬─────────┬────────┬──────────────────────────────────────────────┐
│ status  │   agg   │ value  │                   message                    │
│ varchar │ varchar │ double │                   varchar                    │
├─────────┼─────────┼────────┼──────────────────────────────────────────────┤
│ pass    │ median  │   59.0 │ median value 59.0 is within acceptable range │
└─────────┴─────────┴────────┴──────────────────────────────────────────────┘
```

In a [`run_checks`](#run_checks) suite this check type is called `agg`.

---

## `duplicate_count`

Canonical name: `anofox_tab_duplicate_count`. Table function.

Passes when the number of duplicate keys is at most `max_duplicates`. Duplicates are the rows
beyond the first of each distinct key. NULL keys are duplicates of each other only: one row with
a NULL key is no duplicate, two such rows are one duplicate. A compound key compares its NULL
components as equal, so `('x', NULL)` twice is one duplicate and once is none.

```text
duplicate_count(table_name VARCHAR, column_names VARCHAR | VARCHAR[] [, max_duplicates BIGINT])
  -> TABLE(status VARCHAR, duplicate_count BIGINT, total_count BIGINT, threshold BIGINT, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_names` | `VARCHAR` or `VARCHAR[]` | required | One column, or a compound key as a comma-separated string (`'customer_id, status'`) or a list (`['customer_id', 'status']`) |
| `max_duplicates` | `BIGINT` | `0` (also for `NULL`) | Highest allowed number of duplicates |

The message of a failing check lists up to 5 duplicated keys, sorted, as
[`run_checks`](#run_checks) does: `2 duplicate(s) in 8 rows exceeds maximum 0; samples: 'T-5002', 'T-5004'`.
A compound key is shown as its row, `'(T-5002, C-1002)'`, and a NULL key as `'NULL'`.

```sql
SELECT 'order_id' AS key, status, duplicate_count FROM duplicate_count('orders', 'order_id')
UNION ALL
SELECT 'customer_id, status', status, duplicate_count FROM duplicate_count('orders', 'customer_id, status', 10);
```

```text
┌─────────────────────┬─────────┬─────────────────┐
│         key         │ status  │ duplicate_count │
│       varchar       │ varchar │      int64      │
├─────────────────────┼─────────┼─────────────────┤
│ order_id            │ pass    │               0 │
│ customer_id, status │ fail    │             255 │
└─────────────────────┴─────────┴─────────────────┘
```

---

## `occurrence`

Canonical name: `anofox_tab_occurrence`. Table function.

Passes when the frequency of the most common (`'max'`) or least common (`'min'`) value is within
bounds. NULL values are not counted.

```text
occurrence(table_name VARCHAR, column_name VARCHAR, mode VARCHAR, lower_threshold BIGINT, upper_threshold BIGINT)
  -> TABLE(status VARCHAR, mode VARCHAR, occurrence BIGINT, extreme_value VARCHAR,
           distinct_values BIGINT, lower_threshold BIGINT, upper_threshold BIGINT, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check |
| `mode` | `VARCHAR` | required | `'max'` or `'min'`; any other value is a binder error |
| `lower_threshold` | `BIGINT` | required | Lower bound; `NULL` = unbounded |
| `upper_threshold` | `BIGINT` | required | Upper bound; `NULL` = unbounded |

`extreme_value` is the value with that frequency. When several values share it, any one of
them is returned. An empty table or an all-NULL column gives
`occurrence = NULL` and passes.

```sql
SELECT status, mode, occurrence, extreme_value, distinct_values
FROM occurrence('orders', 'status', 'max', NULL, 150);
```

```text
┌─────────┬─────────┬────────────┬───────────────┬─────────────────┐
│ status  │  mode   │ occurrence │ extreme_value │ distinct_values │
│ varchar │ varchar │   int64    │    varchar    │      int64      │
├─────────┼─────────┼────────────┼───────────────┼─────────────────┤
│ fail    │ max     │        175 │ completed     │               4 │
└─────────┴─────────┴────────────┴───────────────┴─────────────────┘
```

---

## `match_rate`

Canonical name: `anofox_tab_match_rate`. Table function.

Passes when the share of left-table rows that have a partner in the right table is at least
`min_rate`. Use it as a foreign-key check. The right side is deduplicated on its keys, so
duplicate right rows do not raise the rate.

A left row with a NULL key column (for a key of several columns: any of them) matches nothing
and is left out of the rate: `total_count` counts the rows with a complete key only, and the
message ends with `; 2 row(s) with a NULL key excluded`. Check the NULL keys with
[`null_rate`](#null_rate). This matches SQL foreign keys, which accept a NULL reference.

```text
match_rate(left_table VARCHAR, right_table VARCHAR, left_keys VARCHAR, right_keys VARCHAR [, min_rate DOUBLE])
  -> TABLE(status VARCHAR, match_rate DOUBLE, matched_count BIGINT, total_count BIGINT, threshold DOUBLE,
           message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `left_table` | `VARCHAR` | required | The referencing table |
| `right_table` | `VARCHAR` | required | The referenced table |
| `left_keys` | `VARCHAR` | required | Comma-separated key columns of `left_table` |
| `right_keys` | `VARCHAR` | required | Comma-separated key columns of `right_table`, same number as `left_keys` |
| `min_rate` | `DOUBLE` | `1.0` (also for `NULL`) | Lowest allowed match rate |

The message of a failing check lists up to 5 left keys without a partner, sorted:
`Match rate 0.75 (6/8) is below minimum 0.9; samples: 'C-1009', 'C-2000'`.

An empty left table passes with `total_count = 0`, and so does a left table whose keys are all
NULL (message `No left rows with a non-NULL key (2 row(s) with a NULL key excluded); match rate
check passed trivially`).

```sql
SELECT status, round(match_rate, 4) AS match_rate, matched_count, total_count
FROM match_rate('orders', 'customers', 'customer_id', 'id');
```

```text
┌─────────┬────────────┬───────────────┬─────────────┐
│ status  │ match_rate │ matched_count │ total_count │
│ varchar │   double   │     int64     │    int64    │
├─────────┼────────────┼───────────────┼─────────────┤
│ fail    │     0.9178 │           268 │         292 │
└─────────┴────────────┴───────────────┴─────────────┘
```

---

## `compliance`

Canonical name: `anofox_tab_compliance`. Table function.

Passes when the share of rows for which a SQL boolean expression is true is at least
`min_rate`. Rows where the expression is NULL count as not compliant.

```text
compliance(table_name VARCHAR, expression VARCHAR [, min_rate DOUBLE])
  -> TABLE(status VARCHAR, compliance_rate DOUBLE, compliant_count BIGINT, total_count BIGINT,
           threshold DOUBLE, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `expression` | `VARCHAR` | required | A SQL boolean expression over the table's columns |
| `min_rate` | `DOUBLE` | `1.0` (also for `NULL`) | Lowest allowed share of compliant rows |

The expression is parsed on its own and must be exactly one SQL expression. A comment in it
(`amount > 0 -- refunds excluded`) is fine. Text that does not parse, such as an unterminated
`/*` or `false) OR (true`, is rejected (`compliance: failed to parse expression ...`), and so
is anything that parses as more than one expression: a second statement, a list
(`a > 0, b > 0`), a `FROM`, `WHERE` or `ORDER BY` clause, a `UNION` or an alias
(`compliance: expression '...' must be a single expression`). `CASE`, `IN` lists and
subqueries (`customer_id IN (SELECT id FROM customers)`) work.

> **Security note:** the expression runs as SQL with your privileges. At bind time it must parse
> as a single expression, so a second statement is rejected. Subqueries are still allowed. Pass
> only expressions you trust; the trust model is the same as running the SQL yourself.

```sql
SELECT status, compliance_rate, compliant_count, total_count
FROM compliance('orders', 'amount > 0 AND amount < 1000', 0.95);
```

```text
┌─────────┬────────────────────┬─────────────────┬─────────────┐
│ status  │  compliance_rate   │ compliant_count │ total_count │
│ varchar │       double       │      int64      │    int64    │
├─────────┼────────────────────┼─────────────────┼─────────────┤
│ pass    │ 0.9965753424657534 │             291 │         292 │
└─────────┴────────────────────┴─────────────────┴─────────────┘
```

```sql
SELECT * FROM compliance('orders', 'amount > 0; DROP TABLE orders');
```

```text
Binder Error: compliance: expression 'amount > 0; DROP TABLE orders' must be a single expression
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM compliance('orders', 'amount > 0; DROP TABLE orders');
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

---

## Time-aware checks

`rel_count_change`, `metric_anomaly_iqr` and `rolling_values_in_set` evaluate one reference
date. By default this is the latest date in the data; the optional `reference_date` argument
sets it, positionally or by name (`reference_date := DATE '2026-01-30'`, which leaves the
arguments before it at their defaults). Passing it both ways is an error.

The date column may be a `DATE`, a `TIMESTAMP` or a `TIMESTAMP WITH TIME ZONE` (bucketed by
its UTC date), or text holding dates (`'2026-10-21'`); a text value that is no date, such as
`'n/a'`, counts as no date and its row is left out, and the message ends with how many rows
were (`; 2 row(s) skipped (shipped_on is not a date)`). A text timestamp with a UTC offset
counts on its UTC date (`'2026-10-25 22:30:00-05'` on 2026-10-26); text without an offset is a
UTC date. Another column type is a binder error:
`rel_count_change: date_column 'event_day' has type BIGINT; expected DATE, TIMESTAMP, TIMESTAMP WITH TIME ZONE or a VARCHAR holding dates`.

An `infinity` or `-infinity` date (the `valid_to` of the current version in a slowly changing
dimension) is no day of data either: such rows are left out, so the default reference date is
the latest finite date, and the message ends with how many rows were, for example
`Count 6 on 2026-10-24 deviates by 2.0 from baseline average 2.0; 5 row(s) with an infinite date ignored`.
A `reference_date`, `start_date` or `end_date` of `infinity` is a binder error
(`rel_count_change: reference_date must be a finite date`). `window_days` must be between 1
and 36500 (100 years); a larger value is a binder error
(`rel_count_change: window_days must be at most 36500 (got 999999999999)`).

All three also accept the named parameters `start_date := DATE` and `end_date := DATE`. With
them, the check evaluates every day of the inclusive range and returns one row per date,
ordered by `reference_date`, with the same columns. Each row equals the single-date call for
that date. Rules:

- Pass both or neither, with `start_date <= end_date`.
- The range cannot be combined with a non-NULL `reference_date`.
- The `_events` companions accept the same named parameters.

Passing only one of the two is a binder error:

```sql
SELECT * FROM rel_count_change('orders', 'order_date', start_date := DATE '2026-01-27');
```

```text
Binder Error: rel_count_change: start_date and end_date must be given together
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM rel_count_change('orders', 'order_date', start_date := DATE...
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

## `rel_count_change`

Canonical name: `anofox_tab_rel_count_change`. Table function.

Compares the daily count on the reference date with the average of the `window_days` days
before it. Passes when the relative change `(count - baseline) / baseline` is between the
thresholds.

```text
rel_count_change(table_name VARCHAR, date_column VARCHAR
                 [, count_column VARCHAR [, window_days BIGINT [, lower_threshold DOUBLE
                 [, upper_threshold DOUBLE [, reference_date DATE]]]]]
                 [, start_date := DATE, end_date := DATE] [, fill_missing_days := BOOLEAN])
  -> TABLE(status VARCHAR, reference_date DATE, reference_count DOUBLE, baseline_avg DOUBLE,
           baseline_days BIGINT, rel_change DOUBLE, lower_threshold DOUBLE, upper_threshold DOUBLE,
           message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `date_column` | `VARCHAR` | required | Date or timestamp column; cast to `DATE` (a `TIMESTAMP WITH TIME ZONE` by its UTC date) |
| `count_column` | `VARCHAR` | `NULL` | Column counted with `COUNT(DISTINCT ...)` per day; `NULL` = daily row count |
| `window_days` | `BIGINT` | `7` | Baseline length: the days strictly before the reference date (1 to 36500) |
| `lower_threshold` | `DOUBLE` | `-0.5` | Lowest allowed relative change; `NULL` or `-inf` = unbounded |
| `upper_threshold` | `DOUBLE` | `0.5` | Highest allowed relative change; `NULL` or `+inf` = unbounded |
| `reference_date` | `DATE` | `NULL` = latest date in the data | The date to evaluate |
| `start_date`, `end_date` | `DATE`, named | none | Evaluate every day of this range |
| `fill_missing_days` | `BOOLEAN`, named | `false` | Count calendar days without rows as 0 in the baseline |

Empty input, no baseline days and a zero baseline pass with a message that says so.

By default the baseline is the average of the days *with rows* in the window, and
`baseline_days` counts them: an outage of several days does not lower the baseline, it only
shortens it. With `fill_missing_days := true` every calendar day of the window counts, a day
without rows as 0, from the first day with data on (the days before the data starts are no
history, not an outage). `baseline_days` is then the number of calendar days. A day of the
window that is missing makes the baseline smaller, so a normal day after an outage reads as a
jump. The reference day itself counts as 0 when it has no rows, with or without the option.

```sql
SELECT status, reference_date, reference_count, baseline_avg, rel_change
FROM rel_count_change('orders', 'order_date');
```

```text
┌─────────┬────────────────┬─────────────────┬──────────────┬────────────┐
│ status  │ reference_date │ reference_count │ baseline_avg │ rel_change │
│ varchar │      date      │     double      │    double    │   double   │
├─────────┼────────────────┼─────────────────┼──────────────┼────────────┤
│ fail    │ 2026-01-30     │             2.0 │         10.0 │       -0.8 │
└─────────┴────────────────┴─────────────────┴──────────────┴────────────┘
```

The same check for every day of a range:

```sql
SELECT reference_date, status, rel_change
FROM rel_count_change('orders', 'order_date',
                      start_date := DATE '2026-01-27', end_date := DATE '2026-01-30');
```

```text
┌────────────────┬─────────┬────────────┐
│ reference_date │ status  │ rel_change │
│      date      │ varchar │   double   │
├────────────────┼─────────┼────────────┤
│ 2026-01-27     │ pass    │        0.0 │
│ 2026-01-28     │ pass    │        0.0 │
│ 2026-01-29     │ pass    │        0.0 │
│ 2026-01-30     │ fail    │       -0.8 │
└────────────────┴─────────┴────────────┘
```

---

## `metric_anomaly_iqr`

Canonical name: `anofox_tab_metric_anomaly_iqr`. Table function.

Computes a daily metric (the row count, or the average of a column) and flags the reference
date when its value falls outside `Q1 - k * IQR` to `Q3 + k * IQR` of the `window_days` days
before it.

```text
metric_anomaly_iqr(table_name VARCHAR, date_column VARCHAR
                   [, metric_column VARCHAR [, window_days BIGINT [, k DOUBLE
                   [, mode VARCHAR [, reference_date DATE]]]]]
                   [, start_date := DATE, end_date := DATE])
  -> TABLE(status VARCHAR, reference_date DATE, metric_value DOUBLE, q1 DOUBLE, q3 DOUBLE,
           lower_bound DOUBLE, upper_bound DOUBLE, baseline_days BIGINT, mode VARCHAR, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `date_column` | `VARCHAR` | required | Date or timestamp column; cast to `DATE` (a `TIMESTAMP WITH TIME ZONE` by its UTC date) |
| `metric_column` | `VARCHAR` | `NULL` | Numeric column averaged per day; `NULL` = daily row count |
| `window_days` | `BIGINT` | `30` | Baseline length: the days strictly before the reference date |
| `k` | `DOUBLE` | `1.5` | IQR multiplier |
| `mode` | `VARCHAR` | `'both'` | `'both'`, `'upper'` or `'lower'`: which bound to enforce |
| `reference_date` | `DATE` | `NULL` = latest date in the data | The date to evaluate |
| `start_date`, `end_date` | `DATE`, named | none | Evaluate every day of this range |

In row-count mode (no `metric_column`) a reference date without rows has a metric of 0 rows,
as in [`rel_count_change`](#rel_count_change): when a nightly feed stops, the next days fail
(`Metric 0.0 on 2026-10-25 is outside IQR bounds [34.875, 47.875] (mode: both)`) and
`metric_anomaly_iqr_events` lists them. The baseline holds the days with rows only, as the
default of `rel_count_change`; days without rows before the reference date do not lower Q1. With
a `metric_column` a reference date without values has no metric: `metric_value` is `NULL` and
the check passes (`No metric value on 2026-10-25; check passed trivially`); in
[`run_checks`](#run_checks) `"fail_on_empty": true` makes it fail.

Empty input or an empty baseline window pass with a message that says so. A `metric_column`
must be numeric (or `BOOLEAN`). The numbers in the message (and in the `label` of
`metric_anomaly_iqr_events`) are rounded to three decimals, for example
`Metric 142.857 on 2026-09-11 is outside IQR bounds [30.708, 37.708] (mode: both)`; the
columns keep the full precision.

```sql
SELECT status, reference_date, metric_value, lower_bound, upper_bound, baseline_days
FROM metric_anomaly_iqr('orders', 'order_date');
```

```text
┌─────────┬────────────────┬──────────────┬─────────────┬─────────────┬───────────────┐
│ status  │ reference_date │ metric_value │ lower_bound │ upper_bound │ baseline_days │
│ varchar │      date      │    double    │   double    │   double    │     int64     │
├─────────┼────────────────┼──────────────┼─────────────┼─────────────┼───────────────┤
│ fail    │ 2026-01-30     │          2.0 │        10.0 │        10.0 │            29 │
└─────────┴────────────────┴──────────────┴─────────────┴─────────────┴───────────────┘
```

The daily average amount, enforcing only the upper bound:

```sql
SELECT reference_date, status, metric_value, upper_bound
FROM metric_anomaly_iqr('orders', 'order_date', 'amount', 14, 1.5, 'upper',
                        start_date := DATE '2026-01-04', end_date := DATE '2026-01-07');
```

```text
┌────────────────┬─────────┬──────────────┬─────────────┐
│ reference_date │ status  │ metric_value │ upper_bound │
│      date      │ varchar │    double    │   double    │
├────────────────┼─────────┼──────────────┼─────────────┤
│ 2026-01-04     │ pass    │         60.5 │        60.5 │
│ 2026-01-05     │ pass    │         63.5 │        65.0 │
│ 2026-01-06     │ fail    │        560.2 │        69.5 │
│ 2026-01-07     │ pass    │         61.5 │        74.0 │
└────────────────┴─────────┴──────────────┴─────────────┘
```

To watch the values of a check suite over time, point it at a persisted `run_checks` result
table, as shown in [Persisting results](#persisting-results).

---

## `rolling_values_in_set`

Canonical name: `anofox_tab_rolling_values_in_set`. Table function.

[`values_in_set`](#values_in_set) restricted to the `window_days` days that end on the
reference date (inclusive).

```text
rolling_values_in_set(table_name VARCHAR, column_name VARCHAR, allowed_values VARCHAR[], date_column VARCHAR
                      [, window_days BIGINT [, min_rate DOUBLE [, reference_date DATE]]]
                      [, start_date := DATE, end_date := DATE])
  -> TABLE(status VARCHAR, reference_date DATE, in_set_rate DOUBLE, in_set_count BIGINT, total_count BIGINT,
           threshold DOUBLE, sample_violations VARCHAR[], message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check |
| `allowed_values` | `VARCHAR[]` | required | The allowed values |
| `date_column` | `VARCHAR` | required | Date or timestamp column; cast to `DATE` (a `TIMESTAMP WITH TIME ZONE` by its UTC date) |
| `window_days` | `BIGINT` | `7` | Window length, ending on the reference date |
| `min_rate` | `DOUBLE` | `1.0` (also for `NULL`) | Lowest allowed share of values in the set |
| `reference_date` | `DATE` | `NULL` = latest date in the data | The last day of the window |
| `start_date`, `end_date` | `DATE`, named | none | Evaluate every day of this range |

```sql
SELECT reference_date, status, in_set_count, total_count, sample_violations
FROM rolling_values_in_set('orders', 'status', ['completed', 'pending', 'failed'], 'order_date', 7);
```

```text
┌────────────────┬─────────┬──────────────┬─────────────┬───────────────────┐
│ reference_date │ status  │ in_set_count │ total_count │ sample_violations │
│      date      │ varchar │    int64     │    int64    │     varchar[]     │
├────────────────┼─────────┼──────────────┼─────────────┼───────────────────┤
│ 2026-01-30     │ fail    │           61 │          62 │ [refunded]        │
└────────────────┴─────────┴──────────────┴─────────────┴───────────────────┘
```

---

## Events companions

`rel_count_change_events` (canonical `anofox_tab_rel_count_change_events`),
`metric_anomaly_iqr_events` (canonical `anofox_tab_metric_anomaly_iqr_events`) and
`rolling_values_in_set_events` (canonical `anofox_tab_rolling_values_in_set_events`) are table
functions. Each takes exactly the arguments of its base check, including `reference_date`,
the `start_date := DATE, end_date := DATE` range and, for `rel_count_change_events`,
`fill_missing_days`. It returns one row per reference date whose
check fails or warns, ordered by `ds`. Passing dates are not events, so a passing single-date
call returns no rows.

```text
rel_count_change_events(<arguments of rel_count_change>)
metric_anomaly_iqr_events(<arguments of metric_anomaly_iqr>)
rolling_values_in_set_events(<arguments of rolling_values_in_set>)
  -> TABLE(ds DATE, kind VARCHAR, label VARCHAR, severity VARCHAR, value DOUBLE,
           threshold DOUBLE, series VARCHAR)
```

| Column | Type | Description |
|--------|------|-------------|
| `ds` | `DATE` | The reference date |
| `kind` | `VARCHAR` | The check: `rel_count_change`, `metric_anomaly_iqr` or `rolling_values_in_set` |
| `label` | `VARCHAR` | The base check's `message` |
| `severity` | `VARCHAR` | `'alarm'` for `fail`, `'warning'` for `warn` |
| `value` | `DOUBLE` | The measure: `rel_change`, `metric_value` or `in_set_rate` |
| `threshold` | `DOUBLE` | The bound that was crossed: `lower_threshold`/`upper_threshold`, the IQR bound, or `min_rate` |
| `series` | `VARCHAR` | The table, or `table.column` when the check measures a column |

The output follows the `events` schema of the
[integration contract](11-integration-contract.md); the functions carry the tags
`{'anofox.output': 'events', 'anofox.contract': '1'}`.

```sql
SELECT ds, kind, severity, value, threshold, series
FROM rel_count_change_events('orders', 'order_date',
                             start_date := DATE '2026-01-20', end_date := DATE '2026-01-30');
```

```text
┌────────────┬──────────────────┬──────────┬────────┬───────────┬─────────┐
│     ds     │       kind       │ severity │ value  │ threshold │ series  │
│    date    │     varchar      │ varchar  │ double │  double   │ varchar │
├────────────┼──────────────────┼──────────┼────────┼───────────┼─────────┤
│ 2026-01-30 │ rel_count_change │ alarm    │   -0.8 │      -0.5 │ orders  │
└────────────┴──────────────────┴──────────┴────────┴───────────┴─────────┘
```

```sql
SELECT ds, kind, round(value, 4) AS value, threshold, series
FROM rolling_values_in_set_events('orders', 'status', ['completed', 'pending', 'failed'], 'order_date', 7,
                                  start_date := DATE '2026-01-28', end_date := DATE '2026-01-30');
```

```text
┌────────────┬───────────────────────┬────────┬───────────┬───────────────┐
│     ds     │         kind          │ value  │ threshold │    series     │
│    date    │        varchar        │ double │  double   │    varchar    │
├────────────┼───────────────────────┼────────┼───────────┼───────────────┤
│ 2026-01-30 │ rolling_values_in_set │ 0.9839 │       1.0 │ orders.status │
└────────────┴───────────────────────┴────────┴───────────┴───────────────┘
```

```sql
SELECT ds, kind, value, threshold, series
FROM metric_anomaly_iqr_events('orders', 'order_date', 'amount', 14, 1.5, 'upper',
                               start_date := DATE '2026-01-04', end_date := DATE '2026-01-07');
```

```text
┌────────────┬────────────────────┬────────┬───────────┬───────────────┐
│     ds     │        kind        │ value  │ threshold │    series     │
│    date    │      varchar       │ double │  double   │    varchar    │
├────────────┼────────────────────┼────────┼───────────┼───────────────┤
│ 2026-01-06 │ metric_anomaly_iqr │  560.2 │      69.5 │ orders.amount │
└────────────┴────────────────────┴────────┴───────────┴───────────────┘
```

---

## `run_checks`

Canonical name: `anofox_tab_run_checks`. Table function.

Runs every check defined in a checks table and returns one uniform row per check, and per
partition when the check has an `identifier_column`. The rows are ordered by `check_name`, then
by `identifier` (compared as text, the `NULL` partition last), the same on every run.
`run_checks` works in UTC; see [Time zone](#time-zone).

```text
run_checks(checks_table VARCHAR)
  -> TABLE(run_ts TIMESTAMP, check_name VARCHAR, check_type VARCHAR, table_name VARCHAR,
           column_name VARCHAR, identifier VARCHAR, value DOUBLE, lower_threshold DOUBLE,
           upper_threshold DOUBLE, status VARCHAR, message VARCHAR)
```

### The checks table

`run_checks` reads a checks table in the caller's transaction when the query is bound, like a
plain `SELECT` does: definitions inserted, updated or deleted inside `BEGIN ... COMMIT` are used
before the commit, and a TEMP table works. This holds for every DuckDB table, also one of an
attached DuckDB database. The checked tables may be TEMP tables or views. A table name can
be qualified (`'mig.orders'`); see [Table and column arguments](00-conventions.md#table-and-column-arguments).

A checks *view* (or a table of another database type, such as an attached Postgres database) is
read through a second connection, which sees only committed data. So is a file:
`run_checks('dq_checks.csv')` reads a CSV (or Parquet, JSON) file with the checks table's
columns, as `FROM 'dq_checks.csv'` does. A TEMP view is therefore the
error `run_checks: the checks view 'my_checks' is a TEMP view; ...`, and inside a transaction
with uncommitted changes the view is refused instead of silently showing the committed
definitions:
`run_checks: the checks view 'my_checks' is read outside the current transaction, which has uncommitted changes; commit them before running it, or pass the checks table instead of a view`.

Three columns are required; the other seven are optional, and a missing optional column reads
as `NULL` (`monitor_only` as `false`). Further columns, such as an owner or a description, are
ignored. A checks table without a required column is an error that names it, for example
`run_checks: checks table 'my_checks' is missing the required column check_type (required: check_name, check_type, table_name; optional: ...)`.

| Column | Type | Required | Description |
|--------|------|----------|-------------|
| `check_name` | `VARCHAR` | yes | Label of the check, unique in the table; results are ordered by it, then by `identifier` |
| `check_type` | `VARCHAR` | yes | One of the types below; case and surrounding spaces do not matter (`' Volume '` is `volume`) |
| `table_name` | `VARCHAR` | yes | The table or view to check |
| `column_name` | `VARCHAR` | no | The column; `NULL` where the type needs none. For the key of `duplicate_count` and `match_rate` a comma-separated list (`'order_id, customer_id'`) or a JSON array (`'["order_id", "customer_id"]'`) |
| `params` | `VARCHAR` | no | A JSON object with type-specific parameters, or `NULL` |
| `lower_threshold` | `DOUBLE` | no | Fail when `value` is below it; `NULL` or `-inf` = unbounded |
| `upper_threshold` | `DOUBLE` | no | Fail when `value` is above it; `NULL` or `+inf` = unbounded |
| `monitor_only` | `BOOLEAN` | no | `true` records `'warn'` instead of `'fail'` |
| `identifier_column` | `VARCHAR` | no | One result row per distinct value of this column; the rows where it is NULL form one more partition, reported with a NULL `identifier` |
| `filter_expr` | `VARCHAR` | no | SQL boolean pre-filter on the checked table |

### Check types

`run_checks` runs 14 check types. `zscore`, `iqr`, `schema_check` and the `_events` functions
are not available in a suite; such a row is an `error` row with the message
`check type 'zscore' is not supported in run_checks (use the zscore() function)`. The direct
`agg_check` is the type `agg` here (`check type 'agg_check' is called 'agg' in run_checks ...`).
The `value` column holds the measure of each type:

| `check_type` | `value` | `params` keys | `identifier_column` |
|--------------|---------|---------------|---------------------|
| `volume` | row count | none | yes |
| `null_rate` | share of NULLs | none | yes |
| `distinct_count` | distinct values | none | yes |
| `regex_match` | match rate | `pattern` (required) | yes |
| `values_in_set` | share in set | `allowed_values` (JSON array of strings) | yes |
| `agg` | the aggregate (`min`, `max`, `sum` of an integer or `DECIMAL` column and `median` of a `DECIMAL` column are compared exactly, as in [`agg_check`](#agg_check); a `sum` or `avg` that overflows its exact sum is an `error` row) | `agg` (required): `avg`, `min`, `max`, `sum`, `median`, `stddev` | yes |
| `duplicate_count` | duplicates | none (the key is `column_name`: a comma-separated list or a JSON array) | yes |
| `compliance` | compliance rate | `expression` (required) | yes |
| `freshness` | age in seconds (the thresholds are seconds too); `column_name` must be a date, timestamp or text column | `reference_time` (default: the current UTC time, no ICU needed) | yes |
| `occurrence` | frequency of the extreme value | `mode`: `max` (default) or `min` | no |
| `match_rate` | match rate over the rows with a non-NULL key | `right_table`, `right_keys` (required), `left_keys` (default `column_name`); the keys as a comma-separated string or a JSON array (`["customer_id", "region"]`) | no |
| `rel_count_change` | relative change; without `lower_threshold` and `upper_threshold` a monitor row that only records it (the direct function defaults to -0.5 and 0.5) | `date_column` (required), `count_column` (counted with `COUNT(DISTINCT ...)` per day; default: rows), `window_days` (7), `reference_date`, `fill_missing_days` (`false`) | no |
| `metric_anomaly_iqr` | daily metric: `AVG` of `metric_column`, else of `column_name`, else the row count (0 on a reference date without rows) | `date_column` (required), `metric_column`, `window_days` (30), `k` (1.5), `reference_date` | no |
| `rolling_values_in_set` | share in set | `date_column` (required), `allowed_values`, `window_days` (7), `reference_date` | no |

For `metric_anomaly_iqr` the IQR bounds replace the table's thresholds and appear in
`lower_threshold` and `upper_threshold`. The `mode` parameter of the direct function is not
available in a suite; both bounds apply. The metric column is `params.metric_column` or the
check's `column_name`; when both are set they must name the same column, otherwise the check
is an `error` row (`column_name 'amount' and params.metric_column 'order_id' name different
columns; set only one`). Without a metric column a reference date without rows is a `value` of
0 rows and fails against the bounds, as in the direct function. With a metric column such a
date has no value and passes trivially; with `"fail_on_empty": true` it fails with the message
`No metric value on 2026-10-25 (fail_on_empty)`.

`match_rate` leaves left rows with a NULL key out of the rate, like the direct
[`match_rate`](#match_rate), and its message says how many:
`match_rate value 0.833 violates thresholds (0.833 < lower 1.0); samples: '99'; 2 row(s) with a NULL key excluded`.
A `null_rate` check on the key column reports the NULL keys.

The `date_column` of `rel_count_change`, `metric_anomaly_iqr` and `rolling_values_in_set` must
be a `DATE`, `TIMESTAMP` or `TIMESTAMP WITH TIME ZONE` column, or text holding dates. A text
value that is not a date (`'n/a'`) does not stop the suite: its row is skipped and the message
says how many rows were, for example
`rel_count_change value -0.5 is within thresholds; 2 row(s) skipped (shipped_on is not a date)`.
An `infinity` or `-infinity` date (the `valid_to` of the current version in an SCD2 dimension)
is left out the same way, so the default reference date is the latest finite date, and the
message says how many rows were:
`rel_count_change value 2.0 violates thresholds (2.0 > upper 0.5); 5 row(s) with an infinite date ignored`.
A `freshness` check ignores infinite timestamps likewise
(`freshness value 43200.0 seconds is within thresholds; 5 row(s) with an infinite timestamp ignored`).
A `reference_date` or `reference_time` of `infinity` is an `error` row
(`params.reference_date must be a finite date`).
A column of another type (a `BIGINT` such as `20261021`) is an `error` row:
`params.date_column 'event_day' has type BIGINT; expected DATE, TIMESTAMP, TIMESTAMP WITH TIME ZONE or a VARCHAR holding dates`.
The direct functions raise the same binder error
(`rel_count_change: date_column 'event_day' has type BIGINT; ...`) and also count the skipped
text values in their message
(`No baseline days before 2026-10-27; check passed trivially; 2 row(s) skipped (seen_at_text is not a date)`).

The `column_name` of a `freshness` check follows the same rules. Text that is no timestamp is
skipped and counted
(`freshness value 5400.0 seconds is within thresholds; 2 row(s) skipped (seen_at_text is not a timestamp)`);
when no value parses the check fails with
`No timestamp values found; 2 row(s) skipped (last_seen is not a timestamp)`. Another column
type (epoch seconds as `BIGINT` or `DOUBLE`, a `TIME`, an `ENUM`, a `BOOLEAN`) is an `error`
row:
`check 'fresh_epoch' (freshness) cannot run: column_name 'seen_epoch' has type BIGINT; expected DATE, TIMESTAMP, TIMESTAMP WITH TIME ZONE or a VARCHAR holding timestamps`.

Every type also accepts the `params` key `fail_on_empty` (`true` or `false`, default `false`):
with `true` a check that has no rows to evaluate (an empty table, or a `filter_expr` that
matches no row) is `fail` (or `warn`) with a NULL `value` and the message
`No rows to evaluate: filter matched 0/399 rows (fail_on_empty)` or
`No rows to evaluate: table 'orders_staging' is empty (fail_on_empty)`, instead of passing
trivially.

`params` may only hold the keys listed for its type and `fail_on_empty`. Any other key, for
example a misspelled `refernce_date`, is an `error` row that lists the valid keys:
`unknown params key 'refernce_date' (valid keys for rel_count_change: date_column, count_column, window_days, reference_date, fill_missing_days, fail_on_empty)`.
Typed values are checked when the suite is built: `reference_date` must be a date
(`YYYY-MM-DD`) or a date token, `reference_time` a timestamp (`YYYY-MM-DD[ HH:MM[:SS]]`,
optionally with a UTC offset such as `+02:00`) or a date token, `window_days` a positive
integer up to 36500 (100 years), `k` a finite number and
`fill_missing_days` `true` or `false`. A malformed
value is an `error` row of that check that names the key, the value and the expected format,
for example `params.reference_date '21/10/2026' is not a valid date (expected YYYY-MM-DD)`; the
other checks run.

`filter_expr` accepts the date tokens `${today}`, `${yesterday}`, `${today+N}`, `${today-N}`,
`${yesterday+N}` and `${yesterday-N}` (UTC dates), for example `order_date >= ${today-7}`.
`params.reference_date` and `params.reference_time` accept one such token as their whole
value: `{"reference_date": "${yesterday}"}` evaluates the last complete day of every run, and
`{"reference_time": "${today}"}` is midnight UTC of the run's date. A nightly checks table then
needs no daily `UPDATE`. An unknown token is an `error` row:
`unknown token '${tomorrow}' in params.reference_date (supported: today, yesterday, today+N, today-N, yesterday-N)`.

The message of a check with a `filter_expr` ends with how many rows the filter kept, for
example `null_rate value 0.0 is within thresholds; filter matched 0/399 rows`: a nightly check
whose filter matches nothing (the load did not arrive) is visible as such. With an
`identifier_column` the counts are those of the row's partition; when the filter leaves no row
at all, the check still returns one row with a NULL `identifier` (`pass` with
`No value to evaluate; check passed trivially; filter matched 0/399 rows`, or `fail` with
`fail_on_empty`).

### Status

| `status` | Meaning |
|----------|---------|
| `pass` | `value` is within the thresholds; or the check has neither a `lower_threshold` nor an `upper_threshold` (a monitor row that only records the value; message `<check_type> value <value>: no thresholds configured (monitor)`); or `value` is NULL (nothing to evaluate; message `No value to evaluate; check passed trivially`), unless `fail_on_empty` is set |
| `fail` | `value` is outside a threshold; or `params.fail_on_empty` is `true` and there are no rows to evaluate. For `freshness` also a NULL `value` (empty table, all-NULL column or, with `identifier_column`, a group without timestamps), like the direct [`freshness`](#freshness) function; message `No timestamp values found (table is empty or column is all NULL)` |
| `warn` | as `fail`, for a check with `monitor_only = true` |
| `error` | the check could not run; `value` is NULL and the message says why. Also an `agg` check whose `sum` or `avg` overflows its exact sum (`check 'avg_balance' (agg) cannot run: avg of DECIMAL(38,0) column 'balance_wei' overflows its exact sum`), per partition with an `identifier_column` |

A `NaN` value (for example an `agg` average over a column with NaN values) is `fail` (or `warn`),
with the message `agg value is NaN: the column contains <n> NaN/Inf value(s)`.

A `-inf` `lower_threshold` or `+inf` `upper_threshold` means no bound, exactly like `NULL`
(the result row shows `NULL`). A `NaN` threshold, a `+inf` lower or a `-inf` upper threshold
is an `error` row:
`check 'orders_rows' (volume) cannot run: upper_threshold must not be NaN (NULL or +inf mean no upper bound)`.

The message of a `fail` or `warn` row names the violated bound, for example
`null_rate value 0.099 violates thresholds (0.099 > upper 0.05)` or
`values_in_set value 0.751 violates thresholds (0.751 < lower 0.95)`. For `values_in_set`,
`duplicate_count` and `match_rate` it also lists up to 5 offending values, the smallest first:
the values not in the set, the duplicated keys (`(a, b)` for a key of several columns) and the
left keys without a match, for example
`values_in_set value 0.75 violates thresholds (0.75 < lower 0.95); samples: 'SHIPPED', 'shiped'`.
With an `identifier_column` the samples are those of the row's partition.

Every check produces its row. A check that cannot be built or bound gets one `error` row, with
a NULL `identifier` also when it has an `identifier_column`, and the message
`check '<check_name>' (<check_type>) cannot run: <cause>`. The other checks run normally. Causes
are, for example:

- the target table does not exist: `target table 'nightly_returns' does not exist`
- a configured column (`column_name`, `identifier_column`, `date_column`, `count_column`,
  `metric_column`, `left_keys`) is not in the table:
  `column 'coupon_code' not found in table 'nightly_orders' (available columns: ...)`.
  Column names match case-insensitively.
- an `agg` check on a column that is not numeric
- an unknown `check_type` or one that exists only as a direct function, invalid `params` JSON, a missing required `params` key, a `params`
  key the type does not accept, or a malformed typed `params` value (see above)
- a NULL or empty `check_name` (the row's `check_name` is NULL or `''` as configured, and the
  message `check without check_name (volume) cannot run: every check needs a check_name`), or a
  `check_name` that several checks share: each of them is an `error` row
  (`check_name 'orders_volume' is used by 2 checks; check names must be unique`), since their
  results could not be told apart
- a `date_column`, or the `column_name` of a `freshness` check, that is not a date, timestamp or
  text column (see above)
- a `compliance` expression or `filter_expr` that does not parse, is not exactly one expression
  (as for [`compliance`](#compliance)) or references an unknown column
  (`params.expression: failed to parse expression 'amount >': syntax error at or near ">"`,
  `filter_expr: expression 'id > 0; DROP TABLE orders' must be a single expression`)
- a `regex_match` pattern that is not a valid regular expression
  (`params.pattern '[a-z' is not a valid regular expression: missing ]: [a-z`)
- a `match_rate` `right_table` that does not exist
- a `NaN` threshold, a `+inf` lower or a `-inf` upper threshold (see above)
- `lower_threshold` greater than `upper_threshold`, or a rate threshold outside 0 to 1 for
  `null_rate`, `regex_match`, `values_in_set`, `compliance`, `match_rate` and
  `rolling_values_in_set`
- `identifier_column` set for a type that does not support it

A user-supplied `compliance` expression or `filter_expr` that fails at runtime on some rows (for
example a `CAST` of free text such as `'n/a'`) does not stop the call: such rows count as not
compliant in a `compliance` check and are left out by `filter_expr`, as if the expression were
wrapped in `TRY()`. The message of a `compliance` check says how many rows its expression could
not evaluate, for example
`compliance value 0.98 is within thresholds; 8 row(s) could not be evaluated (counted as non-compliant)`.
Check the expression on its own when a rate looks too low. The standalone
[`compliance`](#compliance) function does not do this; there a failing expression is an error.

DuckDB cannot wrap an expression that contains a subquery or a volatile function such as
`random()` in `TRY()`. Such an expression is evaluated as it is, so a referential check
`{"expression": "customer_id IN (SELECT id FROM customers)"}` or a sampling filter
`random() < 0.1` works. A row on which such an expression fails at runtime is then not
tolerated: the error aborts the whole `run_checks` call, as it would in a plain query.
`compliance` expressions and `filter_expr` are validated like the
[`compliance`](#compliance) expression and run with your privileges. Messages print values
rounded to three decimals (an exact `agg` value and its thresholds in full:
`agg value 918624.95 is within thresholds`); the `value` column keeps the full precision.

### Time zone

`run_checks` works in UTC, with or without the ICU extension and whatever `SET TimeZone` says:

- `run_ts` is the current time in UTC.
- `${today}`, `${yesterday}` and their `+N`/`-N` forms are UTC dates. Compared with a
  `TIMESTAMP WITH TIME ZONE` column they are midnight UTC: `received_at >= ${yesterday}` keeps
  the same rows under `SET TimeZone = 'America/Los_Angeles'` as under `'UTC'`. That holds for
  a token in a comparison, `BETWEEN` or `IN` with the column, also with arithmetic such as
  `received_at >= ${today} - INTERVAL 6 HOUR`. Against a `DATE` or `TIMESTAMP` column the
  token is a plain date. Your own casts follow the session time zone as usual: compare
  `CAST(received_at AS DATE) = ${yesterday}` only under `TimeZone 'UTC'`, or compare
  `received_at` itself.
- `freshness` compares UTC timestamps. Its default reference is the current UTC time; a
  `reference_time` with an offset (`2026-10-29 08:00:00+02:00`) is converted to UTC, one without
  an offset is taken as UTC. A `TIMESTAMP WITH TIME ZONE` column is compared as its UTC
  instant; a `TIMESTAMP` or `DATE` column is taken as UTC. A text column with an offset
  (`'2026-10-29 04:00:00+02'`, `'...Z'`) is the instant it names; text without one is UTC.
- The time-aware checks bucket rows by their UTC date (see
  [Time-aware checks](#time-aware-checks)); text with an offset counts on its UTC date.
- An `identifier_column` of type `TIMESTAMP WITH TIME ZONE` is reported as its UTC instant
  with a `+00` suffix (`2026-10-26 22:00:00+00`), so results stored from sessions in different
  time zones join on `identifier`.

### Example

```sql
CREATE TABLE dq_checks (
    check_name        VARCHAR,
    check_type        VARCHAR,
    table_name        VARCHAR,
    column_name       VARCHAR,
    params            VARCHAR,
    lower_threshold   DOUBLE,
    upper_threshold   DOUBLE,
    monitor_only      BOOLEAN,
    identifier_column VARCHAR,
    filter_expr       VARCHAR
);

INSERT INTO dq_checks VALUES
    ('orders_volume',  'volume',        'orders', NULL,          NULL,
     100, NULL, false, NULL, NULL),
    ('email_nulls',    'null_rate',     'orders', 'email',       NULL,
     NULL, 0.05, true, NULL, NULL),
    ('status_allowed', 'values_in_set', 'orders', 'status',
     '{"allowed_values": ["completed", "pending", "failed"]}', 1.0, NULL, false, NULL, NULL),
    ('customer_fk',    'match_rate',    'orders', 'customer_id',
     '{"right_table": "customers", "right_keys": "id"}', 1.0, NULL, false, NULL, NULL),
    ('fresh',          'freshness',     'orders', 'created_at',
     '{"reference_time": "2026-01-31 08:00:00"}', NULL, 86400, false, NULL, NULL),
    ('daily_iqr',      'metric_anomaly_iqr', 'orders', NULL,
     '{"date_column": "order_date"}', NULL, NULL, false, NULL, NULL),
    ('missing_table',  'volume',        'no_such_table', NULL,   NULL,
     1, NULL, false, NULL, NULL),
    ('status_volume',  'volume',        'orders', NULL,          NULL,
     50, NULL, false, 'status', NULL);

SELECT check_name, identifier, round(value, 4) AS value,
       lower_threshold AS lower, upper_threshold AS upper, status
FROM run_checks('dq_checks')
ORDER BY check_name, identifier;
```

```text
┌────────────────┬────────────┬─────────┬────────┬─────────┬─────────┐
│   check_name   │ identifier │  value  │ lower  │  upper  │ status  │
│    varchar     │  varchar   │ double  │ double │ double  │ varchar │
├────────────────┼────────────┼─────────┼────────┼─────────┼─────────┤
│ customer_fk    │ NULL       │  0.9178 │    1.0 │    NULL │ fail    │
│ daily_iqr      │ NULL       │     2.0 │   10.0 │    10.0 │ fail    │
│ email_nulls    │ NULL       │  0.0993 │   NULL │    0.05 │ warn    │
│ fresh          │ NULL       │ 79200.0 │   NULL │ 86400.0 │ pass    │
│ missing_table  │ NULL       │    NULL │   NULL │    NULL │ error   │
│ orders_volume  │ NULL       │   292.0 │  100.0 │    NULL │ pass    │
│ status_allowed │ NULL       │  0.9966 │    1.0 │    NULL │ fail    │
│ status_volume  │ completed  │   175.0 │   50.0 │    NULL │ pass    │
│ status_volume  │ failed     │    58.0 │   50.0 │    NULL │ pass    │
│ status_volume  │ pending    │    58.0 │   50.0 │    NULL │ pass    │
│ status_volume  │ refunded   │     1.0 │   50.0 │    NULL │ fail    │
└────────────────┴────────────┴─────────┴────────┴─────────┴─────────┘
  11 rows                                                  6 columns
```

```sql
SELECT check_name, status, message
FROM run_checks('dq_checks')
WHERE status IN ('error', 'warn')
ORDER BY check_name;
```

```text
┌───────────────┬─────────┬────────────────────────────────────────────────────────────────────────────────────────┐
│  check_name   │ status  │                                        message                                         │
│    varchar    │ varchar │                                        varchar                                         │
├───────────────┼─────────┼────────────────────────────────────────────────────────────────────────────────────────┤
│ email_nulls   │ warn    │ null_rate value 0.099 violates thresholds (0.099 > upper 0.05)                         │
│ missing_table │ error   │ check 'missing_table' (volume) cannot run: target table 'no_such_table' does not exist │
└───────────────┴─────────┴────────────────────────────────────────────────────────────────────────────────────────┘
```

### Persisting results

Store each run to keep a history, then watch the check values over time with
[`metric_anomaly_iqr`](#metric_anomaly_iqr) on `run_ts`:

```sql
CREATE TABLE IF NOT EXISTS dq_results AS SELECT * FROM run_checks('dq_checks') LIMIT 0;
INSERT INTO dq_results SELECT * FROM run_checks('dq_checks');
SELECT count(*) AS result_rows FROM dq_results;
```

```text
┌─────────────┐
│ result_rows │
│    int64    │
├─────────────┤
│          11 │
└─────────────┘
```

```sql no-run
-- Needs a history of several daily runs in dq_results; see the data quality guide
SELECT * FROM metric_anomaly_iqr('dq_results', 'run_ts', 'value', 30, 1.5, 'both');
```

`run_checks` carries the tags `{'anofox.output': 'check', 'anofox.contract': '1'}`; see the
[integration contract](11-integration-contract.md).

## See also

- [Guide: data quality checks](../guides/02-data-quality-checks.md)
- [Anomaly detection](08-anomaly-detection.md) for row-level outlier scores
- [Profiling](10-profiling.md) for column statistics
- [Integration contract](11-integration-contract.md)
- [Conventions](00-conventions.md)

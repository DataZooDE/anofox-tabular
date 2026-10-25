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

All checks run locally. The thresholds you pass come back in the output with the type of the
literal you passed (see [Conventions](00-conventions.md#types)).

The arguments are checked when the query is bound:

- A `NULL` table, column or key argument is an error such as
  `null_rate: table_name must not be NULL`, also for a prepared statement executed with a
  `NULL` parameter.
- A lower bound greater than the upper bound is an error, for example
  `volume: min_rows (100) must not be greater than max_rows (10)`.
- Count bounds (`min_rows`, `max_distinct`, `iqr_multiplier`, ...) must not be negative, and a
  `zscore` threshold must be positive.
- Rates (`max_null_rate`, `min_match_rate`, `min_rate`, ...) must be between 0 and 1, for
  example `null_rate: max_null_rate must be between 0 and 1 (got 1.5)`.

Rates in the messages are rounded to three decimals (`Null rate 0.333 (2/6) ...`); the rate
columns keep the full precision.

NaN and `±Inf` values in numeric columns do not break a check. `zscore` leaves them out of the
statistics and names how many it ignored. `agg_check`, and an `agg` check in `run_checks`,
report a `NaN` value and fail when the column contains NaN or `±Inf` values, with the message
`<agg> value is NaN: the column contains <n> NaN/Inf value(s)`.

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
  -> TABLE(status VARCHAR, row_count BIGINT, min_threshold, max_threshold, message VARCHAR)
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
           threshold, message VARCHAR)
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
  -> TABLE(status VARCHAR, distinct_count BIGINT, min_threshold, max_threshold, message VARCHAR)
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
freshness(table_name VARCHAR, timestamp_column VARCHAR, max_age INTERVAL [, reference_time TIMESTAMP])
  -> TABLE(status VARCHAR, metric_value TIMESTAMP, threshold TIMESTAMP, age_seconds BIGINT, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `timestamp_column` | `VARCHAR` | required | Timestamp column |
| `max_age` | `INTERVAL` | required | Largest allowed age of the newest row |
| `reference_time` | `TIMESTAMP` | current time | The time the age is measured from |

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

Without `reference_time` the check compares against the current time, `CAST(now() AS TIMESTAMP)`
(UTC unless the ICU extension is loaded, then the session time zone). It does not need ICU.
A `TIMESTAMP WITH TIME ZONE` column is compared as its UTC `TIMESTAMP`, so it works without ICU
too, and `metric_value` is then the newest value in UTC. A `DATE` column is compared as
midnight of that day.

An empty table, or a column that is all `NULL`, fails with `metric_value` and `age_seconds`
`NULL` and the message `No timestamp values found (table is empty or column is all NULL)`.

---

## `zscore`

Canonical name: `anofox_tab_zscore`. Table function.

Fails when any value lies more than `threshold` standard deviations from the mean. The method
assumes roughly normal data. For skewed data use [`iqr`](#iqr).

```text
zscore(table_name VARCHAR, column_name VARCHAR, threshold DOUBLE)
  -> TABLE(status VARCHAR, mean DOUBLE, stddev DOUBLE, outlier_count BIGINT, total_count BIGINT,
           outlier_rate DOUBLE, threshold, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Numeric column |
| `threshold` | `DOUBLE` | required | Z-score limit, typically `3.0`; must be positive |

The mean, the standard deviation and the outliers use the finite values only. NaN and `±Inf`
values are left out (`total_count` does not count them) and the message ends with
`; <n> NaN/Inf value(s) ignored`.

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
           outlier_count BIGINT, total_count BIGINT, multiplier, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Numeric column |
| `iqr_multiplier` | `DOUBLE` | required | Fence multiplier `k`, typically `1.5` |

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

`iqr` cannot run inside a [`run_checks`](#run_checks) suite.

---

## `regex_match`

Canonical name: `anofox_tab_regex_match`. Table function.

Passes when the share of non-NULL values that match a regular expression is within bounds.

```text
regex_match(table_name VARCHAR, column_name VARCHAR, pattern VARCHAR, min_match_rate DOUBLE
            [, max_match_rate DOUBLE])
  -> TABLE(status VARCHAR, match_rate DOUBLE, matched_count BIGINT, total_count BIGINT,
           min_threshold, max_threshold, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check; values are cast to `VARCHAR` |
| `pattern` | `VARCHAR` | required | RE2 pattern, as for `regexp_matches` |
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
           threshold, sample_violations VARCHAR[], message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check |
| `allowed_values` | `VARCHAR[]` | required | The allowed values |
| `min_rate` | `DOUBLE` | `1.0` | Lowest allowed share of values in the set |

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
  -> TABLE(status VARCHAR, agg VARCHAR, value DOUBLE, lower_threshold, upper_threshold, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Numeric column |
| `agg` | `VARCHAR` | required | `avg`, `min`, `max`, `sum`, `median` or `stddev`; any other value is a binder error |
| `lower_threshold` | `DOUBLE` | required | Lower bound; `NULL` = unbounded |
| `upper_threshold` | `DOUBLE` | required | Upper bound; `NULL` = unbounded |

An empty table or an all-NULL column gives `value = NULL` and passes. When the column contains
NaN or `±Inf` values, `value` is `NaN` and the check fails with the message
`<agg> value is NaN: the column contains <n> NaN/Inf value(s)`; an average or a sum over such a
column has no meaningful value.

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
duplicate_count(table_name VARCHAR, column_names VARCHAR [, max_duplicates BIGINT])
  -> TABLE(status VARCHAR, duplicate_count BIGINT, total_count BIGINT, threshold, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_names` | `VARCHAR` | required | One column, or a comma-separated list for a compound key |
| `max_duplicates` | `BIGINT` | `0` | Highest allowed number of duplicates |

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
           distinct_values BIGINT, lower_threshold, upper_threshold, message VARCHAR)
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

```text
match_rate(left_table VARCHAR, right_table VARCHAR, left_keys VARCHAR, right_keys VARCHAR [, min_rate DOUBLE])
  -> TABLE(status VARCHAR, match_rate DOUBLE, matched_count BIGINT, total_count BIGINT, threshold, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `left_table` | `VARCHAR` | required | The referencing table |
| `right_table` | `VARCHAR` | required | The referenced table |
| `left_keys` | `VARCHAR` | required | Comma-separated key columns of `left_table` |
| `right_keys` | `VARCHAR` | required | Comma-separated key columns of `right_table`, same number as `left_keys` |
| `min_rate` | `DOUBLE` | `1.0` | Lowest allowed match rate |

An empty left table passes with `total_count = 0`.

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
           threshold, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `expression` | `VARCHAR` | required | A SQL boolean expression over the table's columns |
| `min_rate` | `DOUBLE` | `1.0` | Lowest allowed share of compliant rows |

> **Security note:** the expression runs as SQL with your privileges. At bind time it must parse
> as a single boolean expression, so a second statement is rejected. Scalar subqueries are still
> allowed. Pass only expressions you trust; the trust model is the same as running the SQL
> yourself.

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
Binder Error: compliance: failed to parse expression 'amount > 0; DROP TABLE orders': {"exception_type":"Parser","exception_message":"syntax error at or near \";\"","location":"[18,1]","position":"18","error_subtype":"SYNTAX_ERROR"}
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM compliance('orders', 'amount > 0; DROP TABLE orders');
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

---

## Time-aware checks

`rel_count_change`, `metric_anomaly_iqr` and `rolling_values_in_set` evaluate one reference
date. By default this is the latest date in the data; the optional `reference_date` argument
sets it.

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
                 [, start_date := DATE, end_date := DATE])
  -> TABLE(status VARCHAR, reference_date DATE, reference_count DOUBLE, baseline_avg DOUBLE,
           baseline_days BIGINT, rel_change DOUBLE, lower_threshold, upper_threshold, message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `date_column` | `VARCHAR` | required | Date or timestamp column; cast to `DATE` |
| `count_column` | `VARCHAR` | `NULL` | Column counted with `COUNT(DISTINCT ...)` per day; `NULL` = daily row count |
| `window_days` | `BIGINT` | `7` | Baseline length: the days strictly before the reference date |
| `lower_threshold` | `DOUBLE` | `-0.5` | Lowest allowed relative change |
| `upper_threshold` | `DOUBLE` | `0.5` | Highest allowed relative change |
| `reference_date` | `DATE` | `NULL` = latest date in the data | The date to evaluate |
| `start_date`, `end_date` | `DATE`, named | none | Evaluate every day of this range |

Empty input, no baseline days and a zero baseline pass with a message that says so.

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
| `date_column` | `VARCHAR` | required | Date or timestamp column; cast to `DATE` |
| `metric_column` | `VARCHAR` | `NULL` | Column averaged per day; `NULL` = daily row count |
| `window_days` | `BIGINT` | `30` | Baseline length: the days strictly before the reference date |
| `k` | `DOUBLE` | `1.5` | IQR multiplier |
| `mode` | `VARCHAR` | `'both'` | `'both'`, `'upper'` or `'lower'`: which bound to enforce |
| `reference_date` | `DATE` | `NULL` = latest date in the data | The date to evaluate |
| `start_date`, `end_date` | `DATE`, named | none | Evaluate every day of this range |

Empty input or an empty baseline window pass with a message that says so.

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
           threshold, sample_violations VARCHAR[], message VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `column_name` | `VARCHAR` | required | Column to check |
| `allowed_values` | `VARCHAR[]` | required | The allowed values |
| `date_column` | `VARCHAR` | required | Date or timestamp column; cast to `DATE` |
| `window_days` | `BIGINT` | `7` | Window length, ending on the reference date |
| `min_rate` | `DOUBLE` | `1.0` | Lowest allowed share of values in the set |
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
functions. Each takes exactly the arguments of its base check, including `reference_date` and
the `start_date := DATE, end_date := DATE` range. It returns one row per reference date whose
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
partition when the check has an `identifier_column`.

```text
run_checks(checks_table VARCHAR)
  -> TABLE(run_ts TIMESTAMP, check_name VARCHAR, check_type VARCHAR, table_name VARCHAR,
           column_name VARCHAR, identifier VARCHAR, value DOUBLE, lower_threshold DOUBLE,
           upper_threshold DOUBLE, status VARCHAR, message VARCHAR)
```

### The checks table

The checks table must be a regular table, not a TEMP table: `run_checks` reads it through a
second connection when the query is bound. The checked tables may be TEMP tables or views. All
ten columns must exist:

| Column | Type | Description |
|--------|------|-------------|
| `check_name` | `VARCHAR` | Label of the check; results are ordered by it |
| `check_type` | `VARCHAR` | One of the types below |
| `table_name` | `VARCHAR` | The table or view to check |
| `column_name` | `VARCHAR` | The column; `NULL` where the type needs none. Comma-separated for `duplicate_count` |
| `params` | `VARCHAR` | A JSON object with type-specific parameters, or `NULL` |
| `lower_threshold` | `DOUBLE` | Fail when `value` is below it; `NULL` = unbounded |
| `upper_threshold` | `DOUBLE` | Fail when `value` is above it; `NULL` = unbounded |
| `monitor_only` | `BOOLEAN` | `true` records `'warn'` instead of `'fail'` |
| `identifier_column` | `VARCHAR` | Optional: one result row per distinct value of this column |
| `filter_expr` | `VARCHAR` | Optional SQL boolean pre-filter on the checked table |

### Check types

`run_checks` runs 14 check types. `zscore`, `iqr` and `schema_check` are not available in a
suite. The `value` column holds the measure of each type:

| `check_type` | `value` | `params` keys | `identifier_column` |
|--------------|---------|---------------|---------------------|
| `volume` | row count | none | yes |
| `null_rate` | share of NULLs | none | yes |
| `distinct_count` | distinct values | none | yes |
| `regex_match` | match rate | `pattern` (required) | yes |
| `values_in_set` | share in set | `allowed_values` (JSON array of strings) | yes |
| `agg` | the aggregate | `agg` (required): `avg`, `min`, `max`, `sum`, `median`, `stddev` | yes |
| `duplicate_count` | duplicates | none | yes |
| `compliance` | compliance rate | `expression` (required) | yes |
| `freshness` | age in seconds | `reference_time` (default: the current time, no ICU needed) | yes |
| `occurrence` | frequency of the extreme value | `mode`: `max` (default) or `min` | no |
| `match_rate` | match rate | `right_table`, `right_keys` (required), `left_keys` (default `column_name`) | no |
| `rel_count_change` | relative change | `date_column` (required), `count_column`, `window_days` (7), `reference_date` | no |
| `metric_anomaly_iqr` | daily metric | `date_column` (required), `metric_column`, `window_days` (30), `k` (1.5), `reference_date` | no |
| `rolling_values_in_set` | share in set | `date_column` (required), `allowed_values`, `window_days` (7), `reference_date` | no |

For `metric_anomaly_iqr` the IQR bounds replace the table's thresholds and appear in
`lower_threshold` and `upper_threshold`. The `mode` parameter of the direct function is not used
in a suite; both bounds apply.

`filter_expr` accepts the date tokens `${today}`, `${yesterday}`, `${today+N}`, `${today-N}`,
`${yesterday+N}` and `${yesterday-N}`, for example `order_date >= ${today-7}`.

### Status

| `status` | Meaning |
|----------|---------|
| `pass` | `value` is within the thresholds, or `value` is NULL (nothing to evaluate; message `No value to evaluate; check passed trivially`) |
| `fail` | `value` is outside a threshold. For `freshness` also a NULL `value` (empty table, all-NULL column or, with `identifier_column`, a group without timestamps), like the direct [`freshness`](#freshness) function; message `No timestamp values found (table is empty or column is all NULL)` |
| `warn` | as `fail`, for a check with `monitor_only = true` |
| `error` | the check could not run; `value` is NULL and the message says why |

A `NaN` value (for example an `agg` average over a column with NaN values) is `fail` (or `warn`),
with the message `agg value is NaN: the column contains <n> NaN/Inf value(s)`.

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
- an unknown `check_type`, invalid `params` JSON or a missing required `params` key
- a `compliance` expression or `filter_expr` that does not parse or references an unknown column
- a `match_rate` `right_table` that does not exist
- `lower_threshold` greater than `upper_threshold`, or a rate threshold outside 0 to 1 for
  `null_rate`, `regex_match`, `values_in_set`, `compliance`, `match_rate` and
  `rolling_values_in_set`
- `identifier_column` set for a type that does not support it

A runtime error inside a user-supplied `compliance` expression or `filter_expr` (for example a
failing `CAST` on one row) still stops the whole call; `run_checks` runs all checks in one query.
`compliance` expressions and `filter_expr` are validated like the
[`compliance`](#compliance) expression and run with your privileges. Messages print values
rounded to three decimals; the `value` column keeps the full precision.

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
│ email_nulls   │ warn    │ null_rate value 0.099 violates thresholds                                              │
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

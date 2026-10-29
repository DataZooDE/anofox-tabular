# Integration Contract

> The shared output schemas that let other anofox extensions read tabular results without adapter SQL.

**Use this page to:**
- Find the functions whose output follows a contract schema
- Read the `check`, `matrix` and `events` schemas
- Get one contract-shaped result for the one-row checks through `run_checks`

anofox-tabular takes part in the anofox integration contract (described in anofox-visualization,
`docs/plans/integration-contract.md`). A function whose output follows a contract schema carries
two tags in `duckdb_functions().tags`: `anofox.output` names the schema and `anofox.contract`
the contract version. Consumers such as `anofox_plot` in anofox-visualization find producers by
these tags.

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`anofox_contract_version`](#anofox_contract_version) | `anofox_contract_version` (no alias) | scalar | `VARCHAR`, the contract version |

## `anofox_contract_version`

Canonical name: `anofox_contract_version`. Scalar function. It has no `anofox_tab_` prefix and no
alias, because every anofox extension shares it: the extension that loads first registers it, and
the others leave it in place.

```text
anofox_contract_version() -> VARCHAR
```

```sql
SELECT anofox_contract_version() AS contract_version;
```

```text
┌──────────────────┐
│ contract_version │
│     varchar      │
├──────────────────┤
│ 1                │
└──────────────────┘
```

## Tagged producers

Both the canonical name and the alias carry the tags:

```sql
SELECT DISTINCT function_name, tags['anofox.output'] AS output, tags['anofox.contract'] AS contract
FROM duckdb_functions()
WHERE tags['anofox.contract'] IS NOT NULL
ORDER BY function_name;
```

```text
┌─────────────────────────────────────────┬─────────┬──────────┐
│              function_name              │ output  │ contract │
│                 varchar                 │ varchar │ varchar  │
├─────────────────────────────────────────┼─────────┼──────────┤
│ anofox_tab_metric_anomaly_iqr_events    │ events  │ 1        │
│ anofox_tab_profile_correlations         │ matrix  │ 1        │
│ anofox_tab_rel_count_change_events      │ events  │ 1        │
│ anofox_tab_rolling_values_in_set_events │ events  │ 1        │
│ anofox_tab_run_checks                   │ check   │ 1        │
│ metric_anomaly_iqr_events               │ events  │ 1        │
│ profile_correlations                    │ matrix  │ 1        │
│ rel_count_change_events                 │ events  │ 1        │
│ rolling_values_in_set_events            │ events  │ 1        │
│ run_checks                              │ check   │ 1        │
└─────────────────────────────────────────┴─────────┴──────────┘
  10 rows                                            3 columns
```

| Producer | Canonical name | Schema | Columns |
|----------|----------------|--------|---------|
| [`run_checks`](07-data-quality.md#run_checks) | `anofox_tab_run_checks` | `check` | `run_ts, check_name, check_type, table_name, column_name, identifier, value, lower_threshold, upper_threshold, status, message` |
| [`profile_correlations`](10-profiling.md#profile_correlations) | `anofox_tab_profile_correlations` | `matrix` | `column_a, column_b, pearson, spearman, n` |
| [`rel_count_change_events`](07-data-quality.md#events-companions) | `anofox_tab_rel_count_change_events` | `events` | `ds, kind, label, severity, value, threshold, series` |
| [`metric_anomaly_iqr_events`](07-data-quality.md#events-companions) | `anofox_tab_metric_anomaly_iqr_events` | `events` | same |
| [`rolling_values_in_set_events`](07-data-quality.md#events-companions) | `anofox_tab_rolling_values_in_set_events` | `events` | same |

## Schemas

### `check`

One row per check (and per identifier value when the check is partitioned).

| Column | Type | Description |
|--------|------|-------------|
| `run_ts` | `TIMESTAMP` | When the suite ran |
| `check_name` | `VARCHAR` | The label from the checks table |
| `check_type` | `VARCHAR` | `volume`, `null_rate`, ... |
| `table_name` | `VARCHAR` | The checked table |
| `column_name` | `VARCHAR` | The checked column, `NULL` where not applicable |
| `identifier` | `VARCHAR` | The partition value, `NULL` without `identifier_column` |
| `value` | `DOUBLE` | The measured value |
| `lower_threshold` | `DOUBLE` | Lower bound, `NULL` = unbounded |
| `upper_threshold` | `DOUBLE` | Upper bound, `NULL` = unbounded |
| `status` | `VARCHAR` | `pass`, `warn` or `fail`; `run_checks` adds `error` for a check that cannot run |
| `message` | `VARCHAR` | Human-readable result |

### `matrix`

A pair list: each pair of columns appears once, and the reader mirrors it into a matrix.

| Column | Type | Description |
|--------|------|-------------|
| `column_a` | `VARCHAR` | First column |
| `column_b` | `VARCHAR` | Second column |
| `pearson` | `DOUBLE` | Pearson correlation |
| `spearman` | `DOUBLE` | Spearman rank correlation |
| `n` | `BIGINT` | Rows used |

### `events`

One row per date on which a time-aware check fails or warns. Passing dates are not events.

| Column | Type | Description |
|--------|------|-------------|
| `ds` | `DATE` | The reference date |
| `kind` | `VARCHAR` | The check: `rel_count_change`, `metric_anomaly_iqr` or `rolling_values_in_set` |
| `label` | `VARCHAR` | The check's `message` |
| `severity` | `VARCHAR` | `alarm` for `fail`, `warning` for `warn` |
| `value` | `DOUBLE` | The measure: `rel_change`, `metric_value` or `in_set_rate` |
| `threshold` | `DOUBLE` | The bound the measure crossed |
| `series` | `VARCHAR` | The table, or `table.column` when the check measures a column |

## The one-row checks and `run_checks`

The one-row checks (`volume`, `null_rate`, `regex_match`, `agg_check`, ...) keep their own result
columns and are not tagged. To get them in the `check` schema, put them in a checks table and run
it with [`run_checks`](07-data-quality.md#run_checks). It supports 14 check types: `volume`,
`null_rate`, `distinct_count`, `regex_match`, `values_in_set`, `agg`, `duplicate_count`,
`occurrence`, `match_rate`, `compliance`, `freshness`, `rel_count_change`,
`metric_anomaly_iqr` and `rolling_values_in_set`. `zscore`, `iqr` and `schema_check` are not
available in a suite.

A checks table (TEMP tables work too):

```sql
CREATE TABLE orders AS
SELECT i AS order_id,
       CASE WHEN i % 20 = 0 THEN NULL ELSE 'user' || i || '@example.com' END AS email
FROM range(1, 101) t(i);

CREATE TABLE dq_checks (
    check_name VARCHAR, check_type VARCHAR, table_name VARCHAR, column_name VARCHAR,
    params VARCHAR, lower_threshold DOUBLE, upper_threshold DOUBLE,
    monitor_only BOOLEAN, identifier_column VARCHAR, filter_expr VARCHAR
);
INSERT INTO dq_checks VALUES
    ('orders_volume', 'volume',    'orders', NULL,    NULL, 50,   NULL, false, NULL, NULL),
    ('email_nulls',   'null_rate', 'orders', 'email', NULL, NULL, 0.01, true,  NULL, NULL);

SELECT check_name, check_type, value, status
FROM run_checks('dq_checks')
ORDER BY check_name;
```

```text
┌───────────────┬────────────┬────────┬─────────┐
│  check_name   │ check_type │ value  │ status  │
│    varchar    │  varchar   │ double │ varchar │
├───────────────┼────────────┼────────┼─────────┤
│ email_nulls   │ null_rate  │   0.05 │ warn    │
│ orders_volume │ volume     │  100.0 │ pass    │
└───────────────┴────────────┴────────┴─────────┘
```

`email_nulls` reports `warn` instead of `fail` because `monitor_only` is `true`.

The `_events` companions return the `events` schema:

```sql
CREATE TABLE daily_orders AS
SELECT DATE '2026-01-01' + (i % 10)::INTEGER AS order_date
FROM range(0, 100) t(i)
UNION ALL
SELECT DATE '2026-01-10' FROM range(0, 40);

SELECT ds, kind, severity, round(value, 2) AS value, threshold, series
FROM rel_count_change_events('daily_orders', 'order_date');
```

```text
┌────────────┬──────────────────┬──────────┬────────┬───────────┬──────────────┐
│     ds     │       kind       │ severity │ value  │ threshold │    series    │
│    date    │     varchar      │ varchar  │ double │  double   │   varchar    │
├────────────┼──────────────────┼──────────┼────────┼───────────┼──────────────┤
│ 2026-01-10 │ rel_count_change │ alarm    │    4.0 │       0.5 │ daily_orders │
└────────────┴──────────────────┴──────────┴────────┴───────────┴──────────────┘
```

## See also

- [Data quality checks](07-data-quality.md)
- [Profiling](10-profiling.md)
- [Guide: data quality checks](../guides/02-data-quality-checks.md)

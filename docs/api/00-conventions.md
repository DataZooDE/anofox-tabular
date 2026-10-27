# API Conventions

> Naming, parameter passing, types and shared behavior of the anofox-tabular SQL functions

**Use this page to:**
- Understand the two names of every function
- Know when you can pass arguments by name with `:=`
- Read the signatures on the module pages
- Know which functions need a regular table instead of a TEMP table
- Keep one bad value from stopping a query with `TRY()`

## Function names

Every function has a canonical name with the prefix `anofox_tab_` and a short alias without it.
Both names call the same code, so `vat_is_valid` and `anofox_tab_vat_is_valid` return the same
result. The examples in these docs use the short alias. Use the canonical name when another
extension registers a function with the same short name.

Two functions have no alias: [`anofox_ner_status`](06-pii.md#anofox_ner_status) and
[`anofox_contract_version`](11-integration-contract.md#anofox_contract_version).

```sql
SELECT vat_is_valid('DE111111125') AS alias,
       anofox_tab_vat_is_valid('DE111111125') AS canonical;
```

```text
┌─────────┬───────────┐
│  alias  │ canonical │
│ boolean │  boolean  │
├─────────┼───────────┤
│ true    │ true      │
└─────────┴───────────┘
```

To list the functions of the loaded extension, query `duckdb_functions()`:

```sql
SELECT DISTINCT function_name, function_type
FROM duckdb_functions()
WHERE function_name LIKE 'anofox_tab_vat%'
ORDER BY function_name;
```

```text
┌────────────────────────────────┬───────────────┐
│         function_name          │ function_type │
│            varchar             │    varchar    │
├────────────────────────────────┼───────────────┤
│ anofox_tab_vat                 │ scalar        │
│ anofox_tab_vat_country_name    │ scalar        │
│ anofox_tab_vat_exists          │ scalar        │
│ anofox_tab_vat_format          │ scalar        │
│ anofox_tab_vat_is_eu_member    │ scalar        │
│ anofox_tab_vat_is_valid        │ scalar        │
│ anofox_tab_vat_is_valid_syntax │ scalar        │
│ anofox_tab_vat_normalize       │ scalar        │
│ anofox_tab_vat_split           │ scalar        │
└────────────────────────────────┴───────────────┘
```

## Modules

| Module | Page | Kind of functions |
|--------|------|-------------------|
| Email | [01-email.md](01-email.md) | scalar, one table function |
| Postal addresses | [02-postal.md](02-postal.md) | scalar, table |
| Phone numbers | [03-phone.md](03-phone.md) | scalar, one table function |
| Money and currency | [04-money.md](04-money.md) | scalar |
| VAT numbers | [05-vat.md](05-vat.md) | scalar |
| PII detection and masking | [06-pii.md](06-pii.md) | scalar, table |
| Data quality checks | [07-data-quality.md](07-data-quality.md) | table |
| Anomaly detection | [08-anomaly-detection.md](08-anomaly-detection.md) | table |
| Diffing | [09-diffing.md](09-diffing.md) | table |
| Profiling | [10-profiling.md](10-profiling.md) | table |
| Integration contract | [11-integration-contract.md](11-integration-contract.md) | scalar, tags |

## Parameters

### Positional parameters

Arguments are positional. Optional parameters come last and are marked with `[ ]` in the
signatures. To set a later optional parameter you must also pass the ones before it.

`NULL` keeps the default for every optional parameter of the anomaly functions
(`isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`, `outlier_tree`), and elsewhere
where a parameter says so (for example `reference_date`, or an unbounded threshold). In all other
places pass the default value itself.

### Named parameters

A few parameters also accept the named form `name := value`:

| Functions | Named parameters |
|-----------|------------------|
| [`diff_joindiff`](09-diffing.md#diff_joindiff) | every parameter: `source_table`, `target_table`, `primary_key`, `primary_keys`, `compare_columns`, `include_all` |
| [`diff_hashdiff`](09-diffing.md#diff_hashdiff) | `source_table`, `target_table`, `primary_key`, `primary_keys`; `bisection_threshold` and `bisection_factor` are declared but raise a "not implemented" error |
| [`diff_schema`](09-diffing.md#diff_schema) | `source_table`, `target_table` |
| [`rel_count_change`](07-data-quality.md#rel_count_change), [`metric_anomaly_iqr`](07-data-quality.md#metric_anomaly_iqr), [`rolling_values_in_set`](07-data-quality.md#rolling_values_in_set) and their `_events` companions | `start_date`, `end_date` (only by name) |
| [`isolation_forest`](08-anomaly-detection.md#isolation_forest), [`isolation_forest_mv`](08-anomaly-detection.md#isolation_forest_mv), [`dbscan`](08-anomaly-detection.md#dbscan), [`dbscan_mv`](08-anomaly-detection.md#dbscan_mv), [`outlier_tree`](08-anomaly-detection.md#outlier_tree), [`pii_audit_table`](06-pii.md#pii_audit_table) | `id_column` (only by name) |

On the diff functions a named value overrides the positional value of the same parameter. Any
other named parameter is a binder error:

```sql
CREATE TABLE readings AS SELECT i AS id, i * 1.0 AS v FROM range(1, 30) t(i);
SELECT status FROM isolation_forest('readings', 'v', n_trees := 10);
```

```text
Binder Error: Invalid named parameter "n_trees" for function isolation_forest

LINE 1: ... status FROM isolation_forest('readings', 'v', n_trees := 10);
                                                                     ^^
```

Error output in these docs is shown as DuckDB 2.0 prints it. DuckDB v1.5.6 prints the same
messages but marks the position with a single `^` and lists the accepted named parameters.

## Table and column arguments

Table functions take the table name as a string: `null_rate('orders', 'email', 0.05)`. Pass the
plain name; the extension quotes it, so names with spaces or quotes work as they are.

Column lists come in two forms. Check the signature of each function:

| Form | Example | Used by |
|------|---------|---------|
| comma-separated `VARCHAR` | `'amount, quantity'` | `isolation_forest_mv`, `dbscan_mv`, `outlier_tree`, `duplicate_count`, `match_rate`, `pii_scan_table`, `pii_audit_table` |
| `VARCHAR[]` list | `['id', 'created_at']` | `schema_check`, `values_in_set`, `rolling_values_in_set`, `profile_table`, `profile_correlations`, `diff_joindiff`, `duplicate_count` |

Column names match case-insensitively, like DuckDB identifiers: `'EMAIL'` finds a column
`Email`. In `profile_table`, `profile_correlations`, `pii_scan_table`, `pii_audit_table`,
`isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`, `zscore`, `iqr`, `agg_check`
and `metric_anomaly_iqr` (its `metric_column`) an unknown column name is an error naming the
column and the table; `run_checks` reports it in an `error` row of that check;
`schema_check` reports it as missing.

The table, column and key arguments of a table function are required: a `NULL` there is a
binder error `<function>: <argument> must not be NULL`, also when it comes from a `NULL`
parameter of a prepared statement. A `NULL` `id_column` means "no id column", like leaving it
out.

```sql
SELECT * FROM null_rate(NULL, 'v', 0.1);
```

```text
Binder Error: null_rate: table_name must not be NULL
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM null_rate(NULL, 'v', 0.1);
                      ^^^^^^^^^^^^^^^^^^^^^^^^^
```

Some functions read the table through a second connection. That connection cannot see TEMP
tables of your session, so these functions reject a TEMP table when the query is bound, with
`TEMP tables are not supported by <function>; use a regular table`:

| Functions | Accept |
|-----------|--------|
| `isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`, `outlier_tree`, `pii_scan_table`, `pii_audit_table`, `profile_table`, `profile_summary`, `profile_correlations` | regular tables and views, not TEMP tables |
| `run_checks` | a regular checks table; the checked tables may be TEMP tables or views |
| all other table functions | tables, views and TEMP tables |

## Types

The signatures use DuckDB's own type names: `VARCHAR`, `BIGINT`, `DOUBLE`, `BOOLEAN`, `DATE`,
`TIMESTAMP`, `INTERVAL`, lists as `VARCHAR[]` and structs as `STRUCT(name TYPE, ...)`. Money
values are `STRUCT(amount DECIMAL(18,3), currency VARCHAR)`. A table function's result is
written as `TABLE(column TYPE, ...)`.

The check functions return the thresholds you passed in their output with a fixed type,
whatever literal you pass: `DOUBLE` for rates, multipliers and value bounds, `BIGINT` for count
bounds. `null_rate(..., 0.05)` returns `threshold` as the `DOUBLE` `0.05`.

Date and time values are evaluated in UTC. A `TIMESTAMP WITH TIME ZONE` column is read as its UTC
instant and bucketed by its UTC date, with or without the ICU extension and whatever the session
`TimeZone`; a plain `TIMESTAMP` is taken as UTC as it is. The default reference time of
`freshness` is the current time in UTC.

## NULL and empty input

- Scalar functions return `NULL` for a `NULL` input.
- The one-row checks (`volume`, `null_rate`, `regex_match`, ...) always return one row, also for
  an empty table. Empty input passes, with a message that says so.
- Functions with an `output_mode` (`'summary'`, `'scores'`, `'clusters'`, `'outliers'`) return one
  summary row or one row per input row. The module pages list both shapes. `output_mode` and
  the other option strings (`agg`, `mode`) are case-insensitive: `'SCORES'` works like `'scores'`.
- NaN and `±Inf` values in numeric columns are counted, not averaged: `profile_table` reports
  them in `nan_count` / `inf_count` and computes its statistics over the finite values, the
  statistical checks `zscore` and `iqr` and `profile_correlations` leave them out (the checks
  name how many in the message), and `agg_check` fails with a `NaN` value.

## Errors and `TRY()`

A scalar function raises an error for input it cannot handle, such as an unknown currency code
in `money` or an unknown strategy in `pii_mask`, and the error stops the whole query. Wrap the
call in DuckDB's `TRY()` to get `NULL` for those rows instead:

```sql
SELECT TRY(money(10, 'XYZ')) AS m;
```

```text
┌────────────────────────────────────────────────┐
│                       m                        │
│ struct(amount decimal(18,3), currency varchar) │
├────────────────────────────────────────────────┤
│ NULL                                           │
└────────────────────────────────────────────────┘
```

Validation functions such as `email_is_valid` or `phonenumber_is_valid` do not raise for bad
values; they return `false`.

## Tracing output

Tracing is on by default at level `info`. In the DuckDB CLI the extension prints lines such as
`[anofox] [info] Money module functions registered` to stderr when it loads and when some
functions run. To quiet them:

```sql
SET anofox_tab_trace_level = 'warn';
-- or turn tracing off
SET anofox_tab_trace_enabled = false;
```

The examples in these docs show stdout only. See
[Configuration: tracing](../reference/configuration.md#tracing).

## See also

- [Configuration](../reference/configuration.md)
- [Network and privacy](../reference/network-and-privacy.md)
- [Getting started](../guides/01-getting-started.md)

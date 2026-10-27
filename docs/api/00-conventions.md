# API Conventions

> Naming, parameter passing, types and shared behavior of the anofox-tabular SQL functions

**Use this page to:**
- Understand the two names of every function
- Know when you can pass arguments by name with `:=`
- Read the signatures on the module pages
- Know which functions need a regular table instead of a TEMP table, and committed data
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
│ anofox_tab_vat_validate        │ scalar        │
└────────────────────────────────┴───────────────┘
  10 rows                              2 columns
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

Table functions take the table name as a string: `null_rate('orders', 'email', 0.05)`. The
string is read like a table name in a query, except that spaces need no quoting: `'q4 orders'`
works as it is. A name that contains a dot or a double quote must be passed as a quoted
identifier, the way you would write it in SQL: the table `orders.archive` is
`'"orders.archive"'` (unquoted, `'orders.archive'` is the table `archive` of the schema
`orders`), and the table `partner "EU" orders` is `'"partner ""EU"" orders"'`. A single quote
in a name is doubled as in every SQL string: `'o''brien_orders'`. When such a name is not
found, the error says how to quote it:
`volume: table 'orders.archive' does not exist (a table whose name contains a dot or a double quote is passed as a quoted identifier: "orders.archive")`.

A qualified name resolves as in a DuckDB query: `'sales.orders'` is the table `orders` of the
schema `sales` and, when no such schema exists, the table `orders` of the attached database
`sales` (`ATTACH 'sales.duckdb' AS sales`); `'sales.main.orders'` names database, schema and
table. Every function resolves names this way, `run_checks` and the diff functions included.

Column lists come in two forms. Check the signature of each function:

| Form | Example | Used by |
|------|---------|---------|
| comma-separated `VARCHAR` | `'amount, quantity'` | `isolation_forest_mv`, `dbscan_mv`, `outlier_tree`, `duplicate_count`, `match_rate`, `pii_scan_table`, `pii_audit_table` |
| `VARCHAR[]` list | `['id', 'created_at']` | `schema_check`, `values_in_set`, `rolling_values_in_set`, `profile_table`, `profile_correlations`, `diff_joindiff`, `duplicate_count` |

Column names match case-insensitively, like DuckDB identifiers: `'EMAIL'` finds a column
`Email`. In `profile_table`, `profile_correlations`, `pii_scan_table`, `pii_audit_table`,
`isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`, `zscore`, `iqr`, `agg_check`
and every direct data quality check (`volume`, `null_rate`, ... `rolling_values_in_set`) an
unknown column name is an error naming the column and the table; `run_checks` reports it in an
`error` row of that check; `schema_check` reports a missing required column as `fail`. An
unknown table is an error such as `null_rate: table 'ordrs' does not exist` in the data quality
checks, and an empty name is `<function>: table_name must not be empty`.

The table, column and key arguments of a table function are required: a `NULL` there is a
binder error `<function>: <argument> must not be NULL`, also when it comes from a `NULL`
parameter of a prepared statement. The same holds for a required threshold or list of a data
quality check that has no `NULL` meaning, such as `null_rate`'s `max_null_rate`; a threshold
documented as `NULL` = unbounded or with a default accepts `NULL`. A `NULL` `id_column` means "no id column", like leaving it
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
| `run_checks` | a checks table (TEMP too, read in your transaction) or a regular checks view; the checked tables may be TEMP tables or views |
| all other table functions | tables, views and TEMP tables |

The second connection also reads only committed data. Inside an explicit transaction
(`BEGIN ... COMMIT`) these ten functions would not see the rows the transaction inserted,
updated or deleted, nor a table it created. So once the transaction has uncommitted changes they
refuse when the query is bound, with `<function> reads the last committed data, but this
transaction has uncommitted changes; call it after COMMIT, or outside a transaction`. On DuckDB
2.0 this error, like any error, aborts the transaction: roll back, run the load again and call
the function after `COMMIT`. A transaction that has only read data, or has written only TEMP
tables, runs them on the committed data. Outside a transaction (autocommit) nothing changes:

```sql
CREATE TABLE cold_store AS SELECT i AS reading_id, 3.5 + (i % 10) * 0.1 AS temperature_c FROM range(100) t(i);
BEGIN;
INSERT INTO cold_store VALUES (100, 19.8);
SELECT * FROM isolation_forest('cold_store', 'temperature_c');
ROLLBACK;
```

```text
Binder Error: isolation_forest reads the last committed data, but this transaction has uncommitted changes; call it after COMMIT, or outside a transaction

LINE 1: SELECT * FROM isolation_forest('cold_store', 'temperature_c');
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

The other functions, such as `volume` or `null_rate`, read the table in your transaction and see
the uncommitted rows.

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
  them in `nan_count` / `inf_count` and computes its statistics, `min_val`, `max_val` and
  `top_values` over the finite values, the
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

### NULL or error

The rule of thumb: a bad *value* (a row of your data) gives `false`, `NULL` or a `reason`; a bad
*option* (a style, format, mode or strategy you typed) raises an error. Money is the exception: an
unknown currency code raises, because a money value without a valid currency cannot exist.

| Module | Bad value | Bad option |
|--------|-----------|------------|
| [Email](01-email.md) | `email_is_valid` is `false`; `email_validate` has `valid = false` and a `reason`. A DNS or SMTP check that gets no answer is `NULL` (`valid = NULL`) | An unknown mode raises (`Unknown email validation mode: ...`) |
| [Postal](02-postal.md) | No error: parts that libpostal does not find are `NULL` | none; missing data starts the download, and a failed download raises |
| [Phone](03-phone.md) | `phonenumber_is_valid` is `false`; `phonenumber_format` and `phonenumber_region` are `NULL`; `phonenumber_parse` has `valid = false` and a `reason`. An unsupported `region` argument makes the row invalid (`unsupported_region`) | An unknown format raises; an unsupported code in `SET anofox_tab_phonenumber_default_region` raises |
| [Money](04-money.md) | An unknown currency code raises in `money` and `money_from_cents`; arithmetic on two currencies raises | An unknown `money_format` style raises |
| [VAT](05-vat.md) | `vat_is_valid` is `false`; `vat_validate` has `valid = false` and a `reason`; `vat_format`, `vat_split` and `vat_country_name` are `NULL`; `vat_normalize` only cleans the text and does not validate | An unknown `vat_format` style raises |
| [PII](06-pii.md) | No error: text without PII gives an empty list or the unchanged text | An unknown mask strategy raises |
| Table functions ([checks](07-data-quality.md), [anomaly](08-anomaly-detection.md), [diff](09-diffing.md), [profile](10-profiling.md)) | Data never raises; checks report `fail` | An unknown table or column raises; `run_checks` reports it in an `error` row |

`TRY()` turns a raised error into `NULL` only for errors raised while the query runs. A binder
error, for example a function called with the wrong number or types of arguments, still stops
the query:

```sql
SELECT TRY(money('1250.00', 'EUR')) AS m;
```

```text
Binder Error: Could not choose a best candidate function for the function call "money(STRING_LITERAL, STRING_LITERAL)". In order to select one, please add explicit type casts.
	Candidate functions:
	money(amount DOUBLE, currency_code VARCHAR, /) -> STRUCT(amount DECIMAL(18,3), currency VARCHAR)
	money(amount DECIMAL(18,3), currency_code VARCHAR, /) -> STRUCT(amount DECIMAL(18,3), currency VARCHAR)


LINE 1: SELECT TRY(money('1250.00', 'EUR')) AS m;
                   ^^^^^^^^^^^^^^^^^^^^^^^
```

Cast a string amount to `DECIMAL(18,3)` first:

```sql
SELECT TRY(money(CAST('1250.00' AS DECIMAL(18,3)), 'EUR')) AS m;
```

```text
┌────────────────────────────────────────────────┐
│                       m                        │
│ struct(amount decimal(18,3), currency varchar) │
├────────────────────────────────────────────────┤
│ {'amount': 1250.000, 'currency': EUR}          │
└────────────────────────────────────────────────┘
```

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

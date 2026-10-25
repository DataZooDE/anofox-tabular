# Getting Started

> Install anofox-tabular and run your first checks on data you create yourself.

**Use this guide to:**
- Install and load the extension in DuckDB
- Validate values with scalar functions
- Run a data quality check, a PII scan and an anomaly score on a small table
- Find the right page for each module

## Install

The extension is in the DuckDB community repository. It works with DuckDB v1.5.6 and DuckDB 2.0
on Linux (x64), macOS (arm64 and x64) and Windows (x64).

```sql no-run
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;

-- later, to upgrade to the newest release
FORCE INSTALL anofox_tabular FROM community;
```

In the DuckDB CLI the extension prints a few `[anofox] [info]` lines when it loads. They go to
stderr. To hide them:

```sql
SET anofox_tab_trace_level = 'warn';
```

For Python, see [the Python guide](06-python.md).

## Validate single values

Scalar functions work on one value at a time, like any DuckDB function:

```sql
SELECT email_is_valid('user@example.com') AS email_ok,
       vat_is_valid('DE111111125') AS vat_ok,
       phonenumber_format('6502530000', 'US', 'INTERNATIONAL') AS phone,
       pii_mask('Call +1 650-253-0000 or mail ana@example.com') AS masked;
```

```text
┌──────────┬─────────┬─────────────────┬──────────────────────────────┐
│ email_ok │ vat_ok  │      phone      │            masked            │
│ boolean  │ boolean │     varchar     │           varchar            │
├──────────┼─────────┼─────────────────┼──────────────────────────────┤
│ true     │ true    │ +1 650 253 0000 │ Call [PHONE] or mail [EMAIL] │
└──────────┴─────────┴─────────────────┴──────────────────────────────┘
```

Every function also has a long name with the prefix `anofox_tab_`, for example
`anofox_tab_email_is_valid`. See [API conventions](../api/00-conventions.md).

## Create a small table

The rest of this guide uses one table of customer orders:

```sql
CREATE TABLE orders AS
SELECT
    i AS order_id,
    DATE '2026-01-01' + (i % 30)::INTEGER AS order_date,
    CASE WHEN i % 10 = 0 THEN NULL ELSE 'customer' || (i % 7) || '@example.com' END AS email,
    CASE WHEN i = 77 THEN 5000.0 ELSE 20.0 + (i % 13) END AS amount,
    CASE WHEN i % 25 = 0 THEN 'SSN 123-45-6789' ELSE 'ok' END AS note
FROM range(1, 201) t(i);
```

## Check the data

The check functions return one row with a `status` of `pass` or `fail` and a message:

```sql
SELECT status, message FROM null_rate('orders', 'email', 0.05);
```

```text
┌─────────┬─────────────────────────────────────────────┐
│ status  │                   message                   │
│ varchar │                   varchar                   │
├─────────┼─────────────────────────────────────────────┤
│ fail    │ Null rate 0.1 (20/200) exceeds maximum 0.05 │
└─────────┴─────────────────────────────────────────────┘
```

Ten percent of the emails are missing, more than the 5% allowed. Check the format of the ones
that are present:

```sql
SELECT status, match_rate, message
FROM regex_match('orders', 'email', '^[^@]+@[^@]+$', 0.99);
```

```text
┌─────────┬────────────┬─────────────────────────────────────────────────────┐
│ status  │ match_rate │                       message                       │
│ varchar │   double   │                       varchar                       │
├─────────┼────────────┼─────────────────────────────────────────────────────┤
│ pass    │        1.0 │ Match rate 1.0 (180/180) is within acceptable range │
└─────────┴────────────┴─────────────────────────────────────────────────────┘
```

There are 17 checks. You can also store many checks in a table and run them with one call; see
[Data quality checks](02-data-quality-checks.md).

## Find PII

`pii_scan_table` reports which columns contain personal data:

```sql
SELECT column_name, pii_type, match_count
FROM pii_scan_table('orders')
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬──────────┬─────────────┐
│ column_name │ pii_type │ match_count │
│   varchar   │ varchar  │    int64    │
├─────────────┼──────────┼─────────────┤
│ email       │ EMAIL    │         180 │
│ note        │ US_SSN   │           8 │
└─────────────┴──────────┴─────────────┘
```

See [PII detection and masking](04-pii.md).

## Find unusual values

Isolation Forest scores every row. A fixed seed makes the scores reproducible:

```sql
SELECT order_id, value AS amount, round(anomaly_score, 3) AS score
FROM isolation_forest('orders', 'amount', 100, 256, 0.01, 'scores', 42, id_column := 'order_id')
WHERE is_anomaly
ORDER BY score DESC;
```

```text
┌──────────┬────────┬────────┐
│ order_id │ amount │ score  │
│  int64   │ double │ double │
├──────────┼────────┼────────┤
│       77 │ 5000.0 │  0.931 │
└──────────┴────────┴────────┘
```

See [Anomaly detection](03-anomaly-detection.md).

## Profile the table

```sql
SELECT column_name, column_type, null_rate, distinct_count, pattern_summary
FROM profile_table('orders')
ORDER BY column_name;
```

```text
┌─────────────┬───────────────┬───────────┬────────────────┬─────────────────┐
│ column_name │  column_type  │ null_rate │ distinct_count │ pattern_summary │
│   varchar   │    varchar    │  double   │     int64      │     varchar     │
├─────────────┼───────────────┼───────────┼────────────────┼─────────────────┤
│ amount      │ DECIMAL(21,1) │       0.0 │             14 │ NULL            │
│ email       │ VARCHAR       │       0.1 │              7 │ email           │
│ note        │ VARCHAR       │       0.0 │              2 │ mixed           │
│ order_date  │ DATE          │       0.0 │             30 │ NULL            │
│ order_id    │ BIGINT        │       0.0 │            200 │ NULL            │
└─────────────┴───────────────┴───────────┴────────────────┴─────────────────┘
```

## List the functions

`duckdb_functions()` lists what the extension registers. Each module has its own prefix:

```sql
SELECT DISTINCT function_name
FROM duckdb_functions()
WHERE function_name LIKE 'anofox_tab_money%'
ORDER BY function_name
LIMIT 5;
```

```text
┌───────────────────────────┐
│       function_name       │
│          varchar          │
├───────────────────────────┤
│ anofox_tab_money          │
│ anofox_tab_money_abs      │
│ anofox_tab_money_add      │
│ anofox_tab_money_amount   │
│ anofox_tab_money_currency │
└───────────────────────────┘
```

## Next steps

- [Data quality checks](02-data-quality-checks.md): suites of checks, history, time-aware checks
- [Anomaly detection](03-anomaly-detection.md): Isolation Forest, DBSCAN, OutlierTree
- [PII detection and masking](04-pii.md): find, audit and mask personal data
- [Diffing and migration](05-diffing-and-migration.md): compare two versions of a table
- [Python](06-python.md): the `anofox` package
- [API reference](../api/00-conventions.md) and [configuration](../reference/configuration.md)
- [Network and privacy](../reference/network-and-privacy.md): what uses the network, and how to
  turn it off

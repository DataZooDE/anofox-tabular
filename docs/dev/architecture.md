# Architecture

> How the extension is organized: modules, source files, registration and shared helpers.

**Use this page to:**
- Find the source of a module
- Understand how functions and settings are registered
- Know the external libraries each module uses

## Overview

```text
┌──────────────────────────────────────────────────────────────────────────┐
│                              DuckDB SQL                                  │
├──────────────────────────────────────────────────────────────────────────┤
│ anofox_tabular extension                                                 │
│                                                                          │
│  Validation          Email · Postal · Phone · Money · VAT                │
│  Privacy             PII (patterns, checksums, NER)                      │
│  Data quality        Checks · Check suites · Events                      │
│  Analysis            Anomaly detection · Diffing · Profiling             │
│                                                                          │
│  Shared              aliases · id_column · SQL quoting · tracing ·       │
│                      telemetry · banner · integration contract           │
├──────────────────────────────────────────────────────────────────────────┤
│ libpostal · c-ares · OpenSSL · curl · spdlog · OpenVINO · duckdb-compat  │
└──────────────────────────────────────────────────────────────────────────┘
```

## The 10 modules

| Module | Source | External libraries | API page |
|--------|--------|--------------------|----------|
| Email | `anofox_email.cpp`, `anofox_email_dns.cpp`, `anofox_email_smtp.cpp` | c-ares (DNS); sockets for SMTP | [01-email](../api/01-email.md) |
| Postal | `anofox_postal.cpp` | libpostal, curl (data download) | [02-postal](../api/02-postal.md) |
| Phone | `anofox_phonenumber.cpp`, `anofox_phonenumber_metadata.cpp` | none: own implementation with metadata for 21 regions | [03-phone](../api/03-phone.md) |
| Money | `anofox_money.cpp`, `anofox_money_currency.cpp` | none | [04-money](../api/04-money.md) |
| VAT | `anofox_vat.cpp`, `anofox_vat_country.cpp`, `anofox_vat_checksum.cpp` | none | [05-vat](../api/05-vat.md) |
| PII | `anofox_pii.cpp`, `anofox_ner.cpp` | OpenSSL (hash masking), OpenVINO (NER, optional), curl (model download) | [06-pii](../api/06-pii.md) |
| Data quality | `anofox_metric.cpp`, `anofox_check.cpp`, `anofox_check_suite.cpp` | none | [07-data-quality](../api/07-data-quality.md) |
| Anomaly detection | `anofox_isolation_forest.cpp`, `anofox_dbscan.cpp`, `anofox_outlier_tree.cpp` (registered from `anofox_metric.cpp` and `anofox_outlier_tree.cpp`) | none | [08-anomaly-detection](../api/08-anomaly-detection.md) |
| Diffing | `anofox_diff.cpp` | none | [09-diffing](../api/09-diffing.md) |
| Profiling | `anofox_profile.cpp` | none | [10-profiling](../api/10-profiling.md) |

Headers are in `src/include/anofox_*.hpp`. OpenVINO and libpostal are optional at build time:
without OpenVINO the NER types are not detected, and without libpostal the postal module is not
compiled.

## Registration

`src/anofox_tabular_extension.cpp` registers everything when the extension loads. For each
module it first registers the settings (`Register<Module>Options`), then the functions
(`Register<Module>Functions`). It also registers the telemetry settings, the integration
contract (`anofox_contract_version`) and the banner setting.

Every function is registered twice: under the canonical `anofox_tab_*` name and under its short
alias. `src/include/anofox_function_alias.hpp` provides the helpers that do this, so new
functions should register through them.

## Two kinds of table functions

- **SQL rewrites.** The checks and the diffs use `bind_replace`: at bind time they build a SQL
  query from the arguments, and DuckDB plans and runs it. Table and column names are quoted
  through `anofox_sql_utils`. The profiling functions also generate SQL, but first read the table
  schema through a second connection.
- **Native table functions.** Isolation Forest, DBSCAN, OutlierTree and the PII table scans
  read the input through a second connection, compute in C++, and stream the result. That second
  connection cannot see TEMP tables of the caller.

## Shared helpers

| File | Purpose |
|------|---------|
| `anofox_function_alias.hpp` | Register a function under both names |
| `anofox_id_column.cpp` | The `id_column` named parameter of the row-level outputs |
| `anofox_sql_utils.cpp` | Quoting of identifiers and literals in generated SQL |
| `anofox_contract.cpp` | `anofox_contract_version` and the contract tags |
| `anofox_trace.cpp` | Tracing via spdlog, settings `anofox_tab_trace_*` |
| `anofox_raii.hpp`, `anofox_duckdb_vectors.hpp` | Resource and vector helpers |
| `posthog-telemetry/` (submodule) | Telemetry, see [TELEMETRY.md](../../TELEMETRY.md) |
| `datazoo-banner/` (submodule) | The feedback banner and `datazoo_banner` |
| `duckdb-compat/` (submodule) | Shims for API differences between DuckDB v1.5.6 and 2.0 |

## Python package

`python/` holds the `anofox-tabular` package. It downloads the extension, opens a connection
with it loaded, and wraps the SQL functions for pandas and polars DataFrames. See
[the Python guide](../guides/06-python.md).

## Next steps

- [Building](building.md)
- [Testing](testing.md)
- [Updating DuckDB](updating-duckdb.md)

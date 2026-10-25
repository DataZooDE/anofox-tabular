# API Reference

> The function reference now lives in one page per module under [docs/api/](api/00-conventions.md).

This file is kept so that old links still land somewhere. Find the module of the section you
were looking for below.

| Old section | New page |
|-------------|----------|
| Function naming and parameter conventions | [00 Conventions](api/00-conventions.md) |
| Email Validation | [01 Email](api/01-email.md) |
| Address Parsing & Normalization | [02 Postal](api/02-postal.md) |
| Phone Number Validation | [03 Phone](api/03-phone.md) |
| Money & Currency Operations | [04 Money](api/04-money.md) |
| VAT Validation | [05 VAT](api/05-vat.md) |
| PII Detection | [06 PII](api/06-pii.md) |
| Data Quality Metrics | [07 Data quality](api/07-data-quality.md) |
| Check Suites | [07 Data quality: `run_checks`](api/07-data-quality.md#run_checks) |
| anofox Integration Contract | [11 Integration contract](api/11-integration-contract.md) |
| Anomaly Detection | [08 Anomaly detection](api/08-anomaly-detection.md) |
| Identifier pass-through (`id_column`) | [08 Anomaly detection](api/08-anomaly-detection.md#identifier-pass-through-id_column) and [00 Conventions](api/00-conventions.md#named-parameters) |
| Data Diffing | [09 Diffing](api/09-diffing.md) |
| Data Profiling | [10 Profiling](api/10-profiling.md) |
| Configuration | [Configuration reference](reference/configuration.md) |
| Telemetry | [Network and privacy](reference/network-and-privacy.md#telemetry) |
| Function Coverage Matrix | `SELECT DISTINCT function_name FROM duckdb_functions() WHERE function_name LIKE 'anofox%'`; every name is documented, which `test/sql/docs_coverage.test` checks |

See [docs/README.md](README.md) for the whole documentation map.

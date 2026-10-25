# anofox-tabular Documentation

> A map of the docs: guides for tasks, API pages per module, reference pages and developer notes.

New to the extension? Start with [Getting started](guides/01-getting-started.md).

## Guides

Task-oriented walkthroughs. Every example creates its own data.

| Guide | What you do |
|-------|-------------|
| [01 Getting started](guides/01-getting-started.md) | Install, load, and run a first check, PII scan and anomaly score |
| [02 Data quality checks](guides/02-data-quality-checks.md) | Run single checks, then a table-driven suite with a result history |
| [03 Anomaly detection](guides/03-anomaly-detection.md) | Score rows, explain outliers, gate a pipeline |
| [04 PII](guides/04-pii.md) | Find, audit and mask personal data |
| [05 Diffing and migration](guides/05-diffing-and-migration.md) | Compare two versions of a table and sign off a migration |
| [06 Python](guides/06-python.md) | Use the `anofox` package with pandas DataFrames |
| [07 Clean a customer master](guides/07-customer-master-data.md) | Validate and standardise emails, phones, VAT numbers and money |

## API reference

One page per module. Each function has its signature, parameters, output columns and an example.

| Page | Covers |
|------|--------|
| [00 Conventions](api/00-conventions.md) | Names and aliases, positional and named parameters, types, TEMP tables, tracing |
| [01 Email](api/01-email.md) | `email_is_valid`, `email_validate`, `email_config` |
| [02 Postal](api/02-postal.md) | Address parsing and expansion with libpostal |
| [03 Phone](api/03-phone.md) | Phone number parsing, validation and formatting |
| [04 Money](api/04-money.md) | Money values, currency-safe arithmetic, formatting |
| [05 VAT](api/05-vat.md) | European VAT number validation |
| [06 PII](api/06-pii.md) | PII detection, validation, masking, table scans, NER |
| [07 Data quality](api/07-data-quality.md) | 17 checks, `_events` companions, `run_checks` |
| [08 Anomaly detection](api/08-anomaly-detection.md) | Isolation Forest, DBSCAN, OutlierTree, `id_column` |
| [09 Diffing](api/09-diffing.md) | `diff_joindiff`, `diff_hashdiff` |
| [10 Profiling](api/10-profiling.md) | `profile_table`, `profile_summary`, `profile_correlations` |
| [11 Integration contract](api/11-integration-contract.md) | Output schemas shared with other anofox extensions |

## Reference

| Page | Covers |
|------|--------|
| [Configuration](reference/configuration.md) | Every setting with type and default |
| [Network and privacy](reference/network-and-privacy.md) | What uses the network, telemetry, offline use |
| [Troubleshooting](reference/troubleshooting.md) | Common errors and fixes |

## Development

| Page | Covers |
|------|--------|
| [Building](dev/building.md) | Source build with vcpkg and ninja |
| [Testing](dev/testing.md) | SQL tests, NER tests, the docs coverage test, Python tests |
| [Architecture](dev/architecture.md) | Modules, source files, registration |
| [Updating DuckDB](dev/updating-duckdb.md) | Supporting DuckDB v1.5.6 and 2.0 from one tree |

The old single-file reference [API_REFERENCE.md](API_REFERENCE.md) is being replaced by the
pages above.

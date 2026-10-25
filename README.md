<h1 align="center">Anofox Tabular</h1>
<p align="center"><strong>Data quality, validation, PII detection and anomaly detection for DuckDB</strong></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-BSL%201.1-blue.svg" alt="License: BSL 1.1"></a>
  <a href="https://duckdb.org"><img src="https://img.shields.io/badge/DuckDB-1.5.6%20%7C%202.0-green.svg" alt="DuckDB 1.5.6 | 2.0"></a>
  <a href="https://pypi.org/project/anofox-tabular/"><img src="https://img.shields.io/pypi/v/anofox-tabular.svg" alt="PyPI"></a>
  <img src="https://img.shields.io/badge/modules-10-blue.svg" alt="10 modules">
</p>

Anofox Tabular is a DuckDB extension with about 100 SQL functions for checking and cleaning
tabular data. It validates emails, phone numbers, addresses, VAT numbers and money values, runs
data quality checks, finds and masks personal data, scores anomalies, diffs tables and profiles
columns. Everything runs inside DuckDB, on your tables, with SQL.

## Key Features

| Module | What it does | Docs |
|--------|--------------|------|
| Email | Validate addresses by regex, DNS MX lookup or SMTP handshake | [api/01-email](docs/api/01-email.md) |
| Postal | Parse and normalize addresses with libpostal | [api/02-postal](docs/api/02-postal.md) |
| Phone | Parse, validate, format and compare phone numbers for 21 regions | [api/03-phone](docs/api/03-phone.md) |
| Money | Exact `DECIMAL(18,3)` money values, currency-safe arithmetic, formatting for 38 currencies | [api/04-money](docs/api/04-money.md) |
| VAT | Validate VAT numbers of 28 countries (27 EU member states plus GB/XI): syntax for all, check digits for 16 of them | [api/05-vat](docs/api/05-vat.md) |
| PII | Detect and mask emails, card numbers, IBANs, SSNs, names and more; scan whole tables | [api/06-pii](docs/api/06-pii.md) |
| Data quality | 17 checks, a table-driven suite runner and event outputs | [api/07-data-quality](docs/api/07-data-quality.md) |
| Anomaly detection | Isolation Forest, DBSCAN and OutlierTree with explanations | [api/08-anomaly-detection](docs/api/08-anomaly-detection.md) |
| Diffing | Row-level diff of two tables on a primary key | [api/09-diffing](docs/api/09-diffing.md) |
| Profiling | Per-column statistics, table overview, correlations | [api/10-profiling](docs/api/10-profiling.md) |

## Table of Contents

- [Installation](#installation)
- [Quick Start](#quick-start)
- [Conventions](#conventions)
- [Network and Privacy](#network-and-privacy)
- [Python](#python)
- [Documentation](#documentation)
- [Development](#development)
- [Support and Feedback](#support-and-feedback)
- [License](#license)
- [Citation](#citation)
- [Acknowledgments](#acknowledgments)

## Installation

### Community extension

```sql
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;
```

To upgrade to the newest release:

```sql
FORCE INSTALL anofox_tabular FROM community;
```

Supported: DuckDB v1.5.6 and 2.0 on `linux_amd64`, `osx_arm64`, `osx_amd64` and
`windows_amd64`. Releases use date versions such as `v2026.10.25`.

Some parts depend on the platform: the NER-based PII types (names, organizations, locations)
need OpenVINO, which the Linux x64 and macOS x64 builds include, and the postal module is not
available on Windows.

### Python

```bash
pip install anofox-tabular      # or: uv add anofox-tabular
```

The package is imported as `anofox`. See [Python](#python).

### From source

See [docs/dev/building.md](docs/dev/building.md).

## Quick Start

This runs as pasted in the DuckDB CLI after the `INSTALL` above. It creates its own data.

```sql
LOAD anofox_tabular;
SET anofox_tab_trace_level = 'warn';   -- optional: hide the [anofox] [info] trace lines

-- Validate contact data
SELECT email_is_valid('user@example.com') AS email_ok,
       vat_is_valid('DE136695976') AS vat_ok;

-- Find and mask PII in free text
SELECT pii_mask('Reach jane@corp.com, card 4111 1111 1111 1111', 'redact') AS masked;

-- A data quality check: one pass/fail row with a message
CREATE TABLE orders AS
  SELECT i AS id,
         CASE WHEN i % 50 = 0 THEN NULL ELSE i % 7 END AS customer_id,
         CASE WHEN i = 77 THEN 99999.0 ELSE 20 + i % 13 END AS amount
  FROM range(1, 501) t(i);
SELECT status, message FROM null_rate('orders', 'customer_id', 0.01);

-- Anomaly scores with Isolation Forest
-- (trees, sample size, contamination, output mode, seed)
SELECT row_id, value, round(anomaly_score, 3) AS score, is_anomaly
FROM isolation_forest('orders', 'amount', 100, 256, 0.01, 'scores', 42)
ORDER BY anomaly_score DESC
LIMIT 3;

-- Compare two versions of a table
CREATE TABLE customers_v1 AS
  FROM (VALUES (1, 'Ada', 'ada@example.com'),
               (2, 'Bob', 'bob@example.com'),
               (3, 'Cy',  'cy@example.com')) t(id, name, email);
CREATE TABLE customers_v2 AS
  FROM (VALUES (1, 'Ada', 'ada@example.com'),
               (2, 'Bob', 'robert@example.com'),
               (4, 'Dee', 'dee@example.com')) t(id, name, email);
SELECT * FROM diff_joindiff('customers_v1', 'customers_v2', 'id') ORDER BY id;
```

Expected output:

```
┌──────────┬─────────┐
│ email_ok │ vat_ok  │
├──────────┼─────────┤
│ true     │ true    │
└──────────┴─────────┘
┌───────────────────────────────────┐
│              masked               │
├───────────────────────────────────┤
│ Reach [EMAIL], card [CREDIT_CARD] │
└───────────────────────────────────┘
┌─────────┬──────────────────────────────────────────────┐
│ status  │                   message                    │
├─────────┼──────────────────────────────────────────────┤
│ fail    │ Null rate 0.02 (10/500) exceeds maximum 0.01 │
└─────────┴──────────────────────────────────────────────┘
┌────────┬─────────┬────────┬────────────┐
│ row_id │  value  │ score  │ is_anomaly │
├────────┼─────────┼────────┼────────────┤
│     77 │ 99999.0 │  0.752 │ true       │
│     25 │    32.0 │  0.557 │ false      │
│     38 │    32.0 │  0.557 │ false      │
└────────┴─────────┴────────┴────────────┘
┌───────────┬───────┬─────────┬────────────────────┐
│ diff_type │  id   │  name   │       email        │
├───────────┼───────┼─────────┼────────────────────┤
│ changed   │     2 │ Bob     │ robert@example.com │
│ removed   │     3 │ NULL    │ NULL               │
│ added     │     4 │ Dee     │ dee@example.com    │
└───────────┴───────┴─────────┴────────────────────┘
```

The first PII call downloads the NER model (~66 MB) used for names, organizations and
locations. Run `SET anofox_ner_auto_download = false;` first to skip it; the pattern-based
types (email, credit card, IBAN, …) work without it, and the output above is the same.

To try the Python example scripts, see [`examples/README.md`](examples/README.md).

More: [Getting started](docs/guides/01-getting-started.md).

## Conventions

Every function has a canonical name with the prefix `anofox_tab_` and a short alias, so
`vat_is_valid` and `anofox_tab_vat_is_valid` are the same function. Arguments are positional;
`id_column`, `start_date`/`end_date` and every `diff_*` parameter also accept the named form
`name := value`. Table functions take the table name as a string, and a few of them need a
regular table instead of a TEMP table; see [API conventions](docs/api/00-conventions.md).

## Network and Privacy

The core functions run locally and send nothing. These parts use the network:

| Feature | Network use | Opt out |
|---------|-------------|---------|
| PII names, organizations, locations (NER) | Downloads a ~66 MB model from Hugging Face on first use | `SET anofox_ner_auto_download = false` |
| Postal addresses | Downloads ~500 MB of libpostal data on first use or with `postal_load_data()` | Do not call the postal functions |
| Email modes `dns` and `smtp` | DNS query; SMTP connection to the mail host | Use mode `regex` (the default) |
| Telemetry | Anonymous usage events to PostHog EU; on by default | `SET anofox_telemetry_enabled = false` or `DATAZOO_DISABLE_TELEMETRY=1` |

Telemetry sends the extension and DuckDB version, the platform and per-function call counts,
never your data, table names or SQL. The environment variable also stops the event sent when
the extension loads. Details: [Network and privacy](docs/reference/network-and-privacy.md) and
[TELEMETRY.md](TELEMETRY.md).

## Python

```python
import anofox
import pandas as pd
from anofox import validate, anomaly

conn = anofox.connect()  # downloads the extension on first use

customers = pd.DataFrame({
    "id": [1, 2, 3],
    "email": ["ana@example.com", "bob(at)example.com", "carla@example.org"],
})
print(validate.email_is_valid(conn, customers, column="email"))

readings = pd.DataFrame({
    "sensor_id": range(1, 21),
    "value": [10.0 + (i % 5) for i in range(19)] + [95.0],
})
scores = anomaly.isolation_forest(
    conn, readings, "value",
    output_mode="scores", n_trees=100, sample_size=16, contamination=0.05,
    id_column="sensor_id",
)
print(scores[scores["is_anomaly"]][["sensor_id", "value"]])
```

The extension also loads into any DuckDB client (R, Node.js, Java, Go, Rust, ...) with
`INSTALL anofox_tabular FROM community`. See the [Python guide](docs/guides/06-python.md)
and [python/README.md](python/README.md).

## Documentation

The full map is in [docs/README.md](docs/README.md).

### Guides

| Guide | Topic |
|-------|-------|
| [Getting started](docs/guides/01-getting-started.md) | Install and run a first check, PII scan and anomaly score |
| [Data quality checks](docs/guides/02-data-quality-checks.md) | Single checks, check suites, result history |
| [Anomaly detection](docs/guides/03-anomaly-detection.md) | Score, explain and gate on outliers |
| [PII](docs/guides/04-pii.md) | Find, audit and mask personal data |
| [Diffing and migration](docs/guides/05-diffing-and-migration.md) | Compare two versions of a table |
| [Python](docs/guides/06-python.md) | The `anofox` package with DataFrames |
| [Customer master data](docs/guides/07-customer-master-data.md) | Clean emails, phones, VAT numbers and money |

### API reference

| Page | Page | Page |
|------|------|------|
| [00 Conventions](docs/api/00-conventions.md) | [04 Money](docs/api/04-money.md) | [08 Anomaly detection](docs/api/08-anomaly-detection.md) |
| [01 Email](docs/api/01-email.md) | [05 VAT](docs/api/05-vat.md) | [09 Diffing](docs/api/09-diffing.md) |
| [02 Postal](docs/api/02-postal.md) | [06 PII](docs/api/06-pii.md) | [10 Profiling](docs/api/10-profiling.md) |
| [03 Phone](docs/api/03-phone.md) | [07 Data quality](docs/api/07-data-quality.md) | [11 Integration contract](docs/api/11-integration-contract.md) |

### Reference

- [Configuration](docs/reference/configuration.md): all settings with type and default
- [Network and privacy](docs/reference/network-and-privacy.md)
- [Troubleshooting](docs/reference/troubleshooting.md)

## Development

Build, test and architecture notes are in [docs/dev/](docs/dev/):
[building](docs/dev/building.md), [testing](docs/dev/testing.md),
[architecture](docs/dev/architecture.md) and [updating DuckDB](docs/dev/updating-duckdb.md).
Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).

## Support and Feedback

- Documentation: [docs/](docs/README.md)
- Issues: [GitHub Issues](https://github.com/DataZooDE/anofox-tabular/issues)
- Discussions: [GitHub Discussions](https://github.com/DataZooDE/anofox-tabular/discussions)

If `anofox_tabular` misbehaves (a profile that looks wrong, a PII match it missed, a diff that
surprises you), please [open an issue](https://github.com/DataZooDE/anofox-tabular/issues).
Results depend on real data shapes we cannot reproduce here, so a report with a small sample of
your input is the fastest path to a fix. If it saved you time, a star on the repo helps other
people find it.

The first time you load the extension in an interactive terminal each day, a small banner says
the same. It never prints when output is piped, in notebooks or in CI. Silence it with
`SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

## License

Business Source License 1.1. Production use is permitted, except offering the extension to
third parties on a hosted or embedded basis. Each version converts to MPL 2.0 five years after
its first publication. See [LICENSE](LICENSE) for the full terms and
[THIRD_PARTY_NOTICE](THIRD_PARTY_NOTICE) for the licenses of the bundled libraries.

## Citation

If you use this extension in research, please cite:

```bibtex
@software{anofox_tabular,
  title  = {Anofox Tabular: Data Quality and Validation for DuckDB},
  author = {Joachim Rosskopf and DataZoo GmbH},
  year   = {2026},
  url    = {https://github.com/DataZooDE/anofox-tabular}
}
```

## Acknowledgments

Built on:

- [DuckDB](https://duckdb.org) and its extension framework
- [libpostal](https://github.com/openvenues/libpostal) for address parsing
- [OpenVINO](https://github.com/openvinotoolkit/openvino) and a DistilBERT NER model for
  name, organization and location detection
- [c-ares](https://github.com/c-ares/c-ares) for DNS lookups, [spdlog](https://github.com/gabime/spdlog)
  for logging, [curl](https://curl.se/) and [OpenSSL](https://www.openssl.org/)
- [isotree](https://github.com/david-cortes/isotree), whose ideas (Extended IF, SCiForest,
  density scoring) shaped the Isolation Forest options
- Google's [libphonenumber](https://github.com/google/libphonenumber), the model for the phone
  module

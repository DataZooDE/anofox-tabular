# Anofox Tabular

**Data quality, validation and anomaly detection for DuckDB**

[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](LICENSE)
[![DuckDB](https://img.shields.io/badge/DuckDB-1.5.6%20%7C%202.0-green.svg)](https://duckdb.org/)

A DuckDB extension with about 100 SQL functions in 10 modules: data quality checks and check
suites, column profiling, table diffing, anomaly detection (Isolation Forest, DBSCAN,
OutlierTree), PII detection and masking, and validators for emails, postal addresses, phone
numbers, EU VAT IDs and money. Everything is plain SQL. The core functions run locally inside
DuckDB; [Network and privacy](#-network-and-privacy) lists the few features that use the network.

Every function has a canonical `anofox_tab_*` name and a short alias, for example
`anofox_tab_email_is_valid` and `email_is_valid`. The examples use the short names.

---

## Key Features

**Validation & Normalization:**
- Email validation with optional DNS/SMTP verification
- International address parsing via libpostal
- Phone number parsing, validation and formatting (built-in, no external library)
- EU VAT validation (28 countries: the 27 EU member states plus GB, with XI for Northern Ireland)
- Multi-currency arithmetic (10 currencies)

**Data Quality & Anomaly Detection:**
- 17 data quality checks (volume, nulls, distinct counts, freshness, schema, z-score and IQR outliers, regex, value sets, aggregates, duplicates, occurrence, referential integrity, custom predicates, time-aware baselines), a table-driven check-suite runner (`run_checks`) and `_events` outputs for the time-aware checks
- Isolation Forest with categorical support, Extended IF and SCiForest
- DBSCAN density-based clustering
- OutlierTree with human-readable explanations

**PII, Diffing & Profiling:**
- Detect and mask 17 PII types (pattern-based plus NER-based names, organizations and locations)
- Hash-based and join-based table diffing for migration validation
- Column profiling, table summaries and correlations, including nested types

---

## 📑 Table of Contents

- [Installation](#-installation)
- [Quick Start](#-quick-start)
- [Network and privacy](#-network-and-privacy)
- [Feature Overview](#-feature-overview)
- [Features](#-features---all-10-modules)
  - [Email Validation](#-email-validation)
  - [Address Parsing](#-address-parsing--normalization)
  - [Phone Number Validation](#-phone-number-validation)
  - [Money & Currency Operations](#-money--currency-operations)
  - [VAT Validation](#-vat-validation)
  - [PII Detection](#%EF%B8%8F-pii-detection)
  - [Data Quality Metrics](#-data-quality-metrics)
  - [Anomaly Detection](#-anomaly-detection)
  - [Data Diffing](#-data-diffing)
  - [Data Profiling](#-data-profiling)
- [Real-World Examples](#-real-world-examples)
- [SQL Function Reference](#-sql-function-reference)
- [Configuration](#%EF%B8%8F-configuration)
- [Building from Source](#%EF%B8%8F-building-from-source)
- [Testing](#-testing)
- [Use Cases](#-use-cases)
- [Architecture](#%EF%B8%8F-architecture)
- [Contributing](#-contributing)
- [Troubleshooting](#-troubleshooting)
- [Performance Tips](#-performance-tips)
- [Telemetry](#-telemetry)
- [Feedback](#feedback)
- [License](#-license)

---

## 📦 Installation

### Community Extension

```sql
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;
```

To upgrade an installed copy, run `FORCE INSTALL anofox_tabular FROM community;`. A plain
`INSTALL` does nothing when a build is already cached.

Builds are published for DuckDB v1.5.6 and 2.0 on linux_amd64, osx_arm64, osx_amd64 and
windows_amd64.

### Python Package

```bash
pip install anofox-tabular        # or: uv add anofox-tabular
```

The package is imported as `anofox`. `anofox.connect()` returns a DuckDB connection with the
extension loaded and downloads the extension for your `duckdb` version on first use:

```python
import anofox
from anofox import validate

conn = anofox.connect()
print(validate.email_is_valid(conn, "hi@example.com"))   # True
```

See [`python/README.md`](python/README.md) for the full Python API.

### From Source

See [Building from Source](#%EF%B8%8F-building-from-source).

---

## 🚀 Quick Start

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

---

## 🔒 Network and privacy

The core functions run locally. These features use the network:

| Feature | What it fetches | Opt out |
|---|---|---|
| PII name/organization/location detection (NER) | DistilBERT model, ~66 MB, from Hugging Face on first use, into `~/.duckdb/extensions/anofox/ner/` | `SET anofox_ner_auto_download = false` (status becomes `NOT_DOWNLOADED`; pattern recognizers keep working) |
| Address parsing (`postal_load_data()`, or the first `postal_parse_address` / `postal_expand_address` call) | libpostal data, ~500 MB | do not call the postal functions |
| `email_validate` / `email_is_valid` modes `dns` and `smtp` | DNS MX lookup / SMTP connection to the mail host | use mode `regex` (the default) |
| Telemetry | anonymous usage events to PostHog EU; on by default; see [TELEMETRY.md](TELEMETRY.md) | `SET anofox_telemetry_enabled = false` or env `DATAZOO_DISABLE_TELEMETRY=1` (env also suppresses the load event) |
| Feedback banner | none (local print, interactive terminals, once a day) | `SET datazoo_banner = false` |

Telemetry never includes query text, table or column names, or any data values. See
[Telemetry](#-telemetry) below.

---

## 📊 Feature Overview

Function counts are canonical `anofox_tab_*` names; each also has a short alias.

| Module | Functions | Use Case |
|--------|-----------|----------|
| 📧 **Email Validation** | 3 | Regex (RFC 5322 inspired), DNS and SMTP verification |
| 📮 **Address Parsing** | 4 | International address normalization (libpostal; not built on Windows) |
| 📞 **Phone Numbers** | 9 | Built-in parser and formatter |
| 💰 **Money & Currency** | 17 | Multi-currency operations, 10 currencies |
| 💼 **VAT Validation** | 10 | EU VAT validation, 28 countries |
| 🕵️ **PII Detection** | 24 | Detect & mask 17 PII types (SSN, IBAN, NAME, etc.) |
| 🔍 **Quality Checks** | 22 | 17 checks, 3 `_events` companions, the `run_checks` suite runner, `anofox_contract_version` |
| 🤖 **Anomaly Detection** | 5 | Isolation Forest, DBSCAN, OutlierTree (explainable) |
| 🔄 **Data Diffing** | 2 | Table comparison, migration validation |
| 📊 **Data Profiling** | 3 | Column stats, correlations, table overview |

### Multi-Language Accessibility

Works with any DuckDB language binding:
- **Python**: `pip install anofox-tabular` (native wrapper with DataFrame support) or the plain `duckdb` package
- **R**: `duckdb` package
- **Node.js**: `duckdb` package
- **Java**: DuckDB JDBC driver
- **Go, Rust, Julia, C++**: Native DuckDB bindings

---

## ✨ Features - All 10 Modules

### 📧 Email Validation

Multi-stage email verification with configurable validation modes:

- **Regex** - Fast RFC 5322-inspired syntax checking
- **DNS** - MX record verification with configurable timeouts
- **SMTP** - Full SMTP handshake validation with detailed transcripts

```sql
-- Quick validation
SELECT email_is_valid('john.doe@company.com', 'regex');

-- Detailed results with MX records and SMTP debug info
SELECT email_validate('support@example.org', 'smtp');
```

**Configuration:**
- Custom regex patterns
- DNS timeout/retry settings
- SMTP port, timeouts, HELO domain
- Transcript logging for debugging

[📖 See complete Email module documentation](#email-functions)

---

### 📮 Address Parsing & Normalization

> **Requires a one-time ~500 MB download.** Run `SELECT postal_load_data();` to fetch the libpostal
> data up front; otherwise the first postal call downloads it. The data goes to
> `anofox_tab_postal_data_path` (default `.duckdb/extensions/libpostal`, relative to the working
> directory; its parent directory must exist). The address module is not built on Windows.

Powered by **[libpostal](https://github.com/openvenues/libpostal)**, a statistical NLP library for parsing addresses:

```sql
-- Parse unstructured addresses into components
SELECT postal_parse_address('620 Bolger Place, The Burren, NSW 4726');
-- Returns: {house_number: '620', road: 'Bolger Place', city: 'The Burren', ...}

-- Generate normalized variants for fuzzy matching
SELECT postal_expand_address('123 Main St');
-- Returns: ['123 Main Street', '123 Main St', '123 main street', ...]
```

**Includes:**
- Support for international addresses
- Configurable data directory

[📖 See complete Postal module documentation](#postal-functions)

---

### 📞 Phone Number Validation

International phone parsing, validation and formatting with a built-in implementation (no external library):

```sql
-- Parse and validate phone numbers
SELECT phonenumber_parse('+1 (415) 555-1234', 'US');
-- Returns: {'valid': true, 'country_code': 1, 'national_number': 4155551234, 'region': US, 'type': fixed_line_or_mobile}

-- Format in different styles
SELECT phonenumber_format('4155551234', 'US', 'INTERNATIONAL');
-- Returns: '+1 415 555 1234'
```

**Formats:** E164, INTERNATIONAL, NATIONAL, RFC3966

[📖 See complete Phone module documentation](#phone-functions)

---

### 💰 Money & Currency Operations

International monetary value handling with currency-aware arithmetic and formatting.

**Key Features:**
- 17 SQL functions for financial operations
- 10 major currencies (USD, EUR, GBP, JPY, CAD, AUD, CHF, CNY, INR, BRL)
- Exact DECIMAL(18,3) amounts with overflow-checked, currency-safe arithmetic
- Locale-aware formatting (symbol placement, per-currency scale, decimal marks, thousands separators)
- Quality validation (range checks, currency consistency)

```sql
-- Create, format, and calculate with currency safety
SELECT
    money_format(
        money_add(
            money(100.00, 'EUR'),
            money(50.00, 'EUR')
        ),
        'symbol'
    ) as total;
-- Returns: 150,00 €
```

**Use Cases:**
- Multi-currency financial reporting
- Invoice calculations and reconciliation
- Data quality checks for transactions
- Currency validation and normalization

[📖 See complete Money module documentation](#money--currency-functions)

---

### 💼 VAT Validation

European VAT number validation for regulatory compliance and data quality.

**Key Features:**
- 10 SQL functions for VAT operations
- 28 countries: the 27 EU member states plus GB (`XI` for Northern Ireland; Greece is `EL`, `UK` is not a valid code)
- Syntax validation with country-specific regex patterns
- Check-digit (checksum) validation for 16 countries: AT, BE, DE, DK, ES, FI, FR, GR/EL, IE, IT, LU, NL, PL, PT, SE, SI
- EU membership checks (accepts lowercase codes and the EL/XI VAT aliases)
- Country name and information lookup

```sql
-- Validate and extract VAT information
SELECT
    vat_id,
    vat_is_valid(vat_id) as is_valid,
    vat_country_name((vat_split(vat_id)).country) as country
FROM customers;
```

**Use Cases:**
- Customer compliance verification
- B2B transaction validation
- International customer categorization
- Regulatory data quality checks

[📖 See complete VAT module documentation](#vat-validation-functions)

---

### 🕵️ PII Detection

Detect and mask Personally Identifiable Information (PII) in text data with 17 supported types.

**PII Types Detected:**
- **Pattern-based (13 types):** EMAIL, PHONE, CREDIT_CARD, US_SSN, IBAN, IP_ADDRESS, URL, MAC_ADDRESS, UK_NINO, US_PASSPORT, API_KEY, CRYPTO_ADDRESS, DE_TAX_ID
- **NER-based (4 types):** NAME, ORGANIZATION, LOCATION, MISC (using OpenVINO + DistilBERT)
  - Available on: Linux x64 (glibc), macOS (x64 & ARM64)
  - Not available on: Windows x64, Linux ARM64, Linux musl (Alpine)
  - On unsupported platforms: NAME uses dictionary fallback, other NER types unavailable
  - Inputs are truncated at the model's 512-token limit; entities beyond that are not detected
  - The built-in tokenizer is ASCII-focused (English model); non-ASCII punctuation is not treated as a word boundary
  - Model selection via `SET anofox_ner_model` (currently `distilbert-en` only) must happen before the model is first loaded
  - **Network access:** the first NER-based detection downloads the model (~66 MB) from Hugging Face into `~/.duckdb/extensions/anofox/ner/`. This applies to `pii_detect`, `pii_mask`, `pii_scan_table` and `pii_audit_table` unless `anofox_pii_enabled_types` is limited to pattern-based types. Offline, run `SET anofox_ner_auto_download = false` (see [NER Settings](#ner-settings)): NER detection is then skipped (NAME falls back to the dictionary) and nothing is downloaded

**Masking Strategies** (case-insensitive):
- `redact` - Replace with [TYPE] label (default)
- `partial` - Show partial value (e.g., `***-**-6789` for SSN)
- `asterisk` - Replace with same-length asterisks
- `hash` - Replace with SHA-256 hash (truncated)
- `none` - Leave the text unchanged

```sql
-- Detect all PII in text
SELECT unnest(pii_detect('My SSN is 123-45-6789 and email is test@example.com'));
-- Returns two rows: {'type': US_SSN, 'text': 123-45-6789, 'start_pos': 10, ...} and {'type': EMAIL, ...}

-- Mask PII with partial strategy
SELECT pii_mask('Contact: john@example.com, SSN: 123-45-6789', 'partial');
-- Returns: 'Contact: jo**@example.com, SSN: ***-**-6789'

-- Scan entire table for PII
SELECT * FROM pii_scan_table('customer_data');

-- Check if text contains any PII
SELECT pii_contains('This is safe text');
-- Returns: false
```

**Use Cases:**
- Data privacy compliance (GDPR, CCPA)
- Customer data anonymization
- Pre-migration PII discovery
- Audit logging and redaction

[📖 See complete PII module documentation](#pii-detection-functions)

---

### 🔍 Data Quality Metrics

Track essential data quality dimensions:

| Metric | Purpose | Example |
|--------|---------|---------|
| **Volume** | Row count thresholds | `volume('orders', 1000, 1000000)` |
| **Null Rate** | Missing value detection | `null_rate('users', 'email', 0.05)` |
| **Distinctness** | Cardinality validation | `distinct_count('products', 'sku', 100, NULL)` |
| **Freshness** | Data recency checks | `freshness('events', 'timestamp', INTERVAL '1 hour')` |
| **Schema** | Required column validation | `schema_check('table', ['id', 'created_at'])` |
| **Regex Match** | Format conformance rate | `regex_match('users', 'email', '^[^@]+@[^@]+$', 0.99)` |
| **Values In Set** | Allowed value membership | `values_in_set('orders', 'status', ['completed', 'pending'])` |
| **Aggregate** | avg/min/max/sum/median/stddev bounds | `agg_check('orders', 'amount', 'avg', 10.0, 500.0)` |
| **Duplicates** | Duplicate key detection | `duplicate_count('orders', 'order_id')` |
| **Occurrence** | Value frequency bounds | `occurrence('orders', 'customer_id', 'max', NULL, 100)` |
| **Match Rate** | Referential integrity across tables | `match_rate('orders', 'customers', 'customer_id', 'id')` |
| **Compliance** | Custom SQL predicate satisfaction | `compliance('orders', 'amount > 0', 0.95)` |
| **Rel. Count Change** | Daily count vs rolling baseline | `rel_count_change('orders', 'order_date', NULL, 7, -0.5, 0.5)` |
| **Metric IQR Anomaly** | Daily metric outside IQR band | `metric_anomaly_iqr('orders', 'order_date', NULL, 30, 1.5, 'both')` |
| **Rolling Values In Set** | Set membership over a date window | `rolling_values_in_set('orders', 'status', ['completed'], 'order_date', 14)` |

The three time-aware checks evaluate one reference date (default: the latest date in the data), or,
with `start_date := DATE, end_date := DATE`, every day of that inclusive range — one row per date,
ordered by date, with the same columns:

```sql
SELECT reference_date, status, rel_change
FROM rel_count_change('orders', 'order_date', start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');
```

Their `_events` companions (`rel_count_change_events`, `metric_anomaly_iqr_events`,
`rolling_values_in_set_events`, same arguments) return only the failing/warning dates in the anofox
contract's `events` shape (`ds, kind, label, severity, value, threshold, series`), ready for an
alarm timeline in anofox-visualization:

```sql
SELECT * FROM rel_count_change_events('orders', 'order_date', start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');
```

**anofox integration contract:** contract producers carry the `duckdb_functions()` tags
`{'anofox.output': '<schema>', 'anofox.contract': '1'}` and `anofox_contract_version()` returns
`'1'`, so anofox-visualization plots them without adapter SQL:

| Producer (primary `anofox_tab_*` name and alias) | Schema |
|----------|--------|
| `run_checks` | `check` |
| `profile_correlations` | `matrix` |
| `rel_count_change_events`, `metric_anomaly_iqr_events`, `rolling_values_in_set_events` | `events` |

The one-row checks (including `agg_check`) keep their own columns and are not tagged; `run_checks`
is the contract-shaped `check` producer for them (14 check types; not `zscore`, `iqr`,
`schema_check`). See [docs/API_REFERENCE.md](docs/API_REFERENCE.md#anofox-integration-contract).

**Check suites** — define checks once in a SQL table and run them all with one call:

```sql
CREATE TABLE dq_checks (
    check_name VARCHAR, check_type VARCHAR, table_name VARCHAR, column_name VARCHAR,
    params VARCHAR,               -- type-specific JSON, e.g. '{"pattern": "^..."}'
    lower_threshold DOUBLE, upper_threshold DOUBLE,
    monitor_only BOOLEAN,         -- true records 'warn' instead of 'fail'
    identifier_column VARCHAR,    -- optional: one result row per partition value
    filter_expr VARCHAR           -- optional SQL pre-filter; supports ${today}, ${yesterday}, ${today-N}
);

INSERT INTO dq_checks VALUES
    ('orders_volume', 'volume', 'orders', NULL, NULL, 1000, NULL, false, NULL, NULL),
    ('orders_fk', 'match_rate', 'orders', 'customer_id',
     '{"right_table": "customers", "left_keys": "customer_id", "right_keys": "id"}', 1.0, NULL, false, NULL, NULL);

SELECT * FROM run_checks('dq_checks');  -- uniform rows: run_ts, check_name, ..., value, status, message

-- persist results for historical monitoring, then detect metric anomalies over time:
CREATE TABLE IF NOT EXISTS dq_results AS SELECT * FROM run_checks('dq_checks') LIMIT 0;
INSERT INTO dq_results SELECT * FROM run_checks('dq_checks');
SELECT * FROM metric_anomaly_iqr('dq_results', 'run_ts', 'value', 30, 1.5, 'both');
```

The checks table must be a regular (non-temporary) table (it is read through a separate
connection at bind time); the *target* tables may be temporary tables or views.

**Statistical Outlier Detection:**
- **Z-Score** - Parametric outlier detection (assumes normal distribution)
- **IQR** - Non-parametric outlier detection (robust to distribution)

```sql
-- Find statistical outliers using IQR method
SELECT * FROM iqr('transactions', 'amount', 1.5);
```

[📖 See complete Metrics module documentation](#data-quality-metrics-functions)

---

### 🤖 Anomaly Detection

#### Isolation Forest (Enhanced)

Industry-grade anomaly detection with [isotree](https://github.com/david-cortes/isotree)-inspired features:

**Core Capabilities:**
- **Categorical Support** - Auto-detect VARCHAR columns with random subset splitting
- **Extended IF (ndim)** - Hyperplane splits for diagonal/curved anomaly patterns
- **Density Scoring** - Alternative metric based on points-to-volume ratio
- **Sample Weights** - Weighted sampling for imbalanced datasets
- **SCiForest** - Information-gain guided splitting with configurable candidates

```sql
-- Basic univariate detection
SELECT * FROM isolation_forest(
    'sales_data',
    'amount',
    100,        -- n_trees
    256,        -- sample_size
    0.1,        -- contamination (expected anomaly rate)
    'scores'    -- output mode: 'summary' or 'scores'
) WHERE is_anomaly = true;

-- Extended IF with hyperplane splits (ndim=3)
SELECT * FROM isolation_forest_mv(
    'transactions',
    'amount, quantity, duration',
    100, 256, 0.1, 'scores',
    NULL,       -- seed (NULL = random)
    3,          -- ndim: hyperplane dimensions
    'normal',   -- coef_type: coefficient distribution
    'depth'     -- scoring_metric
);

-- SCiForest with gain-based split selection
SELECT * FROM isolation_forest_mv(
    'customer_events',
    'purchase_amount, session_duration',
    100, 256, 0.05, 'scores',
    NULL, 1, 'uniform', 'depth',  -- seed, ndim, coef_type, scoring_metric
    NULL,       -- weight_column
    10,         -- ntry: split candidates
    0.5         -- prob_pick_avg_gain
);
```

**Why Isolation Forest?**
- No distributional assumptions (works on any data shape)
- Excellent for high-dimensional data
- Detects both global and local anomalies
- Fast training and prediction (O(n log n))

**`isolation_forest_mv` parameters after `output_mode`** (in this order):
| Parameter | Values | Description |
|-----------|--------|-------------|
| `seed` | integer (default: NULL = random) | Fixed seed for reproducible scores (also on `isolation_forest`) |
| `ndim` | 1-N (default: 1) | Dimensions for hyperplane splits |
| `coef_type` | 'uniform' (default), 'normal' | Coefficient distribution |
| `scoring_metric` | 'depth' (default), 'density', 'adj_depth' | Scoring method |
| `weight_column` | column name (default: NULL = uniform) | Sample weight column |
| `ntry` | >= 1 (default: 1) | Split candidates per node |
| `prob_pick_avg_gain` | 0.0-1.0 (default: 0.0) | Gain-based selection probability |

#### DBSCAN Clustering

Density-based anomaly detection for finding outliers in spatial data:

```sql
-- Univariate: find noise points in single dimension
SELECT * FROM dbscan(
    'transactions',
    'amount',
    10.0,       -- eps: neighborhood radius
    5,          -- min_pts: minimum points for dense region
    'clusters'  -- output mode: 'summary' or 'clusters'
) WHERE point_type = 'NOISE';

-- Multivariate over several columns; all trailing parameters are optional
SELECT * FROM dbscan_mv('transactions', 'amount,quantity');
```

**Point Classifications:**
- **CORE** - Dense region centers (low anomaly score)
- **BORDER** - Cluster edges (moderate anomaly score)
- **NOISE** - Isolated outliers (high anomaly score: 1.0)

[📖 See complete Anomaly Detection module documentation](#anomaly-detection-functions)

#### OutlierTree (Explainable Anomaly Detection)

Unlike Isolation Forest which provides anomaly scores, OutlierTree generates **human-readable explanations** for why specific values are outliers based on conditional distributions.

**Key Features:**
- Detects outliers in context (e.g., "salary is high *for a Junior Developer*")
- Returns natural language explanations with statistical backing
- Uses robust statistics (median + MAD) resistant to outliers
- Supports both numeric and categorical columns

```sql
-- Detect salary outliers conditioned on job title
SELECT row_id, column_name, outlier_value, explanation
FROM outlier_tree('employees', 'job_title,salary,years_exp', 'outliers');

-- Returns explanations like:
-- "Value 150000 for column 'salary' is unusually high (expected: 52333 ± 7413)
--  when job_title = 'Junior Developer'"
```

**Output Modes:**
- `'summary'`: Single row with pass/fail status and outlier count
- `'outliers'`: Per-outlier rows with z-scores, bounds, and explanations. The value is returned as text in `outlier_value` and, for numeric columns, as a DOUBLE in `outlier_value_num` (NULL for categorical columns), so it can be compared with `lower_bound`/`upper_bound` directly

**Semantics:**
- Rows containing NULL in any selected column are excluded from the analysis
- `row_id` is the 1-based position of the row in the source table (NULL rows still count toward the position)
- `id_column := '<column>'` (`'outliers'` mode) adds that column's value after `row_id`, see [Identifier pass-through](#anomaly-detection-functions)
- `total_rows` (summary mode) is the number of analyzed rows, i.e. rows that are non-NULL in every selected column

---

### 🔄 Data Diffing

Compare tables and identify changes:

```sql
-- Join-based diff (row-level changes)
SELECT * FROM diff_joindiff('source_tbl', 'target_tbl', ['user_id', 'date'])
WHERE diff_type IN ('added', 'changed')
LIMIT 100;

-- Every parameter can also be passed by name; a named value overrides the
-- positional one
SELECT * FROM diff_joindiff('source_tbl', 'target_tbl', 'id', include_all := true);
SELECT * FROM diff_joindiff(source_table := 'source_tbl', target_table := 'target_tbl',
                            primary_keys := ['user_id', 'date'], compare_columns := ['amount']);

-- diff_hashdiff currently computes the same row-level diff as diff_joindiff.
-- Its bisection_threshold/bisection_factor parameters are not implemented and
-- are rejected with a binder error.
SELECT * FROM diff_hashdiff('source_tbl', 'target_tbl', ['id']);
```

**Use Cases:**
- Migration validation (compare old vs new systems)
- Regression testing (ensure transformations didn't break data)
- Incremental change detection (CDC scenarios)
- Schema evolution tracking

**Features:**
- Single and compound primary keys
- NULL-safe primary key matching (`IS NOT DISTINCT FROM`): rows with NULL key
  values are matched across sides instead of being misreported as added/removed
- Column-specific comparison (default: all non-key columns shared by both tables)
- Primary key and compare columns are validated against both schemas at bind time
- Efficient SQL-based implementation
- Detailed change tracking

[📖 See complete Data Diffing module documentation](#data-diffing-functions)

---

### 📊 Data Profiling

Generate comprehensive column-level statistics, pairwise correlations, and table-wide quality
summaries in a single SQL call — with full support for complex types (LIST, MAP, STRUCT, UNION).

```sql
-- Per-column statistics: one row per column
SELECT column_name, column_type, null_rate, distinct_count, mean, stddev, pattern_summary
FROM profile_table('orders');

-- Focus on specific columns only
SELECT * FROM profile_table('orders', ['amount', 'customer_id'], 500000, false);

-- Table overview: row counts, type breakdowns, null totals, duplicates
SELECT * FROM profile_summary('orders');

-- Pairwise Pearson and Spearman correlations for all numeric columns
SELECT column_a, column_b, pearson, spearman
FROM profile_correlations('orders')
WHERE abs(pearson) > 0.7;
```

**`profile_table` output** (27 columns per row):

| Column | Description |
|--------|-------------|
| `column_name`, `column_type` | Column identity and DuckDB type |
| `row_count`, `null_count`, `null_rate` | Total and missing value stats |
| `distinct_count`, `distinct_rate`, `is_unique`, `is_constant` | Cardinality metrics |
| `min_val`, `max_val` | Min/max as VARCHAR (NULL for STRUCT/MAP/UNION) |
| `mean`, `median`, `stddev`, `p25`, `p75`, `skewness`, `kurtosis` | Numeric distribution (NULL for non-numeric) |
| `avg_length`, `min_length`, `max_length` | String character length; list element count; map cardinality |
| `top_values` | Most-frequent values |
| `pattern_summary` | Detected pattern: `email`, `phone`, `uuid`, `url`, `ip_address`, `iso_date`, `numeric_string`, `mixed`, `list`, `map`, `struct`, `union` |
| `zero_count`, `negative_count` | Numeric zero/negative counts (NULL for non-numeric) |
| `is_sampled`, `actual_sample_size` | Sampling metadata |

**`profile_summary` output** (11 columns, one row):

`row_count`, `column_count`, `numeric_columns`, `string_columns`, `temporal_columns`,
`boolean_columns`, `complex_columns`, `total_nulls`, `total_null_rate`, `duplicate_row_count`,
`estimated_memory_bytes`

**Python API:**

```python
import anofox

with anofox.connect() as conn:
    stats   = anofox.profile.profile_table(conn, df)          # pandas/polars DataFrame input
    summary = anofox.profile.profile_summary(conn, 'orders')
    corr    = anofox.profile.profile_correlations(conn, df, columns=['price', 'qty'])
```

[📖 See complete Data Profiling module documentation](#data-profiling-functions)

---

## 📊 Real-World Examples

### Email Verification at Scale

```sql
-- Load the fraudulent emails dataset (about 4,000 spam/phishing samples; run from the repo root)
CREATE TABLE emails AS
SELECT * FROM read_parquet('examples/data/fraudulent_emails.parquet');

-- Extract and validate sender emails
WITH extracted AS (
    SELECT
        "from" as original,
        CASE
            WHEN "from" LIKE '%<%>%'
            THEN TRIM(SUBSTRING("from",
                POSITION('<' IN "from") + 1,
                POSITION('>' IN "from") - POSITION('<' IN "from") - 1))
            ELSE "from"
        END as email
    FROM emails
)
SELECT
    COUNT(*) as total,
    SUM(CASE WHEN email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as valid,
    SUM(CASE WHEN NOT email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as invalid
FROM extracted;
```

### Address Quality Analysis

```sql
-- Parse Australian addresses (537K FEBRL dataset)
CREATE TABLE addresses AS
SELECT * FROM read_parquet('examples/data/febrl_data.parquet');

-- Parse and analyze address completeness
WITH parsed AS (
    SELECT
        *,
        postal_parse_address(
            address_1 || ' ' || COALESCE(address_2, '') || ' ' ||
            street_number || ' ' || postcode || ' ' || state
        ) as components
    FROM addresses
)
SELECT
    state,
    COUNT(*) as total_addresses,
    COUNT(DISTINCT components.postcode) as unique_postcodes,
    COUNT(CASE WHEN components.house_number IS NOT NULL THEN 1 END) as has_house_number
FROM parsed
GROUP BY state
ORDER BY total_addresses DESC;
```

### Anomaly Detection Pipeline

```sql
-- Create sample transaction data
CREATE TABLE transactions AS
SELECT
    row_number() OVER () as id,
    random() * 1000 as amount,
    random() * 100 as quantity,
    CASE WHEN random() < 0.02 THEN random() * 10000 ELSE random() * 1000 END as suspicious_amount
FROM range(10000);

-- Detect outliers using Isolation Forest
CREATE TABLE anomalies AS
SELECT
    t.*,
    if_result.anomaly_score,
    if_result.is_anomaly
FROM transactions t
JOIN (
    SELECT * FROM isolation_forest_mv(
        'transactions',
        'amount, quantity, suspicious_amount',
        100, 256, 0.05, 'scores'
    )
) if_result ON t.id = if_result.row_id
WHERE if_result.is_anomaly = true;

-- Analyze anomaly distribution
SELECT
    CASE
        WHEN anomaly_score > 0.8 THEN 'High Risk'
        WHEN anomaly_score > 0.6 THEN 'Medium Risk'
        ELSE 'Low Risk'
    END as risk_level,
    COUNT(*) as count,
    AVG(suspicious_amount) as avg_amount
FROM anomalies
GROUP BY risk_level;
```

### Data Migration Validation

```sql
-- Compare production vs staging after migration
SELECT
    diff_type,
    COUNT(*) as row_count,
    COUNT(*) * 100.0 / SUM(COUNT(*)) OVER () as percentage
FROM diff_joindiff('prod.users', 'staging.users', ['user_id'])
GROUP BY diff_type
ORDER BY row_count DESC;

-- Identify specific changed records: the diff returns the target values,
-- so join the source table for the old ones
SELECT
    d.user_id,
    s.email AS old_email,
    d.email AS new_email
FROM diff_joindiff('prod.users', 'staging.users', ['user_id']) d
JOIN prod.users s USING (user_id)
WHERE d.diff_type = 'changed'
  AND s.email IS DISTINCT FROM d.email
LIMIT 10;
```

### Financial Transaction Validation

```sql
-- Validate international transactions with multi-currency support
SELECT
    transaction_id,
    customer_id,
    vat_country_name((vat_split(customer_vat)).country) as country,
    money_format(amount, 'symbol') as formatted_amount,
    CASE
        WHEN money_is_negative(amount) THEN 'Refund'
        WHEN money_is_zero(amount) THEN 'Warning'
        ELSE 'Valid'
    END as transaction_status,
    vat_is_valid(customer_vat) as vat_valid
FROM transactions
WHERE money_in_range(amount, 0.01, 99999.99)
  AND email_is_valid(customer_email, 'dns');
```

---

## 📚 SQL Function Reference

### Email Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_email_is_valid` | `(email VARCHAR [, mode VARCHAR]) → BOOLEAN` | Quick validation (modes: `regex`, `dns`, `smtp`) |
| `anofox_tab_email_validate` | `(email VARCHAR [, mode VARCHAR]) → STRUCT` | Detailed validation with stage, reason, MX hosts, SMTP transcript |
| `anofox_tab_email_config` | `() → TABLE(key VARCHAR, value VARCHAR)` | Current configuration settings |

### Postal Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_postal_parse_address` | `(address VARCHAR) → STRUCT` | Parse address into components (house_number, road, city, state, postcode, country) |
| `anofox_tab_postal_expand_address` | `(address VARCHAR) → LIST<VARCHAR>` | Generate normalized variants |
| `anofox_tab_postal_status` | `() → TABLE` | Library initialization status |
| `anofox_tab_postal_load_data` | `() → BOOLEAN` | Download and extract libpostal data (~500MB) |

### Phone Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_phonenumber_parse` | `(number VARCHAR, region VARCHAR) → STRUCT` | Parse and validate (returns validity, country_code, national_number, region, type) |
| `anofox_tab_phonenumber_format` | `(number VARCHAR, region VARCHAR, format VARCHAR) → VARCHAR` | Format number (E164, INTERNATIONAL, NATIONAL, RFC3966) |
| `anofox_tab_phonenumber_region` | `(number VARCHAR, region VARCHAR) → VARCHAR` | Extract ISO region code |
| `anofox_tab_phonenumber_is_valid` | `(number VARCHAR, region VARCHAR) → BOOLEAN` | Full validation using length and prefix information |
| `anofox_tab_phonenumber_is_possible` | `(number VARCHAR, region VARCHAR) → BOOLEAN` | Quick possibility check using length-only analysis |
| `anofox_tab_phonenumber_is_valid_for_region` | `(number VARCHAR, region VARCHAR) → BOOLEAN` | Region-specific validation |
| `anofox_tab_phonenumber_match` | `(number1 VARCHAR, number2 VARCHAR, region VARCHAR) → VARCHAR` | Fuzzy matching returns (EXACT_MATCH, NSN_MATCH, SHORT_NSN_MATCH, NO_MATCH) |
| `anofox_tab_phonenumber_example` | `(region VARCHAR) → VARCHAR` | Generate example phone number for region |
| `anofox_tab_phonenumber_status` | `() → TABLE` | Library status and default region |

### Money & Currency Functions

Money values are `STRUCT(amount DECIMAL(18,3), currency VARCHAR)`: amounts are exact decimals (no binary floating point drift), NaN/Inf are unrepresentable, and arithmetic raises a clear error on overflow instead of producing Inf. `money()` accepts a DOUBLE amount, which must be finite and is rounded half away from zero to 3 decimals; use `money_from_cents()` for fully exact construction from integer subunits.

#### Basic Operations
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_money` | `(amount, currency_code)` | STRUCT | Create a money value from amount and currency code (amount must be finite and within the DECIMAL(18,3) range) |
| `anofox_tab_money_from_cents` | `(cents, currency_code)` | STRUCT | Create a money value from an integer amount in the smallest currency unit, exactly (e.g. 10050 cents → 100.50 USD) |
| `anofox_tab_money_amount` | `(money)` | DECIMAL(18,3) | Extract the exact amount from money struct |
| `anofox_tab_money_currency` | `(money)` | VARCHAR | Extract currency code from money struct |

#### Currency Information
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_is_valid_currency` | `(code)` | BOOLEAN | Check if currency code is valid |
| `anofox_tab_currency_symbol` | `(code)` | VARCHAR | Get currency symbol (e.g., '$', '€') |
| `anofox_tab_currency_name` | `(code)` | VARCHAR | Get currency name (e.g., 'United States Dollar') |

#### Formatting
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_money_format` | `(money, style)` | VARCHAR | Format money for display (3 styles: 'symbol', 'code', 'long'). The scale comes from the currency's `subunit_to_unit` (JPY has no decimals); 'symbol' and 'long' localize with the currency's thousands separator and decimal mark (`$1,234,567.89`, `1.234.567,89 €`, `¥1,235`), 'code' stays canonical (dot decimal mark, no grouping: `1234567.89 USD`) |

#### Validation & Properties
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_money_is_positive` | `(money)` | BOOLEAN | Check if amount > 0 |
| `anofox_tab_money_is_negative` | `(money)` | BOOLEAN | Check if amount < 0 |
| `anofox_tab_money_is_zero` | `(money)` | BOOLEAN | Check if amount == 0 |
| `anofox_tab_money_abs` | `(money)` | STRUCT | Get absolute value (sign removed) |

#### Arithmetic Operations
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_money_add` | `(money1, money2)` | STRUCT | Add two money values (same currency required) |
| `anofox_tab_money_subtract` | `(money1, money2)` | STRUCT | Subtract money2 from money1 (same currency) |
| `anofox_tab_money_multiply` | `(money, factor)` | STRUCT | Multiply money by a scalar factor |

#### Quality & Data Validation
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_money_in_range` | `(money, min, max)` | BOOLEAN | Check if amount is within range |
| `anofox_tab_money_same_currency` | `(money1, money2)` | BOOLEAN | Check if two money values have same currency |

### VAT Validation Functions

#### Basic Operations
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_vat` | `(vat_string)` | STRUCT | Parse VAT string into country and digits |
| `anofox_tab_is_valid_vat_country` | `(code)` | BOOLEAN | Check if country code is valid VAT country |
| `anofox_tab_vat_normalize` | `(vat_string)` | VARCHAR | Normalize VAT string (uppercase, remove punctuation) |

#### Syntax Validation
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_vat_is_valid_syntax` | `(vat_string)` | BOOLEAN | Validate VAT syntax against country pattern |
| `anofox_tab_vat_split` | `(vat_string)` | STRUCT | Parse VAT into country and normalized digits |
| `anofox_tab_vat_exists` | `(vat_string)` | BOOLEAN | Check if VAT has valid country prefix |

#### EU Utilities
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_vat_is_eu_member` | `(country_code)` | BOOLEAN | Check if country is EU member |
| `anofox_tab_vat_country_name` | `(country_code)` | VARCHAR | Get full country name |
| `anofox_tab_vat_format` | `(vat_string, style)` | VARCHAR | Format VAT for display: `'plain'` (digits only) or `'iso'` (VAT country prefix + digits) |

#### Combined Validation
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_vat_is_valid` | `(vat_string)` | BOOLEAN | Full validation (syntax + check digit where implemented) |

Check-digit (checksum) validation is implemented for: AT, BE, DE, DK, ES, FI, FR, GR/EL, IE, IT, LU, NL, PL, PT, SE, SI.
All other countries are validated by syntax only. French VAT keys containing letters are accepted without check-digit verification.

### PII Detection Functions

#### Detection & Masking
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_pii_detect` | `(text VARCHAR)` | LIST(STRUCT) | Detect all PII in text (returns type, text, start_pos, end_pos, confidence) |
| `anofox_tab_pii_detect_batch` | `(texts VARCHAR[])` | LIST(LIST(STRUCT)) | `pii_detect` for each element of a list |
| `anofox_tab_pii_mask` | `(text VARCHAR [, strategy VARCHAR])` | VARCHAR | Mask all PII in text (default: 'redact') |
| `anofox_tab_pii_mask_column` | `(value VARCHAR, pii_type VARCHAR, strategy VARCHAR)` | VARCHAR | Mask only the matches of one PII type |
| `anofox_tab_pii_redact_column` | `(value VARCHAR [, strategy VARCHAR])` | VARCHAR | Mask all PII in a value (default: `anofox_pii_default_mask_strategy`) |
| `anofox_tab_pii_contains` | `(text VARCHAR)` | BOOLEAN | Check if text contains any PII |
| `anofox_tab_pii_count` | `(text VARCHAR)` | BIGINT | Count PII occurrences in text |

#### Type-Specific Detection
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_pii_detect_emails` | `(text VARCHAR)` | LIST(STRUCT) | Detect only EMAIL PII |
| `anofox_tab_pii_detect_phones` | `(text VARCHAR)` | LIST(STRUCT) | Detect only PHONE PII |
| `anofox_tab_pii_detect_credit_cards` | `(text VARCHAR)` | LIST(STRUCT) | Detect only CREDIT_CARD PII |
| `anofox_tab_pii_detect_ssns` | `(text VARCHAR)` | LIST(STRUCT) | Detect only US_SSN PII |
| `anofox_tab_pii_detect_names` | `(text VARCHAR)` | LIST(STRUCT) | Detect only NAME PII (NER-based) |
| `anofox_tab_pii_detect_ibans` | `(text VARCHAR)` | LIST(STRUCT) | Detect only IBAN PII |

#### Validation (Checksum-based)
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_pii_is_valid_ssn` | `(text VARCHAR)` | BOOLEAN | Validate US SSN format |
| `anofox_tab_pii_is_valid_iban` | `(text VARCHAR)` | BOOLEAN | Validate IBAN with MOD-97 checksum |
| `anofox_tab_pii_is_valid_credit_card` | `(text VARCHAR)` | BOOLEAN | Validate credit card with Luhn checksum |
| `anofox_tab_pii_is_valid_nino` | `(text VARCHAR)` | BOOLEAN | Validate UK National Insurance Number |
| `anofox_tab_pii_is_valid_de_tax_id` | `(text VARCHAR)` | BOOLEAN | Validate German Tax ID |
| `anofox_tab_pii_is_valid_crypto_address` | `(text VARCHAR)` | BOOLEAN | Validate Bitcoin/Ethereum address |

#### Table Functions
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_pii_scan_table` | `(table VARCHAR [, columns VARCHAR])` | TABLE | Scan table columns for PII |
| `anofox_tab_pii_audit_table` | `(table VARCHAR [, columns VARCHAR] [, id_column := VARCHAR])` | TABLE | Row-level PII audit with masking; `id_column` passes an identifier column through |
| `anofox_tab_pii_status` | `()` | TABLE | List supported PII types and recognizers |
| `anofox_tab_pii_config` | `()` | TABLE | Current PII configuration |
| `anofox_ner_status` | `()` | TABLE | NER model status (`model_status`, `model_path`, …); no alias |

### Data Quality Metrics Functions

All functions are registered under their canonical `anofox_tab_*` name plus the short alias shown.

| Function (alias) | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_volume` (`volume`) | `(table VARCHAR, min_rows BIGINT, max_rows BIGINT) → TABLE` | Validate row count |
| `anofox_tab_null_rate` (`null_rate`) | `(table VARCHAR, column VARCHAR, max_null_rate DOUBLE) → TABLE` | Check null percentage |
| `anofox_tab_distinct_count` (`distinct_count`) | `(table VARCHAR, column VARCHAR, min BIGINT, max BIGINT) → TABLE` | Validate cardinality |
| `anofox_tab_schema_check` (`schema_check`) | `(table VARCHAR, required_cols LIST<VARCHAR>) → TABLE` | Check required columns exist |
| `anofox_tab_freshness` (`freshness`) | `(table VARCHAR, ts_col VARCHAR, max_age INTERVAL [, ref_time TIMESTAMP]) → TABLE` | Validate data recency |
| `anofox_tab_zscore` (`zscore`) | `(table VARCHAR, column VARCHAR, threshold DOUBLE) → TABLE` | Detect outliers via z-score |
| `anofox_tab_iqr` (`iqr`) | `(table VARCHAR, column VARCHAR, multiplier DOUBLE) → TABLE` | Detect outliers via IQR |
| `anofox_tab_regex_match` (`regex_match`) | `(table VARCHAR, column VARCHAR, pattern VARCHAR, min_match_rate DOUBLE [, max_match_rate DOUBLE]) → TABLE` | Share of values matching a regex |
| `anofox_tab_values_in_set` (`values_in_set`) | `(table VARCHAR, column VARCHAR, allowed LIST<VARCHAR> [, min_rate DOUBLE = 1.0]) → TABLE` | Share of values inside an allowed set (reports up to 5 sample violations) |
| `anofox_tab_agg_check` (`agg_check`) | `(table VARCHAR, column VARCHAR, agg VARCHAR, lower DOUBLE, upper DOUBLE) → TABLE` | Assert avg/min/max/sum/median/stddev bounds (NULL = unbounded) |
| `anofox_tab_duplicate_count` (`duplicate_count`) | `(table VARCHAR, columns VARCHAR [, max_duplicates BIGINT = 0]) → TABLE` | Duplicate count for a column or comma-separated key |
| `anofox_tab_occurrence` (`occurrence`) | `(table VARCHAR, column VARCHAR, mode VARCHAR, lower BIGINT, upper BIGINT) → TABLE` | Highest (`'max'`) or lowest (`'min'`) frequency of any single value |
| `anofox_tab_match_rate` (`match_rate`) | `(left VARCHAR, right VARCHAR, left_keys VARCHAR, right_keys VARCHAR [, min_rate DOUBLE = 1.0]) → TABLE` | Share of left rows with a join partner (referential integrity) |
| `anofox_tab_compliance` (`compliance`) | `(table VARCHAR, expression VARCHAR [, min_rate DOUBLE = 1.0]) → TABLE` | Share of rows satisfying a SQL boolean expression |
| `anofox_tab_rel_count_change` (`rel_count_change`) | `(table VARCHAR, date_col VARCHAR [, count_col VARCHAR, window_days BIGINT = 7, lower DOUBLE = -0.5, upper DOUBLE = 0.5, reference_date DATE] [, start_date := DATE, end_date := DATE]) → TABLE` | Daily (distinct) count vs rolling baseline average; one row per date with a date range |
| `anofox_tab_metric_anomaly_iqr` (`metric_anomaly_iqr`) | `(table VARCHAR, date_col VARCHAR [, metric_col VARCHAR, window_days BIGINT = 30, k DOUBLE = 1.5, mode VARCHAR = 'both', reference_date DATE] [, start_date := DATE, end_date := DATE]) → TABLE` | Daily metric outside Q1/Q3 ± k·IQR of the trailing window; one row per date with a date range |
| `anofox_tab_rolling_values_in_set` (`rolling_values_in_set`) | `(table VARCHAR, column VARCHAR, allowed LIST<VARCHAR>, date_col VARCHAR [, window_days BIGINT = 7, min_rate DOUBLE = 1.0, reference_date DATE] [, start_date := DATE, end_date := DATE]) → TABLE` | Values-in-set rate over a trailing date window; one row per date with a date range |
| `anofox_tab_rel_count_change_events` (`rel_count_change_events`) | same arguments as `rel_count_change` → `TABLE(ds, kind, label, severity, value, threshold, series)` | Contract `events`: one row per failing/warning reference date |
| `anofox_tab_metric_anomaly_iqr_events` (`metric_anomaly_iqr_events`) | same arguments as `metric_anomaly_iqr` → `TABLE(ds, kind, label, severity, value, threshold, series)` | Contract `events`: one row per anomalous reference date |
| `anofox_tab_rolling_values_in_set_events` (`rolling_values_in_set_events`) | same arguments as `rolling_values_in_set` → `TABLE(ds, kind, label, severity, value, threshold, series)` | Contract `events`: one row per failing/warning reference date |
| `anofox_tab_run_checks` (`run_checks`) | `(checks_table VARCHAR) → TABLE` | Run every check in a checks table; uniform result rows (contract `check` schema) |
| `anofox_contract_version` | `() → VARCHAR` | anofox integration contract version (`'1'`) |

> **Security note:** `compliance` executes the caller-supplied expression as SQL with the caller's
> privileges. The expression is validated at bind time to be a single boolean expression
> (multi-statement payloads are rejected), but pass only trusted expressions — the trust model is
> the same as running SQL directly.

**Parameter validation:** numeric parameters are validated at bind time. Negative or out-of-range
integer parameters and non-finite (`NaN`/`Infinity`) double parameters (`max_null_rate`, `threshold`,
`multiplier`, `eps`, `contamination`, `prob_pick_avg_gain`, ...) are rejected with a binder error.

**Empty and degenerate input semantics:** every metric returns exactly one summary row, even for
an empty table or an all-NULL column:

| Metric | Empty table / all-NULL column | Constant column (stddev = 0) |
|--------|-------------------------------|------------------------------|
| `volume` | `row_count = 0`, status from thresholds | n/a |
| `null_rate` | `null_count = 0`, `total_count = 0`, `null_rate = 0.0`, status `pass` ("passed trivially") | n/a |
| `distinct_count` | `distinct_count = 0`, status from thresholds | n/a |
| `zscore` | `total_count = 0`, `outlier_count = 0`, `outlier_rate = 0.0`, NULL `mean`/`stddev`, status `pass` | `outlier_count = 0`, status `pass` (z-scores are defined as 0) |
| `iqr` | `total_count = 0`, `outlier_count = 0`, NULL quantiles/bounds, status `pass` | `outlier_count = 0`, status `pass` |
| `freshness` | NULL `metric_value`/`age_seconds`, status `fail` ("No timestamp values found") | n/a |
| `regex_match` | `match_rate = 0.0`, `total_count = 0`, status `pass` ("passed trivially") | n/a |
| `values_in_set` | `in_set_rate = 0.0`, `total_count = 0`, status `pass` ("passed trivially") | n/a |
| `agg_check` | NULL `value`, status `pass` ("passed trivially") | n/a |
| `duplicate_count` | `duplicate_count = 0`, `total_count = 0`, status `pass` | n/a |
| `occurrence` | NULL `occurrence`/`extreme_value`, status `pass` ("passed trivially") | n/a |
| `match_rate` | empty left table: `match_rate = 0.0`, `total_count = 0`, status `pass` ("passed trivially") | n/a |
| `compliance` | `compliance_rate = 0.0`, `total_count = 0`, status `pass` ("passed trivially") | n/a |
| `rel_count_change` | empty input, no baseline days, or zero baseline: status `pass` ("passed trivially") | n/a |
| `metric_anomaly_iqr` | empty input or empty baseline window: status `pass` ("passed trivially") | n/a |
| `rolling_values_in_set` | empty window: `total_count = 0`, status `pass` ("passed trivially") | n/a |

### Anomaly Detection Functions

| Function | Description |
|----------|-------------|
| `anofox_tab_isolation_forest` (alias `isolation_forest`) | Univariate Isolation Forest with all enhancements |
| `anofox_tab_isolation_forest_mv` (alias `isolation_forest_mv`) | Multivariate Isolation Forest |
| `anofox_tab_dbscan` (alias `dbscan`) | Univariate DBSCAN clustering |
| `anofox_tab_dbscan_mv` (alias `dbscan_mv`) | Multivariate DBSCAN clustering |
| `anofox_tab_outlier_tree` (alias `outlier_tree`) | Explainable outlier detection with conditional distributions |

**Isolation Forest Full Signature:**
```sql
isolation_forest(
    table_name VARCHAR,
    column_name VARCHAR,
    n_trees BIGINT,           -- 1-500, default 100
    sample_size BIGINT,       -- 1-10000, default 256
    contamination DOUBLE,     -- 0.0-0.5, default 0.1
    output_mode VARCHAR,      -- 'summary' (default) or 'scores'
    seed BIGINT               -- optional RNG seed
) → TABLE
-- isolation_forest_mv additionally accepts (in order):
--   ndim BIGINT,              -- 1-N, default 1 (Extended IF)
--   coef_type VARCHAR,        -- 'uniform' or 'normal'
--   scoring_metric VARCHAR,   -- 'depth', 'density', or 'adj_depth'
--   weight_column VARCHAR,    -- Column for sample weights (NULL = uniform)
--   ntry BIGINT,              -- 1-100, default 1 (SCiForest)
--   prob_pick_avg_gain DOUBLE -- 0.0-1.0, default 0.0
```

All parameters after `column_name` are optional; `isolation_forest('sales', 'amount')` uses the defaults.

**Parameters:**
| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `n_trees` | 1-500 | 100 | Number of isolation trees |
| `sample_size` | 1-10000 | 256 | Subsample size per tree |
| `contamination` | 0.0-0.5 | 0.1 | Expected anomaly fraction |
| `output_mode` | - | 'summary' | `'summary'` or `'scores'` |
| `seed` | - | NULL (random) | Fixed seed for reproducible scores |
| `ndim` | 1-N | 1 | Hyperplane dimensions (Extended IF; `isolation_forest_mv` only) |
| `coef_type` | - | 'uniform' | `'uniform'` or `'normal'` (`isolation_forest_mv` only) |
| `scoring_metric` | - | 'depth' | `'depth'`, `'density'`, `'adj_depth'` (`isolation_forest_mv` only) |
| `weight_column` | - | NULL | Sample weight column name (`isolation_forest_mv` only) |
| `ntry` | 1-100 | 1 | Split candidates (SCiForest; `isolation_forest_mv` only) |
| `prob_pick_avg_gain` | 0.0-1.0 | 0.0 | Gain-based selection probability (`isolation_forest_mv` only) |

**DBSCAN Full Signature:**
```sql
dbscan(
    table_name VARCHAR,
    column_name VARCHAR,
    eps DOUBLE,          -- > 0.0, default 0.5: neighborhood radius
    min_pts BIGINT,      -- >= 1, default 5: minimum points for dense region
    output_mode VARCHAR  -- 'summary' (default) or 'clusters'
) → TABLE
-- dbscan_mv takes comma-separated column names as the second argument
```

All parameters after `column_name` are optional; `dbscan('orders', 'amount')` uses the defaults.

**DBSCAN Output:**
- `'summary'` (one row): `status` (`fail` when noise points exist, else `pass`), `cluster_count`, `noise_count`, `total_count`, `noise_rate`, `largest_cluster_size`, `eps`, `min_pts`, `n_columns` (`dbscan_mv` only), `message`
- `'clusters'` (one row per non-NULL input row): `row_id`, `value` (univariate only), `cluster_id` (`-1` = noise), `point_type` (`'CORE'`, `'BORDER'`, `'NOISE'`), `neighbor_count`, `anomaly_score` (noise = 1.0), `is_anomaly`

**Identifier pass-through (`id_column`):**

The row-level outputs (`isolation_forest`/`isolation_forest_mv` in `'scores'` mode, `dbscan`/`dbscan_mv` in `'clusters'` mode, `outlier_tree` in `'outliers'` mode, and `pii_audit_table`) take an optional named parameter `id_column := '<column>'`. The value of that column is added to every output row, directly after `row_id`, under the column's own name and type, so results join back to the input without relying on row order. `row_id` is kept unchanged; without `id_column` the output schema is unchanged.

```sql
-- Score every order and keep its key
SELECT order_id, anomaly_score
FROM isolation_forest('orders', 'amount', 100, 256, 0.05, 'scores', id_column := 'order_id')
WHERE is_anomaly;
-- row_id | order_id | value | anomaly_score | is_anomaly
```

- The id is read in the same pass as the analyzed values, so it always belongs to the scored row; rows skipped for NULL features are skipped together with their id
- NULL ids are passed through (the row is still analyzed)
- Errors: an unknown column, a non-row-level `output_mode` (e.g. `'summary'`), or a name that collides with an output column (e.g. an input column named `anomaly_score`)

### Data Diffing Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_diff_hashdiff` | `(source_table VARCHAR, target_table VARCHAR, primary_key VARCHAR\|LIST<VARCHAR>) → TABLE` | Row-level diff (currently identical to joindiff; `bisection_threshold`/`bisection_factor` are not implemented and rejected). Parameters also by name (`primary_keys :=` for a list); named values override positional ones |
| `anofox_tab_diff_joindiff` | `(source_table VARCHAR, target_table VARCHAR, primary_key VARCHAR\|LIST<VARCHAR> [, compare_columns LIST<VARCHAR> [, include_all BOOLEAN]]) → TABLE` | Detailed row-level diff with target data. Parameters also by name (`primary_keys :=` for a list); named values override positional ones |

### Data Profiling Functions

<a name="data-profiling-functions"></a>

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_profile_table` | `(table_name VARCHAR, columns VARCHAR[] DEFAULT [], sample_size BIGINT DEFAULT 1000000, exact BOOLEAN DEFAULT false) → TABLE` | Per-column statistics (27 output columns) |
| `anofox_tab_profile_summary` | `(table_name VARCHAR) → TABLE` | Single-row table overview (11 output columns) |
| `anofox_tab_profile_correlations` | `(table_name VARCHAR, columns VARCHAR[] DEFAULT []) → TABLE` | Pairwise Pearson and Spearman correlations for numeric columns |

**Aliases:** `profile_table`, `profile_summary`, `profile_correlations`

**`profile_table` Parameters:**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | VARCHAR | — | Name of the table to profile |
| `columns` | VARCHAR[] | `[]` (all columns) | Optional column name filter |
| `sample_size` | BIGINT | 1,000,000 | Maximum rows to sample |
| `exact` | BOOLEAN | false | When true, always scan the full table (ignores sample_size) |

**`profile_table` Output Schema (27 columns):**

| Column | Type | Description |
|--------|------|-------------|
| `column_name` | VARCHAR | Column name |
| `column_type` | VARCHAR | DuckDB data type |
| `row_count` | BIGINT | Total rows in table |
| `null_count` | BIGINT | Number of NULL values |
| `null_rate` | DOUBLE | Fraction of NULLs |
| `distinct_count` | BIGINT | Distinct value count |
| `distinct_rate` | DOUBLE | Fraction of distinct values |
| `min_val` | VARCHAR | Minimum value as string (NULL for STRUCT/MAP/UNION) |
| `max_val` | VARCHAR | Maximum value as string (NULL for STRUCT/MAP/UNION) |
| `mean` | DOUBLE | Arithmetic mean (numeric columns only) |
| `median` | DOUBLE | Median value (numeric columns only) |
| `stddev` | DOUBLE | Standard deviation (numeric columns only) |
| `p25` | DOUBLE | 25th percentile (numeric columns only) |
| `p75` | DOUBLE | 75th percentile (numeric columns only) |
| `skewness` | DOUBLE | Skewness (numeric columns only) |
| `kurtosis` | DOUBLE | Kurtosis (numeric columns only) |
| `top_values` | LIST(STRUCT(value VARCHAR, count BIGINT)) | Most-frequent values with counts |
| `avg_length` | DOUBLE | Avg string length / list element count / map cardinality |
| `min_length` | BIGINT | Min string length / list element count / map cardinality |
| `max_length` | BIGINT | Max string length / list element count / map cardinality |
| `pattern_summary` | VARCHAR | Detected pattern: `email`, `phone`, `uuid`, `url`, `ip_address`, `iso_date`, `numeric_string`, `mixed`, `list`, `map`, `struct`, `union` |
| `is_unique` | BOOLEAN | True if all non-NULL values are distinct |
| `is_constant` | BOOLEAN | True if all non-NULL values are the same |
| `zero_count` | BIGINT | Number of zero values (numeric only) |
| `negative_count` | BIGINT | Number of negative values (numeric only) |
| `is_sampled` | BOOLEAN | True if a sample was taken |
| `actual_sample_size` | BIGINT | Number of rows actually scanned |

**`profile_summary` Output Schema (11 columns, one row):**

| Column | Type | Description |
|--------|------|-------------|
| `row_count` | BIGINT | Total rows in the table |
| `column_count` | BIGINT | Total columns in the table |
| `numeric_columns` | BIGINT | Number of numeric columns |
| `string_columns` | BIGINT | Number of string columns |
| `temporal_columns` | BIGINT | Number of date/timestamp columns |
| `boolean_columns` | BIGINT | Number of boolean columns |
| `complex_columns` | BIGINT | Number of complex-type columns (LIST, MAP, STRUCT, UNION) |
| `total_nulls` | BIGINT | Total NULL cells across all columns |
| `total_null_rate` | DOUBLE | Fraction of cells that are NULL (total_nulls / row_count / column_count) |
| `duplicate_row_count` | BIGINT | Number of duplicate rows (row_count minus distinct rows) |
| `estimated_memory_bytes` | BIGINT | Estimated in-memory size in bytes (row_count × column_count × 8) |

**`profile_correlations` Output Schema:**

| Column | Type | Description |
|--------|------|-------------|
| `column_a` | VARCHAR | First column name |
| `column_b` | VARCHAR | Second column name |
| `pearson` | DOUBLE | Pearson correlation coefficient (−1 to 1) |
| `spearman` | DOUBLE | Spearman rank correlation coefficient (−1 to 1) |
| `n` | BIGINT | Number of non-NULL row pairs used |

---

## ⚙️ Configuration

Set options with `SET name = value;` after `LOAD anofox_tabular;`. List them all with:

```sql
SELECT name, input_type, value, description
FROM duckdb_settings()
WHERE name LIKE 'anofox%' OR name = 'datazoo_banner'
ORDER BY name;
```

The 24 settings are grouped by prefix: `anofox_tab_*` for email, phone, postal and tracing,
`anofox_pii_*` and `anofox_ner_*` for PII detection, `anofox_telemetry_*` for telemetry, and
`datazoo_banner` for the DataZoo feedback banner.

| Setting | Type | Default | Description |
|---------|------|---------|-------------|
| `anofox_tab_email_default_validation` | VARCHAR | `regex` | Default mode of `email_is_valid` / `email_validate` (`regex`, `dns`, `smtp`) |
| `anofox_tab_email_regex_pattern` | VARCHAR | RFC 5322 inspired pattern | Regular expression used by the `regex` stage |
| `anofox_tab_email_dns_timeout_ms` | BIGINT | `1000` | DNS resolver timeout in milliseconds |
| `anofox_tab_email_dns_tries` | INTEGER | `1` | DNS attempts before failing (1-10) |
| `anofox_tab_email_smtp_port` | INTEGER | `25` | SMTP port used when connecting to MX hosts |
| `anofox_tab_email_smtp_connect_timeout_ms` | BIGINT | `5000` | SMTP connect timeout in milliseconds |
| `anofox_tab_email_smtp_read_timeout_ms` | BIGINT | `5000` | SMTP read/write timeout in milliseconds |
| `anofox_tab_email_smtp_helo_domain` | VARCHAR | `duckdb.local` | Domain sent in SMTP EHLO |
| `anofox_tab_email_smtp_mail_from` | VARCHAR | `validator@duckdb.local` | MAIL FROM address used during SMTP verification |
| `anofox_tab_phonenumber_default_region` | VARCHAR | `US` | Region used when the region hint is NULL; must be a supported 2-letter ISO code |
| `anofox_tab_postal_data_path` | VARCHAR | `.duckdb/extensions/libpostal` | Directory of the libpostal data, relative to the working directory unless absolute; its parent directory must exist |
| `anofox_tab_trace_enabled` | BOOLEAN | `true` | Print `[anofox] …` trace lines to the console |
| `anofox_tab_trace_level` | VARCHAR | `info` | Minimum trace level (`trace`, `debug`, `info`, `warn`, `error`, `critical`, `off`) |
| `anofox_pii_enabled_types` | VARCHAR | empty (all types) | Comma-separated PII types to detect |
| `anofox_pii_min_confidence` | DOUBLE | `0.5` | Minimum confidence for NER-based detection (0.0-1.0) |
| `anofox_pii_default_mask_strategy` | VARCHAR | `redact` | Strategy of `pii_redact_column(value)` and `pii_audit_table` (`redact`, `hash`, `partial`, `asterisk`, `none`) |
| `anofox_pii_deep_validation` | BOOLEAN | `false` | Validate detected phone numbers with the phone module |
| `anofox_ner_auto_download` | BOOLEAN | `true` | Download the NER model (~66 MB) on first use; `false` skips NER detection instead |
| `anofox_ner_model` | VARCHAR | `distilbert-en` | NER model; set it before the model is first loaded |
| `anofox_ner_device` | VARCHAR | `AUTO` | OpenVINO device (`AUTO`, `CPU`, `GPU`, `GPU.0`, …) |
| `anofox_ner_cache_size` | BIGINT | `10000` | LRU cache size for NER results (0 disables) |
| `anofox_telemetry_enabled` | BOOLEAN | `true` | Anonymous usage telemetry |
| `anofox_telemetry_key` | VARCHAR | built-in project key | PostHog API key for telemetry |
| `datazoo_banner` | BOOLEAN | `true` | Show the feedback banner in interactive terminals (at most once a day) |

### Email Settings

```sql
SET anofox_tab_email_default_validation = 'regex';  -- Default: regex
SET anofox_tab_email_regex_pattern = '<your-pattern>';  -- RFC 5322 inspired
SET anofox_tab_email_dns_timeout_ms = 1000;  -- DNS timeout per try (1-5000ms)
SET anofox_tab_email_dns_tries = 1;  -- DNS retry count (1-10)
SET anofox_tab_email_smtp_port = 25;  -- SMTP port
SET anofox_tab_email_smtp_connect_timeout_ms = 5000;  -- TCP connect timeout
SET anofox_tab_email_smtp_read_timeout_ms = 5000;  -- Read/write timeout
SET anofox_tab_email_smtp_helo_domain = 'duckdb.local';  -- HELO/EHLO domain
SET anofox_tab_email_smtp_mail_from = 'validator@duckdb.local';  -- MAIL FROM address
```

### Postal Settings

```sql
SET anofox_tab_postal_data_path = '/data/libpostal';  -- the parent directory must exist

-- Download the libpostal data (~500 MB) up front; the first postal call does it otherwise
SELECT postal_load_data();
```

### Phone Settings

```sql
SET anofox_tab_phonenumber_default_region = 'US';  -- Default region code
```

The default region must be a supported 2-letter ISO region code; unknown codes are rejected.

### NER Settings

NER-based PII detection (NAME, ORGANIZATION, LOCATION, MISC) downloads its model from Hugging Face on first use (~66 MB, cached in `~/.duckdb/extensions/anofox/ner/<model>/`).

```sql
SET anofox_ner_auto_download = false;  -- Never download; skip NER detection when the model is not on disk (default: true)
SET anofox_ner_model = 'distilbert-en';  -- Must be set before the model is first loaded
SET anofox_ner_device = 'AUTO';  -- OpenVINO device: AUTO, CPU, GPU, ...
SET anofox_ner_cache_size = 10000;  -- LRU cache of NER results (0 disables)

SELECT model_status, status_message FROM anofox_ner_status();  -- NOT_DOWNLOADED when the download was skipped
```

For offline machines, copy `model_quantized.onnx` and `tokenizer.json` from a machine that has them into the same directory.

### PII Settings

```sql
SET anofox_pii_enabled_types = 'EMAIL,US_SSN';    -- detect only these types (empty = all)
SET anofox_pii_min_confidence = 0.7;              -- NER confidence threshold
SET anofox_pii_default_mask_strategy = 'partial'; -- used by pii_redact_column(value) and pii_audit_table
SET anofox_pii_deep_validation = true;            -- check detected phone numbers with the phone module
SELECT * FROM pii_config();
```

### Tracing

Tracing is on by default at level `info`, so the CLI prints `[anofox] [info] …` lines when the
extension loads and while functions run. To quiet it:

```sql
SET anofox_tab_trace_level = 'warn';   -- only warnings and errors
SET anofox_tab_trace_enabled = false;  -- or no trace output at all
```

### Telemetry and banner

```sql
SET anofox_telemetry_enabled = false;  -- see Telemetry below
SET datazoo_banner = false;            -- no feedback banner
```

---

## 🛠️ Building from Source

| Requirement | Version | Notes |
|-------------|---------|-------|
| **DuckDB** | v1.5.6 or 2.0 | Required |
| **C++ Compiler** | GCC 8+, Clang 7+, MSVC 2019+ | C++17 compatible |
| **CMake** | ≥ 3.21 | Build system |
| **Ninja** | Any | Recommended for faster builds |
| **vcpkg** | Latest | Dependency management |
| **libpostal** | Optional | For address parsing (~500MB data) |

**vcpkg Setup:**

```bash
# Clone vcpkg
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

**Build:**

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-tabular.git
cd anofox-tabular

# Build (uses ninja for speed)
GEN=ninja make release

# Run tests
make test
```

**Build Artifacts:**
- `build/release/duckdb` - DuckDB CLI with extension statically linked
- `build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension` - Loadable extension binary
- `build/release/test/unittest` - Test runner

---

## 🧪 Testing

Anofox Tabular includes comprehensive SQL test coverage:

```bash
# Run full test suite
make test

# Run specific test file
./build/release/test/unittest test/sql/anofox_email_basic.test

# Debug mode tests
make test_debug

# NER tests (OpenVINO build; downloads the NER model on first run)
make test_ner
```

The SQL tests live in [`test/sql/`](test/sql/), one or more `.test` files per module. The postal
tests (`anofox_postal.test.disabled`, `anofox_postal.test_disabled`) are disabled; postal
coverage that needs no data runs in `anofox_postal_null.test` when `POSTAL_AVAILABLE` is set.

---

## 🎯 Use Cases

### Data Engineering

- **ETL Validation**: Verify data quality at each pipeline stage
- **Schema Evolution**: Track schema changes over time with diff functions
- **Data Profiling**: Generate quality reports with metric functions

### Data Science

- **Outlier Detection**: Identify anomalies in training data
- **Feature Engineering**: Parse and normalize addresses/phones for ML
- **Data Cleaning**: Validate and standardize contact information

### Data Operations

- **Monitoring**: Set up alerts based on quality metric thresholds
- **Regression Testing**: Compare production vs staging environments
- **Migration Validation**: Ensure data integrity during migrations

### Compliance & Security

- **PII Validation**: Verify email/phone format compliance
- **VAT Compliance**: Validate European customer VAT numbers
- **Fraud Detection**: Use anomaly detection for suspicious transactions
- **Audit Trails**: Track data changes with diffing capabilities

### Financial Operations

- **Multi-Currency Reporting**: Handle international transactions
- **Invoice Reconciliation**: Validate financial calculations
- **Currency-Safe Arithmetic**: Prevent mixing currencies in operations

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    DuckDB SQL Layer                      │
├─────────────────────────────────────────────────────────┤
│                Anofox Tabular Extension                  │
│  ┌────────────┬────────────┬────────────┬─────────────┐ │
│  │   Email    │   Postal   │   Phone    │   Quality   │ │
│  │ Validation │  Parsing   │  Parsing   │   Checks    │ │
│  └────────────┴────────────┴────────────┴─────────────┘ │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  Money · VAT · PII · Profiling · Diff                │ │
│  │  Anomaly Detection (IF, DBSCAN, OutlierTree)         │ │
│  └─────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                 External Libraries                       │
│ libpostal · c-ares · spdlog · OpenVINO │
└─────────────────────────────────────────────────────────┘
```

**Design Principles:**
- **Zero-copy processing**: Operate directly on DuckDB's columnar data
- **Streaming evaluation**: Process large datasets without memory spikes
- **Vectorized execution**: Leverage SIMD for performance
- **Shared configuration**: Centralized settings via DuckDB's option system

---

## 🤝 Contributing

We welcome contributions! Here's how to get started:

1. **Fork the repository**
2. **Create a feature branch**: `git checkout -b feature/amazing-feature`
3. **Make your changes** following our coding standards (see `CLAUDE.md`)
4. **Add tests**: All new features must include SQL tests
5. **Commit**: `git commit -m 'Add amazing feature'`
6. **Push**: `git push origin feature/amazing-feature`
7. **Open a Pull Request**

### Development Guidelines

- Write SQL tests in `test/sql/` using SQLLogicTest format
- Add tracing statements for debugging
- Update documentation for new features
- Follow the existing code structure and naming conventions
- Ensure all tests pass: `make test`

See [`CLAUDE.md`](CLAUDE.md) for detailed development context.

---

## 🔧 Troubleshooting

### Extension not loading

**Error**: `Extension "anofox_tabular" not found`

**Solution**: Install it from the community repository, or upgrade a cached copy:
```sql
INSTALL anofox_tabular FROM community;
FORCE INSTALL anofox_tabular FROM community;  -- upgrade
LOAD anofox_tabular;
```

### Libpostal data missing

**Error**: `Failed to initialize libpostal. Use anofox_tab_postal_load_data() to download assets.`
or `Failed to create directory ".duckdb/extensions/libpostal"`

**Solution**: The data directory defaults to `.duckdb/extensions/libpostal` relative to the working
directory, and its parent directory must exist. Point it at a writable directory and download
the data (~500 MB):
```sql
SET anofox_tab_postal_data_path = '/path/to/libpostal/data';
SELECT postal_load_data();
```

### SMTP timeout errors

**Solution**: Increase timeout settings:
```sql
SET anofox_tab_email_smtp_connect_timeout_ms = 10000;
SET anofox_tab_email_smtp_read_timeout_ms = 10000;
```

### Memory issues with large datasets

**Solution**: Use streaming output modes and process in batches:
```sql
-- Use 'summary' mode for large tables
SELECT * FROM isolation_forest(
    'large_table', 'column', 100, 256, 0.1, 'summary'
);

-- Process in batches
CREATE TABLE anomalies AS
SELECT * FROM isolation_forest(
    'large_table', 'column', 100, 256, 0.1, 'scores'
)
WHERE is_anomaly = true;  -- Filter early
```

---

## 🌟 Performance Tips

1. **Choose the right validation mode**: Use `regex` for speed, `dns` for verification, `smtp` for full validation
2. **Batch processing**: Process rows in SQL rather than in application code
3. **Filter early**: Apply WHERE clauses before validation when possible
4. **Use summary modes**: For large datasets, use `summary` output mode first
5. **Connection pooling**: Reuse DuckDB connections across queries

```sql
-- ✓ Good: Filter before validation
SELECT *
FROM large_table
WHERE created_at > NOW() - INTERVAL '1 day'
  AND email_is_valid(email, 'regex');

-- ✗ Slow: Validate all rows first
SELECT *
FROM (
    SELECT *, email_is_valid(email, 'regex') as valid
    FROM large_table
)
WHERE valid AND created_at > NOW() - INTERVAL '1 day';
```

---

## 📡 Telemetry

Anofox Tabular collects **anonymous usage data** to help improve the extension. This is enabled by default.

### What We Collect

- Extension load events (name, version, DuckDB version, platform)
- Function execution counts, aggregated per function and session (function name, call count, median duration)
- A pseudonymous machine identifier (SHA-256 of a machine id)

**We do NOT collect:** query text, table or column names, data values, file paths, or any personally identifiable information. See [TELEMETRY.md](TELEMETRY.md) for the full list.

### Disabling Telemetry

**Option 1: Environment Variable** (recommended — also suppresses the extension-load event)

```bash
export DATAZOO_DISABLE_TELEMETRY=1
```

**Option 2: SQL Setting**

```sql
SET anofox_telemetry_enabled = false;
```

Note: SQL `SET` can only run after the extension is loaded, so it disables all
*subsequent* events (function executions) but cannot suppress the one-time
extension-load event. To opt out of that event too, use the environment variable,
or pre-set the option in the client configuration before loading the extension
(requires `allow_unrecognized_options`), e.g. in Python:

```python
con = duckdb.connect(config={
    "allow_unrecognized_options": True,
    "anofox_telemetry_enabled": False,
})
```

---

## Feedback

If `anofox_tabular` misbehaves — a profile that looks wrong, a PII match it missed,
a diff that surprises you — please
[open an issue](https://github.com/DataZooDE/anofox-tabular/issues). Results depend on
real data shapes we cannot reproduce here, so a report with your input is the fastest
path to a fix. Errors from the diff and metric functions end with that link.

If it saved you time, a star on the repo helps other people find it.

The first time you load the extension in an interactive terminal each day, a small
banner says the same thing. It never prints when output is piped, in notebooks, or in
CI. Silence it with `SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

---

## 📜 License

This project is licensed under the **Business Source License (BSL) 1.1**.

| Permission | Status |
|------------|--------|
| Production use | ✅ Permitted |
| Development & testing | ✅ Permitted |
| Hosted/embedded service | ❌ Restricted |
| MPL 2.0 conversion | 🔄 After 5 years |

See [LICENSE](LICENSE) for full details.

---

## 📋 Third-Party Attributions

This project incorporates third-party libraries and their respective licenses. For complete license information and copyright notices, please see [THIRD_PARTY_NOTICE](THIRD_PARTY_NOTICE).

**Third-Party Libraries:**
- **[libpostal](https://github.com/openvenues/libpostal)** - Address parsing and normalization (MIT License)
- **[spdlog](https://github.com/gabime/spdlog)** - Fast C++ logging library (MIT License)
- **[c-ares](https://github.com/c-ares/c-ares)** - Asynchronous DNS resolver (MIT License)
- **[curl](https://curl.se/)** - HTTP client library (curl License)
- **[OpenSSL](https://www.openssl.org/)** - Cryptography library (Apache License 2.0)
- **[OpenVINO](https://github.com/openvinotoolkit/openvino)** - Intel AI inference toolkit for NER (Apache License 2.0)

---

## 🙏 Acknowledgments

- **[DuckDB Team](https://duckdb.org/)** - For the amazing embedded analytics database
- **[isotree](https://github.com/david-cortes/isotree)** - Inspiration for Enhanced Isolation Forest features (Extended IF, SCiForest, density scoring)
- **[libpostal](https://github.com/openvenues/libpostal)** - Statistical NLP library for parsing world addresses
- **[libphonenumber](https://github.com/google/libphonenumber)** - Google's comprehensive phone number handling library (custom implementation)
- **[OpenVINO](https://github.com/openvinotoolkit/openvino)** - Intel's inference optimization toolkit for NER-based entity detection (NAME, ORGANIZATION, LOCATION, MISC)
- **c-ares** - Asynchronous DNS resolver library

---

## 🔗 Links

- **GitHub**: https://github.com/DataZooDE/anofox-tabular
- **DuckDB**: https://duckdb.org/
- **Issues**: https://github.com/DataZooDE/anofox-tabular/issues

---

<p align="center">
  <sub>Built with ❤️ for the DuckDB community</sub>
</p>

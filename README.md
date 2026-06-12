# Anofox Tabular

> **A data quality and validation toolkit for DuckDB**

[![DuckDB](https://img.shields.io/badge/DuckDB-1.4_LTS_%7C_1.5.3-green)](https://duckdb.org/)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/badge/license-BSL%201.1-blue.svg)](LICENSE)
[![Functions](https://img.shields.io/badge/Functions-81-green)]()
[![Modules](https://img.shields.io/badge/Modules-10-blue)]()

SQL-native validation, anomaly detection, and data diffing—all without leaving your database.

> **⚠️ Breaking Change in v0.2.0:** Function names updated from `anofox_*` to `anofox_tab_*` prefix with convenient aliases (e.g., `email_is_valid`). See [API Reference](docs/API_REFERENCE.md) for details.

### Important Notes

- **Naming Convention:** Functions use `anofox_tab_*` prefix with shorter aliases available
- **Positional Parameters:** All functions use positional parameters, NOT named parameters (`:=` syntax)
- **Backward Compatible:** New isolation forest parameters are optional; existing queries work unchanged
- **Enhanced in v0.2.0:** Isolation Forest now supports categorical columns, Extended IF, SCiForest, density scoring

```sql
-- Email validation with DNS verification
SELECT email_validate('user@example.com', 'dns') as result;

-- Detect outliers using Isolation Forest
SELECT * FROM metric_isolation_forest('sales', 'amount', 100, 256, 0.1, 'scores');

-- Validate European VAT and multi-currency transactions
SELECT * FROM transactions
WHERE vat_is_valid(vat_id)
  AND money_is_positive(amount);
```

---

## 🌟 Why Anofox Tabular?

**The DuckDB extension combining validation, anomaly detection, and data diffing.**

- ✅ **10 Production-Ready Modules** - 81 SQL functions for email, postal, phone, money, VAT, PII, metrics, anomalies, diffing, and profiling
- ⚡ **Blazing Fast** - Vectorized C++17 implementation processes millions of rows per second
- 🔌 **Zero Friction** - SQL-native with no external services; works entirely within DuckDB
- 📦 **Self-Contained** - Embedded validation patterns; no API keys or network calls required
- 🎯 **Production-Grade** - Used in financial compliance, fraud detection, and migration validation

**vs. Python Libraries:** No context switching, no data movement, 10-100x faster
**vs. External APIs:** No latency, no rate limits, works offline, data stays local

---

## Key Features

**Validation & Normalization:**
- Email validation with DNS/SMTP verification
- International address parsing via libpostal
- Phone number handling via libphonenumber
- European VAT compliance (29 countries)
- Multi-currency arithmetic (10 currencies)

**Data Quality & Anomaly Detection:**
- 8 data quality metrics (volume, nulls, freshness, schema)
- Enhanced Isolation Forest with categorical support, Extended IF, SCiForest
- DBSCAN density-based clustering
- Statistical outliers via Z-Score and IQR

**Data Operations:**
- Hash-based and join-based table diffing
- Migration validation and change detection

**Performance:**
- Native DuckDB vectorized execution
- Zero external API dependencies
- Millions of rows per second throughput

---

## 📑 Table of Contents

- [Feature Overview](#-feature-overview)
- [Quick Start](#-quick-start)
- [Installation](#-installation)
  - [Python Package](#python-package)
- [Features](#-features-all-10-modules)
  - [Email Validation](#-email-validation)
  - [Address Parsing](#-address-parsing--normalization)
  - [Phone Number Validation](#-phone-number-validation)
  - [Money & Currency Operations](#-money--currency-operations)
  - [VAT Validation](#-vat-validation)
  - [PII Detection](#-pii-detection)
  - [Data Quality Metrics](#-data-quality-metrics)
  - [Anomaly Detection](#-anomaly-detection)
  - [Data Diffing](#-data-diffing)
  - [Data Profiling](#-data-profiling)
- [Real-World Examples](#-real-world-examples)
- [SQL Function Reference](#-sql-function-reference)
- [Configuration](#-configuration)
- [Testing](#-testing)
- [Use Cases](#-use-cases)
- [Architecture](#-architecture)
- [Contributing](#-contributing)
- [Troubleshooting](#-troubleshooting)
- [Performance Tips](#-performance-tips)
- [Telemetry](#-telemetry)
- [License](#-license)

---

## 📊 Feature Overview

| Module | Functions | Use Case | Status |
|--------|-----------|----------|--------|
| 📧 **Email Validation** | 3 | RFC 5322, DNS, SMTP verification | Stable |
| 📮 **Address Parsing** | 4 | International address normalization | Stable |
| 📞 **Phone Numbers** | 9 | Google libphonenumber integration | Stable |
| 💰 **Money & Currency** | 17 | Multi-currency operations, 10 currencies | ✨ New |
| 💼 **VAT Validation** | 10 | European VAT compliance, 29 countries | ✨ New |
| 🕵️ **PII Detection** | 20 | Detect & mask 17 PII types (SSN, IBAN, NAME, etc.) | ✨ New |
| 🔍 **Quality Metrics** | 8 | Volume, nulls, freshness, schema checks | Stable |
| 🤖 **Anomaly Detection** | 5 | Isolation Forest, DBSCAN, OutlierTree (explainable) | ✨ Enhanced |
| 🔄 **Data Diffing** | 2 | Table comparison, migration validation | Stable |
| 📊 **Data Profiling** | 3 | Column stats, correlations, table overview | ✨ New |

**Total: 81 SQL Functions** | **Zero Required Dependencies***

<sub>*Except libpostal (address parsing) and optional DNS/SMTP for email</sub>

---

## 🚀 Quick Start

```bash
# Clone and build
git clone https://github.com/datazoo/anofox-tabular.git
cd anofox-tabular
git submodule update --init --recursive
make release

# Launch DuckDB with extension loaded
./build/release/duckdb
```

```sql
-- Inside DuckDB
LOAD anofox_tabular;

-- Try email validation
SELECT email_is_valid('user@example.com', 'regex') as valid;

-- Detect data anomalies
SELECT * FROM metric_isolation_forest(
    'your_table', 'numeric_column', 100, 256, 0.1, 'scores'
) WHERE is_anomaly = true;

-- Validate European VAT numbers
SELECT vat_id, vat_is_valid(vat_id) as is_valid FROM customers;
```

Try the examples:
```bash
cd examples
uv run email_verification.py
uv run postal_verification.py
```

---

## 📦 Installation

### Technical Requirements

| Requirement | Version | Notes |
|-------------|---------|-------|
| **DuckDB** | ≥ v1.4.2 | Required |
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

### Building from Source

```bash
# Clone repository with submodules
git clone https://github.com/datazoo/anofox-tabular.git
cd anofox-tabular
git submodule update --init --recursive

# Build (uses ninja for speed)
GEN=ninja make release

# Run tests
make test
```

**Build Artifacts:**
- `build/release/duckdb` - DuckDB CLI with extension statically linked
- `build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension` - Loadable extension binary
- `build/release/test/unittest` - Test runner

### Python Package

Install the native Python wrapper — no raw SQL required:

```bash
pip install anofox-tabular
# with DataFrame support:
pip install "anofox-tabular[pandas]"
pip install "anofox-tabular[polars]"
```

```python
import anofox
from anofox import validate, quality, anomaly, pii, diff

conn = anofox.connect()  # extension downloaded automatically

validate.email_is_valid(conn, "hi@example.com")           # True
validate.email_is_valid(conn, "hi@example.com", mode="dns")

import pandas as pd
df = pd.DataFrame({"email": ["a@b.com", "bad@", "c@d.org"]})
result_df = validate.email_is_valid(conn, df, column="email")

quality.volume(conn, "my_table", min_rows=100)             # {"status": "pass", ...}
anomaly.isolation_forest(conn, "my_table", "amount")       # {"status": ..., "n_anomalies": ...}
changes = diff.joindiff(conn, df_before, df_after, primary_keys="id")
```

See [`python/README.md`](python/README.md) for the full Python API reference.

### Running raw-SQL examples

```bash
cd examples
uv sync  # or: pip install -r requirements.txt
uv run email_verification.py
uv run postal_verification.py
```

### Multi-Language Accessibility

Works with any DuckDB language binding:
- **Python**: `pip install anofox-tabular` (native wrapper with DataFrame support) or raw `duckdb` package
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
- Automatic data download (~500MB language models)
- Support for international addresses
- Configurable data directory

[📖 See complete Postal module documentation](#postal-functions)

---

### 📞 Phone Number Validation

International phone parsing via **[libphonenumber](https://github.com/google/libphonenumber)**, Google's library for parsing and formatting phone numbers:

```sql
-- Parse and validate phone numbers
SELECT phonenumber_parse('+1 (415) 555-1234', 'US');
-- Returns: {valid: true, country_code: 1, national_number: '4155551234', ...}

-- Format in different styles
SELECT phonenumber_format('4155551234', 'US', 'INTERNATIONAL');
-- Returns: '+1 415-555-1234'
```

**Formats:** E164, INTERNATIONAL, NATIONAL, RFC3966

[📖 See complete Phone module documentation](#phone-functions)

---

### 💰 Money & Currency Operations

International monetary value handling with currency-aware arithmetic and formatting.

**Key Features:**
- 17 SQL functions for financial operations
- 10 major currencies (USD, EUR, GBP, JPY, CAD, AUD, CHF, CNY, INR, BRL)
- Currency-safe arithmetic (prevents mixing currencies)
- Locale-aware formatting (symbol placement, decimal marks)
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
- 29 countries supported (28 EU + UK)
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

**Masking Strategies:**
- `REDACT` - Replace with [TYPE] label
- `PARTIAL` - Show partial value (e.g., `***-**-6789` for SSN)
- `ASTERISK` - Replace with same-length asterisks
- `HASH` - Replace with SHA-256 hash (truncated)

```sql
-- Detect all PII in text
SELECT unnest(pii_detect('My SSN is 123-45-6789 and email is test@example.com'));
-- Returns: [{type: 'US_SSN', text: '123-45-6789', ...}, {type: 'EMAIL', ...}]

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
| **Volume** | Row count thresholds | `metric_volume('orders', 1000, 1000000)` |
| **Null Rate** | Missing value detection | `metric_null_rate('users', 'email', 0.05)` |
| **Distinctness** | Cardinality validation | `metric_distinct_count('products', 'sku', 100, NULL)` |
| **Freshness** | Data recency checks | `metric_freshness('events', 'timestamp', INTERVAL '1 hour')` |
| **Schema** | Required column validation | `metric_schema('table', ['id', 'created_at'])` |

**Statistical Outlier Detection:**
- **Z-Score** - Parametric outlier detection (assumes normal distribution)
- **IQR** - Non-parametric outlier detection (robust to distribution)

```sql
-- Find statistical outliers using IQR method
SELECT * FROM metric_iqr('transactions', 'amount', 1.5);
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
SELECT * FROM metric_isolation_forest(
    'sales_data',
    'amount',
    100,        -- n_trees
    256,        -- sample_size
    0.1,        -- contamination (expected anomaly rate)
    'scores'    -- output mode: 'summary' or 'scores'
) WHERE is_anomaly = true;

-- Extended IF with hyperplane splits (ndim=3)
SELECT * FROM metric_isolation_forest_multivariate(
    'transactions',
    'amount, quantity, duration',
    100, 256, 0.1, 'scores',
    3,          -- ndim: hyperplane dimensions
    'normal',   -- coef_type: coefficient distribution
    'depth'     -- scoring_metric
);

-- SCiForest with gain-based split selection
SELECT * FROM metric_isolation_forest_multivariate(
    'customer_events',
    'purchase_amount, session_duration',
    100, 256, 0.05, 'scores',
    1, 'uniform', 'depth',
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

**New Parameters (v0.2.0):**
| Parameter | Values | Description |
|-----------|--------|-------------|
| `ndim` | 1-N (default: 1) | Dimensions for hyperplane splits |
| `coef_type` | 'uniform', 'normal' | Coefficient distribution |
| `scoring_metric` | 'depth', 'density', 'adj_depth' | Scoring method |
| `weight_column` | column name | Sample weight column |
| `ntry` | 1-100 (default: 1) | Split candidates per node |
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
- `'outliers'`: Per-outlier rows with z-scores, bounds, and explanations

**Semantics:**
- Rows containing NULL in any selected column are excluded from the analysis
- `row_id` is the 1-based position of the row in the source table (NULL rows still count toward the position)
- `total_rows` (summary mode) is the number of analyzed rows, i.e. rows that are non-NULL in every selected column

---

### 🔄 Data Diffing

Compare tables and identify changes:

```sql
-- Join-based diff (row-level changes)
SELECT * FROM diff_joindiff('source_tbl', 'target_tbl', ['user_id', 'date'])
WHERE diff_type IN ('added', 'changed')
LIMIT 100;

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
-- Load fraudulent emails dataset (5000+ spam/phishing samples)
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
    SELECT * FROM metric_isolation_forest_multivariate(
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

-- Identify specific changed records
SELECT
    user_id,
    source_email,
    target_email,
    source_created_at,
    target_created_at
FROM diff_joindiff('prod.users', 'staging.users', ['user_id'])
WHERE diff_type = 'changed'
  AND source_email != target_email
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

#### Basic Operations
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_tab_money` | `(amount, currency_code)` | STRUCT | Create a money value from amount and currency code |
| `anofox_tab_money_from_cents` | `(cents, currency_code)` | STRUCT | Create a money value from integer cents |
| `anofox_tab_money_amount` | `(money)` | DOUBLE | Extract amount from money struct |
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
| `anofox_tab_money_format` | `(money, style)` | VARCHAR | Format money for display (3 styles: 'symbol', 'code', 'long') |

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
| `anofox_tab_pii_detect` | `(text VARCHAR)` | LIST(STRUCT) | Detect all PII in text (returns type, text, start, end, confidence) |
| `anofox_tab_pii_mask` | `(text VARCHAR [, strategy VARCHAR])` | VARCHAR | Mask all PII in text (default: 'redact') |
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
| `anofox_tab_pii_audit_table` | `(table VARCHAR [, columns VARCHAR])` | TABLE | Row-level PII audit with masking |
| `anofox_tab_pii_status` | `()` | TABLE | List supported PII types and recognizers |
| `anofox_tab_pii_config` | `()` | TABLE | Current PII configuration |

### Data Quality Metrics Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_metric_volume` | `(table VARCHAR [, min_rows BIGINT, max_rows BIGINT]) → TABLE` | Validate row count |
| `anofox_tab_metric_null_rate` | `(table VARCHAR, column VARCHAR [, max_null_rate DOUBLE]) → TABLE` | Check null percentage |
| `anofox_tab_metric_distinct_count` | `(table VARCHAR, column VARCHAR [, min BIGINT, max BIGINT]) → TABLE` | Validate cardinality |
| `anofox_tab_metric_schema` | `(table VARCHAR, required_cols LIST<VARCHAR>) → TABLE` | Check required columns exist |
| `anofox_tab_metric_freshness` | `(table VARCHAR, ts_col VARCHAR, max_age INTERVAL [, ref_time TIMESTAMP]) → TABLE` | Validate data recency |
| `anofox_tab_metric_zscore` | `(table VARCHAR, column VARCHAR [, threshold DOUBLE]) → TABLE` | Detect outliers via z-score (default: 3.0) |
| `anofox_tab_metric_iqr` | `(table VARCHAR, column VARCHAR [, multiplier DOUBLE]) → TABLE` | Detect outliers via IQR (default: 1.5) |

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

### Anomaly Detection Functions

| Function | Description |
|----------|-------------|
| `anofox_tab_isolation_forest` (alias `isolation_forest`) | Univariate Isolation Forest with all enhancements |
| `anofox_tab_isolation_forest_mv` (alias `isolation_forest_mv`) | Multivariate Isolation Forest |
| `anofox_tab_dbscan` (alias `dbscan`) | Univariate DBSCAN clustering |
| `anofox_tab_dbscan_mv` (alias `dbscan_mv`) | Multivariate DBSCAN clustering |
| `outlier_tree` | Explainable outlier detection with conditional distributions |

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
| `ndim` | 1-N | 1 | Hyperplane dimensions (Extended IF) |
| `coef_type` | - | 'uniform' | `'uniform'` or `'normal'` |
| `scoring_metric` | - | 'depth' | `'depth'`, `'density'`, `'adj_depth'` |
| `weight_column` | - | NULL | Sample weight column name |
| `ntry` | 1-100 | 1 | Split candidates (SCiForest) |
| `prob_pick_avg_gain` | 0.0-1.0 | 0.0 | Gain-based selection probability |

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

### Data Diffing Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_diff_hashdiff` | `(source VARCHAR, target VARCHAR, pk VARCHAR\|LIST<VARCHAR>) → TABLE` | Row-level diff (currently identical to joindiff; `bisection_threshold`/`bisection_factor` are not implemented and rejected) |
| `anofox_tab_diff_joindiff` | `(source VARCHAR, target VARCHAR, pk VARCHAR\|LIST<VARCHAR> [, compare_cols LIST<VARCHAR> [, include_all BOOLEAN]]) → TABLE` | Detailed row-level diff with target data |

### Data Profiling Functions

<a name="data-profiling-functions"></a>

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_tab_profile_table` | `(table_name VARCHAR, columns VARCHAR[] DEFAULT [], sample_size BIGINT DEFAULT 1000000, exact BOOLEAN DEFAULT false) → TABLE` | Per-column statistics (27 output columns) |
| `anofox_tab_profile_summary` | `(table_name VARCHAR) → TABLE` | Single-row table overview (10 output columns) |
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
| `pattern_summary` | VARCHAR | Detected pattern: `email`, `uuid`, `url`, `ip_address`, `iso_date`, `numeric_string`, `mixed`, `list`, `map`, `struct`, `union` |
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

Set options via SQL or DuckDB's configuration file:

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
SET anofox_tab_postal_data_path = '.duckdb/extensions/libpostal';  -- Data directory

-- Download libpostal data on first use
SELECT postal_load_data();
```

### Phone Settings

```sql
SET anofox_tab_phonenumber_default_region = 'US';  -- Default region code
```

The default region must be a supported 2-letter ISO region code; unknown codes are rejected.

### Tracing

```sql
SET anofox_tab_trace_enabled = true;  -- Enable/disable logging
SET anofox_tab_trace_level = 'info';  -- trace|debug|info|warn|error|critical|off
```

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
```

**Test Files:**
- `anofox_email_basic.test` - Email validation modes
- `anofox_postal.test` - Address parsing
- `anofox_phonenumber.test` - Phone validation
- `anofox_metric.test` - Data quality metrics
- `anofox_isolation_forest.test` - Anomaly detection
- `anofox_dbscan.test` - Clustering algorithms
- `anofox_diff.test` - Table comparison
- `anofox_money.test` - Money operations
- `anofox_vat.test` - VAT validation

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
│  │ Validation │  Parsing   │  Parsing   │   Metrics   │ │
│  └────────────┴────────────┴────────────┴─────────────┘ │
│  ┌─────────────────────────────────────────────────────┐ │
│  │  Money · VAT · Anomaly Detection (IF, DBSCAN) · Diff  │ │
│  └─────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│                 External Libraries                       │
│ libpostal · libphonenumber · c-ares · spdlog · OpenVINO │
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

**Solution**: Build the extension first:
```bash
make release
```

### Libpostal data missing

**Error**: `Libpostal data not found`

**Solution**: Download data automatically:
```sql
SELECT postal_load_data();
```

Or manually set the path:
```sql
SET anofox_tab_postal_data_path = '/path/to/libpostal/data';
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
SELECT * FROM metric_isolation_forest(
    'large_table', 'column', 100, 256, 0.1, 'summary'
);

-- Process in batches
CREATE TABLE anomalies AS
SELECT * FROM metric_isolation_forest(
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

- Extension load events (name, version, platform)
- Anonymized device identifier (hashed MAC address)
- Timestamp of events

**We do NOT collect:** query content, table data, file paths, or any personally identifiable information.

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

- **GitHub**: https://github.com/datazoo/anofox-tabular
- **DuckDB**: https://duckdb.org/
- **Issues**: https://github.com/datazoo/anofox-tabular/issues
- **Discussions**: https://github.com/datazoo/anofox-tabular/discussions

---

<p align="center">
  <sub>Built with ❤️ for the DuckDB community</sub>
</p>

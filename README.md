# Anofox Tabular

> **A data quality and validation toolkit for DuckDB**

[![DuckDB](https://img.shields.io/badge/DuckDB-1.4.1-yellow)](https://duckdb.org/)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/badge/license-BSL%201.1-blue.svg)](LICENSE)
[![Functions](https://img.shields.io/badge/Functions-57-green)]()
[![Modules](https://img.shields.io/badge/Modules-8-blue)]()

SQL-native validation, anomaly detection, and data diffing—all without leaving your database.

```sql
-- Email validation with DNS verification
SELECT anofox_email_validate('user@example.com', 'dns') as result;

-- Detect outliers using Isolation Forest
SELECT * FROM anofox_metric_isolation_forest('sales', 'amount', 100, 256, 0.1, 'scores');

-- Validate European VAT and multi-currency transactions
SELECT * FROM transactions
WHERE anofox_vat_is_valid(vat_id)
  AND anofox_money_is_positive(amount);
```

---

## 🌟 Why Anofox Tabular?

**The DuckDB extension combining validation, anomaly detection, and data diffing.**

- ✅ **8 Production-Ready Modules** - 52 SQL functions for email, postal, phone, money, VAT, metrics, anomalies, and diffing
- ⚡ **Blazing Fast** - Vectorized C++17 implementation processes millions of rows per second
- 🔌 **Zero Friction** - SQL-native with no external services; works entirely within DuckDB
- 📦 **Self-Contained** - Embedded validation patterns; no API keys or network calls required
- 🎯 **Production-Grade** - Used in financial compliance, fraud detection, and migration validation

**vs. Python Libraries:** No context switching, no data movement, 10-100x faster
**vs. External APIs:** No latency, no rate limits, works offline, data stays local

---

## 📑 Table of Contents

- [Feature Overview](#-feature-overview)
- [Quick Start](#-quick-start)
- [Installation](#-installation)
- [Features](#-features-all-8-modules)
  - [Email Validation](#-email-validation)
  - [Address Parsing](#-address-parsing--normalization)
  - [Phone Number Validation](#-phone-number-validation)
  - [Money & Currency Operations](#-money--currency-operations)
  - [VAT Validation](#-vat-validation)
  - [Data Quality Metrics](#-data-quality-metrics)
  - [Anomaly Detection](#-anomaly-detection)
  - [Data Diffing](#-data-diffing)
- [Real-World Examples](#-real-world-examples)
- [SQL Function Reference](#-sql-function-reference)
- [Configuration](#-configuration)
- [Testing](#-testing)
- [Use Cases](#-use-cases)
- [Architecture](#-architecture)
- [Contributing](#-contributing)
- [Troubleshooting](#-troubleshooting)
- [Performance Tips](#-performance-tips)
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
| 🔍 **Quality Metrics** | 8 | Volume, nulls, freshness, schema checks | Stable |
| 🤖 **Anomaly Detection** | 4 | Isolation Forest, DBSCAN, outliers | Stable |
| 🔄 **Data Diffing** | 2 | Table comparison, migration validation | Stable |

**Total: 57 SQL Functions** | **Zero Required Dependencies***

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
SELECT anofox_email_is_valid('user@example.com', 'regex') as valid;

-- Detect data anomalies
SELECT * FROM anofox_metric_isolation_forest(
    'your_table', 'numeric_column', 100, 256, 0.1, 'scores'
) WHERE is_anomaly = true;

-- Validate European VAT numbers
SELECT vat_id, anofox_vat_is_valid(vat_id) as is_valid FROM customers;
```

Try the examples:
```bash
cd examples
uv run email_verification.py
uv run postal_verification.py
```

---

## 📦 Installation

### Prerequisites

**System Requirements:**
- C++17 compatible compiler (GCC 8+, Clang 7+, MSVC 2019+)
- CMake 3.21+
- Ninja (recommended for faster builds)
- vcpkg (for dependency management)

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

### Python Integration

```bash
cd examples
uv sync  # or: pip install -r requirements.txt

# Run examples
uv run email_verification.py
uv run postal_verification.py
```

---

## ✨ Features - All 8 Modules

### 📧 Email Validation

Multi-stage email verification with configurable validation modes:

- **Regex** - Fast RFC 5322-inspired syntax checking
- **DNS** - MX record verification with configurable timeouts
- **SMTP** - Full SMTP handshake validation with detailed transcripts

```sql
-- Quick validation
SELECT anofox_email_is_valid('john.doe@company.com', 'regex');

-- Detailed results with MX records and SMTP debug info
SELECT anofox_email_validate('support@example.org', 'smtp');
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
SELECT anofox_postal_parse_address('620 Bolger Place, The Burren, NSW 4726');
-- Returns: {house_number: '620', road: 'Bolger Place', city: 'The Burren', ...}

-- Generate normalized variants for fuzzy matching
SELECT anofox_postal_expand_address('123 Main St');
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
SELECT anofox_phonenumber_parse('+1 (415) 555-1234', 'US');
-- Returns: {valid: true, country_code: 1, national_number: '4155551234', ...}

-- Format in different styles
SELECT anofox_phonenumber_format('4155551234', 'US', 'INTERNATIONAL');
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
    anofox_money_format(
        anofox_money_add(
            anofox_money(100.00, 'EUR'),
            anofox_money(50.00, 'EUR')
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
- EU membership checks
- Country name and information lookup

```sql
-- Validate and extract VAT information
SELECT
    vat_id,
    anofox_vat_is_valid(vat_id) as is_valid,
    anofox_vat_country_name((anofox_vat_split(vat_id)).country) as country
FROM customers;
```

**Use Cases:**
- Customer compliance verification
- B2B transaction validation
- International customer categorization
- Regulatory data quality checks

[📖 See complete VAT module documentation](#vat-validation-functions)

---

### 🔍 Data Quality Metrics

Track essential data quality dimensions:

| Metric | Purpose | Example |
|--------|---------|---------|
| **Volume** | Row count thresholds | `anofox_metric_volume('orders', 1000, 1000000)` |
| **Null Rate** | Missing value detection | `anofox_metric_null_rate('users', 'email', 0.05)` |
| **Distinctness** | Cardinality validation | `anofox_metric_distinct_count('products', 'sku', 100, NULL)` |
| **Freshness** | Data recency checks | `anofox_metric_freshness('events', 'timestamp', INTERVAL '1 hour')` |
| **Schema** | Required column validation | `anofox_metric_schema('table', ['id', 'created_at'])` |

**Statistical Outlier Detection:**
- **Z-Score** - Parametric outlier detection (assumes normal distribution)
- **IQR** - Non-parametric outlier detection (robust to distribution)

```sql
-- Find statistical outliers using IQR method
SELECT * FROM anofox_metric_iqr('transactions', 'amount', 1.5);
```

[📖 See complete Metrics module documentation](#data-quality-metrics-functions)

---

### 🤖 Anomaly Detection

#### Isolation Forest

A unsupervised anomaly detection that scales to high dimensions:

```sql
-- Univariate: detect outliers in single column
SELECT * FROM anofox_metric_isolation_forest(
    'sales_data',
    'amount',
    100,        -- n_trees
    256,        -- sample_size
    0.1,        -- contamination (expected anomaly rate)
    'scores'    -- output mode: 'summary' or 'scores'
) WHERE is_anomaly = true;

-- Multivariate: detect anomalies across multiple features
SELECT * FROM anofox_metric_isolation_forest_multivariate(
    'customer_events',
    'purchase_amount, session_duration, page_views',
    100, 256, 0.1, 'scores'
) ORDER BY anomaly_score DESC LIMIT 10;
```

**Why Isolation Forest?**
- No distributional assumptions (works on any data shape)
- Excellent for high-dimensional data
- Detects both global and local anomalies
- Fast training and prediction (O(n log n))

#### DBSCAN Clustering

Density-based anomaly detection for finding outliers in spatial data:

```sql
-- Univariate: find noise points in single dimension
SELECT * FROM anofox_metric_dbscan(
    'transactions',
    'amount',
    10.0,       -- eps: neighborhood radius
    5,          -- min_pts: minimum points for dense region
    'clusters'  -- output mode: 'summary' or 'clusters'
) WHERE point_type = 'NOISE';
```

**Point Classifications:**
- **CORE** - Dense region centers (low anomaly score)
- **BORDER** - Cluster edges (moderate anomaly score)
- **NOISE** - Isolated outliers (high anomaly score: 1.0)

[📖 See complete Anomaly Detection module documentation](#anomaly-detection-functions)

---

### 🔄 Data Diffing

Compare tables and identify changes:

```sql
-- Hash-based diff (fast, summary statistics)
SELECT * FROM anofox_diff_hashdiff('source_tbl', 'target_tbl', ['id']);
-- Returns: {added: 150, removed: 25, changed: 300, unchanged: 10000}

-- Join-based diff (detailed, row-level changes)
SELECT * FROM anofox_diff_joindiff('source_tbl', 'target_tbl', ['user_id', 'date'])
WHERE diff_type IN ('added', 'changed')
LIMIT 100;
```

**Use Cases:**
- Migration validation (compare old vs new systems)
- Regression testing (ensure transformations didn't break data)
- Incremental change detection (CDC scenarios)
- Schema evolution tracking

**Features:**
- Single and compound primary keys
- Column-specific comparison
- Efficient SQL-based implementation
- Detailed change tracking

[📖 See complete Data Diffing module documentation](#data-diffing-functions)

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
    SUM(CASE WHEN anofox_email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as valid,
    SUM(CASE WHEN NOT anofox_email_is_valid(email, 'regex') THEN 1 ELSE 0 END) as invalid
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
        anofox_postal_parse_address(
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
    SELECT * FROM anofox_metric_isolation_forest_multivariate(
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
FROM anofox_diff_joindiff('prod.users', 'staging.users', ['user_id'])
GROUP BY diff_type
ORDER BY row_count DESC;

-- Identify specific changed records
SELECT
    user_id,
    source_email,
    target_email,
    source_created_at,
    target_created_at
FROM anofox_diff_joindiff('prod.users', 'staging.users', ['user_id'])
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
    anofox_vat_country_name((anofox_vat_split(customer_vat)).country) as country,
    anofox_money_format(amount, 'symbol') as formatted_amount,
    CASE
        WHEN anofox_money_is_negative(amount) THEN 'Refund'
        WHEN anofox_money_is_zero(amount) THEN 'Warning'
        ELSE 'Valid'
    END as transaction_status,
    anofox_vat_is_valid(customer_vat) as vat_valid
FROM transactions
WHERE anofox_money_in_range(amount, 0.01, 99999.99)
  AND anofox_email_is_valid(customer_email, 'dns');
```

---

## 📚 SQL Function Reference

### Email Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_email_is_valid` | `(email VARCHAR [, mode VARCHAR]) → BOOLEAN` | Quick validation (modes: `regex`, `dns`, `smtp`) |
| `anofox_email_validate` | `(email VARCHAR [, mode VARCHAR]) → STRUCT` | Detailed validation with stage, reason, MX hosts, SMTP transcript |
| `anofox_email_config` | `() → TABLE(key VARCHAR, value VARCHAR)` | Current configuration settings |

### Postal Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_postal_parse_address` | `(address VARCHAR) → STRUCT` | Parse address into components (house_number, road, city, state, postcode, country) |
| `anofox_postal_expand_address` | `(address VARCHAR) → LIST<VARCHAR>` | Generate normalized variants |
| `anofox_postal_status` | `() → TABLE` | Library initialization status |
| `anofox_postal_load_data` | `() → BOOLEAN` | Download and extract libpostal data (~500MB) |

### Phone Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_phonenumber_parse` | `(number VARCHAR, region VARCHAR) → STRUCT` | Parse and validate (returns validity, country_code, national_number, region, type) |
| `anofox_phonenumber_format` | `(number VARCHAR, region VARCHAR, format VARCHAR) → VARCHAR` | Format number (E164, INTERNATIONAL, NATIONAL, RFC3966) |
| `anofox_phonenumber_region` | `(number VARCHAR, region VARCHAR) → VARCHAR` | Extract ISO region code |
| `anofox_phonenumber_is_valid` | `(number VARCHAR, region VARCHAR) → BOOLEAN` | Full validation using length and prefix information |
| `anofox_phonenumber_is_possible` | `(number VARCHAR, region VARCHAR) → BOOLEAN` | Quick possibility check using length-only analysis |
| `anofox_phonenumber_is_valid_for_region` | `(number VARCHAR, region VARCHAR) → BOOLEAN` | Region-specific validation |
| `anofox_phonenumber_match` | `(number1 VARCHAR, number2 VARCHAR, region VARCHAR) → VARCHAR` | Fuzzy matching returns (EXACT_MATCH, NSN_MATCH, SHORT_NSN_MATCH, NO_MATCH) |
| `anofox_phonenumber_example` | `(region VARCHAR) → VARCHAR` | Generate example phone number for region |
| `anofox_phonenumber_status` | `() → TABLE` | Library status and default region |

### Money & Currency Functions

#### Basic Operations
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_money` | `(amount, currency_code)` | STRUCT | Create a money value from amount and currency code |
| `anofox_money_from_cents` | `(cents, currency_code)` | STRUCT | Create a money value from integer cents |
| `anofox_money_amount` | `(money)` | DOUBLE | Extract amount from money struct |
| `anofox_money_currency` | `(money)` | VARCHAR | Extract currency code from money struct |

#### Currency Information
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_is_valid_currency` | `(code)` | BOOLEAN | Check if currency code is valid |
| `anofox_currency_symbol` | `(code)` | VARCHAR | Get currency symbol (e.g., '$', '€') |
| `anofox_currency_name` | `(code)` | VARCHAR | Get currency name (e.g., 'United States Dollar') |

#### Formatting
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_money_format` | `(money, style)` | VARCHAR | Format money for display (3 styles: 'symbol', 'code', 'long') |

#### Validation & Properties
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_money_is_positive` | `(money)` | BOOLEAN | Check if amount > 0 |
| `anofox_money_is_negative` | `(money)` | BOOLEAN | Check if amount < 0 |
| `anofox_money_is_zero` | `(money)` | BOOLEAN | Check if amount == 0 |
| `anofox_money_abs` | `(money)` | STRUCT | Get absolute value (sign removed) |

#### Arithmetic Operations
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_money_add` | `(money1, money2)` | STRUCT | Add two money values (same currency required) |
| `anofox_money_subtract` | `(money1, money2)` | STRUCT | Subtract money2 from money1 (same currency) |
| `anofox_money_multiply` | `(money, factor)` | STRUCT | Multiply money by a scalar factor |

#### Quality & Data Validation
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_money_in_range` | `(money, min, max)` | BOOLEAN | Check if amount is within range |
| `anofox_money_same_currency` | `(money1, money2)` | BOOLEAN | Check if two money values have same currency |

### VAT Validation Functions

#### Basic Operations
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_vat` | `(vat_string)` | STRUCT | Parse VAT string into country and digits |
| `anofox_is_valid_vat_country` | `(code)` | BOOLEAN | Check if country code is valid VAT country |
| `anofox_vat_normalize` | `(vat_string)` | VARCHAR | Normalize VAT string (uppercase, remove punctuation) |

#### Syntax Validation
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_vat_is_valid_syntax` | `(vat_string)` | BOOLEAN | Validate VAT syntax against country pattern |
| `anofox_vat_split` | `(vat_string)` | STRUCT | Parse VAT into country and normalized digits |
| `anofox_vat_exists` | `(vat_string)` | BOOLEAN | Check if VAT has valid country prefix |

#### EU Utilities
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_vat_is_eu_member` | `(country_code)` | BOOLEAN | Check if country is EU member |
| `anofox_vat_country_name` | `(country_code)` | VARCHAR | Get full country name |
| `anofox_vat_format` | `(vat_string, style)` | VARCHAR | Format VAT for display |

#### Combined Validation
| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `anofox_vat_is_valid` | `(vat_string)` | BOOLEAN | Full validation (syntax + country check) |

### Data Quality Metrics Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_metric_volume` | `(table VARCHAR [, min_rows BIGINT, max_rows BIGINT]) → TABLE` | Validate row count |
| `anofox_metric_null_rate` | `(table VARCHAR, column VARCHAR [, max_null_rate DOUBLE]) → TABLE` | Check null percentage |
| `anofox_metric_distinct_count` | `(table VARCHAR, column VARCHAR [, min BIGINT, max BIGINT]) → TABLE` | Validate cardinality |
| `anofox_metric_schema` | `(table VARCHAR, required_cols LIST<VARCHAR>) → TABLE` | Check required columns exist |
| `anofox_metric_freshness` | `(table VARCHAR, ts_col VARCHAR, max_age INTERVAL [, ref_time TIMESTAMP]) → TABLE` | Validate data recency |
| `anofox_metric_zscore` | `(table VARCHAR, column VARCHAR [, threshold DOUBLE]) → TABLE` | Detect outliers via z-score (default: 3.0) |
| `anofox_metric_iqr` | `(table VARCHAR, column VARCHAR [, multiplier DOUBLE]) → TABLE` | Detect outliers via IQR (default: 1.5) |

### Anomaly Detection Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_metric_isolation_forest` | `(table VARCHAR, column VARCHAR [, n_trees BIGINT, sample_size BIGINT, contamination DOUBLE, output_mode VARCHAR]) → TABLE` | Univariate Isolation Forest |
| `anofox_metric_isolation_forest_multivariate` | `(table VARCHAR, columns VARCHAR [, n_trees BIGINT, sample_size BIGINT, contamination DOUBLE, output_mode VARCHAR]) → TABLE` | Multivariate Isolation Forest |
| `anofox_metric_dbscan` | `(table VARCHAR, column VARCHAR [, eps DOUBLE, min_pts BIGINT, output_mode VARCHAR]) → TABLE` | Univariate DBSCAN clustering |
| `anofox_metric_dbscan_multivariate` | `(table VARCHAR, columns VARCHAR [, eps DOUBLE, min_pts BIGINT, output_mode VARCHAR]) → TABLE` | Multivariate DBSCAN clustering |

**Parameters:**
- **n_trees** (1-500, default 100): Number of isolation trees
- **sample_size** (1-10000, default 256): Subsample size per tree
- **contamination** (0.0-0.5, default 0.1): Expected anomaly fraction
- **eps** (default 0.5): DBSCAN neighborhood radius
- **min_pts** (default 5): DBSCAN minimum points for dense region
- **output_mode**: `summary` (aggregate stats) or `scores`/`clusters` (per-row results)

### Data Diffing Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `anofox_diff_hashdiff` | `(source VARCHAR, target VARCHAR, pk_cols LIST<VARCHAR> [, compare_cols LIST<VARCHAR>]) → TABLE` | Fast hash-based summary diff |
| `anofox_diff_joindiff` | `(source VARCHAR, target VARCHAR, pk_cols LIST<VARCHAR> [, compare_cols LIST<VARCHAR>]) → TABLE` | Detailed row-level diff with source/target data |

---

## ⚙️ Configuration

Set options via SQL or DuckDB's configuration file:

### Email Settings

```sql
SET anofox_email_default_validation = 'regex';  -- Default: regex
SET anofox_email_regex_pattern = '<your-pattern>';  -- RFC 5322 inspired
SET anofox_email_dns_timeout_ms = 1000;  -- DNS timeout per try (1-5000ms)
SET anofox_email_dns_tries = 1;  -- DNS retry count
SET anofox_email_smtp_port = 25;  -- SMTP port
SET anofox_email_smtp_connect_timeout_ms = 5000;  -- TCP connect timeout
SET anofox_email_smtp_read_timeout_ms = 5000;  -- Read/write timeout
SET anofox_email_smtp_helo_domain = 'duckdb.local';  -- HELO/EHLO domain
SET anofox_email_smtp_mail_from = 'validator@duckdb.local';  -- MAIL FROM address
```

### Postal Settings

```sql
SET anofox_postal_data_path = '.duckdb/extensions/libpostal';  -- Data directory

-- Download libpostal data on first use
SELECT anofox_postal_load_data();
```

### Phone Settings

```sql
SET anofox_phonenumber_default_region = 'US';  -- Default region code
```

### Tracing

```sql
SET anofox_trace_enabled = true;  -- Enable/disable logging
SET anofox_trace_level = 'info';  -- trace|debug|info|warn|error|critical|off
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
│   libpostal · libphonenumber · c-ares · spdlog          │
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
SELECT anofox_postal_load_data();
```

Or manually set the path:
```sql
SET anofox_postal_data_path = '/path/to/libpostal/data';
```

### SMTP timeout errors

**Solution**: Increase timeout settings:
```sql
SET anofox_email_smtp_connect_timeout_ms = 10000;
SET anofox_email_smtp_read_timeout_ms = 10000;
```

### Memory issues with large datasets

**Solution**: Use streaming output modes and process in batches:
```sql
-- Use 'summary' mode for large tables
SELECT * FROM anofox_metric_isolation_forest(
    'large_table', 'column', 100, 256, 0.1, 'summary'
);

-- Process in batches
CREATE TABLE anomalies AS
SELECT * FROM anofox_metric_isolation_forest(
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
  AND anofox_email_is_valid(email, 'regex');

-- ✗ Slow: Validate all rows first
SELECT *
FROM (
    SELECT *, anofox_email_is_valid(email, 'regex') as valid
    FROM large_table
)
WHERE valid AND created_at > NOW() - INTERVAL '1 day';
```

---

## 📜 License

This project is licensed under the Business Source License (BSL) 1.1 - see the [LICENSE](LICENSE) file for details.

**Key Points:**
- ✅ Free for production use
- ❌ Cannot be offered to third parties on a hosted or embedded basis
- 🔄 Converts to MPL 2.0 after 5 years from first publication

---

## 🙏 Acknowledgments

- **[DuckDB Team](https://duckdb.org/)** - For the amazing embedded analytics database
- **[libpostal](https://github.com/openvenues/libpostal)** - Statistical NLP library for parsing world addresses
- **[libphonenumber](https://github.com/google/libphonenumber)** - Google's comprehensive phone number handling library
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

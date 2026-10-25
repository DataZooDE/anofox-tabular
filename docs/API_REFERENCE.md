# Anofox Tabular Extension - API Reference

**Applies to:** anofox-tabular releases from v2026.10 on  
**DuckDB Version:** v1.5.6 or 2.0

---

## Overview

The Anofox Tabular extension provides data quality checks, validation, anomaly detection, PII detection, data diffing and profiling directly within DuckDB. All computations run in native C++ inside DuckDB.

### Key Features

- **10 modules**: email, postal, phone, money, VAT, PII, data quality, anomaly detection, diffing and profiling
- **99 SQL functions** (canonical names; see the [Function Coverage Matrix](#function-coverage-matrix))
- **Runs locally**: the core functions need no external service. Network is used only by the NER
  model download (PII), the libpostal data download (address parsing), the email `dns`/`smtp`
  modes and the anonymous telemetry. See [Network use](#network-use).

### Function Naming Conventions

Functions follow consistent naming patterns:
- `anofox_tab_*` prefix for all extension functions (with aliases without prefix)
- `anofox_tab_*_is_valid` suffix for validation functions
- `anofox_tab_*_format` suffix for formatting functions
- `anofox_tab_diff_*` prefix for data diffing operations

All `anofox_tab_*` functions have aliases without the `anofox_tab_` prefix. The two helpers
`anofox_ner_status` and `anofox_contract_version` have no alias. For example:
- `anofox_tab_email_is_valid` can also be called as `email_is_valid`
- `anofox_tab_vat` can also be called as `vat`

### Parameter Conventions

Arguments are positional. In addition, these parameters also accept the named form (`name := value`):

- every parameter of `diff_joindiff` and `diff_hashdiff` (`source_table`, `target_table`,
  `primary_key` / `primary_keys`, `compare_columns`, `include_all`; a named value overrides the
  positional one);
- `start_date` and `end_date` on `rel_count_change`, `metric_anomaly_iqr`,
  `rolling_values_in_set` and their `_events` companions (named only);
- `id_column` on `isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`,
  `outlier_tree` and `pii_audit_table` (named only).

All other functions reject named arguments with a binder error.

**Common Parameter Types**:
- `email`: Email address - `VARCHAR`
- `address`: Street address - `VARCHAR`
- `number`: Phone number - `VARCHAR`
- `region`: ISO region code - `VARCHAR`
- `money`: Money struct - `STRUCT(amount DECIMAL(18,3), currency VARCHAR)` (exact decimal amounts; NaN/Inf unrepresentable, overflow raises)
- `vat_string`: VAT number - `VARCHAR`
- `table_name`: Source table - `VARCHAR`
- `column_name`: Column name - `VARCHAR`

---

## Table of Contents

1. [Email Validation](#email-validation)
2. [Address Parsing & Normalization](#address-parsing--normalization)
3. [Phone Number Validation](#phone-number-validation)
4. [Money & Currency Operations](#money--currency-operations)
5. [VAT Validation](#vat-validation)
6. [PII Detection](#pii-detection)
7. [Data Quality Metrics](#data-quality-metrics)
8. [Check Suites](#check-suites)
9. [anofox Integration Contract](#anofox-integration-contract)
10. [Anomaly Detection](#anomaly-detection)
11. [Data Diffing](#data-diffing)
12. [Data Profiling](#data-profiling)
13. [Configuration](#configuration)
14. [Function Coverage Matrix](#function-coverage-matrix)
15. [Notes](#notes)

---

## Email Validation

Multi-stage email verification with configurable validation modes.

### Functions

#### `anofox_tab_email_is_valid` (alias: `email_is_valid`)

Quick boolean validation of email addresses.

**Signature:**
```sql
anofox_tab_email_is_valid(email VARCHAR [, mode VARCHAR]) → BOOLEAN
```

**Parameters:**
- `email`: Email address to validate
- `mode`: Validation mode - `'regex'` (default), `'dns'`, or `'smtp'`

**Returns:**
- `BOOLEAN`: `true` if email is valid, `false` otherwise

**Example:**
```sql
SELECT anofox_tab_email_is_valid('user@example.com', 'regex');
-- or using alias:
SELECT email_is_valid('user@example.com', 'regex');
-- Returns: true
```

---

#### `anofox_tab_email_validate` (alias: `email_validate`)

Detailed email validation with stage information, failure reasons, MX hosts, and SMTP transcripts.

**Signature:**
```sql
anofox_tab_email_validate(email VARCHAR [, mode VARCHAR]) → STRUCT
```

**Returns:**
```sql
STRUCT(
    valid BOOLEAN,
    stage VARCHAR,           -- 'regex', 'dns', or 'smtp'
    reason VARCHAR,          -- Failure reason if invalid
    mx_hosts VARCHAR[],      -- MX record hosts (DNS/SMTP modes)
    smtp_transcript VARCHAR[] -- SMTP conversation log (SMTP mode)
)
```

**Example:**
```sql
SELECT anofox_tab_email_validate('support@example.org', 'smtp');
-- or using alias:
SELECT email_validate('support@example.org', 'smtp');
-- Returns: {valid: true, stage: 'smtp', reason: NULL, mx_hosts: [...], smtp_transcript: [...]}
```

---

#### `anofox_tab_email_config` (alias: `email_config`)

Returns current email validation configuration settings.

**Signature:**
```sql
anofox_tab_email_config() → TABLE(key VARCHAR, value VARCHAR)
```

**Returns:**
- Configuration keys: `dns_timeout_ms`, `dns_tries`, `smtp_port`, `smtp_connect_timeout_ms`, `smtp_read_timeout_ms`, `smtp_helo_domain`, `smtp_mail_from`, `trace_enabled`, `trace_level`

**Example:**
```sql
SELECT * FROM anofox_tab_email_config();
-- or using alias:
SELECT * FROM email_config();
```

---

## Address Parsing & Normalization

Powered by **[libpostal](https://github.com/openvenues/libpostal)**, a statistical NLP library for parsing addresses.

### Functions

#### `anofox_tab_postal_parse_address` (alias: `postal_parse_address`)

Parse unstructured addresses into structured components.

**Signature:**
```sql
anofox_tab_postal_parse_address(address VARCHAR) → STRUCT
```

**Returns:**
```sql
STRUCT(
    house_number VARCHAR,
    road VARCHAR,
    city VARCHAR,
    state VARCHAR,
    postcode VARCHAR,
    country VARCHAR
)
```

**Example:**
```sql
SELECT anofox_tab_postal_parse_address('620 Bolger Place, The Burren, NSW 4726');
-- Returns: {house_number: '620', road: 'Bolger Place', city: 'The Burren', state: 'NSW', postcode: '4726', country: NULL}
```

---

#### `anofox_tab_postal_expand_address` (alias: `postal_expand_address`)

Generate normalized address variants for fuzzy matching.

**Signature:**
```sql
anofox_tab_postal_expand_address(address VARCHAR) → LIST<VARCHAR>
```

**Returns:**
- `LIST<VARCHAR>`: Array of normalized address variants

**Example:**
```sql
SELECT anofox_tab_postal_expand_address('123 Main St');
-- Returns: ['123 Main Street', '123 Main St', '123 main street', ...]
```

---

#### `anofox_tab_postal_status` (alias: `postal_status`)

Returns library initialization status and data availability.

**Signature:**
```sql
anofox_tab_postal_status() → TABLE
```

**Returns:**
- Status information including `initialized`, `data_present`, `data_directory`

**Example:**
```sql
SELECT * FROM anofox_tab_postal_status();
```

---

#### `anofox_tab_postal_load_data` (alias: `postal_load_data`)

Download and extract libpostal data (~500MB) to the configured data directory.

**Signature:**
```sql
anofox_tab_postal_load_data() → BOOLEAN
```

**Returns:**
- `BOOLEAN`: `true` if data was successfully loaded, `false` otherwise

**Example:**
```sql
SELECT anofox_tab_postal_load_data();
```

---

## Phone Number Validation

International phone parsing via **[libphonenumber](https://github.com/google/libphonenumber)**, Google's library for parsing and formatting phone numbers.

### Functions

#### `anofox_tab_phonenumber_parse` (alias: `phonenumber_parse`)

Parse and validate phone numbers with detailed information.

**Signature:**
```sql
anofox_tab_phonenumber_parse(number VARCHAR, region VARCHAR) → STRUCT
```

**Returns:**
```sql
STRUCT(
    valid BOOLEAN,
    country_code INTEGER,
    national_number VARCHAR,
    region_code VARCHAR,
    type VARCHAR
)
```

**Example:**
```sql
SELECT anofox_tab_phonenumber_parse('+1 (415) 555-1234', 'US');
-- Returns: {valid: true, country_code: 1, national_number: '4155551234', region_code: 'US', type: 'FIXED_LINE_OR_MOBILE'}
```

---

#### `anofox_tab_phonenumber_format` (alias: `phonenumber_format`)

Format phone numbers in different styles.

**Signature:**
```sql
anofox_tab_phonenumber_format(number VARCHAR, region VARCHAR, format VARCHAR) → VARCHAR
```

**Parameters:**
- `format`: Format style - `'E164'`, `'INTERNATIONAL'`, `'NATIONAL'`, or `'RFC3966'` (case-insensitive). Any other value raises an `Invalid Input` error.

**Returns:**
- `VARCHAR`: Formatted phone number, or `NULL` if the number cannot be parsed as a valid phone number

**Example:**
```sql
SELECT anofox_tab_phonenumber_format('4155551234', 'US', 'INTERNATIONAL');
-- Returns: '+1 415-555-1234'
```

---

#### `anofox_tab_phonenumber_region` (alias: `phonenumber_region`)

Extract ISO region code from phone number.

**Signature:**
```sql
anofox_tab_phonenumber_region(number VARCHAR, region VARCHAR) → VARCHAR
```

**Returns:**
- `VARCHAR`: ISO region code (e.g., 'US', 'GB', 'DE')

**Example:**
```sql
SELECT anofox_tab_phonenumber_region('+1 415-555-1234', 'US');
-- Returns: 'US'
```

---

#### `anofox_tab_phonenumber_is_valid` (alias: `phonenumber_is_valid`)

Full validation using length and prefix information.

**Signature:**
```sql
anofox_tab_phonenumber_is_valid(number VARCHAR, region VARCHAR) → BOOLEAN
```

**Returns:**
- `BOOLEAN`: `true` if phone number is valid

**Example:**
```sql
SELECT anofox_tab_phonenumber_is_valid('+1 415-555-1234', 'US');
-- Returns: true
```

---

#### `anofox_tab_phonenumber_is_possible` (alias: `phonenumber_is_possible`)

Quick possibility check using length-only analysis.

**Signature:**
```sql
anofox_tab_phonenumber_is_possible(number VARCHAR, region VARCHAR) → BOOLEAN
```

**Returns:**
- `BOOLEAN`: `true` if phone number is possible (may not be valid)

**Example:**
```sql
SELECT anofox_tab_phonenumber_is_possible('4155551234', 'US');
-- Returns: true
```

---

#### `anofox_tab_phonenumber_is_valid_for_region` (alias: `phonenumber_is_valid_for_region`)

Region-specific validation.

**Signature:**
```sql
anofox_tab_phonenumber_is_valid_for_region(number VARCHAR, region VARCHAR) → BOOLEAN
```

**Returns:**
- `BOOLEAN`: `true` if phone number is valid for the specified region

---

#### `anofox_tab_phonenumber_match` (alias: `phonenumber_match`)

Fuzzy matching between two phone numbers.

**Signature:**
```sql
anofox_tab_phonenumber_match(number1 VARCHAR, number2 VARCHAR, region VARCHAR) → VARCHAR
```

**Returns:**
- `VARCHAR`: Match type - `'EXACT_MATCH'`, `'NSN_MATCH'`, `'SHORT_NSN_MATCH'`, or `'NO_MATCH'`

**Example:**
```sql
SELECT anofox_tab_phonenumber_match('+1 415-555-1234', '4155551234', 'US');
-- Returns: 'EXACT_MATCH'
```

---

#### `anofox_tab_phonenumber_example` (alias: `phonenumber_example`)

Generate example phone number for a region.

**Signature:**
```sql
anofox_tab_phonenumber_example(region VARCHAR) → VARCHAR
```

**Returns:**
- `VARCHAR`: Example phone number for the region

**Example:**
```sql
SELECT anofox_tab_phonenumber_example('US');
-- Returns: '+1 650-253-0000'
```

---

#### `anofox_tab_phonenumber_status` (alias: `phonenumber_status`)

Returns library status and default region.

**Signature:**
```sql
anofox_tab_phonenumber_status() → TABLE
```

**Returns:**
- Status information including `initialized`, `default_region`

---

## Money & Currency Operations

International monetary value handling with currency-aware arithmetic and formatting.

Money amounts are exact `DECIMAL(18,3)` values: arithmetic never drifts, NaN/Inf are unrepresentable, and results outside the DECIMAL(18,3) range raise an out-of-range error instead of overflowing to Inf. `money()` takes a DOUBLE amount (must be finite, rounded half away from zero to 3 decimals); `money_from_cents()` constructs exactly from integer subunits.

### Supported Currencies

10 major currencies: USD, EUR, GBP, JPY, CAD, AUD, CHF, CNY, INR, BRL

### Basic Operations

#### `anofox_tab_money` (alias: `money`)

Create a money value from amount and currency code.

**Signature:**
```sql
anofox_tab_money(amount DOUBLE, currency_code VARCHAR) → STRUCT(amount DECIMAL(18,3), currency VARCHAR)
```

**Example:**
```sql
SELECT money(100.50, 'USD');
-- Returns: {'amount': 100.500, 'currency': USD}
```

---

#### `anofox_tab_money_from_cents` (alias: `money_from_cents`)

Create a money value from an integer amount in the currency's smallest unit (e.g. cents). The amount is divided by the currency's `subunit_to_unit` (100 for USD/EUR, 1 for JPY).

**Signature:**
```sql
anofox_tab_money_from_cents(cents BIGINT, currency_code VARCHAR) → STRUCT(amount DECIMAL(18,3), currency VARCHAR)
```

**Example:**
```sql
SELECT money_from_cents(10050, 'USD');
-- Returns: {'amount': 100.500, 'currency': USD}
```

---

#### `anofox_tab_money_amount` (alias: `money_amount`)

Extract amount from money struct.

**Signature:**
```sql
anofox_tab_money_amount(money STRUCT) → DECIMAL(18,3)
```

**Example:**
```sql
SELECT money_amount(money(100.50, 'USD'));
-- Returns: 100.500
```

---

#### `anofox_tab_money_currency` (alias: `money_currency`)

Extract currency code from money struct.

**Signature:**
```sql
anofox_tab_money_currency(money STRUCT) → VARCHAR
```

**Example:**
```sql
SELECT money_currency(money(100.50, 'USD'));
-- Returns: 'USD'
```

---

### Currency Information

#### `anofox_tab_is_valid_currency` (alias: `is_valid_currency`)

Check if currency code is valid.

**Signature:**
```sql
anofox_tab_is_valid_currency(code VARCHAR) → BOOLEAN
```

**Example:**
```sql
SELECT is_valid_currency('USD');
-- Returns: true
```

---

#### `anofox_tab_currency_symbol` (alias: `currency_symbol`)

Get currency symbol (e.g., '$', '€').

**Signature:**
```sql
anofox_tab_currency_symbol(code VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT currency_symbol('USD');
-- Returns: '$'
```

---

#### `anofox_tab_currency_name` (alias: `currency_name`)

Get currency name (e.g., 'United States Dollar').

**Signature:**
```sql
anofox_tab_currency_name(code VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT currency_name('USD');
-- Returns: 'United States Dollar'
```

---

### Formatting

#### `anofox_tab_money_format` (alias: `money_format`)

Format money for display. The number of decimals is derived from the currency's `subunit_to_unit` (JPY has none, USD/EUR have two). The `'symbol'` and `'long'` styles localize the amount with the currency's thousands separator and decimal mark; the `'code'` style stays canonical (dot decimal mark, no grouping).

**Signature:**
```sql
anofox_tab_money_format(money STRUCT, style VARCHAR) → VARCHAR
```

**Parameters:**
- `style`: Format style - `'symbol'` (e.g. `$1,234,567.89`, `1.234.567,89 €`, `¥1,235`), `'code'` (e.g. `1234567.89 USD`), or `'long'` (e.g. `1.234,50 Euro`)

**Example:**
```sql
SELECT money_format(money(150.00, 'EUR'), 'symbol');
-- Returns: '150,00 €'
```

---

### Validation & Properties

#### `anofox_tab_money_is_positive` (alias: `money_is_positive`)

Check if amount > 0.

**Signature:**
```sql
anofox_tab_money_is_positive(money STRUCT) → BOOLEAN
```

---

#### `anofox_tab_money_is_negative` (alias: `money_is_negative`)

Check if amount < 0.

**Signature:**
```sql
anofox_tab_money_is_negative(money STRUCT) → BOOLEAN
```

---

#### `anofox_tab_money_is_zero` (alias: `money_is_zero`)

Check if amount == 0.

**Signature:**
```sql
anofox_tab_money_is_zero(money STRUCT) → BOOLEAN
```

---

#### `anofox_tab_money_abs` (alias: `money_abs`)

Get absolute value (sign removed).

**Signature:**
```sql
anofox_tab_money_abs(money STRUCT) → STRUCT(amount DECIMAL(18,3), currency VARCHAR)
```

---

### Arithmetic Operations

#### `anofox_tab_money_add` (alias: `money_add`)

Add two money values (same currency required).

**Signature:**
```sql
anofox_tab_money_add(money1 STRUCT, money2 STRUCT) → STRUCT(amount DECIMAL(18,3), currency VARCHAR)
```

**Example:**
```sql
SELECT money_add(
    money(100.00, 'EUR'),
    money(50.00, 'EUR')
);
-- Returns: {'amount': 150.000, 'currency': EUR}
```

---

#### `anofox_tab_money_subtract` (alias: `money_subtract`)

Subtract money2 from money1 (same currency required).

**Signature:**
```sql
anofox_tab_money_subtract(money1 STRUCT, money2 STRUCT) → STRUCT(amount DECIMAL(18,3), currency VARCHAR)
```

---

#### `anofox_tab_money_multiply` (alias: `money_multiply`)

Multiply money by a scalar factor.

**Signature:**
```sql
anofox_tab_money_multiply(money STRUCT, factor DOUBLE) → STRUCT(amount DECIMAL(18,3), currency VARCHAR)
```

---

### Quality & Data Validation

#### `anofox_tab_money_in_range` (alias: `money_in_range`)

Check if amount is within range.

**Signature:**
```sql
anofox_tab_money_in_range(money STRUCT, min DOUBLE, max DOUBLE) → BOOLEAN
```

**Example:**
```sql
SELECT money_in_range(money(100.00, 'USD'), 0.01, 99999.99);
-- Returns: true
```

---

#### `anofox_tab_money_same_currency` (alias: `money_same_currency`)

Check if two money values have same currency.

**Signature:**
```sql
anofox_tab_money_same_currency(money1 STRUCT, money2 STRUCT) → BOOLEAN
```

---

## VAT Validation

European VAT number validation for regulatory compliance and data quality.

### Supported Countries

28 countries: the 27 EU member states plus the United Kingdom.

AT, BE, BG, CY, CZ, DE, DK, EE, EL (Greece), ES, FI, FR, HR, HU, IE, IT, LT, LU, LV, MT, NL, PL, PT, RO, SE, SI, SK, GB

Greece uses the VAT prefix `EL`; `GR` is accepted as well. Northern Ireland uses `XI`. `UK` is
not a valid code: `is_valid_vat_country('UK')` is false, `'GB'` and `'XI'` are true.

### Basic Operations

#### `anofox_tab_vat` (alias: `vat`)

Parse VAT string into country and digits.

**Signature:**
```sql
anofox_tab_vat(vat_string VARCHAR) → STRUCT(country VARCHAR, digits VARCHAR)
```

**Example:**
```sql
SELECT vat('DE123456789');
-- Returns: {'country': DE, 'digits': 123456789}
```

---

#### `anofox_tab_is_valid_vat_country` (alias: `is_valid_vat_country`)

Check if country code is valid VAT country. Codes are case-insensitive and the
VAT aliases `EL` (Greece) and `XI` (Northern Ireland / GB) are accepted.

**Signature:**
```sql
anofox_tab_is_valid_vat_country(code VARCHAR) → BOOLEAN
```

**Example:**
```sql
SELECT is_valid_vat_country('DE');
-- Returns: true
```

---

#### `anofox_tab_vat_normalize` (alias: `vat_normalize`)

Normalize VAT string (uppercase, remove punctuation).

**Signature:**
```sql
anofox_tab_vat_normalize(vat_string VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT vat_normalize('de 123-456-789');
-- Returns: 'DE123456789'
```

---

### Syntax Validation

#### `anofox_tab_vat_is_valid_syntax` (alias: `vat_is_valid_syntax`)

Validate VAT syntax against country pattern.

**Signature:**
```sql
anofox_tab_vat_is_valid_syntax(vat_string VARCHAR) → BOOLEAN
```

---

#### `anofox_tab_vat_split` (alias: `vat_split`)

Parse VAT into country and normalized digits.

**Signature:**
```sql
anofox_tab_vat_split(vat_string VARCHAR) → STRUCT(country VARCHAR, digits VARCHAR)
```

---

#### `anofox_tab_vat_exists` (alias: `vat_exists`)

Check if VAT has valid country prefix.

**Signature:**
```sql
anofox_tab_vat_exists(vat_string VARCHAR) → BOOLEAN
```

---

### EU Utilities

#### `anofox_tab_vat_is_eu_member` (alias: `vat_is_eu_member`)

Check if country is EU member. Codes are case-insensitive and the VAT aliases
`EL`/`XI` are normalized (e.g. `vat_is_eu_member('EL')` is true, `'XI'` is false).

**Signature:**
```sql
anofox_tab_vat_is_eu_member(country_code VARCHAR) → BOOLEAN
```

---

#### `anofox_tab_vat_country_name` (alias: `vat_country_name`)

Get full country name.

**Signature:**
```sql
anofox_tab_vat_country_name(country_code VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT vat_country_name('DE');
-- Returns: 'Germany'
```

---

#### `anofox_tab_vat_format` (alias: `vat_format`)

Format VAT for display. Supported styles (case-insensitive):

- `'plain'` — digits only, without country prefix
- `'iso'` — VAT country prefix + digits (Greece uses `EL`, Northern Ireland `XI`)

Unparseable VAT numbers return `NULL`; unknown styles raise an error.

**Signature:**
```sql
anofox_tab_vat_format(vat_string VARCHAR, style VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT anofox_tab_vat_format('de 123-456-789', 'iso');
-- Returns: 'DE123456789'

SELECT anofox_tab_vat_format('GR123456783', 'plain');
-- Returns: '123456783'
```

---

### Combined Validation

#### `anofox_tab_vat_is_valid` (alias: `vat_is_valid`)

Full validation: syntax check plus country check-digit (checksum) validation where implemented.

Check digits are verified for: AT, BE, DE, DK, ES, FI, FR, GR/EL, IE, IT, LU, NL, PL, PT, SE, SI.
Other countries are validated by syntax only. French VAT keys containing letters are accepted
without check-digit verification.

**Signature:**
```sql
anofox_tab_vat_is_valid(vat_string VARCHAR) → BOOLEAN
```

**Example:**
```sql
SELECT anofox_tab_vat_is_valid('DE111111125');
-- Returns: true

SELECT anofox_tab_vat_is_valid('DE123456789');
-- Returns: false (valid syntax, invalid check digit)
```

---

## PII Detection

Detect and mask Personally Identifiable Information (PII) in text data. Supports multiple PII types with configurable masking strategies.

### Supported PII Types

| Type | Description | Example |
|------|-------------|---------|
| EMAIL | Email addresses | `user@example.com` |
| CREDIT_CARD | Credit card numbers (Visa, MC, Amex, Discover) | `4111-1111-1111-1111` |
| US_SSN | US Social Security Numbers | `123-45-6789` |
| IBAN | International Bank Account Numbers | `DE89370400440532013000` |
| IP_ADDRESS | IPv4 addresses | `192.168.1.100` |
| URL | HTTP/HTTPS URLs | `https://example.com` |
| DE_TAX_ID | German Tax ID (Steueridentifikationsnummer) | `12345678901` |
| MAC_ADDRESS | Network hardware addresses (MAC) | `00:1A:2B:3C:4D:5E` |
| UK_NINO | UK National Insurance Number | `AB123456C` |
| US_PASSPORT | US Passport numbers | `A12345678` |
| PHONE | International phone numbers | `+1-555-123-4567` |
| API_KEY | API keys (AWS, GitHub, generic) | `AKIAIOSFODNN7EXAMPLE` |
| CRYPTO_ADDRESS | Cryptocurrency addresses (Bitcoin, Ethereum) | `1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa` |
| NAME | Person names (NER-based, PER entities) | `John Smith` |
| ORGANIZATION | Company/organization names (NER-based, ORG entities) | `Microsoft`, `Google Inc` |
| LOCATION | Geographic locations (NER-based, LOC entities) | `Paris`, `New York` |
| MISC | Miscellaneous entities (NER-based, MISC entities) | `French`, `Nobel Prize` |

### Known Limitations

**US_PASSPORT:**
- May match 9-digit sequences (invoice numbers, IDs)
- No checksum validation available (passport numbers have none)
- Use context to interpret overlapping matches with US_SSN

**PHONE:**
- Requires distinctive features (+ prefix or parentheses format)
- Pattern-based detection only, not full libphonenumber validation
- For comprehensive phone parsing, use `phonenumber_parse()`

**API_KEY:**
- Generic pattern uses entropy threshold (≥3.5 bits/char for 32+ chars)
- AWS (`AKIA...`) and GitHub (`ghp_`, `gho_`, etc.) patterns are highly specific
- Possible false positives with random high-entropy strings

**CRYPTO_ADDRESS:**
- Bitcoin legacy (P2PKH) and P2SH addresses validated with Base58 checksum (double SHA-256)
- Bitcoin SegWit (bc1) addresses use format validation only (Bech32 checksum deferred)
- Ethereum addresses validated by hex format (no EIP-55 checksum for all-lowercase)
- Partial masking shows first 4 + last 4 characters

**NAME, ORGANIZATION, LOCATION, MISC (NER-based entities):**
- All four entity types use ML-based NER (Named Entity Recognition) with OpenVINO + DistilBERT
- Model trained on CoNLL-2003 dataset with 92% F1 score
- OpenVINO is required and installed via vcpkg with AUTO device selection (CPU/GPU/NPU)
- Model downloads automatically from Hugging Face on first use (~66 MB quantized ONNX) into `~/.duckdb/extensions/anofox/ner/<model>/`; `SET anofox_ner_auto_download = false` turns the download off (NER detection is then skipped, NAME falls back to the dictionary)
- Use `SELECT * FROM anofox_ner_status()` to check NER availability
- Confidence threshold: 0.7 (70%) for entity acceptance
- NAME has dictionary fallback if NER unavailable; ORGANIZATION, LOCATION, MISC require NER
- **Platform Availability:**
  - ✅ Linux x64 (glibc), macOS x64, macOS ARM64 - Full NER detection available
  - ❌ Windows x64 - OpenVINO not installed, NAME uses dictionary fallback, ORG/LOC/MISC unavailable
  - ❌ Linux ARM64 - vcpkg OpenVINO build timeout issues, excluded from CI
  - ❌ Linux musl (Alpine) - OpenVINO requires glibc, excluded from CI
- Entity types:
  - NAME: Person names (PER entities) - e.g., "John Smith"
  - ORGANIZATION: Company/org names (ORG entities) - e.g., "Microsoft"
  - LOCATION: Geographic locations (LOC entities) - e.g., "Paris"
  - MISC: Miscellaneous entities (events, languages) - e.g., "French", "Nobel Prize"
- Case sensitive: all-caps text may produce fragmented or no results

### Masking Strategies

Strategy names are case-insensitive (`'partial'` and `'PARTIAL'` are the same).

| Strategy | Description | Example Output |
|----------|-------------|----------------|
| `redact` | Replace with type label | `[EMAIL]`, `[US_SSN]` |
| `partial` | Show partial value | `te**@example.com`, `***-**-6789` |
| `asterisk` | Replace with asterisks (same length) | `****************` |
| `hash` | Replace with SHA-256 hash (truncated) | `a1b2c3d4e5f6...` |
| `none` | Leave the text unchanged | `123-45-6789` |

### Functions

#### `anofox_tab_pii_detect` (alias: `pii_detect`)

Detect all PII in input text and return the matches as a list of structs.

**Signature:**
```sql
anofox_tab_pii_detect(text VARCHAR)
  → STRUCT("type" VARCHAR, "text" VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
```

**Returns:**
- One struct per match with its type, matched text, start/end position and confidence

**Example:**
```sql
SELECT pii_detect('Contact: john.doe@example.com, SSN: 123-45-6789');
-- Returns: [{'type': EMAIL, 'text': john.doe@example.com, 'start_pos': 9, 'end_pos': 29, 'confidence': 1.0},
--           {'type': US_SSN, 'text': 123-45-6789, 'start_pos': 36, 'end_pos': 47, 'confidence': 1.0}]

-- NER-based entity detection (downloads the NER model on first use; confidences are rounded here):
SELECT pii_detect('John Smith from Microsoft visited Paris to study French');
-- Returns: [{'type': NAME, 'text': John Smith, 'start_pos': 0, 'end_pos': 10, 'confidence': 0.999...},
--           {'type': ORGANIZATION, 'text': Microsoft, 'start_pos': 16, 'end_pos': 25, ...},
--           {'type': LOCATION, 'text': Paris, 'start_pos': 34, 'end_pos': 39, ...},
--           {'type': MISC, 'text': French, 'start_pos': 49, 'end_pos': 55, ...}]
```

---

#### `anofox_tab_pii_mask` (alias: `pii_mask`)

Mask all detected PII in text using specified strategy.

**Signature:**
```sql
anofox_tab_pii_mask(text VARCHAR [, strategy VARCHAR]) → VARCHAR
```

**Parameters:**
- `text`: Input text containing potential PII
- `strategy`: Masking strategy - `'redact'` (default), `'partial'`, `'asterisk'`, `'hash'` or `'none'` (case-insensitive)

**Returns:**
- `VARCHAR`: Text with PII masked according to strategy

**Examples:**
```sql
-- Default (redact) strategy
SELECT pii_mask('Email: test@example.com');
-- Returns: 'Email: [EMAIL]'

-- Partial masking
SELECT pii_mask('SSN: 123-45-6789', 'partial');
-- Returns: 'SSN: ***-**-6789'

-- Asterisk masking
SELECT pii_mask('Card: 4111111111111111', 'asterisk');
-- Returns: 'Card: ****************'
```

---

#### `anofox_tab_pii_contains` (alias: `pii_contains`)

Check if text contains any PII.

**Signature:**
```sql
anofox_tab_pii_contains(text VARCHAR) → BOOLEAN
```

**Returns:**
- `BOOLEAN`: `true` if any PII detected, `false` otherwise

**Example:**
```sql
SELECT pii_contains('Contact us at support@company.com');
-- Returns: true

SELECT pii_contains('Hello, world!');
-- Returns: false
```

---

#### `anofox_tab_pii_count` (alias: `pii_count`)

Count the number of PII matches in text.

**Signature:**
```sql
anofox_tab_pii_count(text VARCHAR) → BIGINT
```

**Returns:**
- `BIGINT`: Number of PII instances detected

**Example:**
```sql
SELECT pii_count('Email: a@b.com, SSN: 123-45-6789, Card: 4111111111111111');
-- Returns: 3
```

---

#### `anofox_tab_pii_status` (alias: `pii_status`)

Show all registered PII recognizers with metadata.

**Signature:**
```sql
pii_status() → TABLE(pii_type, recognizer_name, enabled, pattern_info)
```

**Returns:**
- `pii_type` (VARCHAR): PII type identifier (EMAIL, CREDIT_CARD, etc.)
- `recognizer_name` (VARCHAR): Human-readable name
- `enabled` (BOOLEAN): Always true (no disable mechanism)
- `pattern_info` (VARCHAR): Description of validation method

**Example:**
```sql
SELECT * FROM pii_status() ORDER BY pii_type;
-- Shows all 17 PII types with their recognizer info
```

---

#### `anofox_ner_status`

Show NER (Named Entity Recognition) model status for NAME detection.

**Signature:**
```sql
anofox_ner_status() → TABLE(onnx_available, model_status, model_path, model_size_mb, status_message)
```

**Returns:**
- `onnx_available` (BOOLEAN): Whether OpenVINO is compiled in (column name kept for backward compatibility)
- `model_status` (VARCHAR): NOT_LOADED, DOWNLOADING, LOADED, FAILED, NOT_AVAILABLE, or NOT_DOWNLOADED (model not on disk and `anofox_ner_auto_download` is false)
- `model_path` (VARCHAR): Path to ONNX model file (or "N/A")
- `model_size_mb` (DOUBLE): Model file size in MB (0 if not loaded)
- `status_message` (VARCHAR): Human-readable status description

**Example:**
```sql
SELECT * FROM anofox_ner_status();
-- onnx_available | model_status  | model_path                                                                 | model_size_mb | status_message
-- true           | LOADED        | /home/user/.duckdb/extensions/anofox/ner/distilbert-en/model_quantized.onnx | 62.7          | Model loaded successfully (distilbert-en, device: CPU)
```

**NER configuration options:**

| Option | Default | Description |
|--------|---------|-------------|
| `anofox_ner_auto_download` | `true` | Download the NER model from Hugging Face on first use when it is not on disk. `false` never downloads: NER detection is skipped (status `NOT_DOWNLOADED`) and NAME falls back to the dictionary. Setting it back to `true` lets the next NER call download the model. |
| `anofox_ner_model` | `distilbert-en` | NER model to use. Only models with downloadable assets are accepted; the value cannot be changed after the model has been loaded (restart required). |
| `anofox_ner_device` | `AUTO` | OpenVINO inference device (`AUTO`, `CPU`, `GPU`, `GPU.0`, ...). Takes effect on next model load. |
| `anofox_ner_cache_size` | `10000` | LRU cache size for NER results (0 disables caching). |

**NER input limits:**

- Inputs are truncated at the model's maximum sequence length (512 tokens for
  DistilBERT). Entities beyond the first ~512 tokens are not detected.
- The built-in WordPiece tokenizer is ASCII-focused: UTF-8 multibyte text is
  kept intact inside words, but non-ASCII punctuation/whitespace is not
  recognized as a word boundary. The bundled DistilBERT model is English-only.

---

#### `anofox_tab_pii_scan_table` (alias: `pii_scan_table`)

Scan entire table for PII across VARCHAR columns.

**Signature:**
```sql
pii_scan_table(table_name VARCHAR [, columns VARCHAR])
  → TABLE(column_name, pii_type, match_count, sample_values, confidence)
```

**Parameters:**
- `table_name`: Name of table to scan
- `columns`: Optional comma-separated column names (scans all VARCHAR if omitted)

**Returns:**
- `column_name` (VARCHAR): Column containing PII
- `pii_type` (VARCHAR): Type of PII detected
- `match_count` (BIGINT): Number of matches found
- `sample_values` (VARCHAR[]): Up to 5 sample values
- `confidence` (DOUBLE): Average confidence (always 1.0)

**Examples:**
```sql
-- Scan all columns
SELECT * FROM pii_scan_table('users');

-- Scan specific columns
SELECT * FROM pii_scan_table('users', 'email,phone');

-- Find high-risk columns
SELECT column_name, COUNT(DISTINCT pii_type) as pii_types
FROM pii_scan_table('customer_data')
GROUP BY column_name
HAVING COUNT(DISTINCT pii_type) >= 2;
```

---

#### `anofox_tab_pii_audit_table` (alias: `pii_audit_table`)

Row-level PII audit with masking for detailed inspection.

**Signature:**
```sql
pii_audit_table(table_name VARCHAR [, columns VARCHAR] [, id_column := VARCHAR])
  → TABLE(row_id, [<id_column>,] column_name, pii_type, original_value, masked_value, start_pos, end_pos, confidence)
```

**Parameters:**
- `table_name`: Name of table to audit
- `columns`: Optional comma-separated column names (audits all VARCHAR if omitted)
- `id_column` (named, optional): Identifier column passed through to every output row (see [Identifier pass-through](#identifier-pass-through-id_column))

**Returns:**
- `row_id` (BIGINT): Row number in the table
- `<id_column>` (the column's own type, only with `id_column`): The row's identifier
- `column_name` (VARCHAR): Column containing PII
- `pii_type` (VARCHAR): Type of PII detected
- `original_value` (VARCHAR): Original text value
- `masked_value` (VARCHAR): Masked version of the value
- `start_pos` (BIGINT): Start position of PII in text
- `end_pos` (BIGINT): End position of PII in text
- `confidence` (DOUBLE): Detection confidence (0.0-1.0)

**Example:**
```sql
SELECT * FROM pii_audit_table('customer_data') WHERE pii_type = 'US_SSN';

-- Keep the customer key with every finding
SELECT customer_id, column_name, pii_type
FROM pii_audit_table('customer_data', id_column := 'customer_id');
```

---

#### Type-Specific Detection Functions

Detect only specific types of PII for targeted scans:

| Function | Alias | Returns |
|----------|-------|---------|
| `anofox_tab_pii_detect_emails` | `pii_detect_emails` | List of EMAIL matches |
| `anofox_tab_pii_detect_phones` | `pii_detect_phones` | List of PHONE matches |
| `anofox_tab_pii_detect_credit_cards` | `pii_detect_credit_cards` | List of CREDIT_CARD matches |
| `anofox_tab_pii_detect_ssns` | `pii_detect_ssns` | List of US_SSN matches |
| `anofox_tab_pii_detect_names` | `pii_detect_names` | List of NAME matches (NER-based) |
| `anofox_tab_pii_detect_ibans` | `pii_detect_ibans` | List of IBAN matches |

**Example:**
```sql
SELECT pii_detect_emails('Contact: john@example.com and jane@example.com');
-- Returns only EMAIL matches
```

`anofox_tab_pii_detect_batch` (alias: `pii_detect_batch`) runs all detectors over a `VARCHAR[]`
and returns one match list per element (the element type is the `pii_detect` struct):

```sql
SELECT pii_detect_batch(['john@example.com', 'SSN: 123-45-6789']);
-- Returns: [[{'type': EMAIL, 'text': john@example.com, 'start_pos': 0, 'end_pos': 16, 'confidence': 1.0}],
--           [{'type': US_SSN, 'text': 123-45-6789, 'start_pos': 5, 'end_pos': 16, 'confidence': 1.0}]]
```

---

#### Validation Functions

Validate specific PII formats with checksum verification:

| Function | Alias | Description |
|----------|-------|-------------|
| `anofox_tab_pii_is_valid_ssn` | `pii_is_valid_ssn` | Validate US SSN format (XXX-XX-XXXX) |
| `anofox_tab_pii_is_valid_iban` | `pii_is_valid_iban` | Validate IBAN with MOD-97 checksum |
| `anofox_tab_pii_is_valid_credit_card` | `pii_is_valid_credit_card` | Validate credit card with Luhn algorithm |
| `anofox_tab_pii_is_valid_nino` | `pii_is_valid_nino` | Validate UK National Insurance Number |
| `anofox_tab_pii_is_valid_de_tax_id` | `pii_is_valid_de_tax_id` | Validate German Tax ID (11 digits + checksum) |
| `anofox_tab_pii_is_valid_crypto_address` | `pii_is_valid_crypto_address` | Validate Bitcoin/Ethereum address |

**Example:**
```sql
SELECT pii_is_valid_credit_card('4111111111111111');  -- true (Luhn valid)
SELECT pii_is_valid_iban('DE89370400440532013000');   -- true (MOD-97 valid)
```

---

#### Advanced Masking Functions

##### `anofox_tab_pii_mask_column` (alias: `pii_mask_column`)

Masks only the matches of one PII type; other PII in the value stays as it is.

**Signature:**
```sql
anofox_tab_pii_mask_column(value VARCHAR, pii_type VARCHAR, strategy VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT pii_mask_column('123-45-6789', 'US_SSN', 'partial');
-- Returns: ***-**-6789
```

---

##### `anofox_tab_pii_redact_column` (alias: `pii_redact_column`)

Mask all PII in text with an optional strategy. Without one it uses
`anofox_pii_default_mask_strategy` (default `'redact'`), which also sets the `masked_value` of
`pii_audit_table`. `pii_mask` without a strategy always redacts.

**Signature:**
```sql
anofox_tab_pii_redact_column(value VARCHAR [, strategy VARCHAR]) → VARCHAR
```

**Example:**
```sql
SELECT pii_redact_column('Email: test@example.com');
-- Returns: Email: [EMAIL]

SELECT pii_redact_column('Email: test@example.com', 'asterisk');
-- Returns: Email: ****************
```

---

#### `anofox_tab_pii_config` (alias: `pii_config`)

Display current PII configuration settings.

**Signature:**
```sql
pii_config() → TABLE(option_name VARCHAR, option_value VARCHAR, description VARCHAR)
```

**Example:**
```sql
SELECT * FROM pii_config();
```

---

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `anofox_pii_min_confidence` | DOUBLE | 0.5 | Minimum confidence threshold (0.0-1.0) |
| `anofox_pii_default_mask_strategy` | VARCHAR | 'redact' | Strategy used by `pii_redact_column(value)` and the `masked_value` of `pii_audit_table` (`redact`, `hash`, `partial`, `asterisk`, `none`; case-insensitive). `pii_mask` without a strategy always redacts. |
| `anofox_pii_enabled_types` | VARCHAR | '' | Comma-separated list of enabled PII types (empty = all) |
| `anofox_pii_deep_validation` | BOOLEAN | false | Validate detected phone numbers with libphonenumber |

**Example:**
```sql
SET anofox_pii_min_confidence = 0.7;
SET anofox_pii_default_mask_strategy = 'partial';
```

---

## Data Quality Metrics

Track essential data quality dimensions. Every function is a table function registered under its
canonical `anofox_tab_*` name plus the short alias shown; each returns exactly one assertion-style
summary row with a `status` ('pass'/'fail') and a human-readable `message`, even for empty input.

### Functions

#### `anofox_tab_volume` (alias: `volume`)

Validate row count against thresholds.

**Signature:**
```sql
volume(table_name VARCHAR, min_rows BIGINT, max_rows BIGINT) → TABLE
```

**Returns:** `status`, `row_count`, `min_threshold`, `max_threshold`, `message`

**Example:**
```sql
SELECT * FROM volume('orders', 1000, 1000000);
```

---

#### `anofox_tab_null_rate` (alias: `null_rate`)

Check null percentage in a column.

**Signature:**
```sql
null_rate(table_name VARCHAR, column_name VARCHAR, max_null_rate DOUBLE) → TABLE
```

**Returns:** `status`, `null_count`, `total_count`, `null_rate`, `threshold`, `message`

**Example:**
```sql
SELECT * FROM null_rate('users', 'email', 0.05);
```

---

#### `anofox_tab_distinct_count` (alias: `distinct_count`)

Validate cardinality (distinct count) of a column.

**Signature:**
```sql
distinct_count(table_name VARCHAR, column_name VARCHAR, min_distinct BIGINT, max_distinct BIGINT) → TABLE
```

**Returns:** `status`, `distinct_count`, `min_threshold`, `max_threshold`, `message`

**Example:**
```sql
SELECT * FROM distinct_count('products', 'sku', 100, NULL);
```

---

#### `anofox_tab_schema_check` (alias: `schema_check`)

Check required columns exist in a table.

**Signature:**
```sql
schema_check(table_name VARCHAR, required_columns LIST<VARCHAR>) → TABLE
```

**Returns:** `status`, `missing_columns`, `message`

**Example:**
```sql
SELECT * FROM schema_check('orders', ['id', 'created_at']);
```

---

#### `anofox_tab_freshness` (alias: `freshness`)

Validate data recency (maximum age check).

**Signature:**
```sql
freshness(table_name VARCHAR, timestamp_column VARCHAR, max_age INTERVAL [, reference_time TIMESTAMP]) → TABLE
```

**Returns:** `status`, `metric_value`, `threshold`, `age_seconds`, `message`

**Example:**
```sql
SELECT * FROM freshness('events', 'created_at', INTERVAL '1 hour');
```

---

#### `anofox_tab_zscore` (alias: `zscore`)

Detect outliers via the z-score method (assumes normal distribution).

**Signature:**
```sql
zscore(table_name VARCHAR, column_name VARCHAR, threshold DOUBLE) → TABLE
```

**Parameters:**
- `threshold`: Z-score threshold (typical value: 3.0)

**Returns:** `status`, `mean`, `stddev`, `outlier_count`, `total_count`, `outlier_rate`, `threshold`, `message`

**Example:**
```sql
SELECT * FROM zscore('transactions', 'amount', 3.0);
```

---

#### `anofox_tab_iqr` (alias: `iqr`)

Detect outliers via the IQR method (non-parametric, robust to distribution).

**Signature:**
```sql
iqr(table_name VARCHAR, column_name VARCHAR, iqr_multiplier DOUBLE) → TABLE
```

**Parameters:**
- `iqr_multiplier`: IQR multiplier for the outlier fences (typical value: 1.5)

**Returns:** `status`, `q1`, `q3`, `iqr`, `lower_bound`, `upper_bound`, `outlier_count`, `total_count`, `multiplier`, `message`

**Example:**
```sql
SELECT * FROM iqr('transactions', 'amount', 1.5);
```

---

#### `anofox_tab_regex_match` (alias: `regex_match`)

Assert that the share of non-NULL values matching a regex pattern is within bounds.

**Signature:**
```sql
regex_match(table_name VARCHAR, column_name VARCHAR, pattern VARCHAR, min_match_rate DOUBLE [, max_match_rate DOUBLE]) → TABLE
```

**Parameters:**
- `pattern`: Regular expression (RE2 syntax, as accepted by `regexp_matches`)
- `min_match_rate`: Minimum acceptable match rate (NULL = unbounded)
- `max_match_rate`: Maximum acceptable match rate (optional; omit or NULL = unbounded)

**Returns:** `status`, `match_rate`, `matched_count`, `total_count`, `min_threshold`, `max_threshold`, `message`

**Empty input:** an empty table or all-NULL column passes trivially with `total_count = 0`.

**Example:**
```sql
SELECT * FROM regex_match('users', 'email', '^[^@]+@[^@]+$', 0.99);
```

---

#### `anofox_tab_values_in_set` (alias: `values_in_set`)

Assert that the share of non-NULL values contained in an allowed value set is at least `min_rate`.
Values are compared as strings.

**Signature:**
```sql
values_in_set(table_name VARCHAR, column_name VARCHAR, allowed_values LIST<VARCHAR> [, min_rate DOUBLE = 1.0]) → TABLE
```

**Returns:** `status`, `in_set_rate`, `in_set_count`, `total_count`, `threshold`, `sample_violations` (up to 5 distinct offending values), `message`

**Empty input:** an empty table or all-NULL column passes trivially with `total_count = 0`.

**Example:**
```sql
SELECT * FROM values_in_set('orders', 'status', ['completed', 'pending', 'failed']);
```

---

#### `anofox_tab_agg_check` (alias: `agg_check`)

Assert that an aggregate of a numeric column is within bounds.

**Signature:**
```sql
agg_check(table_name VARCHAR, column_name VARCHAR, agg VARCHAR, lower_threshold DOUBLE, upper_threshold DOUBLE) → TABLE
```

**Parameters:**
- `agg`: one of `'avg'`, `'min'`, `'max'`, `'sum'`, `'median'`, `'stddev'` (validated at bind time)
- `lower_threshold` / `upper_threshold`: bounds; `NULL` = unbounded

**Returns:** `status`, `agg`, `value`, `lower_threshold`, `upper_threshold`, `message`

**Empty input:** an empty table or all-NULL column yields `value = NULL` and passes trivially.

**Example:**
```sql
SELECT * FROM agg_check('orders', 'amount', 'avg', 10.0, 500.0);
```

---

#### `anofox_tab_duplicate_count` (alias: `duplicate_count`)

Assert that the number of duplicate values in a column (or comma-separated column combination)
does not exceed `max_duplicates`. Duplicates are computed as `COUNT(*) - COUNT(DISTINCT key)`;
for a single column, NULL values count as duplicates of each other.

**Signature:**
```sql
duplicate_count(table_name VARCHAR, column_names VARCHAR [, max_duplicates BIGINT = 0]) → TABLE
```

**Returns:** `status`, `duplicate_count`, `total_count`, `threshold`, `message`

**Example:**
```sql
SELECT * FROM duplicate_count('orders', 'order_id');
SELECT * FROM duplicate_count('orders', 'customer_id,order_date', 10);
```

---

#### `anofox_tab_occurrence` (alias: `occurrence`)

Assert that the highest (`'max'`) or lowest (`'min'`) frequency of any single value in a column is
within bounds. NULL values are excluded.

**Signature:**
```sql
occurrence(table_name VARCHAR, column_name VARCHAR, mode VARCHAR, lower_threshold BIGINT, upper_threshold BIGINT) → TABLE
```

**Parameters:**
- `mode`: `'max'` or `'min'` (validated at bind time)
- `lower_threshold` / `upper_threshold`: bounds; `NULL` = unbounded

**Returns:** `status`, `mode`, `occurrence`, `extreme_value` (the value at the extreme frequency), `distinct_values`, `lower_threshold`, `upper_threshold`, `message`

**Empty input:** an empty table or all-NULL column yields `occurrence = NULL` and passes trivially.

**Example:**
```sql
SELECT * FROM occurrence('orders', 'customer_id', 'max', NULL, 100);
```

---

#### `anofox_tab_match_rate` (alias: `match_rate`)

Assert that the share of left-table rows with a join partner in the right table is at least
`min_rate`. The right side is deduplicated on its key columns so join fan-out cannot inflate the
rate. Doubles as a referential-integrity / foreign-key check.

**Signature:**
```sql
match_rate(left_table VARCHAR, right_table VARCHAR, left_keys VARCHAR, right_keys VARCHAR [, min_rate DOUBLE = 1.0]) → TABLE
```

**Parameters:**
- `left_keys` / `right_keys`: comma-separated key column lists; must have the same number of columns

**Returns:** `status`, `match_rate`, `matched_count`, `total_count`, `threshold`, `message`

**Empty input:** an empty left table passes trivially with `total_count = 0`.

**Example:**
```sql
SELECT * FROM match_rate('orders', 'customers', 'customer_id', 'id', 1.0);
```

---

#### `anofox_tab_compliance` (alias: `compliance`)

Assert that the share of rows satisfying a SQL boolean expression is at least `min_rate`. Rows
where the expression evaluates to NULL count as non-compliant.

> **Security note:** the expression is executed as SQL with the caller's privileges. It is
> validated at bind time to be a single boolean expression (multi-statement injection payloads are
> rejected with a binder error), but scalar subqueries remain allowed — pass only trusted
> expressions. The trust model is the same as running SQL directly.

**Signature:**
```sql
compliance(table_name VARCHAR, expression VARCHAR [, min_rate DOUBLE = 1.0]) → TABLE
```

**Returns:** `status`, `compliance_rate`, `compliant_count`, `total_count`, `threshold`, `message`

**Empty input:** an empty table passes trivially with `total_count = 0`.

**Example:**
```sql
SELECT * FROM compliance('orders', 'amount > 0 AND status IS NOT NULL', 0.95);
```

---

#### `anofox_tab_rel_count_change` (alias: `rel_count_change`)

Assert that the daily (distinct) count on a reference date stays within a relative change band
around the rolling baseline average.

**Signature:**
```sql
rel_count_change(table_name VARCHAR, date_column VARCHAR [, count_column VARCHAR,
                 window_days BIGINT = 7, lower_threshold DOUBLE = -0.5,
                 upper_threshold DOUBLE = 0.5, reference_date DATE]
                 [, start_date := DATE, end_date := DATE]) → TABLE
```

**Parameters:**
- `count_column`: column counted with `COUNT(DISTINCT ...)` per day; NULL/omitted = daily row count
- `window_days`: baseline window, the days *strictly before* the reference date
- `reference_date`: NULL/omitted = the latest date present in the data

**Returns:** `status`, `reference_date`, `reference_count`, `baseline_avg`, `baseline_days`, `rel_change`, `lower_threshold`, `upper_threshold`, `message`

**Trivial pass:** empty input, no baseline days, or a zero baseline pass with an explicit message
(not enough history is not a failure).

**Date range:** `start_date := DATE, end_date := DATE` (named, both or neither, `start_date <= end_date`)
evaluates every day of the inclusive range instead of one reference date and returns one row per
date, ordered by `reference_date`, with the same columns as the single-date form; each row equals the
single-date call for that date. The range cannot be combined with an explicit (non-NULL)
`reference_date`.

**Example:**
```sql
SELECT * FROM rel_count_change('orders', 'order_date', NULL, 7, -0.5, 0.5);

-- one row per day of January, e.g. for a status strip or an alarm timeline
SELECT reference_date, status, rel_change
FROM rel_count_change('orders', 'order_date', start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');
```

---

#### `anofox_tab_metric_anomaly_iqr` (alias: `metric_anomaly_iqr`)

Flag the reference date as anomalous when its daily metric falls outside `Q1/Q3 ± k·IQR` of the
trailing window (distinct from the row-level `iqr` check).

**Signature:**
```sql
metric_anomaly_iqr(table_name VARCHAR, date_column VARCHAR [, metric_column VARCHAR,
                   window_days BIGINT = 30, k DOUBLE = 1.5, mode VARCHAR = 'both',
                   reference_date DATE] [, start_date := DATE, end_date := DATE]) → TABLE
```

**Parameters:**
- `metric_column`: column averaged per day; NULL/omitted = daily row count
- `mode`: `'both'`, `'upper'` or `'lower'` — which bound(s) to enforce
- `reference_date`: NULL/omitted = the latest date present in the data

**Returns:** `status`, `reference_date`, `metric_value`, `q1`, `q3`, `lower_bound`, `upper_bound`, `baseline_days`, `mode`, `message`

**Trivial pass:** empty input or an empty baseline window pass with an explicit message.

**Recipe — anomaly detection on your check history:** point it at a persisted `run_checks`
result table: `SELECT * FROM metric_anomaly_iqr('dq_results', 'run_ts', 'value', 30, 1.5, 'both');`

**Date range:** `start_date := DATE, end_date := DATE` (named, both or neither, `start_date <= end_date`)
evaluates every day of the inclusive range instead of one reference date and returns one row per
date, ordered by `reference_date`, with the same columns as the single-date form; each row equals the
single-date call for that date. The range cannot be combined with an explicit (non-NULL)
`reference_date`.

**Example:**
```sql
SELECT * FROM metric_anomaly_iqr('orders', 'order_date', NULL, 30, 1.5, 'both');
SELECT * FROM metric_anomaly_iqr('orders', 'order_date', start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');
```

---

#### `anofox_tab_rolling_values_in_set` (alias: `rolling_values_in_set`)

Assert values-in-set membership over the trailing `window_days` days ending at the reference date
(inclusive).

**Signature:**
```sql
rolling_values_in_set(table_name VARCHAR, column_name VARCHAR, allowed_values LIST<VARCHAR>,
                      date_column VARCHAR [, window_days BIGINT = 7, min_rate DOUBLE = 1.0,
                      reference_date DATE] [, start_date := DATE, end_date := DATE]) → TABLE
```

**Returns:** `status`, `reference_date`, `in_set_rate`, `in_set_count`, `total_count`, `threshold`, `sample_violations`, `message`

`sample_violations` holds up to 5 distinct values outside the allowed set, sorted.

**Date range:** `start_date := DATE, end_date := DATE` (named, both or neither, `start_date <= end_date`)
evaluates every day of the inclusive range instead of one reference date and returns one row per
date, ordered by `reference_date`, with the same columns as the single-date form; each row equals the
single-date call for that date. The range cannot be combined with an explicit (non-NULL)
`reference_date`.

**Example:**
```sql
SELECT * FROM rolling_values_in_set('orders', 'status', ['completed', 'pending'], 'order_date', 14);
SELECT * FROM rolling_values_in_set('orders', 'status', ['completed', 'pending'], 'order_date', 14,
                                    start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');
```

#### Events companions: `rel_count_change_events`, `metric_anomaly_iqr_events`, `rolling_values_in_set_events`

Primary names `anofox_tab_rel_count_change_events`, `anofox_tab_metric_anomaly_iqr_events`,
`anofox_tab_rolling_values_in_set_events`. Each takes exactly the arguments of its base check,
including `reference_date` and the `start_date := DATE, end_date := DATE` range, and returns the
anofox contract's `events` shape: one row per reference date whose check fails (or warns), ordered
by `ds`. Passing dates are not events, so a passing single-date call returns no rows.

| Column | Type | Meaning |
|--------|------|---------|
| `ds` | DATE | the reference date |
| `kind` | VARCHAR | the check name (`rel_count_change`, `metric_anomaly_iqr`, `rolling_values_in_set`) |
| `label` | VARCHAR | the base check's `message` |
| `severity` | VARCHAR | `'alarm'` for `fail`, `'warning'` for `warn` |
| `value` | DOUBLE | the measure: `rel_change`, `metric_value` or `in_set_rate` |
| `threshold` | DOUBLE | the bound the measure crossed: the violated `lower_threshold`/`upper_threshold`, the crossed IQR bound (`lower_bound`/`upper_bound`), or `min_rate` |
| `series` | VARCHAR | the table, or `table.column` when the check measures a column (`count_column`, `metric_column`, `column_name`) |

Tagged `{'anofox.output': 'events', 'anofox.contract': '1'}`, see
[anofox Integration Contract](#anofox-integration-contract).

**Example:**
```sql
-- the alarm timeline of January
SELECT ds, kind, severity, value, threshold, label
FROM rel_count_change_events('orders', 'order_date', start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');
```

---

## Check Suites

Define many checks once in a SQL table and run them all with one call, getting one uniform result
row per check (and per identifier partition).

#### `anofox_tab_run_checks` (alias: `run_checks`)

**Signature:**
```sql
run_checks(checks_table VARCHAR) → TABLE
```

**Checks-table contract** (validated at bind time):

| Column | Type | Meaning |
|--------|------|---------|
| `check_name` | VARCHAR | Unique label for the check (required) |
| `check_type` | VARCHAR | One of `volume`, `null_rate`, `distinct_count`, `regex_match`, `values_in_set`, `agg`, `duplicate_count`, `occurrence`, `match_rate`, `compliance`, `freshness`, `rel_count_change`, `metric_anomaly_iqr`, `rolling_values_in_set` |
| `table_name` | VARCHAR | Target table or view (required) |
| `column_name` | VARCHAR | Column to check (NULL where not applicable; comma-separated for `duplicate_count`) |
| `params` | VARCHAR (JSON) | Type-specific parameters, see below |
| `lower_threshold` / `upper_threshold` | DOUBLE | Value bounds; NULL = unbounded |
| `monitor_only` | BOOLEAN | true records `'warn'` instead of `'fail'` |
| `identifier_column` | VARCHAR | Optional partition column: one result row per distinct value |
| `filter_expr` | VARCHAR | Optional SQL boolean pre-filter; supports `${today}`, `${yesterday}`, `${today±N}`, `${yesterday±N}` tokens |

**`params` keys by check type:** `pattern` (regex_match), `allowed_values` JSON array
(values_in_set, rolling_values_in_set), `agg` (agg), `mode` (occurrence, metric_anomaly_iqr),
`expression` (compliance), `right_table`/`left_keys`/`right_keys` (match_rate),
`date_column`/`count_column`/`metric_column`/`window_days`/`k`/`reference_date` (time-aware checks),
`reference_time` (freshness; the `value` is the age in seconds vs this timestamp, default `now()`).

**Uniform result schema:** `run_ts TIMESTAMP`, `check_name`, `check_type`, `table_name`,
`column_name`, `identifier`, `value DOUBLE`, `lower_threshold`, `upper_threshold`,
`status` (`'pass' | 'warn' | 'fail' | 'error'`), `message`.

**Behavior notes:**
- The checks table must be a **regular (non-temporary) table** — it is read through a separate
  connection at bind time. Target tables may be temporary tables or views.
- **Error isolation:** a missing target table or an unsupported identifier combination yields a
  single `status = 'error'` row for that check instead of failing the whole run.
- `identifier_column` is supported for `volume`, `null_rate`, `distinct_count`, `regex_match`,
  `values_in_set`, `agg`, `duplicate_count`, `compliance` and `freshness`.
- A NULL `value` (e.g. empty input) passes trivially.
- `compliance` expressions and `filter_expr` are validated at bind time to be single SQL boolean
  expressions (multi-statement payloads are rejected) but still execute with the caller's
  privileges — same trust model as running SQL directly.

**Persistence recipe** (results history for monitoring):
```sql
CREATE TABLE IF NOT EXISTS dq_results AS SELECT * FROM run_checks('dq_checks') LIMIT 0;
INSERT INTO dq_results SELECT * FROM run_checks('dq_checks');
-- metric-level anomaly detection over the history:
SELECT * FROM metric_anomaly_iqr('dq_results', 'run_ts', 'value', 30, 1.5, 'both');
```

**Example:**
```sql
SELECT check_name, status, value, message FROM run_checks('dq_checks') ORDER BY check_name;
```

---

## anofox Integration Contract

anofox-tabular takes part in the anofox integration contract (anofox-visualization
`docs/plans/integration-contract.md`): producers whose output follows a contract schema are tagged
in `duckdb_functions().tags`, so anofox-visualization (`anofox_plot`) and other consumers find them
without adapter SQL.

#### `anofox_contract_version()`

```sql
anofox_contract_version() → VARCHAR   -- '1'
```

The contract version this extension implements. It is shared by every anofox extension: whichever
extension loads first registers it, the others leave it in place.

**Tagged producers** (`{'anofox.output': '<schema>', 'anofox.contract': '1'}`, on the
`anofox_tab_*` primary name and on the alias):

| Producer | Schema | Columns |
|----------|--------|---------|
| `run_checks` | `check` | `run_ts, check_name, check_type, table_name, column_name, identifier, value, lower_threshold, upper_threshold, status, message` |
| `profile_correlations` | `matrix` | `column_a, column_b, pearson, spearman, n` (a pair list; the reader mirrors it) |
| `rel_count_change_events`, `metric_anomaly_iqr_events`, `rolling_values_in_set_events` | `events` | `ds, kind, label, severity, value, threshold, series` |

```sql
SELECT DISTINCT function_name, tags['anofox.output'] AS output
FROM duckdb_functions() WHERE tags['anofox.contract'] IS NOT NULL;
```

**The one-row checks** (`volume`, `null_rate`, `regex_match`, ..., and `agg_check`) keep their own
result columns and are not tagged. `run_checks` is the contract-shaped `check` producer for them:
put the checks in a checks table and every row comes back as `check_name, status, value,
lower_threshold, upper_threshold, ...`. It runs 14 check types (`volume`, `null_rate`,
`distinct_count`, `regex_match`, `values_in_set`, `agg`, `duplicate_count`, `occurrence`,
`match_rate`, `compliance`, `freshness`, `rel_count_change`, `metric_anomaly_iqr`,
`rolling_values_in_set`); `zscore`, `iqr` and `schema_check` are not available in a suite.
Besides the contract statuses `pass`/`warn`/`fail`, `run_checks` reports a check that cannot run
(e.g. a missing target table) as `status = 'error'`.

The time-aware checks map to `events` through their `_events` companions (above); their base form
keeps its columns.

---

## Anomaly Detection

Unsupervised anomaly detection algorithms for finding outliers in data.

### Isolation Forest (Enhanced)

Industry-grade anomaly detection with [isotree](https://github.com/david-cortes/isotree)-inspired features:

**Core Capabilities:**
- **Categorical Support** - Auto-detect VARCHAR columns with random subset splitting
- **Extended IF (ndim)** - Hyperplane splits for diagonal/curved anomaly patterns
- **Density Scoring** - Alternative metric based on points-to-volume ratio
- **Sample Weights** - Weighted sampling for imbalanced datasets
- **SCiForest** - Information-gain guided splitting with configurable candidates

#### `anofox_tab_isolation_forest` (alias: `isolation_forest`)

Univariate Isolation Forest for single column anomaly detection.

**Signature:**
```sql
isolation_forest(
    table_name VARCHAR,
    column_name VARCHAR
    [, n_trees BIGINT          -- 1-500, default 100
    [, sample_size BIGINT      -- 1-10000, default 256
    [, contamination DOUBLE    -- 0.0-0.5, default 0.1
    [, output_mode VARCHAR     -- 'summary' (default) or 'scores'
    [, seed BIGINT ]]]]]       -- fixed seed for reproducible scores; NULL = random
    [, id_column := VARCHAR]   -- named; 'scores' mode only, see Identifier pass-through
) → TABLE
```

Trailing parameters are optional and positional. The Extended IF, SCiForest, weighting and
scoring options are only available on `isolation_forest_mv` (which also accepts a single column).

**Returns:**
- `'scores'`: one row per input row: `row_id`, [`<id_column>`,] `value`, `anomaly_score`, `is_anomaly`
- `'summary'`: one row: `status`, `outlier_count`, `total_count`, `avg_value`, `contamination`,
  `n_trees`, `message`

**Examples:**
```sql
-- Score every row, keep the anomalies
SELECT * FROM isolation_forest('sales_data', 'amount', 100, 256, 0.1, 'scores')
WHERE is_anomaly;

-- Reproducible scores
SELECT * FROM isolation_forest('sales_data', 'amount', 100, 256, 0.1, 'scores', 42);

-- Keep each row's key next to its score
SELECT order_id, anomaly_score
FROM isolation_forest('sales_data', 'amount', 100, 256, 0.1, 'scores', id_column := 'order_id')
WHERE is_anomaly;

-- One summary row
SELECT status, outlier_count, message FROM isolation_forest('sales_data', 'amount');
```

---

#### `anofox_tab_isolation_forest_mv` (alias: `isolation_forest_mv`)

Multivariate Isolation Forest over one or more columns, with categorical columns, Extended IF,
SCiForest, sample weights and alternative scoring.

**Signature:**
```sql
isolation_forest_mv(
    table_name VARCHAR,
    column_names VARCHAR       -- comma-separated, e.g. 'amount, quantity, region'
    [, n_trees BIGINT          -- 1-500, default 100
    [, sample_size BIGINT      -- 1-10000, default 256
    [, contamination DOUBLE    -- 0.0-0.5, default 0.1
    [, output_mode VARCHAR     -- 'summary' (default) or 'scores'
    [, seed BIGINT             -- NULL = random
    [, ndim BIGINT             -- >= 1, default 1; > 1 = Extended IF hyperplane splits
    [, coef_type VARCHAR       -- 'uniform' (default) or 'normal'
    [, scoring_metric VARCHAR  -- 'depth' (default), 'density' or 'adj_depth'
    [, weight_column VARCHAR   -- column of sample weights; NULL = uniform
    [, ntry BIGINT             -- >= 1, default 1; > 1 = SCiForest split candidates
    [, prob_pick_avg_gain DOUBLE ]]]]]]]]]]]  -- 0.0-1.0, default 0.0
    [, id_column := VARCHAR]   -- named; 'scores' mode only, see Identifier pass-through
) → TABLE
```

Pass `NULL` to keep a default while setting a later parameter.

**Returns:**
- `'scores'`: one row per input row: `row_id`, [`<id_column>`,] `anomaly_score`, `is_anomaly`
- `'summary'`: one row: `status`, `outlier_count`, `total_count`, `n_columns`, `contamination`,
  `n_trees`, `message`

**Examples:**
```sql
-- Basic multivariate
SELECT * FROM isolation_forest_mv(
    'customer_events', 'purchase_amount, session_duration, page_views',
    100, 256, 0.1, 'scores'
) ORDER BY anomaly_score DESC LIMIT 10;

-- Extended IF with density scoring
SELECT * FROM isolation_forest_mv(
    'transactions', 'amount, quantity, duration',
    100, 256, 0.05, 'scores',
    NULL,       -- seed
    3,          -- ndim: 3-dimensional hyperplanes
    'normal',   -- coef_type
    'density'   -- scoring_metric
);

-- SCiForest with gain-based split selection
SELECT * FROM isolation_forest_mv(
    'events', 'value', 100, 256, 0.05, 'scores',
    NULL, 1, 'uniform', 'depth',
    NULL,       -- weight_column
    10,         -- ntry: evaluate 10 split candidates
    0.5         -- prob_pick_avg_gain: 50% gain-based
);
```

---

### DBSCAN Clustering

#### `anofox_tab_dbscan` (alias: `dbscan`)

Univariate DBSCAN clustering for single column anomaly detection.

**Signature:**
```sql
anofox_tab_dbscan(
    table_name VARCHAR,
    column_name VARCHAR,
    eps DOUBLE,          -- optional, default 0.5
    min_pts BIGINT,      -- optional, default 5
    output_mode VARCHAR  -- optional, default 'summary'
    [, id_column := VARCHAR]  -- named; 'clusters' mode only, see Identifier pass-through
) → TABLE
```

**Parameters:**
- `eps`: Neighborhood radius, must be > 0.0 (default: 0.5)
- `min_pts`: Minimum points for dense region, must be >= 1 (default: 5)
- `output_mode`: `'summary'` (aggregate stats) or `'clusters'` (per-row results)

All parameters after `column_name` are optional.

**Returns:**
- `'summary'`: one row with `status` (`fail` when noise points exist), `cluster_count`, `noise_count`, `total_count`, `noise_rate`, `largest_cluster_size`, `eps`, `min_pts`, `message`
- `'clusters'`: one row per non-NULL input row with `row_id`, [`<id_column>`,] `value`, `cluster_id` (`-1` = noise), `point_type`, `neighbor_count`, `anomaly_score`, `is_anomaly`
- `point_type`: `'CORE'` (dense region), `'BORDER'` (cluster edge), or `'NOISE'` (outlier)

**Example:**
```sql
SELECT * FROM dbscan(
    'transactions', 'amount', 10.0, 5, 'clusters'
) WHERE point_type = 'NOISE';
```

---

#### `anofox_tab_dbscan_mv` (alias: `dbscan_mv`)

Multivariate DBSCAN clustering for multiple column anomaly detection.

**Signature:**
```sql
anofox_tab_dbscan_mv(
    table_name VARCHAR,
    columns VARCHAR,
    eps DOUBLE,          -- optional, default 0.5
    min_pts BIGINT,      -- optional, default 5
    output_mode VARCHAR  -- optional, default 'summary'
    [, id_column := VARCHAR]  -- named; 'clusters' mode only, see Identifier pass-through
) → TABLE
```

**Parameters:**
- `columns`: Comma-separated column names

**Returns:**
- `'summary'`: same columns as `anofox_tab_dbscan` plus `n_columns` (no `value` column in `'clusters'` mode)
- `'clusters'`: one row per non-NULL input row with `row_id`, [`<id_column>`,] `cluster_id`, `point_type`, `neighbor_count`, `anomaly_score`, `is_anomaly`

---

### Identifier pass-through (`id_column`)

The row-level outputs identify rows by `row_id`, a position that depends on the input order. The
optional named parameter `id_column := '<column>'` adds the value of an input column to every
output row, so results join back to the input by key instead of by position:

| Function | Row-level mode |
|----------|----------------|
| `isolation_forest`, `isolation_forest_mv` | `'scores'` |
| `dbscan`, `dbscan_mv` | `'clusters'` |
| `outlier_tree` | `'outliers'` |
| `pii_audit_table` | always |

- The column is inserted directly after `row_id`, under the input column's name and with its type;
  `row_id` keeps its meaning. Without `id_column` the output schema is unchanged.
- The id is read in the same pass as the analyzed values, so it always belongs to the row the
  result describes; rows skipped because a feature is NULL are skipped together with their id.
- NULL ids are passed through; they never exclude a row.
- Bind errors: a column the table does not have (`id_column 'x' not found in table 't'`), a
  non-row-level `output_mode` (`id_column requires output_mode 'scores'`), and a name that collides
  with an output column (e.g. an input column called `anomaly_score`; alias it in a view first).
- Both the `anofox_tab_*` names and the short aliases accept it.

```sql
SELECT s.order_id, s.anomaly_score, o.customer
FROM isolation_forest_mv('orders', 'amount,quantity', 100, 256, 0.05, 'scores', 42,
                         id_column := 'order_id') s
JOIN orders o USING (order_id)
WHERE s.is_anomaly;
```

---

### OutlierTree (Explainable Anomaly Detection)

Detects outliers using decision tree conditioning, providing human-readable explanations for why specific values are anomalous based on conditional distributions.

**Key Features:**
- Detects outliers in context (e.g., "salary is high *for a Junior Developer*")
- Returns natural language explanations with statistical backing
- Uses robust statistics (median + MAD) resistant to outliers
- Supports both numeric and categorical columns

#### `anofox_tab_outlier_tree` (alias: `outlier_tree`)

Explainable outlier detection with conditional distributions.

**Signature:**
```sql
-- Simple (3 params, uses defaults)
outlier_tree(table_name VARCHAR, columns VARCHAR, output_mode VARCHAR) → TABLE

-- Full (9 params)
outlier_tree(
    table_name VARCHAR,
    columns VARCHAR,
    output_mode VARCHAR,
    max_depth INTEGER,
    max_perc_outliers DOUBLE,
    min_size_numeric INTEGER,
    min_size_categ INTEGER,
    z_norm DOUBLE,
    z_outlier DOUBLE
) → TABLE

-- Both forms take the named parameter id_column := VARCHAR ('outliers' mode only)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | VARCHAR | required | Source table name |
| `columns` | VARCHAR | required | Comma-separated column names to analyze |
| `output_mode` | VARCHAR | 'summary' | `'summary'` or `'outliers'` |
| `max_depth` | INTEGER | 4 | Maximum tree depth |
| `max_perc_outliers` | DOUBLE | 0.01 | Max fraction of outliers per cluster |
| `min_size_numeric` | INTEGER | 25 | Min cluster size for numeric targets |
| `min_size_categ` | INTEGER | 75 | Min cluster size for categorical targets |
| `z_norm` | DOUBLE | 2.67 | Z-threshold for confidence intervals |
| `z_outlier` | DOUBLE | 8.0 | Z-threshold for outlier flagging |
| `id_column` (named) | VARCHAR | — | Identifier column passed through after `row_id` (`'outliers'` mode only, see [Identifier pass-through](#identifier-pass-through-id_column)) |

**Summary Mode Returns:**
| Column | Type | Description |
|--------|------|-------------|
| `status` | VARCHAR | `'pass'` or `'fail'` |
| `total_rows` | BIGINT | Rows analyzed |
| `outlier_count` | BIGINT | Outliers detected |
| `columns_analyzed` | INTEGER | Columns analyzed |
| `clusters_evaluated` | BIGINT | Clusters evaluated |
| `max_depth_reached` | INTEGER | Max tree depth reached |
| `message` | VARCHAR | Summary message |

**Outliers Mode Returns:**
| Column | Type | Description |
|--------|------|-------------|
| `row_id` | BIGINT | Row index (1-indexed) |
| `<id_column>` | the column's type | The row's identifier (only with `id_column`) |
| `column_name` | VARCHAR | Column with outlier |
| `outlier_value` | VARCHAR | The anomalous value, as text (numeric and categorical columns) |
| `outlier_value_num` | DOUBLE | The anomalous value as a number for numeric columns, without the precision loss of the text form; NULL for categorical columns |
| `cluster_mean` | DOUBLE | Mean in cluster |
| `cluster_sd` | DOUBLE | SD in cluster |
| `cluster_size` | BIGINT | Rows in cluster |
| `z_score` | DOUBLE | Robust z-score |
| `lower_bound` | DOUBLE | Lower CI bound |
| `upper_bound` | DOUBLE | Upper CI bound |
| `conditions` | VARCHAR | JSON array of split conditions |
| `explanation` | VARCHAR | Human-readable explanation |
| `outlier_score` | DOUBLE | Rarity score (lower = more anomalous) |

**Examples:**
```sql
-- Summary mode: quick pass/fail check
SELECT * FROM outlier_tree('employees', 'department,salary', 'summary');

-- Outliers mode: get detailed explanations
SELECT row_id, column_name, outlier_value, explanation
FROM outlier_tree('employees', 'department,salary,years_exp', 'outliers');

-- Returns explanations like:
-- "Value 150000 for column 'salary' is unusually high (expected: 52333 ± 7413)
--  when job_title = 'Junior Developer'"

-- Numeric outliers against their cluster's normal range (no TRY_CAST needed)
SELECT row_id, column_name, outlier_value_num, lower_bound, upper_bound
FROM outlier_tree('employees', 'department,salary,years_exp', 'outliers')
WHERE outlier_value_num IS NOT NULL;

-- With custom parameters for small datasets
SELECT * FROM outlier_tree(
    'test_data', 'category,value', 'outliers',
    4,          -- max_depth
    0.5,        -- max_perc_outliers
    3,          -- min_size_numeric
    2,          -- min_size_categ
    2.67,       -- z_norm
    3.0         -- z_outlier
);
```

---

## Data Diffing

Compare tables and identify changes for migration validation and regression testing.

### Functions

#### `anofox_tab_diff_joindiff` (alias: `diff_joindiff`)

Detailed row-level diff between two tables or views.

**Signature:**
```sql
anofox_tab_diff_joindiff(
    source_table VARCHAR,
    target_table VARCHAR,
    primary_key VARCHAR | LIST<VARCHAR>
    [, compare_columns LIST<VARCHAR>]
    [, include_all BOOLEAN]
) → TABLE
```

**Parameters:**
- `source_table`: Source table or view name
- `target_table`: Target table or view name
- `primary_key`: Primary key column(s) for matching rows (single name or list);
  by name it is `primary_key := 'id'` or `primary_keys := ['id', 'date']`
  (passing both names is a binder error)
- `compare_columns`: Optional list of columns to compare (default: all
  non-key columns present in both tables)
- `include_all`: Include `unchanged` rows in the output (default: `false`)

Every parameter can be passed positionally or by name (`:=`), and the two can
be mixed: positional values are read first, and a named parameter **overrides**
the positional value of the same parameter. `source_table`, `target_table` and
the primary key are required (positionally or by name).

```sql
SELECT * FROM diff_joindiff('source_tbl', 'target_tbl', 'id', include_all := true);
SELECT * FROM diff_joindiff(source_table := 'source_tbl', target_table := 'target_tbl',
                            primary_keys := ['user_id', 'date'], compare_columns := ['amount']);
```

**Semantics:**
- Primary keys are matched NULL-safely (`IS NOT DISTINCT FROM`), so rows with
  NULL key values are compared across sides instead of being misreported as
  added/removed.
- Primary key and compare columns are validated against both table schemas at
  bind time; missing columns or tables produce a clear binder error.

**Returns:**
```sql
TABLE(
    diff_type VARCHAR,      -- 'added', 'removed', 'changed', 'unchanged'
    <primary key columns>,  -- COALESCE of source/target key values
    <target columns>        -- remaining target columns (NULL for removed rows)
)
```

**Example:**
```sql
SELECT * FROM diff_joindiff('source_tbl', 'target_tbl', ['user_id', 'date'])
WHERE diff_type IN ('added', 'changed')
LIMIT 100;
```

---

#### `anofox_tab_diff_hashdiff` (alias: `diff_hashdiff`)

Row-level diff between two tables or views. Currently computes exactly the
same result as `diff_joindiff` (without `compare_columns`/`include_all`).

**Signature:**
```sql
anofox_tab_diff_hashdiff(
    source_table VARCHAR,
    target_table VARCHAR,
    primary_key VARCHAR | LIST<VARCHAR>
) → TABLE
```

**Parameters:**
- `source_table`: Source table or view name
- `target_table`: Target table or view name
- `primary_key`: Primary key column(s) for matching rows (single name or list);
  by name it is `primary_key := 'id'` or `primary_keys := ['id', 'sku']`

As for `diff_joindiff`, every parameter can also be passed by name, and a named
parameter overrides the positional value of the same parameter.
`compare_columns` and `include_all` do not exist on `diff_hashdiff`; passing
them by name is a binder error.

**Note:** The `bisection_threshold` and `bisection_factor` parameters of the
hash/bisection algorithm are **not implemented**. Passing them, positionally or
by name, raises a binder error instead of being silently ignored.

**Example:**
```sql
SELECT * FROM diff_hashdiff('source_tbl', 'target_tbl', ['id']);
```

---

## Data Profiling

Generate per-column statistics, pairwise correlations, and table-wide quality summaries.
Full support for complex types (LIST, MAP, STRUCT, UNION).

### Functions

#### `anofox_tab_profile_table` (alias: `profile_table`)

Per-column statistical profile of a table — one output row per column.

**Signature:**
```sql
anofox_tab_profile_table(
    table_name VARCHAR
    [, cols LIST<VARCHAR>]
    [, sample_size BIGINT]
    [, exact BOOLEAN]
) → TABLE
```

**Parameters:**
- `table_name`: Table or view name
- `cols`: Optional column filter (default: all columns)
- `sample_size`: Maximum rows to scan (default: 1,000,000)
- `exact`: Disable sampling even for large tables (default: `false`)

**Output Schema (27 columns):**

| Column | Type | Description |
|--------|------|-------------|
| `column_name` | VARCHAR | Column name |
| `column_type` | VARCHAR | DuckDB column type |
| `row_count` | BIGINT | Total rows in sample |
| `null_count` | BIGINT | Number of NULL values |
| `null_rate` | DOUBLE | Fraction of NULLs |
| `distinct_count` | BIGINT | Number of distinct non-NULL values |
| `distinct_rate` | DOUBLE | Distinct count / row count |
| `min_val` | VARCHAR | Minimum value as string (NULL for STRUCT/MAP/UNION) |
| `max_val` | VARCHAR | Maximum value as string (NULL for STRUCT/MAP/UNION) |
| `mean` | DOUBLE | Mean (numeric columns only) |
| `median` | DOUBLE | Median (numeric columns only) |
| `stddev` | DOUBLE | Standard deviation (numeric columns only) |
| `p25` | DOUBLE | 25th percentile (numeric columns only) |
| `p75` | DOUBLE | 75th percentile (numeric columns only) |
| `skewness` | DOUBLE | Skewness (numeric columns only) |
| `kurtosis` | DOUBLE | Kurtosis (numeric columns only) |
| `top_values` | LIST(STRUCT) | Most-frequent values with counts |
| `avg_length` | DOUBLE | Avg string length / list element count / map cardinality |
| `min_length` | BIGINT | Min string length / list element count / map cardinality |
| `max_length` | BIGINT | Max string length / list element count / map cardinality |
| `pattern_summary` | VARCHAR | Detected pattern: `email`, `phone`, `uuid`, `url`, `ip_address`, `iso_date`, `numeric_string`, `mixed`, `list`, `map`, `struct`, `union` |
| `is_unique` | BOOLEAN | All non-NULL values are distinct |
| `is_constant` | BOOLEAN | All non-NULL values are the same |
| `zero_count` | BIGINT | Number of zero values (numeric columns only) |
| `negative_count` | BIGINT | Number of negative values (numeric columns only) |
| `is_sampled` | BOOLEAN | True if a sample was taken |
| `actual_sample_size` | BIGINT | Number of rows actually scanned |

**Pattern Detection Priority:** `email` → `phone` → `uuid` → `url` → `ip_address` → `iso_date` → `numeric_string` → `mixed`. A pattern is assigned when ≥ 80% of non-NULL values match. Phone detection uses the libphonenumber validator (`US` region hint).

**Example:**
```sql
-- Profile all columns
SELECT column_name, null_rate, pattern_summary, mean
FROM profile_table('orders');

-- Profile specific columns with a 500K row sample
SELECT * FROM profile_table('orders', ['amount', 'customer_id'], 500000);
```

---

#### `anofox_tab_profile_summary` (alias: `profile_summary`)

Single-row table-wide quality overview.

**Signature:**
```sql
anofox_tab_profile_summary(table_name VARCHAR) → TABLE
```

**Output Schema (11 columns, one row):**

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
| `total_null_rate` | DOUBLE | Fraction of cells that are NULL |
| `duplicate_row_count` | BIGINT | Number of duplicate rows |
| `estimated_memory_bytes` | BIGINT | Estimated in-memory size (row_count × column_count × 8 bytes) |

**Example:**
```sql
SELECT * FROM profile_summary('orders');
```

---

#### `anofox_tab_profile_correlations` (alias: `profile_correlations`)

Pairwise Pearson and Spearman correlations for numeric columns.

**Signature:**
```sql
anofox_tab_profile_correlations(
    table_name VARCHAR
    [, cols LIST<VARCHAR>]
) → TABLE
```

**Output Schema:**

| Column | Type | Description |
|--------|------|-------------|
| `column_a` | VARCHAR | First column name |
| `column_b` | VARCHAR | Second column name |
| `pearson` | DOUBLE | Pearson correlation coefficient (−1 to 1) |
| `spearman` | DOUBLE | Spearman rank correlation coefficient (−1 to 1) |
| `n` | BIGINT | Number of non-NULL row pairs used |

**Example:**
```sql
SELECT column_a, column_b, pearson
FROM profile_correlations('orders')
WHERE abs(pearson) > 0.7;
```

---

## Configuration

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
| `anofox_pii_deep_validation` | BOOLEAN | `false` | Validate detected phone numbers with libphonenumber |
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
The value is session-scoped and snapshotted when a query is bound, so changing it mid-query has no effect on running queries.

### PII and NER Settings

See [PII Configuration Options](#configuration-options) and the
[NER configuration options](#anofox_ner_status).

### Tracing

Tracing is on by default at level `info`, so the CLI prints `[anofox] [info] …` lines when the
extension loads and while functions run. To quiet it:

```sql
SET anofox_tab_trace_level = 'warn';   -- only warnings and errors
SET anofox_tab_trace_enabled = false;  -- or no trace output at all
```

### Telemetry

Anonymous usage telemetry is on by default. It sends the extension load event and aggregated
function call counts (function name, count, median duration) to PostHog EU; never query text,
table or column names, or data. See [TELEMETRY.md](../TELEMETRY.md) for the full list.

**Disable via environment variable (also suppresses the load event):**
```bash
export DATAZOO_DISABLE_TELEMETRY=1
```

**Disable via SQL (after loading the extension):**
```sql
SET anofox_telemetry_enabled = false;
```

### Feedback banner

The first load in an interactive terminal each day prints a short feedback banner. It never
prints when output is piped, in notebooks or in CI. Silence it with `SET datazoo_banner = false;`
or `DATAZOO_NO_BANNER=1`.

### Network use

The core functions run locally. These features use the network:

| Feature | What it fetches | Opt out |
|---|---|---|
| PII name/organization/location detection (NER) | DistilBERT model, ~66 MB, from Hugging Face on first use, into `~/.duckdb/extensions/anofox/ner/` | `SET anofox_ner_auto_download = false` (status becomes `NOT_DOWNLOADED`; pattern recognizers keep working) |
| Address parsing (`postal_load_data()`, or the first `postal_parse_address` / `postal_expand_address` call) | libpostal data, ~500 MB | do not call the postal functions |
| `email_validate` / `email_is_valid` modes `dns` and `smtp` | DNS MX lookup / SMTP connection to the mail host | use mode `regex` (the default) |
| Telemetry | anonymous usage events to PostHog EU; on by default; see [TELEMETRY.md](../TELEMETRY.md) | `SET anofox_telemetry_enabled = false` or env `DATAZOO_DISABLE_TELEMETRY=1` (env also suppresses the load event) |
| Feedback banner | none (local print, interactive terminals, once a day) | `SET datazoo_banner = false` |

---

## Function Coverage Matrix

Counts are canonical function names (each `anofox_tab_*` function also has a short alias).

### Summary Statistics

| Category | Count | Function Types |
|----------|-------|----------------|
| Email Validation | 3 | Scalar (2), Table (1) |
| Address Parsing | 4 | Scalar (3), Table (1) |
| Phone Numbers | 9 | Scalar (8), Table (1) |
| Money & Currency | 17 | Scalar functions |
| VAT Validation | 10 | Scalar functions |
| PII Detection | 24 | Scalar (19), Table (5, including `anofox_ner_status`) |
| Data Quality Checks | 22 | 17 checks, 3 `_events` companions, `run_checks` (table) and `anofox_contract_version` (scalar) |
| Anomaly Detection | 5 | Table functions |
| Data Diffing | 2 | Table functions |
| Data Profiling | 3 | Table functions |
| **Total** | **99** | |

### Function Type Breakdown

| Type | Count | Examples |
|------|-------|----------|
| Scalar Functions | 60 | `anofox_tab_email_is_valid` (alias: `email_is_valid`), `anofox_tab_pii_detect` (alias: `pii_detect`), `anofox_tab_vat_is_valid` (alias: `vat_is_valid`) |
| Table Functions | 39 | `anofox_tab_volume` (alias: `volume`), `anofox_tab_compliance` (alias: `compliance`), `anofox_tab_isolation_forest` (alias: `isolation_forest`), `anofox_tab_outlier_tree` (alias: `outlier_tree`) |

Check the numbers against your build with
`SELECT function_type, count(DISTINCT function_name) FROM duckdb_functions() WHERE function_name LIKE 'anofox%' GROUP BY ALL;`.

### Module Status

| Module | Functions | Status | Dependencies |
|--------|-----------|--------|--------------|
| Email Validation | 3 | Stable | c-ares (optional DNS), OpenSSL (optional SMTP) |
| Address Parsing | 4 | Stable | libpostal (required; the module is not built on Windows) |
| Phone Numbers | 9 | Stable | libphonenumber (embedded) |
| Money & Currency | 17 | Stable | None |
| VAT Validation | 10 | Stable | None |
| PII Detection | 24 | Stable | OpenVINO (for NER), OpenSSL (for hash masking) |
| Data Quality Checks | 22 | Stable | None |
| Anomaly Detection | 5 | Stable | None |
| Data Diffing | 2 | Stable | None |
| Data Profiling | 3 | Stable | None |

---

## Notes

1. **All validation calculations** are performed by native C++ implementations with optional integration to external libraries (libpostal, libphonenumber).

2. **Parameters**: arguments are positional. Only the parameters listed under
   [Parameter Conventions](#parameter-conventions) also accept the named form (`:=`).

3. **NULL handling**:
   - Missing values in input will cause functions to return NULL
   - Table functions handle NULLs explicitly in their output

4. **Performance**:
   - Scalar functions: Vectorized execution
   - Table functions: SQL-based implementation
   - Automatic parallelization across CPU cores where applicable

5. **External dependencies**:
   - **libpostal**: Required for address parsing. Its data (~500MB) is downloaded on the first postal call, or up front with `postal_load_data()`.
   - **c-ares**: Optional for DNS email validation
   - **OpenSSL**: Optional for SMTP email validation
   - **libphonenumber**: Embedded implementation, no external dependencies

6. **Money struct format**: All money functions use `STRUCT(amount DECIMAL(18,3), currency VARCHAR)` format. Currency codes must match supported currencies (USD, EUR, GBP, JPY, CAD, AUD, CHF, CNY, INR, BRL).

7. **VAT country codes**: Use the VAT prefixes of the 28 supported countries (27 EU member states plus `GB`, with `XI` for Northern Ireland and `EL` for Greece). `UK` is not accepted.

8. **Anomaly detection parameters**:
   - Isolation Forest: Higher `n_trees` and `sample_size` improve accuracy but increase computation time
   - DBSCAN: `eps` and `min_pts` should be tuned based on data density
   - Use `'summary'` output mode for large tables to reduce memory usage

9. **Data diffing**:
   - `anofox_tab_diff_joindiff` (alias `diff_joindiff`): detailed row-level changes
   - `anofox_tab_diff_hashdiff` (alias `diff_hashdiff`): currently identical to
     `diff_joindiff`; the bisection parameters are not implemented and rejected
   - Both functions support compound primary keys and match keys NULL-safely

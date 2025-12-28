# Anofox Tabular Extension - API Reference

**Version:** 0.1.0  
**DuckDB Version:** ≥ v1.4.2

---

## Overview

The Anofox Tabular extension provides comprehensive data quality validation, anomaly detection, and data diffing capabilities directly within DuckDB. All validation and analysis computations are performed by native C++ implementations with optional integration to external libraries.

### Key Features

- **9 Production-Ready Modules**: Email, postal, phone, money, VAT, PII, metrics, anomalies, and diffing
- **62 SQL Functions**: Complete validation and analysis toolkit
- **Zero Friction**: SQL-native with no external services required (except optional libpostal for address parsing)
- **Blazing Fast**: Vectorized C++17 implementation processes millions of rows per second
- **Self-Contained**: Embedded validation patterns; no API keys or network calls required

### Function Naming Conventions

Functions follow consistent naming patterns:
- `anofox_tab_*` prefix for all extension functions (with aliases without prefix)
- `anofox_tab_*_is_valid` suffix for validation functions
- `anofox_tab_*_format` suffix for formatting functions
- `anofox_tab_metric_*` prefix for data quality metrics
- `anofox_tab_diff_*` prefix for data diffing operations

All functions have aliases without the `anofox_tab_` prefix. For example:
- `anofox_tab_email_is_valid` can also be called as `email_is_valid`
- `anofox_tab_vat` can also be called as `vat`

### Parameter Conventions

**Important**: All functions use **positional parameters**, NOT named parameters (`:=` syntax).

**Common Parameter Types**:
- `email`: Email address - `VARCHAR`
- `address`: Street address - `VARCHAR`
- `number`: Phone number - `VARCHAR`
- `region`: ISO region code - `VARCHAR`
- `money`: Money struct - `STRUCT(amount DOUBLE, currency VARCHAR)`
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
8. [Anomaly Detection](#anomaly-detection)
9. [Data Diffing](#data-diffing)
10. [Configuration](#configuration)
11. [Function Coverage Matrix](#function-coverage-matrix)
12. [Notes](#notes)

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
- `format`: Format style - `'E164'`, `'INTERNATIONAL'`, `'NATIONAL'`, or `'RFC3966'`

**Returns:**
- `VARCHAR`: Formatted phone number

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

#### `anofox_tab_phonenumber_is_possible`

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

#### `anofox_tab_phonenumber_is_valid_for_region`

Region-specific validation.

**Signature:**
```sql
anofox_tab_phonenumber_is_valid_for_region(number VARCHAR, region VARCHAR) → BOOLEAN
```

**Returns:**
- `BOOLEAN`: `true` if phone number is valid for the specified region

---

#### `anofox_tab_phonenumber_match`

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

#### `anofox_tab_phonenumber_example`

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

#### `anofox_tab_phonenumber_status`

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

### Supported Currencies

10 major currencies: USD, EUR, GBP, JPY, CAD, AUD, CHF, CNY, INR, BRL

### Basic Operations

#### `anofox_tab_money (alias: money`

Create a money value from amount and currency code.

**Signature:**
```sql
anofox_tab_money (alias: money(amount DOUBLE, currency_code VARCHAR) → STRUCT(amount DOUBLE, currency VARCHAR)
```

**Example:**
```sql
SELECT anofox_tab_money (alias: money(100.50, 'USD');
-- Returns: {amount: 100.5, currency: 'USD'}
```

---

#### `anofox_tab_money (alias: money_from_cents`

Create a money value from integer cents.

**Signature:**
```sql
anofox_tab_money (alias: money_from_cents(cents BIGINT, currency_code VARCHAR) → STRUCT(amount DOUBLE, currency VARCHAR)
```

**Example:**
```sql
SELECT anofox_tab_money (alias: money_from_cents(10050, 'USD');
-- Returns: {amount: 10050.0, currency: 'USD'}
```

---

#### `anofox_tab_money (alias: money_amount`

Extract amount from money struct.

**Signature:**
```sql
anofox_tab_money (alias: money_amount(money STRUCT) → DOUBLE
```

**Example:**
```sql
SELECT anofox_tab_money (alias: money_amount(anofox_tab_money (alias: money(100.50, 'USD'));
-- Returns: 100.5
```

---

#### `anofox_tab_money (alias: money_currency`

Extract currency code from money struct.

**Signature:**
```sql
anofox_tab_money (alias: money_currency(money STRUCT) → VARCHAR
```

**Example:**
```sql
SELECT anofox_tab_money (alias: money_currency(anofox_tab_money (alias: money(100.50, 'USD'));
-- Returns: 'USD'
```

---

### Currency Information

#### `anofox_tab_is_valid_currency (alias: is_valid_currency`

Check if currency code is valid.

**Signature:**
```sql
anofox_tab_is_valid_currency (alias: is_valid_currency(code VARCHAR) → BOOLEAN
```

**Example:**
```sql
SELECT anofox_tab_is_valid_currency (alias: is_valid_currency('USD');
-- Returns: true
```

---

#### `anofox_tab_currency_ (alias: currency_symbol`

Get currency symbol (e.g., '$', '€').

**Signature:**
```sql
anofox_tab_currency_ (alias: currency_symbol(code VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT anofox_tab_currency_ (alias: currency_symbol('USD');
-- Returns: '$'
```

---

#### `anofox_tab_currency_ (alias: currency_name`

Get currency name (e.g., 'United States Dollar').

**Signature:**
```sql
anofox_tab_currency_ (alias: currency_name(code VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT anofox_tab_currency_ (alias: currency_name('USD');
-- Returns: 'United States Dollar'
```

---

### Formatting

#### `anofox_tab_money (alias: money_format`

Format money for display.

**Signature:**
```sql
anofox_tab_money (alias: money_format(money STRUCT, style VARCHAR) → VARCHAR
```

**Parameters:**
- `style`: Format style - `'symbol'`, `'code'`, or `'long'`

**Example:**
```sql
SELECT anofox_tab_money (alias: money_format(anofox_tab_money (alias: money(150.00, 'EUR'), 'symbol');
-- Returns: '150,00 €'
```

---

### Validation & Properties

#### `anofox_tab_money (alias: money_is_positive`

Check if amount > 0.

**Signature:**
```sql
anofox_tab_money (alias: money_is_positive(money STRUCT) → BOOLEAN
```

---

#### `anofox_tab_money (alias: money_is_negative`

Check if amount < 0.

**Signature:**
```sql
anofox_tab_money (alias: money_is_negative(money STRUCT) → BOOLEAN
```

---

#### `anofox_tab_money (alias: money_is_zero`

Check if amount == 0.

**Signature:**
```sql
anofox_tab_money (alias: money_is_zero(money STRUCT) → BOOLEAN
```

---

#### `anofox_tab_money (alias: money_abs`

Get absolute value (sign removed).

**Signature:**
```sql
anofox_tab_money (alias: money_abs(money STRUCT) → STRUCT(amount DOUBLE, currency VARCHAR)
```

---

### Arithmetic Operations

#### `anofox_tab_money (alias: money_add`

Add two money values (same currency required).

**Signature:**
```sql
anofox_tab_money (alias: money_add(money1 STRUCT, money2 STRUCT) → STRUCT(amount DOUBLE, currency VARCHAR)
```

**Example:**
```sql
SELECT anofox_tab_money (alias: money_add(
    anofox_tab_money (alias: money(100.00, 'EUR'),
    anofox_tab_money (alias: money(50.00, 'EUR')
);
-- Returns: {amount: 150.0, currency: 'EUR'}
```

---

#### `anofox_tab_money (alias: money_subtract`

Subtract money2 from money1 (same currency required).

**Signature:**
```sql
anofox_tab_money (alias: money_subtract(money1 STRUCT, money2 STRUCT) → STRUCT(amount DOUBLE, currency VARCHAR)
```

---

#### `anofox_tab_money (alias: money_multiply`

Multiply money by a scalar factor.

**Signature:**
```sql
anofox_tab_money (alias: money_multiply(money STRUCT, factor DOUBLE) → STRUCT(amount DOUBLE, currency VARCHAR)
```

---

### Quality & Data Validation

#### `anofox_tab_money (alias: money_in_range`

Check if amount is within range.

**Signature:**
```sql
anofox_tab_money (alias: money_in_range(money STRUCT, min DOUBLE, max DOUBLE) → BOOLEAN
```

**Example:**
```sql
SELECT anofox_tab_money (alias: money_in_range(anofox_tab_money (alias: money(100.00, 'USD'), 0.01, 99999.99);
-- Returns: true
```

---

#### `anofox_tab_money (alias: money_same_currency`

Check if two money values have same currency.

**Signature:**
```sql
anofox_tab_money (alias: money_same_currency(money1 STRUCT, money2 STRUCT) → BOOLEAN
```

---

## VAT Validation

European VAT number validation for regulatory compliance and data quality.

### Supported Countries

29 countries supported (28 EU + UK): AT, BE, BG, CY, CZ, DE, DK, EE, EL, ES, FI, FR, HR, HU, IE, IT, LT, LU, LV, MT, NL, PL, PT, RO, SE, SI, SK, UK

### Basic Operations

#### `anofox_tab_vat (alias: vat`

Parse VAT string into country and digits.

**Signature:**
```sql
anofox_tab_vat (alias: vat(vat_string VARCHAR) → STRUCT(country VARCHAR, digits VARCHAR)
```

**Example:**
```sql
SELECT anofox_tab_vat (alias: vat('DE123456789');
-- Returns: {country: 'DE', digits: '123456789'}
```

---

#### `anofox_tab_is_valid_vat_country (alias: is_valid_vat_country`

Check if country code is valid VAT country.

**Signature:**
```sql
anofox_tab_is_valid_vat_country (alias: is_valid_vat_country(code VARCHAR) → BOOLEAN
```

**Example:**
```sql
SELECT anofox_tab_is_valid_vat_country (alias: is_valid_vat_country('DE');
-- Returns: true
```

---

#### `anofox_tab_vat (alias: vat_normalize`

Normalize VAT string (uppercase, remove punctuation).

**Signature:**
```sql
anofox_tab_vat (alias: vat_normalize(vat_string VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT anofox_tab_vat (alias: vat_normalize('de 123-456-789');
-- Returns: 'DE123456789'
```

---

### Syntax Validation

#### `anofox_tab_vat (alias: vat_is_valid_syntax`

Validate VAT syntax against country pattern.

**Signature:**
```sql
anofox_tab_vat (alias: vat_is_valid_syntax(vat_string VARCHAR) → BOOLEAN
```

---

#### `anofox_tab_vat (alias: vat_split`

Parse VAT into country and normalized digits.

**Signature:**
```sql
anofox_tab_vat (alias: vat_split(vat_string VARCHAR) → STRUCT(country VARCHAR, digits VARCHAR)
```

---

#### `anofox_tab_vat (alias: vat_exists`

Check if VAT has valid country prefix.

**Signature:**
```sql
anofox_tab_vat (alias: vat_exists(vat_string VARCHAR) → BOOLEAN
```

---

### EU Utilities

#### `anofox_tab_vat (alias: vat_is_eu_member`

Check if country is EU member.

**Signature:**
```sql
anofox_tab_vat (alias: vat_is_eu_member(country_code VARCHAR) → BOOLEAN
```

---

#### `anofox_tab_vat (alias: vat_country_name`

Get full country name.

**Signature:**
```sql
anofox_tab_vat (alias: vat_country_name(country_code VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT anofox_tab_vat (alias: vat_country_name('DE');
-- Returns: 'Germany'
```

---

#### `anofox_tab_vat (alias: vat_format`

Format VAT for display.

**Signature:**
```sql
anofox_tab_vat (alias: vat_format(vat_string VARCHAR, style VARCHAR) → VARCHAR
```

---

### Combined Validation

#### `anofox_tab_vat (alias: vat_is_valid`

Full validation (syntax + country check).

**Signature:**
```sql
anofox_tab_vat (alias: vat_is_valid(vat_string VARCHAR) → BOOLEAN
```

**Example:**
```sql
SELECT anofox_tab_vat (alias: vat_is_valid('DE123456789');
-- Returns: true
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
- For comprehensive phone parsing, use `anofox_phonenumber_parse()`

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
- Model downloads automatically from HuggingFace on first use (~66 MB quantized ONNX)
- Use `SELECT * FROM anofox_ner_status()` to check NER availability
- Confidence threshold: 0.7 (70%) for entity acceptance
- NAME has dictionary fallback if NER unavailable; ORGANIZATION, LOCATION, MISC require NER
- Entity types:
  - NAME: Person names (PER entities) - e.g., "John Smith"
  - ORGANIZATION: Company/org names (ORG entities) - e.g., "Microsoft"
  - LOCATION: Geographic locations (LOC entities) - e.g., "Paris"
  - MISC: Miscellaneous entities (events, languages) - e.g., "French", "Nobel Prize"
- Case sensitive: all-caps text may produce fragmented or no results

### Masking Strategies

| Strategy | Description | Example Output |
|----------|-------------|----------------|
| `redact` | Replace with type label | `[EMAIL]`, `[US_SSN]` |
| `partial` | Show partial value | `te**@example.com`, `***-**-6789` |
| `asterisk` | Replace with asterisks (same length) | `****************` |
| `hash` | Replace with SHA-256 hash (truncated) | `a1b2c3d4e5f6...` |

### Functions

#### `anofox_tab_pii_detect` (alias: `pii_detect`)

Detect all PII in input text and return matches as JSON.

**Signature:**
```sql
anofox_tab_pii_detect(text VARCHAR) → VARCHAR
```

**Returns:**
- `VARCHAR`: JSON array of detected PII matches with type, position, and confidence

**Example:**
```sql
SELECT pii_detect('Contact: john.doe@example.com, SSN: 123-45-6789');
-- Returns: [{"type":"EMAIL","text":"john.doe@example.com","start":9,"end":29,"confidence":1.00},
--           {"type":"US_SSN","text":"123-45-6789","start":36,"end":47,"confidence":1.00}]

-- NER-based entity detection example:
SELECT pii_detect('John Smith from Microsoft visited Paris to study French');
-- Returns: [{"type":"NAME","text":"John Smith","start":0,"end":10,"confidence":1.00},
--           {"type":"ORGANIZATION","text":"Microsoft","start":16,"end":25,"confidence":1.00},
--           {"type":"LOCATION","text":"Paris","start":34,"end":39,"confidence":1.00},
--           {"type":"MISC","text":"French","start":49,"end":55,"confidence":1.00}]
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
- `strategy`: Masking strategy - `'redact'` (default), `'partial'`, `'asterisk'`, or `'hash'`

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
- `model_status` (VARCHAR): NOT_LOADED, DOWNLOADING, LOADED, FAILED, or NOT_AVAILABLE
- `model_path` (VARCHAR): Path to ONNX model file (or "N/A")
- `model_size_mb` (DOUBLE): Model file size in MB (0 if not loaded)
- `status_message` (VARCHAR): Human-readable status description

**Example:**
```sql
SELECT * FROM anofox_ner_status();
-- onnx_available | model_status  | model_path                                          | model_size_mb | status_message
-- true           | LOADED        | /home/user/.duckdb/extensions/anofox/ner/model.onnx | 66.5          | Model loaded successfully
```

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
pii_audit_table(table_name VARCHAR [, columns VARCHAR])
  → TABLE(row_id, column_name, pii_type, original_value, masked_value, start_pos, end_pos, confidence)
```

**Parameters:**
- `table_name`: Name of table to audit
- `columns`: Optional comma-separated column names (audits all VARCHAR if omitted)

**Returns:**
- `row_id` (BIGINT): Row number in the table
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

Type-specific masking for a single PII type.

**Signature:**
```sql
pii_mask_column(value VARCHAR, pii_type VARCHAR, strategy VARCHAR) → VARCHAR
```

**Example:**
```sql
SELECT pii_mask_column('123-45-6789', 'US_SSN', 'partial');
-- Returns: ***-**-6789
```

---

##### `anofox_tab_pii_redact_column` (alias: `pii_redact_column`)

Mask all PII in text with optional strategy.

**Signature:**
```sql
pii_redact_column(value VARCHAR [, strategy VARCHAR]) → VARCHAR
```

**Example:**
```sql
SELECT pii_redact_column('Email: test@example.com');
-- Returns: Email: [EMAIL]
```

---

#### `anofox_tab_pii_config` (alias: `pii_config`)

Display current PII configuration settings.

**Signature:**
```sql
pii_config() → TABLE(option, value, description)
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
| `anofox_pii_default_mask_strategy` | VARCHAR | 'redact' | Default masking strategy |
| `anofox_pii_enabled_types` | VARCHAR | '' | Comma-separated list of enabled PII types (empty = all) |
| `anofox_pii_deep_validation` | BOOLEAN | false | Enable deep email/phone validation |

**Example:**
```sql
SET anofox_pii_min_confidence = 0.7;
SET anofox_pii_default_mask_strategy = 'partial';
```

---

## Data Quality Metrics

Track essential data quality dimensions.

### Functions

#### `anofox_tab_metric_ (alias: metric_volume`

Validate row count against thresholds.

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_volume(table_name VARCHAR [, min_rows BIGINT, max_rows BIGINT]) → TABLE
```

**Returns:**
- `TABLE`: Row count validation results

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_volume('orders', 1000, 1000000);
```

---

#### `anofox_tab_metric_ (alias: metric_null_rate`

Check null percentage in a column.

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_null_rate(table_name VARCHAR, column_name VARCHAR [, max_null_rate DOUBLE]) → TABLE
```

**Returns:**
- `TABLE`: Null rate validation results

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_null_rate('users', 'email', 0.05);
```

---

#### `anofox_tab_metric_ (alias: metric_distinct_count`

Validate cardinality (distinct count) of a column.

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_distinct_count(table_name VARCHAR, column_name VARCHAR [, min BIGINT, max BIGINT]) → TABLE
```

**Returns:**
- `TABLE`: Distinct count validation results

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_distinct_count('products', 'sku', 100, NULL);
```

---

#### `anofox_tab_metric_ (alias: metric_schema`

Check required columns exist in table.

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_schema(table_name VARCHAR, required_cols LIST<VARCHAR>) → TABLE
```

**Returns:**
- `TABLE`: Schema validation results

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_schema('table', ['id', 'created_at']);
```

---

#### `anofox_tab_metric_ (alias: metric_freshness`

Validate data recency (maximum age check).

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_freshness(table_name VARCHAR, ts_col VARCHAR, max_age INTERVAL [, ref_time TIMESTAMP]) → TABLE
```

**Returns:**
- `TABLE`: Freshness validation results

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_freshness('events', 'timestamp', INTERVAL '1 hour');
```

---

#### `anofox_tab_metric_ (alias: metric_zscore`

Detect outliers via z-score method (assumes normal distribution).

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_zscore(table_name VARCHAR, column_name VARCHAR [, threshold DOUBLE]) → TABLE
```

**Parameters:**
- `threshold`: Z-score threshold (default: 3.0)

**Returns:**
- `TABLE`: Outlier detection results with z-scores

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_zscore('transactions', 'amount', 3.0);
```

---

#### `anofox_tab_metric_ (alias: metric_iqr`

Detect outliers via IQR method (non-parametric, robust to distribution).

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_iqr(table_name VARCHAR, column_name VARCHAR [, multiplier DOUBLE]) → TABLE
```

**Parameters:**
- `multiplier`: IQR multiplier (default: 1.5)

**Returns:**
- `TABLE`: Outlier detection results with IQR scores

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_iqr('transactions', 'amount', 1.5);
```

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

#### `anofox_tab_metric_isolation_forest` (alias: `metric_isolation_forest`)

Univariate Isolation Forest for single column anomaly detection.

**Full Signature:**
```sql
metric_isolation_forest(
    table_name VARCHAR,
    column_name VARCHAR,
    n_trees BIGINT,           -- 1-500, default 100
    sample_size BIGINT,       -- 1-10000, default 256
    contamination DOUBLE,     -- 0.0-0.5, default 0.1
    output_mode VARCHAR,      -- 'summary' or 'scores'
    ndim BIGINT,              -- 1-N, default 1 (Extended IF)
    coef_type VARCHAR,        -- 'uniform' or 'normal'
    scoring_metric VARCHAR,   -- 'depth', 'density', or 'adj_depth'
    weight_column VARCHAR,    -- Column for sample weights (NULL = uniform)
    ntry BIGINT,              -- 1-100, default 1 (SCiForest)
    prob_pick_avg_gain DOUBLE -- 0.0-1.0, default 0.0
) → TABLE
```

**Parameters:**
| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `n_trees` | 1-500 | 100 | Number of isolation trees |
| `sample_size` | 1-10000 | 256 | Subsample size per tree |
| `contamination` | 0.0-0.5 | 0.1 | Expected anomaly fraction |
| `output_mode` | - | 'scores' | `'summary'` or `'scores'` |
| `ndim` | 1-N | 1 | Hyperplane dimensions (Extended IF) |
| `coef_type` | - | 'uniform' | `'uniform'` or `'normal'` |
| `scoring_metric` | - | 'depth' | `'depth'`, `'density'`, `'adj_depth'` |
| `weight_column` | - | NULL | Sample weight column name |
| `ntry` | 1-100 | 1 | Split candidates (SCiForest) |
| `prob_pick_avg_gain` | 0.0-1.0 | 0.0 | Gain-based selection probability |

**Returns:**
- `TABLE`: Anomaly detection results with `row_id`, `anomaly_score`, `is_anomaly`

**Examples:**
```sql
-- Basic usage (backward compatible)
SELECT * FROM metric_isolation_forest(
    'sales_data', 'amount', 100, 256, 0.1, 'scores'
) WHERE is_anomaly = true;

-- Extended IF with hyperplane splits
SELECT * FROM metric_isolation_forest(
    'transactions', 'amount', 100, 256, 0.1, 'scores',
    3,          -- ndim: 3-dimensional hyperplanes
    'normal',   -- coef_type: normal distribution
    'depth'     -- scoring_metric
);

-- SCiForest with gain-based selection
SELECT * FROM metric_isolation_forest(
    'events', 'value', 100, 256, 0.05, 'scores',
    1, 'uniform', 'depth',
    NULL,       -- weight_column
    10,         -- ntry: evaluate 10 candidates
    0.5         -- prob_pick_avg_gain: 50% gain-based
);
```

---

#### `anofox_tab_metric_isolation_forest_multivariate` (alias: `metric_isolation_forest_multivariate`)

Multivariate Isolation Forest for multiple column anomaly detection.

**Full Signature:**
```sql
metric_isolation_forest_multivariate(
    table_name VARCHAR,
    columns VARCHAR,          -- Comma-separated column names
    n_trees BIGINT,
    sample_size BIGINT,
    contamination DOUBLE,
    output_mode VARCHAR,
    ndim BIGINT,
    coef_type VARCHAR,
    scoring_metric VARCHAR,
    weight_column VARCHAR,
    ntry BIGINT,
    prob_pick_avg_gain DOUBLE
) → TABLE
```

**Parameters:**
- `columns`: Comma-separated column names (e.g., `'amount, quantity, suspicious_amount'`)
- All other parameters same as univariate version

**Returns:**
- `TABLE`: Anomaly detection results with `row_id`, `anomaly_score`, `is_anomaly`

**Examples:**
```sql
-- Basic multivariate
SELECT * FROM metric_isolation_forest_multivariate(
    'customer_events', 'purchase_amount, session_duration, page_views',
    100, 256, 0.1, 'scores'
) ORDER BY anomaly_score DESC LIMIT 10;

-- Extended IF with density scoring
SELECT * FROM metric_isolation_forest_multivariate(
    'transactions', 'amount, quantity, duration',
    100, 256, 0.05, 'scores',
    3,          -- ndim
    'normal',   -- coef_type
    'density'   -- scoring_metric: density-based scoring
);
```

---

### DBSCAN Clustering

#### `anofox_tab_metric_ (alias: metric_dbscan`

Univariate DBSCAN clustering for single column anomaly detection.

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_dbscan(
    table_name VARCHAR,
    column_name VARCHAR,
    eps DOUBLE,
    min_pts BIGINT,
    output_mode VARCHAR
) → TABLE
```

**Parameters:**
- `eps`: Neighborhood radius (default: 0.5)
- `min_pts`: Minimum points for dense region (default: 5)
- `output_mode`: `'summary'` (aggregate stats) or `'clusters'` (per-row results)

**Returns:**
- `TABLE`: Clustering results with `row_id`, `cluster_id`, `point_type`, `anomaly_score`
- `point_type`: `'CORE'` (dense region), `'BORDER'` (cluster edge), or `'NOISE'` (outlier)

**Example:**
```sql
SELECT * FROM anofox_tab_metric_ (alias: metric_dbscan(
    'transactions', 'amount', 10.0, 5, 'clusters'
) WHERE point_type = 'NOISE';
```

---

#### `anofox_tab_metric_ (alias: metric_dbscan_multivariate`

Multivariate DBSCAN clustering for multiple column anomaly detection.

**Signature:**
```sql
anofox_tab_metric_ (alias: metric_dbscan_multivariate(
    table_name VARCHAR,
    columns VARCHAR,
    eps DOUBLE,
    min_pts BIGINT,
    output_mode VARCHAR
) → TABLE
```

**Parameters:**
- `columns`: Comma-separated column names

**Returns:**
- `TABLE`: Clustering results with `row_id`, `cluster_id`, `point_type`, `anomaly_score`

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
| `column_name` | VARCHAR | Column with outlier |
| `outlier_value` | VARCHAR | The anomalous value |
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

#### `anofox_tab_diff_ (alias: diff_hashdiff`

Fast hash-based summary diff (efficient for large tables).

**Signature:**
```sql
anofox_tab_diff_ (alias: diff_hashdiff(
    source VARCHAR,
    target VARCHAR,
    pk_cols LIST<VARCHAR>
    [, compare_cols LIST<VARCHAR>]
) → TABLE
```

**Parameters:**
- `source`: Source table name
- `target`: Target table name
- `pk_cols`: Primary key columns for matching rows
- `compare_cols`: Optional list of columns to compare (default: all columns)

**Returns:**
```sql
TABLE(
    added BIGINT,
    removed BIGINT,
    changed BIGINT,
    unchanged BIGINT
)
```

**Example:**
```sql
SELECT * FROM anofox_tab_diff_ (alias: diff_hashdiff('source_tbl', 'target_tbl', ['id']);
-- Returns: {added: 150, removed: 25, changed: 300, unchanged: 10000}
```

---

#### `anofox_tab_diff_ (alias: diff_joindiff`

Detailed row-level diff with source/target data (slower but comprehensive).

**Signature:**
```sql
anofox_tab_diff_ (alias: diff_joindiff(
    source VARCHAR,
    target VARCHAR,
    pk_cols LIST<VARCHAR>
    [, compare_cols LIST<VARCHAR>]
) → TABLE
```

**Returns:**
```sql
TABLE(
    diff_type VARCHAR,      -- 'added', 'removed', 'changed', 'unchanged'
    row_id BIGINT,          -- Row identifier
    source_* VARCHAR,       -- Source column values (prefixed with 'source_')
    target_* VARCHAR        -- Target column values (prefixed with 'target_')
)
```

**Example:**
```sql
SELECT * FROM anofox_tab_diff_ (alias: diff_joindiff('source_tbl', 'target_tbl', ['user_id', 'date'])
WHERE diff_type IN ('added', 'changed')
LIMIT 100;
```

---

## Configuration

Set options via SQL or DuckDB's configuration file.

### Email Settings

```sql
SET anofox_tab_email_default_validation = 'regex';  -- Default: regex
SET anofox_tab_email_regex_pattern = '<your-pattern>';  -- RFC 5322 inspired
SET anofox_tab_email_dns_timeout_ms = 1000;  -- DNS timeout per try (1-5000ms)
SET anofox_tab_email_dns_tries = 1;  -- DNS retry count
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
SELECT anofox_tab_postal_load_data();
-- or using alias:
SELECT postal_load_data();
```

### Phone Settings

```sql
SET anofox_tab_phonenumber_default_region = 'US';  -- Default region code
```

### Tracing

```sql
SET anofox_tab_trace_enabled = true;  -- Enable/disable logging
SET anofox_tab_trace_level = 'info';  -- trace|debug|info|warn|error|critical|off
```

### Telemetry

Anonymous usage telemetry helps improve the extension. No personal data or query content is collected.

**What is collected:**
- Extension load events (extension name, version, platform)
- Function execution counts (function name only, no arguments or data)

**Disable via environment variable (before loading extension):**
```bash
export DATAZOO_DISABLE_TELEMETRY=1
```

**Disable via SQL (after loading extension):**
```sql
SET anofox_telemetry_enabled = false;
```

**Configure API key (advanced):**
```sql
SET anofox_telemetry_key = 'your_custom_key';
```

---

## Function Coverage Matrix

### Summary Statistics

| Category | Count | Function Types |
|----------|-------|----------------|
| Email Validation | 3 | Scalar (2), Table (1) |
| Address Parsing | 4 | Scalar (2), Table (2) |
| Phone Numbers | 9 | Scalar (8), Table (1) |
| Money & Currency | 17 | Scalar functions |
| VAT Validation | 10 | Scalar functions |
| PII Detection | 20 | 15 scalar + 5 table functions |
| Data Quality Metrics | 8 | Table functions |
| Anomaly Detection | 5 | Table functions |
| Data Diffing | 2 | Table functions |
| **Total** | **78** | |

### Function Type Breakdown

| Type | Count | Examples |
|------|-------|----------|
| Scalar Functions | 60 | `anofox_tab_email_is_valid` (alias: `email_is_valid`), `anofox_tab_pii_detect` (alias: `pii_detect`), `anofox_tab_vat_is_valid` (alias: `vat_is_valid`) |
| Table Functions | 18 | `anofox_tab_metric_volume` (alias: `volume`), `anofox_tab_metric_isolation_forest` (alias: `isolation_forest`), `anofox_tab_outlier_tree` (alias: `outlier_tree`) |

### Module Status

| Module | Functions | Status | Dependencies |
|--------|-----------|--------|--------------|
| Email Validation | 3 | Stable | c-ares (optional DNS), OpenSSL (optional SMTP) |
| Address Parsing | 4 | Stable | libpostal (required) |
| Phone Numbers | 9 | Stable | libphonenumber (embedded) |
| Money & Currency | 17 | Stable | None |
| VAT Validation | 10 | Stable | None |
| PII Detection | 20 | Stable | OpenVINO (for NER), OpenSSL (for hash masking) |
| Data Quality Metrics | 8 | Stable | None |
| Anomaly Detection | 5 | Stable | None |
| Data Diffing | 2 | Stable | None |

---

## Notes

1. **All validation calculations** are performed by native C++ implementations with optional integration to external libraries (libpostal, libphonenumber).

2. **Positional parameters only**: Functions do NOT support named parameters (`:=` syntax). Parameters must be provided in the order specified.

3. **NULL handling**:
   - Missing values in input will cause functions to return NULL
   - Table functions handle NULLs explicitly in their output

4. **Performance**:
   - Scalar functions: Vectorized execution, optimized for large datasets
   - Table functions: SQL-based implementation, efficient for large tables
   - Automatic parallelization across CPU cores where applicable

5. **External dependencies**:
   - **libpostal**: Required for address parsing. Data (~500MB) is downloaded automatically on first use via `anofox_tab_postal_load_data()`.
   - **c-ares**: Optional for DNS email validation
   - **OpenSSL**: Optional for SMTP email validation
   - **libphonenumber**: Embedded implementation, no external dependencies

6. **Money struct format**: All money functions use `STRUCT(amount DOUBLE, currency VARCHAR)` format. Currency codes must match supported currencies (USD, EUR, GBP, JPY, CAD, AUD, CHF, CNY, INR, BRL).

7. **VAT country codes**: Use ISO 3166-1 alpha-2 country codes (e.g., 'DE', 'FR', 'GB'). The extension supports 29 countries (28 EU + UK).

8. **Anomaly detection parameters**:
   - Isolation Forest: Higher `n_trees` and `sample_size` improve accuracy but increase computation time
   - DBSCAN: `eps` and `min_pts` should be tuned based on data density
   - Use `'summary'` output mode for large tables to reduce memory usage

9. **Data diffing**:
   - `anofox_tab_diff_ (alias: diff_hashdiff`: Fast, O(n) complexity, returns summary statistics only
   - `anofox_tab_diff_ (alias: diff_joindiff`: Slower, O(n log n) complexity, returns detailed row-level changes
   - Both functions support compound primary keys

---

**Last Updated:** 2025-01-XX  
**API Version:** 0.1.0


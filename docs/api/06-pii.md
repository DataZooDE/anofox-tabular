# PII Detection and Masking

> Find, validate and mask personal data such as emails, card numbers, IBANs and names in text.

**Use this page to:**
- Detect PII in a text value and get its type, position and confidence
- Mask PII with a redact, partial, asterisk or hash strategy
- Scan whole tables for PII columns and audit single rows
- Check card numbers, IBANs, SSNs and other identifiers by checksum
- Check whether the NER model for names, organizations and locations is available

Pattern-based detection runs locally. Detection of `NAME`, `ORGANIZATION`, `LOCATION` and `MISC`
uses a NER model that is downloaded from Hugging Face on first use (see [NER model](#ner-model)
and [Network and privacy](../reference/network-and-privacy.md)).

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`pii_detect`](#pii_detect) | `anofox_tab_pii_detect` | scalar | list of matches |
| [`pii_detect_batch`](#pii_detect_batch) | `anofox_tab_pii_detect_batch` | scalar | one list of matches per list element |
| [`pii_detect_emails`](#type-specific-detection) | `anofox_tab_pii_detect_emails` | scalar | `EMAIL` matches |
| [`pii_detect_phones`](#type-specific-detection) | `anofox_tab_pii_detect_phones` | scalar | `PHONE` matches |
| [`pii_detect_credit_cards`](#type-specific-detection) | `anofox_tab_pii_detect_credit_cards` | scalar | `CREDIT_CARD` matches |
| [`pii_detect_ssns`](#type-specific-detection) | `anofox_tab_pii_detect_ssns` | scalar | `US_SSN` matches |
| [`pii_detect_ibans`](#type-specific-detection) | `anofox_tab_pii_detect_ibans` | scalar | `IBAN` matches |
| [`pii_detect_names`](#type-specific-detection) | `anofox_tab_pii_detect_names` | scalar | `NAME` matches |
| [`pii_mask`](#pii_mask) | `anofox_tab_pii_mask` | scalar | text with all PII masked |
| [`pii_mask_column`](#pii_mask_column) | `anofox_tab_pii_mask_column` | scalar | text with one PII type masked |
| [`pii_redact_column`](#pii_redact_column) | `anofox_tab_pii_redact_column` | scalar | text with all PII masked |
| [`pii_contains`](#pii_contains) | `anofox_tab_pii_contains` | scalar | `BOOLEAN` |
| [`pii_count`](#pii_count) | `anofox_tab_pii_count` | scalar | `BIGINT` |
| [`pii_is_valid_ssn`](#validation-functions) | `anofox_tab_pii_is_valid_ssn` | scalar | `BOOLEAN` |
| [`pii_is_valid_iban`](#validation-functions) | `anofox_tab_pii_is_valid_iban` | scalar | `BOOLEAN` |
| [`pii_is_valid_credit_card`](#validation-functions) | `anofox_tab_pii_is_valid_credit_card` | scalar | `BOOLEAN` |
| [`pii_is_valid_nino`](#validation-functions) | `anofox_tab_pii_is_valid_nino` | scalar | `BOOLEAN` |
| [`pii_is_valid_de_tax_id`](#validation-functions) | `anofox_tab_pii_is_valid_de_tax_id` | scalar | `BOOLEAN` |
| [`pii_is_valid_crypto_address`](#validation-functions) | `anofox_tab_pii_is_valid_crypto_address` | scalar | `BOOLEAN` |
| [`pii_scan_table`](#pii_scan_table) | `anofox_tab_pii_scan_table` | table | one row per column and PII type |
| [`pii_audit_table`](#pii_audit_table) | `anofox_tab_pii_audit_table` | table | one row per match |
| [`pii_status`](#pii_status) | `anofox_tab_pii_status` | table | the registered recognizers |
| [`pii_config`](#pii_config) | `anofox_tab_pii_config` | table | the current PII settings |
| [`anofox_ner_status`](#anofox_ner_status) | `anofox_ner_status` (no alias) | table | the state of the NER model |

## PII types

`pii_status()` lists the types. Detection uses patterns plus a checksum where one exists. The four
NER types need the NER model.

| Type | Detects | Method |
|------|---------|--------|
| `EMAIL` | Email addresses | pattern |
| `PHONE` | Phone numbers with a `+` prefix, parentheses or dashes | pattern |
| `CREDIT_CARD` | Visa, Mastercard, Amex and Discover numbers | pattern + Luhn checksum |
| `US_SSN` | US Social Security Numbers `XXX-XX-XXXX` | pattern + area validation |
| `IBAN` | International Bank Account Numbers | pattern + MOD-97 checksum |
| `IP_ADDRESS` | IPv4 and IPv6 addresses | pattern |
| `URL` | HTTP and HTTPS URLs | pattern |
| `MAC_ADDRESS` | MAC addresses (colon, hyphen, dot or no separator) | pattern |
| `UK_NINO` | UK National Insurance Numbers | pattern |
| `US_PASSPORT` | 9 digits, or a letter and 8 digits | pattern |
| `DE_TAX_ID` | German tax ID (Steueridentifikationsnummer, 11 digits) | pattern + checksum |
| `API_KEY` | AWS and GitHub tokens, high-entropy strings | pattern + entropy |
| `CRYPTO_ADDRESS` | Bitcoin (P2PKH, P2SH, SegWit) and Ethereum addresses | pattern + checksum where available |
| `NAME` | Person names | NER; a name dictionary when the model is not available |
| `ORGANIZATION` | Organization names | NER only |
| `LOCATION` | Places | NER only |
| `MISC` | Other named entities | NER only |

`US_PASSPORT` matches any 9-digit sequence and has no checksum, so it can also match invoice or
order numbers.

## Masking strategies

The strategy argument is case-insensitive.

| Strategy | Effect |
|----------|--------|
| `redact` (default) | Replace each match with its type label, e.g. `[EMAIL]` |
| `partial` | Keep a few characters, e.g. `***-**-6789` |
| `asterisk` | Replace every character with `*` |
| `hash` | Replace with the first 8 hex characters of a SHA-256 hash |
| `none` | Leave the text unchanged |

`pii_mask(text)` without a strategy always redacts. The setting `anofox_pii_default_mask_strategy`
applies only to `pii_redact_column(value)` and `pii_audit_table`. A strategy argument that
is not one of these names is an error (`Invalid mask strategy: …`), in `pii_mask`,
`pii_mask_column`, `pii_redact_column` and in `SET anofox_pii_default_mask_strategy`.

```sql
SELECT s AS strategy, pii_mask('SSN: 123-45-6789, mail a.b@example.com', s) AS masked
FROM (VALUES ('redact'), ('partial'), ('asterisk'), ('hash'), ('none')) t(s);
```

```text
┌──────────┬────────────────────────────────────────┐
│ strategy │                 masked                 │
│ varchar  │                varchar                 │
├──────────┼────────────────────────────────────────┤
│ redact   │ SSN: [US_SSN], mail [EMAIL]            │
│ partial  │ SSN: ***-**-6789, mail a.*@example.com │
│ asterisk │ SSN: ***********, mail *************** │
│ hash     │ SSN: 01a54629, mail a6f1bad1           │
│ none     │ SSN: 123-45-6789, mail a.b@example.com │
└──────────┴────────────────────────────────────────┘
```

## Match structure

The detection functions return a list of structs:

```text
STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | `VARCHAR` | The PII type, e.g. `EMAIL` |
| `text` | `VARCHAR` | The matched text |
| `start_pos` | `BIGINT` | Start offset in the input, 0-based |
| `end_pos` | `BIGINT` | End offset in the input, exclusive |
| `confidence` | `DOUBLE` | `1.0` for pattern matches; the model score for NER matches |

---

## `pii_detect`

Canonical name: `anofox_tab_pii_detect`. Scalar function.

Returns every PII match in the text. Types disabled by `anofox_pii_enabled_types` are skipped.

```text
pii_detect(text VARCHAR) -> STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `text` | `VARCHAR` | required | The text to scan |

```sql
SELECT m.type, m.text, m.start_pos, m.end_pos, m.confidence
FROM (SELECT unnest(pii_detect('Contact john.doe@example.com, SSN 123-45-6789')) AS m);
```

```text
┌─────────┬──────────────────────┬───────────┬─────────┬────────────┐
│  type   │         text         │ start_pos │ end_pos │ confidence │
│ varchar │       varchar        │   int64   │  int64  │   double   │
├─────────┼──────────────────────┼───────────┼─────────┼────────────┤
│ EMAIL   │ john.doe@example.com │         8 │      28 │        1.0 │
│ US_SSN  │ 123-45-6789          │        34 │      45 │        1.0 │
└─────────┴──────────────────────┴───────────┴─────────┴────────────┘
```

With the NER model available, names, organizations and places are found too. The confidence
of a NER match is the model score, so it is below `1.0`:

```sql
SELECT m.type, m.text, round(m.confidence, 3) AS confidence
FROM (SELECT unnest(pii_detect('John Smith from Microsoft visited Paris')) AS m);
```

```text
┌──────────────┬────────────┬────────────┐
│     type     │    text    │ confidence │
│   varchar    │  varchar   │   double   │
├──────────────┼────────────┼────────────┤
│ NAME         │ John Smith │      0.999 │
│ ORGANIZATION │ Microsoft  │      0.999 │
│ LOCATION     │ Paris      │      0.998 │
└──────────────┴────────────┴────────────┘
```

---

## `pii_detect_batch`

Canonical name: `anofox_tab_pii_detect_batch`. Scalar function.

Runs `pii_detect` on every element of a list and returns one match list per element. A `NULL`
element gives `NULL`.

```text
pii_detect_batch(texts VARCHAR[]) -> STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[][]
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `texts` | `VARCHAR[]` | required | The texts to scan |

```sql
SELECT len(r) AS elements, [len(x) FOR x IN r] AS matches_per_element
FROM (SELECT pii_detect_batch(['a@b.com', 'no pii here', 'SSN 123-45-6789 and c@d.org']) AS r);
```

```text
┌──────────┬─────────────────────┐
│ elements │ matches_per_element │
│  int64   │       int64[]       │
├──────────┼─────────────────────┤
│        3 │ [1, 0, 2]           │
└──────────┴─────────────────────┘
```

---

## Type-specific detection

Each function returns the matches of one type, with the same structure as `pii_detect`.

| Function | Canonical name | Type |
|----------|----------------|------|
| `pii_detect_emails` | `anofox_tab_pii_detect_emails` | `EMAIL` |
| `pii_detect_phones` | `anofox_tab_pii_detect_phones` | `PHONE` |
| `pii_detect_credit_cards` | `anofox_tab_pii_detect_credit_cards` | `CREDIT_CARD` |
| `pii_detect_ssns` | `anofox_tab_pii_detect_ssns` | `US_SSN` |
| `pii_detect_ibans` | `anofox_tab_pii_detect_ibans` | `IBAN` |
| `pii_detect_names` | `anofox_tab_pii_detect_names` | `NAME` (NER model, or the name dictionary without it) |

```text
pii_detect_<type>(text VARCHAR) -> STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
```

```sql
SELECT [m.text FOR m IN pii_detect_emails(t)]       AS emails,
       [m.text FOR m IN pii_detect_phones(t)]       AS phones,
       [m.text FOR m IN pii_detect_credit_cards(t)] AS cards,
       [m.text FOR m IN pii_detect_ssns(t)]         AS ssns,
       [m.text FOR m IN pii_detect_ibans(t)]        AS ibans
FROM (VALUES ('a@b.com, +1 (415) 555-1234, 4111 1111 1111 1111, 123-45-6789, DE89370400440532013000')) v(t);
```

```text
┌───────────┬───────────────────────┬───────────────────────┬───────────────┬──────────────────────────┐
│  emails   │        phones         │         cards         │     ssns      │          ibans           │
│ varchar[] │       varchar[]       │       varchar[]       │   varchar[]   │        varchar[]         │
├───────────┼───────────────────────┼───────────────────────┼───────────────┼──────────────────────────┤
│ [a@b.com] │ ['+1 (415) 555-1234'] │ [4111 1111 1111 1111] │ [123-45-6789] │ [DE89370400440532013000] │
└───────────┴───────────────────────┴───────────────────────┴───────────────┴──────────────────────────┘
```

```sql
SELECT [m.text FOR m IN pii_detect_names('Angela Merkel met Barack Obama')] AS names;
```

```text
┌───────────────────────────────┐
│             names             │
│           varchar[]           │
├───────────────────────────────┤
│ [Angela Merkel, Barack Obama] │
└───────────────────────────────┘
```

---

## `pii_mask`

Canonical name: `anofox_tab_pii_mask`. Scalar function.

Replaces every PII match in the text according to the strategy.

```text
pii_mask(text VARCHAR [, strategy VARCHAR]) -> VARCHAR
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `text` | `VARCHAR` | required | The text to mask |
| `strategy` | `VARCHAR` | `redact` (the setting `anofox_pii_default_mask_strategy` does not apply) | See [Masking strategies](#masking-strategies). `NULL` returns `NULL`. |

```sql
SELECT pii_mask('Email: test@example.com')            AS redacted,
       pii_mask('SSN: 123-45-6789', 'partial')        AS partial,
       pii_mask('Card: 4111111111111111', 'asterisk') AS asterisk;
```

```text
┌────────────────┬──────────────────┬────────────────────────┐
│    redacted    │     partial      │        asterisk        │
│    varchar     │     varchar      │        varchar         │
├────────────────┼──────────────────┼────────────────────────┤
│ Email: [EMAIL] │ SSN: ***-**-6789 │ Card: **************** │
└────────────────┴──────────────────┴────────────────────────┘
```

---

## `pii_mask_column`

Canonical name: `anofox_tab_pii_mask_column`. Scalar function.

Masks only the matches of one PII type. Other PII in the text stays as it is. An unknown type
raises an error.

```text
pii_mask_column(value VARCHAR, pii_type VARCHAR, strategy VARCHAR) -> VARCHAR
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `value` | `VARCHAR` | required | The text to mask |
| `pii_type` | `VARCHAR` | required | A type from [PII types](#pii-types), e.g. `US_SSN` |
| `strategy` | `VARCHAR` | required | See [Masking strategies](#masking-strategies) |

The type and strategy can differ from row to row, for example when they come from columns. A
`NULL` argument returns `NULL` for that row.

```sql
SELECT pii_mask_column('SSN 123-45-6789, mail a@b.com', 'US_SSN', 'partial') AS masked;
```

```text
┌───────────────────────────────┐
│            masked             │
│            varchar            │
├───────────────────────────────┤
│ SSN ***-**-6789, mail a@b.com │
└───────────────────────────────┘
```

---

## `pii_redact_column`

Canonical name: `anofox_tab_pii_redact_column`. Scalar function.

Masks all PII in the text, like `pii_mask`.

```text
pii_redact_column(value VARCHAR [, strategy VARCHAR]) -> VARCHAR
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `value` | `VARCHAR` | required | The text to mask |
| `strategy` | `VARCHAR` | `anofox_pii_default_mask_strategy` (`redact`) | See [Masking strategies](#masking-strategies). It can differ from row to row; `NULL` returns `NULL` |

```sql
SELECT pii_redact_column('SSN 123-45-6789, mail a@b.com')         AS redacted,
       pii_redact_column('SSN 123-45-6789, mail a@b.com', 'hash') AS hashed;
```

```text
┌────────────────────────────┬─────────────────────────────┐
│          redacted          │           hashed            │
│          varchar           │           varchar           │
├────────────────────────────┼─────────────────────────────┤
│ SSN [US_SSN], mail [EMAIL] │ SSN 01a54629, mail fb98d44a │
└────────────────────────────┴─────────────────────────────┘
```

---

## `pii_contains`

Canonical name: `anofox_tab_pii_contains`. Scalar function.

Returns `true` when the text contains at least one match.

```text
pii_contains(text VARCHAR) -> BOOLEAN
```

```sql
SELECT pii_contains('Contact us at support@company.com') AS with_email,
       pii_contains('Hello, world!') AS without;
```

```text
┌────────────┬─────────┐
│ with_email │ without │
│  boolean   │ boolean │
├────────────┼─────────┤
│ true       │ false   │
└────────────┴─────────┘
```

---

## `pii_count`

Canonical name: `anofox_tab_pii_count`. Scalar function.

Returns the number of matches in the text.

```text
pii_count(text VARCHAR) -> BIGINT
```

```sql
SELECT pii_count('Email: a@b.com, SSN: 123-45-6789, Card: 4111111111111111') AS matches;
```

```text
┌─────────┐
│ matches │
│  int64  │
├─────────┤
│       3 │
└─────────┘
```

---

## Validation functions

These functions check a single identifier, not free text. They return `true` when the value has
the right format and, where one exists, a valid checksum.

| Function | Canonical name | Checks |
|----------|----------------|--------|
| `pii_is_valid_ssn` | `anofox_tab_pii_is_valid_ssn` | US SSN format and area number |
| `pii_is_valid_iban` | `anofox_tab_pii_is_valid_iban` | IBAN MOD-97 checksum |
| `pii_is_valid_credit_card` | `anofox_tab_pii_is_valid_credit_card` | Luhn checksum |
| `pii_is_valid_nino` | `anofox_tab_pii_is_valid_nino` | UK National Insurance Number format |
| `pii_is_valid_de_tax_id` | `anofox_tab_pii_is_valid_de_tax_id` | German tax ID: 11 digits and check digit |
| `pii_is_valid_crypto_address` | `anofox_tab_pii_is_valid_crypto_address` | Bitcoin (Base58 checksum for legacy and P2SH) or Ethereum (hex format) |

```text
pii_is_valid_ssn(ssn VARCHAR) -> BOOLEAN
pii_is_valid_iban(iban VARCHAR) -> BOOLEAN
pii_is_valid_credit_card(card_number VARCHAR) -> BOOLEAN
pii_is_valid_nino(nino VARCHAR) -> BOOLEAN
pii_is_valid_de_tax_id(tax_id VARCHAR) -> BOOLEAN
pii_is_valid_crypto_address(address VARCHAR) -> BOOLEAN
```

```sql
SELECT pii_is_valid_credit_card('4111111111111111') AS card_ok,
       pii_is_valid_credit_card('4111111111111112') AS card_bad,
       pii_is_valid_iban('DE89370400440532013000')  AS iban_ok,
       pii_is_valid_iban('DE89370400440532013001')  AS iban_bad,
       pii_is_valid_ssn('000-12-3456')              AS ssn_area_000;
```

```text
┌─────────┬──────────┬─────────┬──────────┬──────────────┐
│ card_ok │ card_bad │ iban_ok │ iban_bad │ ssn_area_000 │
│ boolean │ boolean  │ boolean │ boolean  │   boolean    │
├─────────┼──────────┼─────────┼──────────┼──────────────┤
│ true    │ false    │ true    │ false    │ false        │
└─────────┴──────────┴─────────┴──────────┴──────────────┘
```

```sql
SELECT pii_is_valid_nino('AB123456C')                                  AS nino,
       pii_is_valid_de_tax_id('65929970489')                           AS de_tax_id,
       pii_is_valid_crypto_address('1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa') AS bitcoin;
```

```text
┌─────────┬───────────┬─────────┐
│  nino   │ de_tax_id │ bitcoin │
│ boolean │  boolean  │ boolean │
├─────────┼───────────┼─────────┤
│ true    │ true      │ true    │
└─────────┴───────────┴─────────┘
```

---

## `pii_scan_table`

Canonical name: `anofox_tab_pii_scan_table`. Table function.

Scans the `VARCHAR` columns of a table and returns one row per column and PII type found.
It accepts regular tables and views, not TEMP tables (see
[Conventions](00-conventions.md#table-and-column-arguments)).

```text
pii_scan_table(table_name VARCHAR [, columns VARCHAR]) -> TABLE(
    column_name   VARCHAR,
    pii_type      VARCHAR,
    match_count   BIGINT,
    sample_values VARCHAR[],
    confidence    DOUBLE
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | The table to scan |
| `columns` | `VARCHAR` | all `VARCHAR` columns (`JSON` included) | Comma-separated column names. Names match case-insensitively; an unknown name is an error. A named column of another type (`BIGINT`, `STRUCT`, `LIST`, ...) is cast to `VARCHAR` and scanned |

| Column | Type | Description |
|--------|------|-------------|
| `column_name` | `VARCHAR` | The column with PII |
| `pii_type` | `VARCHAR` | The PII type found |
| `match_count` | `BIGINT` | Number of matches |
| `sample_values` | `VARCHAR[]` | Up to 5 matched values |
| `confidence` | `DOUBLE` | Average confidence of the matches |

The example tables of this page:

```sql
CREATE TABLE customers AS
SELECT * FROM (VALUES
    (101, 'anna@example.com', '123-45-6789', '4111 1111 1111 1111', 'Call me on +1 (415) 555-1234'),
    (102, 'bob@example.org',  NULL,          NULL,                  'No contact details here'),
    (103, 'not an email',     '987-65-4320', NULL,                  'Wire to DE89370400440532013000')
) t(customer_id, email, ssn, card, note);
```

```sql
SELECT column_name, pii_type, match_count, sample_values
FROM pii_scan_table('customers')
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬─────────────┬─────────────┬─────────────────────────────────────┐
│ column_name │  pii_type   │ match_count │            sample_values            │
│   varchar   │   varchar   │    int64    │              varchar[]              │
├─────────────┼─────────────┼─────────────┼─────────────────────────────────────┤
│ card        │ CREDIT_CARD │           1 │ [4111 1111 1111 1111]               │
│ email       │ EMAIL       │           2 │ [anna@example.com, bob@example.org] │
│ note        │ IBAN        │           1 │ [DE89370400440532013000]            │
│ note        │ PHONE       │           1 │ ['+1 (415) 555-1234']               │
│ ssn         │ US_SSN      │           1 │ [123-45-6789]                       │
└─────────────┴─────────────┴─────────────┴─────────────────────────────────────┘
```

Scan only some columns:

```sql
SELECT column_name, pii_type, match_count
FROM pii_scan_table('customers', 'email,note')
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬──────────┬─────────────┐
│ column_name │ pii_type │ match_count │
│   varchar   │ varchar  │    int64    │
├─────────────┼──────────┼─────────────┤
│ email       │ EMAIL    │           2 │
│ note        │ IBAN     │           1 │
│ note        │ PHONE    │           1 │
└─────────────┴──────────┴─────────────┘
```

---

## `pii_audit_table`

Canonical name: `anofox_tab_pii_audit_table`. Table function.

Returns one row per PII match, with the original and the masked value. Like `pii_scan_table` it
accepts regular tables and views, not TEMP tables.

```text
pii_audit_table(table_name VARCHAR [, columns VARCHAR] [, id_column := VARCHAR]) -> TABLE(
    row_id         BIGINT,
    [<id_column>,]
    column_name    VARCHAR,
    pii_type       VARCHAR,
    original_value VARCHAR,
    masked_value   VARCHAR,
    start_pos      BIGINT,
    end_pos        BIGINT,
    confidence     DOUBLE
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | The table to audit |
| `columns` | `VARCHAR` | all `VARCHAR` columns (`JSON` included) | Comma-separated column names. Names match case-insensitively; an unknown name is an error. A named column of another type (`BIGINT`, `STRUCT`, `LIST`, ...) is cast to `VARCHAR` and scanned |
| `id_column` | `VARCHAR`, named only | none | A column whose value is added after `row_id`, under its own name and type |

| Column | Type | Description |
|--------|------|-------------|
| `row_id` | `BIGINT` | 1-based row number in the table |
| `<id_column>` | the column's type | Only with `id_column`: the row's identifier |
| `column_name` | `VARCHAR` | The column with the match |
| `pii_type` | `VARCHAR` | The PII type |
| `original_value` | `VARCHAR` | The full cell value |
| `masked_value` | `VARCHAR` | The cell value masked with the default strategy |
| `start_pos`, `end_pos` | `BIGINT` | Position of the match in the cell |
| `confidence` | `DOUBLE` | Match confidence |

```sql
SELECT row_id, column_name, pii_type, original_value, masked_value, start_pos, end_pos
FROM pii_audit_table('customers', 'ssn')
ORDER BY row_id;
```

```text
┌────────┬─────────────┬──────────┬────────────────┬──────────────┬───────────┬─────────┐
│ row_id │ column_name │ pii_type │ original_value │ masked_value │ start_pos │ end_pos │
│ int64  │   varchar   │ varchar  │    varchar     │   varchar    │   int64   │  int64  │
├────────┼─────────────┼──────────┼────────────────┼──────────────┼───────────┼─────────┤
│      1 │ ssn         │ US_SSN   │ 123-45-6789    │ [US_SSN]     │         0 │      11 │
└────────┴─────────────┴──────────┴────────────────┴──────────────┴───────────┴─────────┘
```

With `id_column`, every finding carries the customer key, so it joins back to the table:

```sql
SELECT customer_id, column_name, pii_type, masked_value
FROM pii_audit_table('customers', id_column := 'customer_id')
ORDER BY customer_id, column_name;
```

```text
┌─────────────┬─────────────┬─────────────┬────────────────────┐
│ customer_id │ column_name │  pii_type   │    masked_value    │
│    int32    │   varchar   │   varchar   │      varchar       │
├─────────────┼─────────────┼─────────────┼────────────────────┤
│         101 │ card        │ CREDIT_CARD │ [CREDIT_CARD]      │
│         101 │ email       │ EMAIL       │ [EMAIL]            │
│         101 │ note        │ PHONE       │ Call me on [PHONE] │
│         101 │ ssn         │ US_SSN      │ [US_SSN]           │
│         102 │ email       │ EMAIL       │ [EMAIL]            │
│         103 │ note        │ IBAN        │ Wire to [IBAN]     │
└─────────────┴─────────────┴─────────────┴────────────────────┘
```

---

## `pii_status`

Canonical name: `anofox_tab_pii_status`. Table function.

Lists the registered recognizers.

```text
pii_status() -> TABLE(pii_type VARCHAR, recognizer_name VARCHAR, enabled BOOLEAN, pattern_info VARCHAR)
```

```sql
SELECT pii_type, recognizer_name, pattern_info FROM pii_status() ORDER BY pii_type;
```

```text
┌────────────────┬─────────────────┬────────────────────────────────────────────────────────┐
│    pii_type    │ recognizer_name │                      pattern_info                      │
│    varchar     │     varchar     │                        varchar                         │
├────────────────┼─────────────────┼────────────────────────────────────────────────────────┤
│ API_KEY        │ API Key         │ AWS/GitHub tokens and high-entropy strings             │
│ CREDIT_CARD    │ Credit Card     │ Visa/MC/Amex/Discover with Luhn validation             │
│ CRYPTO_ADDRESS │ Crypto Address  │ Bitcoin (P2PKH/P2SH/SegWit) and Ethereum               │
│ DE_TAX_ID      │ German Tax ID   │ 11-digit Steueridentifikationsnummer                   │
│ EMAIL          │ Email           │ RFC 5322 email pattern                                 │
│ IBAN           │ IBAN            │ ISO 13616 with MOD-97 checksum                         │
│ IP_ADDRESS     │ IP Address      │ IPv4 and IPv6 formats                                  │
│ LOCATION       │ Unknown         │ No description                                         │
│ MAC_ADDRESS    │ MAC Address     │ Colon/hyphen/dot/no-separator formats                  │
│ MISC           │ Unknown         │ No description                                         │
│ NAME           │ Person Name     │ OpenVINO DistilBERT NER (92% F1)                       │
│ ORGANIZATION   │ Unknown         │ No description                                         │
│ PHONE          │ Phone           │ International phone formats (+XX, parentheses, dashes) │
│ UK_NINO        │ UK NINO         │ UK National Insurance Number format                    │
│ URL            │ URL             │ HTTP/HTTPS URLs                                        │
│ US_PASSPORT    │ US Passport     │ 9 digits or letter + 8 digits                          │
│ US_SSN         │ US SSN          │ XXX-XX-XXXX format with area validation                │
└────────────────┴─────────────────┴────────────────────────────────────────────────────────┘
  17 rows                                                                         3 columns
```

---

## `pii_config`

Canonical name: `anofox_tab_pii_config`. Table function.

Shows the current values of the PII settings.

```text
pii_config() -> TABLE(option_name VARCHAR, option_value VARCHAR, description VARCHAR)
```

```sql
SELECT option_name, option_value FROM pii_config();
```

```text
┌──────────────────────────────────┬──────────────┐
│           option_name            │ option_value │
│             varchar              │   varchar    │
├──────────────────────────────────┼──────────────┤
│ anofox_pii_min_confidence        │ 0.500000     │
│ anofox_pii_default_mask_strategy │ REDACT       │
│ anofox_pii_enabled_types         │ (all types)  │
│ anofox_pii_deep_validation       │ false        │
└──────────────────────────────────┴──────────────┘
```

---

## `anofox_ner_status`

Canonical name: `anofox_ner_status`. Table function. It has no short alias.

Shows whether the NER model is available and loaded.

```text
anofox_ner_status() -> TABLE(
    onnx_available BOOLEAN,
    model_status   VARCHAR,
    model_path     VARCHAR,
    model_size_mb  DOUBLE,
    status_message VARCHAR
)
```

| Column | Type | Description |
|--------|------|-------------|
| `onnx_available` | `BOOLEAN` | `true` when the build includes OpenVINO (the name is kept for compatibility) |
| `model_status` | `VARCHAR` | `NOT_LOADED`, `DOWNLOADING`, `LOADED`, `FAILED`, `NOT_AVAILABLE` (no OpenVINO in this build) or `NOT_DOWNLOADED` (model not on disk and `anofox_ner_auto_download` is false) |
| `model_path` | `VARCHAR` | Path of the model file |
| `model_size_mb` | `DOUBLE` | Size of the model file |
| `status_message` | `VARCHAR` | A readable description of the state |

```sql no-run
-- The result depends on the build and on whether the model was downloaded.
SELECT onnx_available, model_status, status_message FROM anofox_ner_status();
```

On a build with OpenVINO, after the model has been loaded, it reports `true`, `LOADED` and
`Model loaded successfully (distilbert-en, device: CPU)`.

## NER model

`NAME`, `ORGANIZATION`, `LOCATION` and `MISC` are found by a DistilBERT NER model (English,
`distilbert-en`) that runs on OpenVINO.

- **Download.** The first detection that needs the model downloads it (about 66 MB) from
  Hugging Face into `~/.duckdb/extensions/anofox/ner/<model>/`. This applies to `pii_detect`,
  `pii_mask`, `pii_scan_table`, `pii_audit_table` and the other detection functions, unless
  `anofox_pii_enabled_types` lists only pattern-based types.
- **Offline use.** `SET anofox_ner_auto_download = false` turns the download off. NER detection
  is then skipped, the status becomes `NOT_DOWNLOADED`, `NAME` falls back to the name dictionary,
  and the pattern-based types keep working. To install the model by hand, place
  `model_quantized.onnx` and `tokenizer.json` in the model directory.
- **Platforms.** Per `vcpkg.json`, OpenVINO is built only for x64 Linux and x64 macOS, and
  `CMakeLists.txt` disables it on musl. On other builds `onnx_available` is `false` and the status
  is `NOT_AVAILABLE`. Check with `anofox_ner_status()`.
- **Confidence.** NER matches below 0.7 are never reported. `anofox_pii_min_confidence`
  (default 0.5) filters on top of that.
- **Limits.** Input is truncated at 512 tokens, so entities after that are not found. The
  tokenizer is ASCII-focused: non-ASCII punctuation is not a word boundary. All-caps text can give
  fragmented or no results.

## Settings

| Setting | Purpose |
|---------|---------|
| `anofox_pii_enabled_types` | Comma-separated list of types to detect; empty means all. An unknown type is an error. |
| `anofox_pii_default_mask_strategy` | The strategy of `pii_redact_column(value)` and of `masked_value` in `pii_audit_table` |
| `anofox_pii_min_confidence` | Minimum confidence of a match; dictionary name matches have 0.5, pattern matches 1.0 |
| `anofox_pii_deep_validation` | Validate detected phone numbers with the built-in phone module |
| `anofox_ner_auto_download` | Download the NER model on first use |
| `anofox_ner_model` | The NER model (`distilbert-en`); set it before the model is loaded |
| `anofox_ner_device` | OpenVINO device (`AUTO`, `CPU`, `GPU`, ...) |
| `anofox_ner_cache_size` | LRU cache size for NER results; 0 disables it |

Types, defaults and details are in [Configuration: PII](../reference/configuration.md#pii).

```sql
SET anofox_pii_enabled_types = 'EMAIL,US_SSN';
SELECT pii_mask('anna@example.com, 123-45-6789, +1 (415) 555-1234') AS masked;
```

```text
┌──────────────────────────────────────┐
│                masked                │
│               varchar                │
├──────────────────────────────────────┤
│ [EMAIL], [US_SSN], +1 (415) 555-1234 │
└──────────────────────────────────────┘
```

```sql
RESET anofox_pii_enabled_types;
```

## See also

- [Guide: find and mask PII](../guides/04-pii.md)
- [Configuration: PII](../reference/configuration.md#pii)
- [Network and privacy](../reference/network-and-privacy.md)
- [Conventions](00-conventions.md)

---
name: anofox-tabular-pii
description: >
  PII detection, validation and masking for the anofox_tabular DuckDB extension: `pii_detect`,
  `pii_contains`, `pii_count`, `pii_mask`, `pii_mask_column`, `pii_mask_value`,
  `pii_redact_column`, the `pii_is_valid_iban`-style validators, `pii_scan_table`,
  `pii_audit_table`, `pii_status`, `pii_config` and the NER model (`anofox_ner_status`).
  Use when a user wants to find, audit, hash, redact or partially mask personal data
  (emails, phones, IBANs, cards, SSNs, German IDs, names) in DuckDB columns or tables.
version: 2026.11.02
user-invocable: false
---

# Anofox Tabular — PII Detection and Masking Cheat Sheet

**Extension:** `anofox_tabular` v2026.11.02 | **DuckDB:** v2.0.0 / v1.5.6 | **Naming:** short alias (`pii_mask`) = canonical `anofox_tab_pii_mask` (exception: `anofox_ner_status` has no alias)

Find PII in text values and tables, check identifiers by checksum, and mask PII with a redact,
partial, asterisk or hash strategy. Pattern types run locally; `NAME`, `ORGANIZATION`,
`LOCATION`, `MISC` use a NER model downloaded from Hugging Face on first use.

Load with `LOAD anofox_tabular;` (install: `INSTALL anofox_tabular FROM community;`).

## Critical gotchas

1. **`pii_mask(text)` always redacts.** `anofox_pii_default_mask_strategy` applies only to
   `pii_redact_column(value)` (one argument) and to `masked_value` of `pii_audit_table`.
2. **Detection-based masking misses PII.** `pii_mask` / `pii_mask_column` change only what
   detection finds: `Li` (short first name), `01511234567` (compact phone without a `Tel`/`Mobil`
   keyword), lower-case names, surnames the NER reads as ORG/LOC all stay in clear text. For a
   column known to hold one PII kind use `pii_mask_value(col, 'NAME', ...)` (masks the whole value,
   no detection). A re-scan of masked output cannot prove it clean.
3. **`pii_scan_table` / `pii_audit_table` read through a second connection:** regular tables and
   views only, never TEMP tables (`Invalid Input Error: pii_scan_table: 't' is a TEMP table, which
   is not supported ...`); committed data only — inside `BEGIN` with uncommitted changes they fail
   at bind (`Binder Error: pii_audit_table reads the last committed data, ...`). On DuckDB 2.0 that
   error aborts the transaction. Call them after `COMMIT`.
4. **Column lists are ONE comma-separated `VARCHAR`** (`'email, note'`), not a `VARCHAR[]`.
   Names are case-insensitive; an unknown name is a Binder Error listing the available columns.
   `id_column` of `pii_audit_table` is **named only** (`id_column := 'customer_id'`).
5. **Settings are process-wide and read once per statement.** `anofox_pii_enabled_types` limits
   every PII function (incl. scans). Empty = all types except `POSTAL_CODE`; add it with
   `'DEFAULT,POSTAL_CODE'`. An unknown type name fails at `SET` (`Unknown PII type: FOO`).
   `pii_mask_column` / `pii_mask_value` take an explicit type that overrides the setting.
   `RESET` after a run.
6. **Bad option raises, bad value does not.** Unknown strategy → `Invalid mask strategy: ...`;
   unknown type → `Unknown PII type: ...`. Text without PII → empty list / unchanged text.
   `NULL` input or `NULL` strategy/type → `NULL`. Use `TRY()` when strategy/type come from data.
7. **`SET anofox_pii_default_mask_strategy = 'none'` is rejected**; `'none'` is fine as an
   explicit argument. Strategy names are case-insensitive.
8. **Checksums on by default.** `anofox_pii_require_checksum = true` drops mistyped IBANs/cards
   (they stay unmasked). For masking before an export set it `false` (matches at confidence 0.6)
   and keep `anofox_pii_min_confidence` ≤ 0.6. Validators (`pii_is_valid_*`) stay strict.
9. **`hash` is a pseudonym, not anonymisation:** first 16 hex chars of unsalted SHA-256 of the
   normalised value. Phones, SSNs, cards, DOBs, postal codes are enumerable. National phone
   numbers hash via `anofox_tab_phonenumber_default_region` (default `US`) — set it before hashing
   and keep it fixed while joining hashes.
10. **Offsets are UTF-8 bytes** (`start_pos` 0-based, `end_pos` exclusive), not characters.
11. **NER/offline:** first NER-needing detection downloads the model (178 MB `bert-multi`) to
    `~/.duckdb/extensions/anofox/ner/<model>/`. `SET anofox_ner_auto_download = false` → status
    `NOT_DOWNLOADED`, `NAME` falls back to a given-name dictionary (confidence 0.5),
    ORG/LOC/MISC are not found, scan status gets the suffix ` (NER unavailable)`. Without the
    model detection goes on silently unless `SET anofox_ner_required = true`. NER only exists on
    x64 Linux and x64 macOS builds (not musl, not arm64 macOS, not Windows) → `NOT_AVAILABLE`.
12. **Raw PII in results:** `sample_values` (scan), `original_value` / `matched_text` (audit) are
    unmasked. Share only `masked_value` or aggregate counts.
13. **Not detected:** GDPR Art. 9 special categories (health, religion, ...), Swiss AHV, Austrian
    SVNR, bare 9-digit numbers, dates without a birth keyword.

## PII types

| Type | Notes |
|------|-------|
| `EMAIL` | incl. non-ASCII; inside URL queries / after `mailto:` the bare address matches |
| `PHONE` | `+`/`00` international, national with separator (`089/12345678`), US forms; compact national only after a phone keyword |
| `CREDIT_CARD` | brand prefix + length + Luhn; may wrap over one line break |
| `US_SSN` | `XXX-XX-XXXX` / `XXX XX XXXX` + area check |
| `IBAN` | registered country + length + MOD-97; irregular grouping OK |
| `IP_ADDRESS`, `URL`, `MAC_ADDRESS` | URL userinfo (`user:pw@`) is part of the URL match |
| `UK_NINO`, `US_PASSPORT`, `DE_TAX_ID`, `API_KEY`, `CRYPTO_ADDRESS` | API_KEY skips UUIDs and md5/sha hex digests |
| `DATE_OF_BIRTH` | a real date after a birth keyword (`geb.`, `DOB`, `born`, ...) |
| `DE_SVNR`, `DE_KVNR`, `DE_ID_CARD` | check digit; KVNR always needs a keyword, ID card a keyword or check digit |
| `POSTAL_CODE` | **off by default**; DE/AT/CH/NL/UK in address context |
| `NAME` | NER, dictionary fallback |
| `ORGANIZATION`, `LOCATION`, `MISC` | NER only; `MISC` only with `distilbert-en` |

Type-argument aliases (case-insensitive, for `pii_mask_column` / `pii_mask_value` / the setting):
`SSN`, `CC`/`CREDITCARD`, `IP`, `PERSON`/`PERSON_NAME`, `ORG`/`COMPANY`, `LOC`/`PLACE`/`GEO`,
`PASSPORT`, `CRYPTO`, `NINO`, `MAC`, `KEY`/`APIKEY`, `DOB`/`BIRTH_DATE`, `SVNR`/`RVNR`, `KVNR`,
`PERSONALAUSWEIS`, `POSTCODE`/`ZIP`/`PLZ`, `STEUER_ID`/`GERMAN_TAX_ID`.

## Detection

### `pii_detect`, `pii_detect_batch`, type detectors

```text
pii_detect(text VARCHAR) -> STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
pii_detect_batch(texts VARCHAR[]) -> STRUCT(...)[][]          -- one list per element; NULL element -> NULL
pii_detect_emails | pii_detect_phones | pii_detect_credit_cards | pii_detect_ssns
  | pii_detect_ibans | pii_detect_names  (text VARCHAR) -> STRUCT(...)[]   -- one type each
```

`confidence`: 1.0 pattern; 0.6 checksum failure (only with `require_checksum = false`); 0.5
dictionary name; NER model score (≥ 0.7). Disabled types are skipped by `pii_detect`.

```sql
SELECT m.type, m.text, m.start_pos, m.end_pos, m.confidence
FROM (SELECT unnest(pii_detect('Contact john.doe@example.com, SSN 123-45-6789')) AS m);
```

```text
EMAIL   john.doe@example.com   8  28  1.0
US_SSN  123-45-6789           34  45  1.0
```

```sql
SELECT [len(x) FOR x IN pii_detect_batch(['a@b.com', 'no pii here', 'SSN 123-45-6789 and c@d.org'])] AS per_element,
       [m.text FOR m IN pii_detect_ibans('Wire to DE89370400440532013000')] AS ibans,
       [m.text FOR m IN pii_detect_phones('Call +1 (415) 555-1234')]        AS phones;
-- per_element = [1, 0, 2]
```

`pii_detect_names('Angela Merkel met Barack Obama')` returns both names with the NER model;
offline (dictionary fallback) only `Angela Merkel` at confidence 0.5.

### `pii_contains`, `pii_count`

```text
pii_contains(text VARCHAR) -> BOOLEAN
pii_count(text VARCHAR) -> BIGINT
```

```sql
SELECT pii_contains('Contact us at support@company.com') AS has_pii,          -- true
       pii_count('Email: a@b.com, SSN: 123-45-6789, Card: 4111111111111111') AS n;  -- 3
```

## Masking

### Strategies (case-insensitive)

| Strategy | Effect |
|----------|--------|
| `redact` | `[TYPE]` label (default of `pii_mask`) |
| `partial` | type-specific, never more than half of letters/digits (`***-**-6789`) |
| `asterisk` | one `*` per character (characters, not bytes) |
| `hash` | 16 hex chars of SHA-256 of the normalised value |
| `none` | unchanged (explicit argument only) |

Partial rules (selection): EMAIL `j*******@*********.com`; PHONE `+***-***-5678`; CREDIT_CARD
`4111********1111` (first 4 only from 16 digits); US_SSN last 4; IBAN `DE89**************3000`;
IP first octet; URL scheme + TLD, path → `/***`; NAME first char per word (words ≤ 4 chars fully
masked); ORG/LOC up to 3 chars; DATE_OF_BIRTH keeps the year (`**.**.1985`); POSTAL_CODE first 2;
DE_SVNR area; DE_KVNR / DE_ID_CARD / US_PASSPORT first char; other types first/last char(s).

Hash normalisation: PHONE → E.164 via the phone module (invalid numbers: separators removed);
CREDIT_CARD, US_SSN, DE_TAX_ID → spaces `-` `.` `/` `(` `)` removed; IBAN, UK_NINO, DE_SVNR,
DE_KVNR, DE_ID_CARD → same plus upper-case; EMAIL → lower-case; others as matched.

### `pii_mask` / `pii_redact_column`

```text
pii_mask(text VARCHAR [, strategy VARCHAR]) -> VARCHAR            -- default 'redact', setting ignored
pii_redact_column(value VARCHAR [, strategy VARCHAR]) -> VARCHAR  -- default anofox_pii_default_mask_strategy
```

```sql
SELECT s AS strategy, pii_mask('SSN: 123-45-6789, mail a.b@example.com', s) AS masked
FROM (VALUES ('redact'), ('partial'), ('asterisk'), ('hash'), ('none')) t(s);
```

```text
redact   │ SSN: [US_SSN], mail [EMAIL]
partial  │ SSN: ***-**-6789, mail a**@*******.com
asterisk │ SSN: ***********, mail ***************
hash     │ SSN: 15e2b0d3c33891eb, mail a6f1bad172104d25
none     │ SSN: 123-45-6789, mail a.b@example.com
```

```sql
SELECT pii_mask('x@y.com', 'blur');
```

```text
Invalid Input Error: Invalid mask strategy: blur. Valid values: redact, hash, partial, asterisk, none (case-insensitive)
```

### `pii_mask_column` — mask one detected type

```text
pii_mask_column(value VARCHAR, pii_type VARCHAR, strategy VARCHAR) -> VARCHAR
```

All three required; type and strategy may vary per row; `NULL` argument → `NULL`. Other PII in
the text stays. Detection runs only for that type (no NER inference unless the type is a NER type).

```sql
SELECT pii_mask_column('SSN 123-45-6789, mail a@b.com', 'US_SSN', 'partial') AS masked;
-- SSN ***-**-6789, mail a@b.com
```

### `pii_mask_value` — mask the whole value, no detection

```text
pii_mask_value(value VARCHAR, pii_type VARCHAR, strategy VARCHAR) -> VARCHAR
```

Whole value = one match of `pii_type`. `hash` gives the same value `pii_mask` gives a detected
match (joinable). Empty string stays empty; `NULL` → `NULL`.

```sql
SELECT pii_mask_value('Li', 'NAME', 'redact')                       AS first_name,  -- [NAME]
       pii_mask_value('01511234567', 'PHONE', 'partial')            AS phone,       -- ***-***-4567
       pii_mask_value('jane.doe@acme-corp.com', 'EMAIL', 'partial') AS email,       -- j*******@*********.com
       pii_mask_column('01511234567', 'PHONE', 'redact')            AS not_found;   -- unchanged
```

Hash equality across formats (set the phone region first):

```sql
SET anofox_tab_phonenumber_default_region = 'DE';
SELECT phone, pii_mask_value(phone, 'PHONE', 'hash') = left(sha256('+49301234567'), 16) AS same_as_e164
FROM (VALUES ('030 1234567'), ('+49 30 1234567'), ('0049 30 1234567')) t(phone);   -- all true
RESET anofox_tab_phonenumber_default_region;
```

## Validation (single identifiers, not free text)

```text
pii_is_valid_ssn(ssn VARCHAR) -> BOOLEAN                    -- format + area number
pii_is_valid_iban(iban VARCHAR) -> BOOLEAN                  -- registered country, length, MOD-97
pii_is_valid_credit_card(card_number VARCHAR) -> BOOLEAN    -- Luhn
pii_is_valid_nino(nino VARCHAR) -> BOOLEAN                  -- UK NINO format
pii_is_valid_de_tax_id(tax_id VARCHAR) -> BOOLEAN           -- 11 digits + check digit
pii_is_valid_crypto_address(address VARCHAR) -> BOOLEAN     -- BTC Base58Check / bech32, ETH hex
```

```sql
SELECT pii_is_valid_credit_card('4111111111111111') AS card_ok,   -- true
       pii_is_valid_iban('DE89370400440532013001')  AS iban_bad,  -- false
       pii_is_valid_ssn('000-12-3456')              AS ssn_bad,   -- false (area 000)
       pii_is_valid_nino('AB123456C')               AS nino,      -- true
       pii_is_valid_de_tax_id('65929970489')        AS tax_id,    -- true
       pii_is_valid_crypto_address('1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa') AS btc;  -- true
```

## Table functions

Sample table used below (regular table, committed):

```sql
CREATE TABLE customers AS
SELECT * FROM (VALUES
    (101, 'anna@example.com', '123-45-6789', '4111 1111 1111 1111', 'Call me on +1 (415) 555-1234'),
    (102, 'bob@example.org',  NULL,          NULL,                  'No contact details here'),
    (103, 'not an email',     '987-65-4320', NULL,                  'Wire to DE89370400440532013000')
) t(customer_id, email, ssn, card, note);
```

### `pii_scan_table` — which columns hold which PII

```text
pii_scan_table(table_name VARCHAR [, columns VARCHAR]) -> TABLE(
    column_name VARCHAR, pii_type VARCHAR, match_count BIGINT, sample_values VARCHAR[],
    confidence DOUBLE, values_scanned BIGINT, status VARCHAR, checked_types VARCHAR[],
    ner_model VARCHAR)
```

| Param | Default | Meaning |
|-------|---------|---------|
| `table_name` | required | table or view (not TEMP) |
| `columns` | all text columns (`VARCHAR`, `JSON`, nested types containing `VARCHAR`, scanned as their VARCHAR rendering) | comma-separated; positional or `columns := '...'`; a named non-text column is cast to VARCHAR and scanned |

One row per (column, type found) plus one row per column without a match, in table column
order, then by `pii_type`. `pii_type` `NULL` / `match_count` 0 on no-match rows.
`status`: `scanned`, `no_match`, `skipped (<TYPE>)` (non-text column), `error (<msg>)`, and
`scanned (NER unavailable)` / `no_match (NER unavailable)` when NER types are enabled but the
model could not run. `checked_types`: types applied (sorted; ORG/LOC/MISC dropped when NER
unavailable). `ner_model`: model that ran, else `NULL`. `match_count` counts matches, not rows.
`sample_values`: up to 5 raw matched values.

```sql
SELECT column_name, pii_type, match_count, values_scanned, status
FROM pii_scan_table('customers')
WHERE pii_type IS NOT NULL;
-- email EMAIL 2 · ssn US_SSN 1 · card CREDIT_CARD 1 · note IBAN 1 · note PHONE 1
-- status is 'scanned' with the NER model, 'scanned (NER unavailable)' offline
```

`no_match` covers only `checked_types`:

```sql
SET anofox_pii_enabled_types = 'EMAIL';
SELECT column_name, status, checked_types, ner_model
FROM pii_scan_table('customers', columns := 'email, note');
-- email scanned [EMAIL] NULL · note no_match [EMAIL] NULL   (no NER type enabled -> no suffix)
RESET anofox_pii_enabled_types;
```

### `pii_audit_table` — one row per match

```text
pii_audit_table(table_name VARCHAR [, columns VARCHAR] [, id_column := VARCHAR]) -> TABLE(
    row_id BIGINT, [<id_column> <its type>,] column_name VARCHAR, pii_type VARCHAR,
    original_value VARCHAR, masked_value VARCHAR, start_pos BIGINT, end_pos BIGINT,
    confidence DOUBLE, matched_text VARCHAR)
```

`row_id` is the 1-based row number. `masked_value` = whole cell masked with
`anofox_pii_default_mask_strategy` (only detected PII). `original_value`, `matched_text` = raw PII.
Positions are UTF-8 bytes within the cell (or its VARCHAR rendering for nested columns).

```sql
SELECT customer_id, column_name, pii_type, masked_value
FROM pii_audit_table('customers', 'ssn, note', id_column := 'customer_id')
ORDER BY customer_id, column_name;
```

```text
101 │ note │ PHONE  │ Call me on [PHONE]
101 │ ssn  │ US_SSN │ [US_SSN]
103 │ note │ IBAN   │ Wire to [IBAN]
```

TEMP tables are rejected:

```sql
CREATE TEMP TABLE scratch_notes AS SELECT 'mail a@b.com' AS note;
SELECT * FROM pii_audit_table('scratch_notes');
```

```text
Invalid Input Error: pii_audit_table: 'scratch_notes' is a TEMP table, which is not supported: the table is read through a separate connection that cannot see TEMP tables. Copy it into a regular table first (CREATE TABLE ... AS SELECT * FROM scratch_notes)
```

### `pii_status`, `pii_config`

```text
pii_status() -> TABLE(pii_type VARCHAR, recognizer_name VARCHAR, enabled BOOLEAN, pattern_info VARCHAR)  -- 22 rows
pii_config() -> TABLE(option_name VARCHAR, option_value VARCHAR, description VARCHAR)                   -- the 5 anofox_pii_* settings
```

```sql
SELECT pii_type FROM pii_status() WHERE NOT enabled;   -- POSTAL_CODE under the default setting
SELECT option_name, option_value FROM pii_config();
```

### `anofox_ner_status` (no alias)

```text
anofox_ner_status() -> TABLE(onnx_available BOOLEAN, model_status VARCHAR, model_path VARCHAR,
                             model_size_mb DOUBLE, status_message VARCHAR)
```

`onnx_available` = build has OpenVINO (name kept for compatibility). `model_status`:
`NOT_LOADED` (loads/downloads on first NER detection), `DOWNLOADING`, `LOADED`, `FAILED`
(`status_message` says why), `NOT_DOWNLOADED` (not on disk and auto-download off; reported
immediately), `NOT_AVAILABLE` (no OpenVINO in the build).

```sql no-run
-- result depends on the build and on whether the model is on disk
SELECT onnx_available, model_status, status_message FROM anofox_ner_status();
```

## NER model

| `anofox_ner_model` | Languages | Entities | Download |
|--------------------|-----------|----------|----------|
| `bert-multi` (default) | de, en, fr, es, it, nl, pt, lv, ar, zh | PER, ORG, LOC | 178 MB |
| `distilbert-multi` | as bert-multi | PER, ORG, LOC | 135 MB |
| `distilbert-en` | English only | PER, ORG, LOC, MISC | 66 MB |

- Source: Hugging Face → `~/.duckdb/extensions/anofox/ner/<model>/` (`model_quantized.onnx` +
  `tokenizer.json`; place them there by hand for air-gapped installs).
- Set `anofox_ner_model` before the first NER detection; changing it after the model loaded is
  an error until process restart. After `FAILED`/`NOT_DOWNLOADED`, setting `anofox_ner_model` or
  `anofox_ner_device` retries the load.
- Models are cased: lower-case names (`klaus fischer`) are not found. NER < 0.7 never reported.
  Pattern matches win over overlapping NER spans; NER spans widen to whole words; bare ISO
  country/currency codes (`DE`, `EUR`) are dropped.
- Cost: ~20–50 ms per distinct text on CPU (LRU cache `anofox_ner_cache_size`). Values with no
  word, or whose letters all lie inside pattern matches, skip the model. Long texts are windowed
  (256 tokens, 64 overlap). For large scans, restrict `anofox_pii_enabled_types` to pattern
  types so no inference runs.
- **Large tables:** `pii_scan_table` reads every row of every column (no sampling), and
  `pii_audit_table` is a second full pass; with NER, 1M distinct texts take hours. Scan a sample
  *view* first (`CREATE VIEW crm_sample AS SELECT * FROM crm USING SAMPLE 10000;` — a view, not a
  TEMP table, which the scans reject), then audit only the columns the scan flagged.
- Dictionary fallback (`NAME` without model): capitalised known given name plus up to two
  following capitalised words (`Sabine Weiß`), confidence 0.5; a surname alone is not found.

With the model loaded (shown, not run here):

```text
pii_mask('Mit freundlichen Grüßen Jürgen Schäfer, Allianz Versicherungs-AG, München')
-> Mit freundlichen Grüßen [NAME], [ORGANIZATION], [LOCATION]
```

## Settings

| Setting | Type | Default | Meaning |
|---------|------|---------|---------|
| `anofox_pii_enabled_types` | VARCHAR | `''` (all except POSTAL_CODE) | comma-separated types; `DEFAULT` = default set; unknown type errors at SET |
| `anofox_pii_min_confidence` | DOUBLE | `0.5` | drop matches below (0.0–1.0, else error) |
| `anofox_pii_default_mask_strategy` | VARCHAR | `redact` | for `pii_redact_column(value)` and audit `masked_value`; `none` rejected; stored lower-case |
| `anofox_pii_require_checksum` | BOOLEAN | `true` | `false`: report checksum-failing IBAN/card at 0.6 |
| `anofox_pii_deep_validation` | BOOLEAN | `false` | drop phone matches whose length the phone module finds impossible |
| `anofox_ner_auto_download` | BOOLEAN | `true` | `false`: never download; NER skipped (`NOT_DOWNLOADED`) |
| `anofox_ner_model` | VARCHAR | `bert-multi` | `bert-multi`, `distilbert-multi`, `distilbert-en` |
| `anofox_ner_device` | VARCHAR | `AUTO` | `CPU`, `GPU`, `NPU`, `GPU.0`, `HETERO:GPU,CPU`, ...; stored upper-case |
| `anofox_ner_cache_size` | BIGINT | `10000` | LRU entries for NER results; `0` disables |
| `anofox_ner_required` | BOOLEAN | `false` | `true`: NER-needing detection raises when model unavailable |
| `anofox_tab_phonenumber_default_region` | VARCHAR | `US` | region for national phone numbers in hashing / deep validation |

Offline / fully pattern-based run:

```sql
SET anofox_ner_auto_download = false;
SET anofox_pii_enabled_types = 'EMAIL,PHONE,CREDIT_CARD,US_SSN,IBAN,IP_ADDRESS';
SELECT pii_mask('Anna Schmidt, anna@example.com, server 10.0.0.7') AS masked;
-- Anna Schmidt, [EMAIL], server [IP_ADDRESS]
RESET anofox_pii_enabled_types;
```

`anofox_ner_required` fails loudly instead of silently skipping NER:

```sql no-run
-- result depends on whether the model is on disk
SET anofox_ner_auto_download = false;
SET anofox_ner_required = true;
SELECT pii_mask('Ticket from Anna Schmidt');
```

```text
Invalid Input Error: anofox_ner_required is true but the NER model is not available (status NOT_DOWNLOADED: NER model not downloaded: anofox_ner_auto_download is false.
```

(Only when the model is not on disk; detections of pattern-only types are not affected.)

## Canonical workflow — scan, audit, mask for export

```sql
-- 1. data in a regular, committed table
CREATE TABLE crm_contacts AS
SELECT * FROM (VALUES
    (1, 'Li',    'li@example.com',    '01511234567', '4111 1111 1111 1111', 'Refund to DE89 3704 0044 0532 0130 00'),
    (2, 'Carol', 'carol@example.net', '030 1234567', NULL,                  'Server 192.168.1.20 is down'),
    (3, 'Bo',    'not an email',      NULL,          '5500 0000 0000 0004', 'SSN 219-09-9999 on file')
) t(contact_id, first_name, email, phone, card, note);

-- 2. which columns hold PII (keep the full result as the audit record)
CREATE TABLE crm_scan AS
SELECT column_name, pii_type, match_count, values_scanned, status, checked_types, ner_model
FROM pii_scan_table('crm_contacts');

-- 3. findings per row, without raw values
SELECT contact_id, column_name, pii_type, masked_value
FROM pii_audit_table('crm_contacts', 'note, card', id_column := 'contact_id')
ORDER BY contact_id, column_name;
```

```sql
-- 4. export preset: mask mistyped numbers too, explicit settings
SET anofox_pii_require_checksum = false;
SET anofox_pii_min_confidence = 0.5;
SET anofox_tab_phonenumber_default_region = 'DE';

-- 5. known-PII columns: pii_mask_value; free text: pii_mask
CREATE TABLE crm_export AS
SELECT contact_id,
       pii_mask_value(first_name, 'NAME', 'redact')   AS first_name,
       pii_mask_value(email, 'EMAIL', 'partial')      AS email,
       pii_mask_value(phone, 'PHONE', 'hash')         AS phone_pseudonym,
       pii_mask_value(card, 'CREDIT_CARD', 'hash')    AS card_pseudonym,
       pii_mask(note, 'redact')                       AS note
FROM crm_contacts;

-- 6. record the effective settings next to the export, then reset
SELECT current_setting('anofox_pii_require_checksum') AS require_checksum,
       current_setting('anofox_pii_enabled_types')    AS enabled_types,
       current_setting('anofox_ner_model')            AS ner_model;
RESET anofox_pii_require_checksum;
RESET anofox_pii_min_confidence;
RESET anofox_tab_phonenumber_default_region;
```

## Common patterns

```sql
-- Flag rows with any PII in a free-text column
SELECT customer_id, pii_count(note) AS findings FROM customers WHERE pii_contains(note);

-- Per-row type/strategy from a policy table; TRY() guards bad policy entries
SELECT TRY(pii_mask_column(v, t, s)) AS masked
FROM (VALUES ('SSN 123-45-6789', 'ssn', 'partial'), ('a@b.com', 'EMAIL', 'hash'), ('x', 'EMAIL', 'blur')) p(v, t, s);

-- Mask in place (scalar functions work on any table, TEMP included)
UPDATE customers SET note = pii_mask(note, 'redact');

-- Add postal codes to the default types
SET anofox_pii_enabled_types = 'DEFAULT,POSTAL_CODE';
SELECT pii_mask('Musterstraße 12, 80331 München') AS masked;   -- Musterstraße 12, [POSTAL_CODE] München
RESET anofox_pii_enabled_types;

-- Pseudonymous join key: hashes of differently formatted cards match
SELECT pii_mask_value('4111-1111-1111-1111', 'CREDIT_CARD', 'hash')
     = pii_mask_value('4111 1111 1111 1111', 'CREDIT_CARD', 'hash') AS same;   -- true
```

---
name: anofox-tabular-master-data
description: >
  Validating and standardising customer master data with the anofox_tabular DuckDB extension:
  email (`email_is_valid`, `email_validate`, `email_config`), phone numbers (`phonenumber_parse`,
  `phonenumber_format`, `phonenumber_is_valid`, `phonenumber_match`, ...), EU VAT numbers
  (`vat_is_valid`, `vat_validate`, `vat_format`, ...), postal addresses (`postal_parse_address`,
  `postal_expand_address`) and money/currency (`money`, `money_from_cents`, `money_format`, ...).
  Use when the user wants to validate, clean, normalise, format or deduplicate emails, phones,
  VAT IDs, addresses, amounts or currency codes in a DuckDB table.
version: 2026.11.13
user-invocable: false
---

# Anofox Tabular — Customer Master Data Cheat Sheet

**Extension:** `anofox_tabular` v2026.11.13 | **DuckDB:** v2.0.0 / v1.5.6 | **Naming:** short alias (`email_is_valid`) = canonical `anofox_tab_email_is_valid`

Scalar functions that validate, parse and format contact and commercial fields row by row, plus
a few status/config table functions. Load with `LOAD anofox_tabular;` (install once:
`INSTALL anofox_tabular FROM community;`).

## Critical gotchas

- **All arguments are positional.** No `name := value` on any function of these modules.
  Optional args only on `email_is_valid` / `email_validate` (`mode`).
- **`region` is required on every phone function** except `phonenumber_example`: pass `NULL`
  for the default region. `phonenumber_region(phone)` with one argument is a binder error.
- **Pass each row's country as the phone `region`.** A local number read in the default region
  (`US` unless `SET anofox_tab_phonenumber_default_region`) fails with reason `region_missing`.
  Numbers starting with `+` (or the region's IDD prefix, `00`/`011`) carry their own country.
  `region` is ISO alpha-2 (`'DE'`; `'UK'` = `'GB'`); `'DEU'`, `'XX'`, names → unsupported.
- **Three-valued phone validity.** Unknown region / unassigned country code (`+999`) → validity
  functions return `NULL` (reason `unsupported_region` / `unsupported_country_code`), not
  `false`. Count `NULL`s separately; `= false` only finds known-bad numbers.
- **Email `dns`/`smtp` use the network and are three-valued**: `NULL` = unknown (timeout,
  blocked port 25, greylisting), `false` = definite failure. `regex` (default) is offline and
  never `NULL` for non-NULL input. `WHERE email_is_valid(e, 'dns')` drops the unknown rows too.
- **Blank is not NULL.** `''` / whitespace-only → `false` with reason `empty`
  (email, phone, VAT). `NULL` input → `NULL` everywhere (struct-returning functions: `NULL` struct).
- **Route on `valid`, not `reason`.** `reason` is `NULL` both for a valid value and for `NULL` input.
- **Bad value vs. bad option.** Bad data → `false` / `NULL` / `reason`, never an error. A typo'd
  option (email mode, phone format, VAT style, money style) raises. Exception: **money** — an
  unknown currency code raises `Invalid currency code: XYZ` in `money` and `money_from_cents`;
  wrap in `TRY()` (or pre-check with `is_valid_currency`).
- **One `TRY(money(...))` per row, reuse the alias.** Two `TRY()`s around the same raising
  call still fail (DuckDB common-subexpression elimination hoists it out of both `TRY`s).
- **`money('1250.00', 'EUR')` is a binder error** (string fits both overloads) and `TRY()` does
  not catch binder errors: `CAST(x AS DECIMAL(18,3))` / `TRY_CAST` first.
- **`money_from_cents` divides by the currency's minor unit**: 100 for USD/EUR, 1 for
  JPY/ISK/KRW, 1000 for KWD/BHD (`money_from_cents(10050, 'USD')` = 100.50).
- **`sum()` over a money column is a binder error** — group by `money_currency(m)` and
  `sum(money_amount(m))`. `money_add`/`money_subtract` raise on mixed currencies.
- **VAT functions take ONE full string with prefix**: `vat_is_valid_syntax('DE123456789')`, not
  `('DE', '123456789')`. A number without prefix → `missing_prefix`; prepend the country. `UK`
  is no prefix (use `GB`, or `XI` for Northern Ireland); Greece is `EL` (also accepts `GR`).
  Purely offline: no VIES lookup — `vat_exists` only checks a known prefix + a digit.
- **Postal needs ~500 MB of libpostal data** (first parse/expand call downloads it inside the
  query unless `anofox_tab_postal_auto_download = false`); not built on Windows. Parts are
  lower-cased; `country` is the written name (`deutschland`), not an ISO code.
- Validation functions ignore surrounding whitespace (incl. no-break spaces) but return no
  cleaned value — store `lower(trim(email))`, `phonenumber_format(..., 'E164')`,
  `vat_format(v, 'iso')` yourself.

Sample data used below:

```sql
CREATE TABLE customers AS SELECT * FROM (VALUES
  (1, 'DE', ' Anna.Schmidt@Example.com ', 'Tel. 030 12345678 ext. 12', 'DE136695976',    1250.00, 'EUR'),
  (2, 'GB', 'tom@example.co.uk',          '07400 123456',              'GB123456782',     880.50, 'gbp'),
  (3, 'FR', 'claire@exemple',             '01 23 45 67 89',            'FR40303265045',   410.00, ' EUR'),
  (4, 'CH', 'luca@example.ch',            '044 668 18 0O',             NULL,               99.90, 'CHF'),
  (5, 'NL', 'not-an-email',               '020 123 4567',              '004495445B01',    310.00, 'EURO'),
  (6, 'AT', 'eva@example.at',             '+43 1 58800',               'ATU13585627',     720.00, 'EUR')
) t(id, country, email, phone, vat, amount, currency);
```

## Email

```text
email_is_valid(email VARCHAR [, mode VARCHAR]) -> BOOLEAN
email_validate(email VARCHAR [, mode VARCHAR]) -> STRUCT(valid BOOLEAN, stage VARCHAR, reason VARCHAR,
                                                         mx_hosts VARCHAR[], smtp_debug STRUCT(transcript VARCHAR[]))
email_config() -> TABLE(key VARCHAR, value VARCHAR)
```

| Mode | Checks | Network |
|---|---|---|
| `regex` (default) | length ≤ 254, local part ≤ 64, `anofox_tab_email_regex_pattern` | none |
| `dns` | regex + domain not reserved + MX (or A/AAAA fallback) | DNS |
| `smtp` (alias `full`) | dns + an MX host accepts `RCPT TO` | DNS + TCP port 25 |

Mode omitted or `NULL` → `anofox_tab_email_default_validation`. Case-insensitive; unknown mode
raises `Unknown email validation mode: mx (expected regex, dns, smtp)`.

Regex rules: no leading/trailing/double dots in local part; `+` tags valid; quoted local parts
rejected; IDN domains only as punycode (`xn--...`); last label not all digits; single-label
domains (`max@localhost`) pass `regex` but fail `dns` (`dns_reserved_domain`; also `.example`,
`.invalid`, `.test`, `.local`).

`email_validate` fields: `stage` = last stage run; `reason` codes — regex: `empty`,
`invalid_format`, `invalid_length`; dns definite (`false`): `dns_domain_not_found`,
`dns_no_records`, `dns_null_mx`, `dns_reserved_domain`, `dns_empty_domain`; dns unknown
(`NULL`): `dns_timeout`, `dns_server_unreachable`, `dns_error_<status>`, ...; smtp definite:
`smtp_recipient_rejected` (5xx), `smtp_no_hosts`; smtp unknown: `smtp_connect_timeout`,
`smtp_recipient_deferred` (4xx), `smtp_mail_from_rejected`, `smtp_overall_timeout`, ...

```sql
SELECT id,
       lower(trim(email))                    AS email_clean,
       email_is_valid(email)                 AS valid,
       (email_validate(email, 'regex')).reason AS reason
FROM customers ORDER BY id;
-- claire@exemple passes regex (format only); not-an-email -> invalid_format
```

```sql
SELECT email_validate('user@example.com', 'mx') AS result;
```

```text
Invalid Input Error: Unknown email validation mode: mx (expected regex, dns, smtp)
```

DNS mode (network; results cached per domain for 300 s, max 10,000 domains; timeouts not cached):

```sql
SELECT count(*) FILTER (WHERE v)         AS valid,
       count(*) FILTER (WHERE NOT v)     AS invalid,
       count(*) FILTER (WHERE v IS NULL) AS unknown
FROM (SELECT email_is_valid(email, 'dns') AS v FROM customers);
```

**On large tables, look up each domain once.** The cache keeps answers, but not timeouts: every
row with a slow domain waits for the timeout again. Check the syntax offline first, then query
the distinct domains and join back:

```sql
WITH syntax AS (
  SELECT id, email, email_is_valid(email) AS syntax_ok,
         split_part(lower(trim(email)), '@', 2) AS domain
  FROM customers
), domains AS (
  SELECT domain, email_is_valid('postmaster@' || domain, 'dns') AS domain_ok
  FROM (SELECT DISTINCT domain FROM syntax WHERE syntax_ok)
)
SELECT s.id, s.email, s.syntax_ok, d.domain_ok
FROM syntax s LEFT JOIN domains d USING (domain)
ORDER BY s.id;
```

`smtp` is never cached, can take up to 10 s per address and is `NULL` where port 25 is blocked
(most cloud hosts): use it only on a small, deduplicated list.

```sql
SELECT key, value FROM email_config() WHERE key LIKE 'dns%';
```

Custom pattern (RE2: no lookahead/backreferences — rejected at `SET`, previous pattern stays):

```sql
SET anofox_tab_email_regex_pattern = '(?i)^[a-z0-9._%+-]+@(?:data-zoo\.de|anofox\.com)$';
SELECT email_is_valid('Jana.Koch@Data-Zoo.de') AS own, email_is_valid('jana@gmail.com') AS other;
RESET anofox_tab_email_regex_pattern;
```

## Phone numbers

Built-in port of libphonenumber (metadata for 245 regions + non-geographic entities, from
`phonenumbers` 9.0.43). No network.

| Function | Signature | Returns |
|---|---|---|
| `phonenumber_parse` | `(phone, region)` | `STRUCT(valid BOOLEAN, country_code INTEGER, national_number VARCHAR, region VARCHAR, type VARCHAR, reason VARCHAR, extension VARCHAR)` |
| `phonenumber_is_valid` | `(phone, region)` | `BOOLEAN` — length + assigned range (`isValidNumber`) |
| `phonenumber_is_possible` | `(phone, region)` | `BOOLEAN` — length only |
| `phonenumber_is_valid_for_region` | `(phone, region)` | `BOOLEAN` — valid AND belongs to that region (Guernsey `+44 1481` is `GG`, not `GB`) |
| `phonenumber_format` | `(phone, region, format)` | `VARCHAR`; `NULL` unless valid |
| `phonenumber_region` | `(phone, region)` | `VARCHAR` region of a valid number (`+1 416...` → `CA`), else `NULL` |
| `phonenumber_match` | `(phone1, phone2, region)` | `EXACT_MATCH`, `NSN_MATCH`, `SHORT_NSN_MATCH`, `NO_MATCH`, `NOT_A_NUMBER` |
| `phonenumber_example` | `(region)` | E.164 fixed-line example, `NULL` for unknown region |
| `phonenumber_status` | `()` | `TABLE(initialized BOOLEAN, default_region VARCHAR)` |

All args `VARCHAR`. `region` `NULL` (or blank) → default region.

**Formats** (case-insensitive; unknown raises `Unknown phone number format 'dotted'. ...`):

| Format | Example (`030 12345678`, DE) |
|---|---|
| `E164` | `+493012345678` — store this |
| `INTERNATIONAL` | `+49 30 12345678` |
| `NATIONAL` | `030 12345678` |
| `RFC3966` | `tel:+49-30-12345678` (only format that writes the extension: `;ext=123`) |

**Parsing rules:** separators `space - . / ( )`; leading labels `Tel`, `Telefon`, `Phone`,
`Mobil(e)`, `Cell`, `Fax` dropped; trailing extension (`ext`, `x`, `Durchwahl`, `DW`, `;ext=`)
split into `extension`; any other letter → `invalid_characters` (no vanity conversion);
full-width digits folded; national trunk prefix (`0`, NANP `1`) and IDD prefix (`00`, `011`)
handled; `+49 (0)30 ...` drops the `(0)`.

**Reasons:** `empty`, `not_a_number`, `invalid_characters`, `invalid_country_code` (`+0...`),
`invalid_length`, `invalid_pattern`, `region_missing`; unjudged (`valid` NULL):
`unsupported_region`, `unsupported_country_code`.
**Types:** `fixed_line`, `mobile`, `fixed_line_or_mobile`, `toll_free`, `premium_rate`,
`shared_cost`, `voip`, `personal_number`, `pager`, `uan`, `voicemail`.

```sql
SELECT id, country,
       p.valid, p.reason, p.extension AS ext,
       phonenumber_format(phone, country, 'E164') AS e164
FROM (SELECT *, phonenumber_parse(phone, country) AS p FROM customers)  -- parse once per row
ORDER BY id;
```

```text
┌───────┬─────────┬─────────┬────────────────────┬─────────┬───────────────┐
│  id   │ country │  valid  │       reason       │   ext   │     e164      │
│ int32 │ varchar │ boolean │      varchar       │ varchar │    varchar    │
├───────┼─────────┼─────────┼────────────────────┼─────────┼───────────────┤
│     1 │ DE      │ true    │ NULL               │ 12      │ +493012345678 │
│     2 │ GB      │ true    │ NULL               │ NULL    │ +447400123456 │
│     3 │ FR      │ true    │ NULL               │ NULL    │ +33123456789  │
│     4 │ CH      │ false   │ invalid_characters │ NULL    │ NULL          │
│     5 │ NL      │ true    │ NULL               │ NULL    │ +31201234567  │
│     6 │ AT      │ true    │ NULL               │ NULL    │ +43158800     │
└───────┴─────────┴─────────┴────────────────────┴─────────┴───────────────┘
```

```sql
SELECT f AS format, phonenumber_format('6502530000', 'US', f) AS formatted
FROM (VALUES ('E164'), ('INTERNATIONAL'), ('NATIONAL'), ('RFC3966')) t(f);
-- +16502530000 | +1 650-253-0000 | (650) 253-0000 | tel:+1-650-253-0000
```

```sql
SELECT (phonenumber_parse('030 12345678', NULL)).reason AS no_region,   -- region_missing
       (phonenumber_parse('01 5321234', 'XX')).valid    AS bad_region,  -- NULL
       phonenumber_is_possible('030 12', 'DE')          AS possible,    -- true
       phonenumber_is_valid('030 12', 'DE')             AS valid,       -- false
       phonenumber_is_valid_for_region('+44 20 7946 0958', 'UK') AS in_uk,  -- true
       phonenumber_example('DE')                        AS example;
```

`phonenumber_match` — `NOT_A_NUMBER` for blanks, placeholders (`n/a`, `-`), letters, unknown
country, so a dedup join never pairs two missing phones. Pass a region for national numbers
(`NULL` uses `anofox_tab_phonenumber_default_region`, default `US`):

```sql
SELECT phonenumber_match('+49 30 12345678', '030 12345678', 'DE') AS same,      -- EXACT_MATCH
       phonenumber_match('+49 30 12345678', '030 12345678', NULL) AS wrong_rg,  -- NO_MATCH (read as US)
       phonenumber_match('', '', 'US')                            AS blanks;    -- NOT_A_NUMBER
```

**Non-geographic numbers (region `001`):** `+800` (intl. freephone), `+808`, `+870`, `+878`,
`+881`, `+882`, `+883`, `+888`, `+979` are valid with region `001`. `001` cannot be used to read a
national number (`unsupported_region`), is not accepted as default region, and
`phonenumber_example('001')` is `NULL`.

```sql
SELECT phonenumber_region('+800 1234 5678', NULL)                AS region,  -- 001
       (phonenumber_parse('+800 1234 5678', NULL)).type          AS type,    -- toll_free
       phonenumber_is_valid_for_region('+800 1234 5678', '001')  AS ok;      -- true
```

Default region (read once per statement at bind time; unknown code raises at `SET`):

```sql
SET anofox_tab_phonenumber_default_region = 'DE';
SELECT phonenumber_format('030 12345678', NULL, 'E164') AS e164, default_region FROM phonenumber_status();
RESET anofox_tab_phonenumber_default_region;
```

```sql
SET anofox_tab_phonenumber_default_region = 'XX';
```

```text
Invalid Input Error: Unknown region code 'XX' for anofox_tab_phonenumber_default_region; expected a supported 2-letter ISO region code (e.g. 'US', 'GB', 'DE')
```

## VAT numbers (EU 27 + GB/XI)

All scalar, offline (no VIES). Every function first normalises (strips spaces/punctuation,
upper-cases, folds full-width and no-break spaces).

| Function | Signature | Returns |
|---|---|---|
| `vat_is_valid` | `(vat_number VARCHAR)` | `BOOLEAN` — prefix + pattern + check digits |
| `vat_validate` | `(vat VARCHAR)` | `STRUCT(valid BOOLEAN, country VARCHAR, reason VARCHAR)` |
| `vat_is_valid_syntax` | `(vat_number VARCHAR)` | `BOOLEAN` — prefix + pattern, no check digits |
| `vat_exists` | `(vat_number VARCHAR)` | `BOOLEAN` — known prefix followed by ≥1 digit (NOT a registry lookup) |
| `vat` / `vat_split` | `(vat_number VARCHAR)` | `STRUCT(country VARCHAR, digits VARCHAR)`; `EL`→`GR`; `NULL` if prefix unsupported; `digits` keeps letters (`U13585627`) |
| `vat_normalize` | `(vat_number VARCHAR)` | `VARCHAR` cleaned text, prefix as written, no validation |
| `vat_format` | `(vat_number VARCHAR, format_style VARCHAR)` | `'iso'` (`DE111111125`, Greece always `EL`) or `'plain'` (national part); `NULL` if prefix unsupported; no validation; unknown style raises |
| `is_valid_vat_country` | `(country_code VARCHAR)` | `BOOLEAN` — one of 28 prefixes incl. `EL`, `GR`, `GB`, `XI` |
| `vat_is_eu_member` | `(country_code VARCHAR)` | `BOOLEAN` — `GB`, `XI` → `false` |
| `vat_country_name` | `(country_code VARCHAR)` | English name, `NULL` if unsupported |

`vat_validate.reason` (first failing check): `empty`, `missing_prefix`, `unsupported_country`,
`invalid_format`, `checksum_mismatch`. Check digits verified for AT BE DE DK ES FI FR EL/GR IE
IT LU NL PL PT SE SI; syntax only for BG CY CZ EE HR HU LT LV MT RO SK GB XI.

```sql
SELECT v,
       vat_is_valid_syntax(v)  AS syntax_ok,
       vat_is_valid(v)         AS valid,
       (vat_validate(v)).reason AS reason,
       vat_format(v, 'iso')    AS iso
FROM (VALUES ('DE111111125'), ('de 111-111-125'), ('DE123456789'), ('GR123456783'),
             ('UK123456789'), ('DE12'), ('136695976')) t(v);
-- DE123456789: syntax ok, checksum_mismatch; UK...: unsupported_country; 136695976: missing_prefix
```

Prefix missing in the data → prepend the country column, then validate and store the `iso` form:

```sql
SELECT id, vat,
       vat_format(CASE WHEN regexp_matches(vat, '^[A-Za-z]{2}') THEN vat ELSE country || vat END, 'iso') AS vat_iso,
       vat_is_valid(CASE WHEN regexp_matches(vat, '^[A-Za-z]{2}') THEN vat ELSE country || vat END)     AS valid
FROM customers WHERE vat IS NOT NULL ORDER BY id;
```

```sql
SELECT (vat_split('ATU13585627')).digits AS digits,
       vat_country_name((vat('EL123456783')).country) AS country,  -- Greece
       vat_is_eu_member('GB') AS gb_eu, is_valid_vat_country('XI') AS xi,
       vat_exists('DE') AS bare_prefix;                             -- false
```

## Postal addresses (libpostal)

Compiled in only when libpostal is found at build time; never on Windows. When missing, the four
functions and both settings are not registered — check first:

```sql
SELECT count(*) > 0 AS postal_available
FROM duckdb_functions() WHERE function_name = 'anofox_tab_postal_parse_address';
```

| Function | Signature | Returns |
|---|---|---|
| `postal_load_data` | `()` | `BOOLEAN` — downloads ~500 MB once into `anofox_tab_postal_data_path` (also when auto-download is off) |
| `postal_status` | `()` | `TABLE(initialized BOOLEAN, data_present BOOLEAN, data_dir VARCHAR)` |
| `postal_parse_address` | `(address VARCHAR)` | `STRUCT(house_number, road, city, state, postcode, country)` all `VARCHAR`, lower-cased, missing parts `NULL` |
| `postal_expand_address` | `(address VARCHAR)` | `VARCHAR[]` normalized variants (abbreviations expanded, umlauts transliterated) |

Data dir default `~/.duckdb/extensions/libpostal` (set the path before the first postal call;
process-wide). Repeated labels are joined with a space; `unit`/`level` labels are dropped;
labels can be wrong (`1016 GV` → postcode `1016`, road `... gv`). A `NULL` address returns
`NULL` without loading data.

```sql no-run
SET anofox_tab_postal_data_path = '/opt/libpostal';   -- optional, before first use
SELECT postal_load_data();
SELECT data_present, data_dir FROM postal_status();
```

```sql
SELECT (p).house_number, (p).road, (p).city, (p).postcode, (p).country
FROM (SELECT postal_parse_address('Hauptstraße 5, 80331 München, Deutschland') AS p);
-- 5 | hauptstraße | münchen | 80331 | deutschland
```

Offline ETL: `SET anofox_tab_postal_auto_download = false;` → a parse/expand call without data
raises `IO Error: libpostal data not downloaded: ...` instead of downloading.

## Money and currency

Money = `STRUCT(amount DECIMAL(18,3), currency VARCHAR)` (written `MONEY` below). 38 ISO 4217
codes: USD EUR GBP JPY CAD AUD CHF CNY INR BRL PLN SEK DKK NOK CZK HUF RON BGN ISK TRY RUB UAH NZD
SGD HKD KRW MXN ZAR ILS AED SAR THB IDR MYR PHP TWD KWD BHD. Codes are case-insensitive, trimmed,
stored upper-case. Minor units: 0 for JPY/ISK/KRW, 3 for KWD/BHD, 2 otherwise.

| Function | Signature | Notes |
|---|---|---|
| `money` | `(amount DECIMAL(18,3) \| DOUBLE, currency_code VARCHAR) -> MONEY` | DECIMAL exact; DOUBLE/int rounded half away from zero to 3 dp; out of ±999,999,999,999,999.999 raises; unknown code raises |
| `money_from_cents` | `(cents BIGINT, currency_code VARCHAR) -> MONEY` | divides by minor unit (100 / 1 / 1000); unknown code raises |
| `money_amount` / `money_currency` | `(money MONEY)` | `DECIMAL(18,3)` / `VARCHAR` |
| `money_add` / `money_subtract` | `(money1 MONEY, money2 MONEY) -> MONEY` | different currencies raise |
| `money_multiply` | `(money MONEY, factor DOUBLE) -> MONEY` | rounded to 3 dp; overflow raises `Out of Range` |
| `money_abs` | `(money MONEY) -> MONEY` | |
| `money_is_positive` / `money_is_negative` / `money_is_zero` | `(money MONEY) -> BOOLEAN` | |
| `money_in_range` | `(money MONEY, min DOUBLE, max DOUBLE) -> BOOLEAN` | inclusive; `NULL` bound = unbounded; `min > max` or NaN bound raises |
| `money_same_currency` | `(money1 MONEY, money2 MONEY) -> BOOLEAN` | |
| `money_format` | `(money MONEY, format_style VARCHAR) -> VARCHAR` | `'symbol'` (`$1,234.50`, `1.234,50 €`), `'long'` (`1.234,50 Euro`), `'code'` (`1234.50 EUR`); no default style; unknown raises |
| `is_valid_currency` | `(currency_code VARCHAR) -> BOOLEAN` | never raises |
| `currency_symbol` / `currency_name` | `(currency_code VARCHAR) -> VARCHAR` | `NULL` for unknown code, never raise |

```sql
SELECT money_from_cents(10050, 'USD') AS usd,   -- 100.500 USD
       money_from_cents(1000, 'JPY')  AS jpy,   -- 1000.000 JPY
       money_from_cents(12345, 'kwd') AS kwd,   -- 12.345 KWD
       money(19.99, ' eur ')          AS eur;   -- 19.990 EUR
```

```sql
SELECT money_from_cents(100, 'XYZ');
```

```text
Invalid Input Error: Invalid currency code: XYZ
```

Unknown codes in data → `TRY()` once, reuse the alias:

```sql
SELECT id, currency, is_valid_currency(currency) AS known,
       TRY(money(amount, currency)) AS price,
       money_format(price, 'symbol') AS shown,
       price IS NULL AS needs_review
FROM customers ORDER BY id;
```

Arithmetic, tests, formatting:

```sql
SELECT money_add(money(100, 'EUR'), money(50.25, 'EUR'))      AS added,       -- 150.250 EUR
       money_multiply(money(10, 'USD'), 1.0 / 3)               AS third,       -- 3.333 USD
       money_in_range(money(19.90, 'EUR'), 50, NULL)           AS min_order,   -- false
       money_format(money(-42.5, 'EUR'), 'symbol')             AS eur,         -- -42,50 €
       money_format(money(1234.5, 'JPY'), 'symbol')            AS jpy,         -- ¥1,235
       currency_symbol('BTC')                                  AS unknown;     -- NULL
```

```sql
SELECT money_add(money(1, 'USD'), money(1, 'EUR'));
```

```text
Invalid Input Error: Cannot operate on money with different currencies: USD and EUR
```

String amounts must be cast (binder error otherwise, not catchable by `TRY`):

```sql
SELECT TRY(money(TRY_CAST('1250.00' AS DECIMAL(18,3)), 'EUR')) AS m;
```

Totals per currency (no `sum` on the struct):

```sql
CREATE TABLE invoices AS
SELECT invoice_no, money(amount, currency) AS amount
FROM (VALUES ('RE-1', 1250.00, 'EUR'), ('RE-2', 7800.50, 'EUR'), ('RE-3', 2600.00, 'CHF'))
     t(invoice_no, amount, currency);

SELECT currency, money_format(money(total, currency), 'symbol') AS total
FROM (SELECT money_currency(amount) AS currency, sum(money_amount(amount)) AS total
      FROM invoices GROUP BY ALL)
ORDER BY currency;
-- CHF | 2'600.00 CHF ; EUR | 9.050,50 €
```

## Settings

| Setting | Type | Default | Effect |
|---|---|---|---|
| `anofox_tab_email_default_validation` | VARCHAR | `regex` | mode when `mode` omitted/NULL (`full` stored as `smtp`) |
| `anofox_tab_email_regex_pattern` | VARCHAR | built-in RFC-ish pattern | RE2, whole-address match; `RESET` restores |
| `anofox_tab_email_dns_timeout_ms` | BIGINT | `1000` | per DNS query (1–5000) |
| `anofox_tab_email_dns_tries` | INTEGER | `1` | DNS attempts (1–10) |
| `anofox_tab_email_smtp_port` | INTEGER | `25` | MX port |
| `anofox_tab_email_smtp_connect_timeout_ms` | BIGINT | `5000` | TCP connect (1–5000) |
| `anofox_tab_email_smtp_read_timeout_ms` | BIGINT | `5000` | read/write (1–5000) |
| `anofox_tab_email_smtp_helo_domain` | VARCHAR | `duckdb.local` | sent with `EHLO` |
| `anofox_tab_email_smtp_mail_from` | VARCHAR | `validator@duckdb.local` | sent with `MAIL FROM` |
| `anofox_tab_phonenumber_default_region` | VARCHAR | `US` | region when `region` is NULL; `UK`→`GB`; unknown raises |
| `anofox_tab_postal_data_path` | VARCHAR | `~/.duckdb/extensions/libpostal` | libpostal data dir (postal builds only) |
| `anofox_tab_postal_auto_download` | BOOLEAN | `true` | `false`: no implicit 500 MB download, raise instead |

Email settings are read once per statement. Quiet trace output with
`SET anofox_tab_trace_level = 'warn';`.

## Canonical workflow

Check a text CRM import in one pass: one struct per field, then route each row to
`clean` / `review` / `missing`.

```sql
CREATE TABLE crm_import AS SELECT * FROM (VALUES
  ('C-1001', 'DE', ' Jana.Koch@Example.de ', '030 1234567',     'DE 136 695 976', '1250.00', 'EUR'),
  ('C-1002', 'GB', 'tom@example.co.uk',      '07400 123456',    'GB123456782',    '880.50',  'gbp'),
  ('C-1003', NULL, 'info@example.fr',        '01 23 45 67 89',  'FR40303265045',  '410',     'EUR'),
  ('C-1004', 'AT', 'eva@@example.at',        '01 5321234',      'ATU13585626',    '99.90',   'EURO'),
  ('C-1005', 'NL', 'piet@example.nl',        '+31 20 123 4567', '004495445B01',   '1250.00', 'EUR'),
  ('C-1006', 'DE', NULL,                     NULL,              NULL,             '75.00',   'EUR'),
  ('C-1007', 'DE', '',                       '   ',             '',               '320.00',  'EUR')
) t(customer_ref, country, email, phone, vat_id, amount, currency);

CREATE TABLE crm_checked AS
WITH checked AS (
  SELECT customer_ref,
         lower(trim(email))                          AS email_clean,
         email_validate(email)                       AS e,
         phonenumber_parse(phone, country)           AS p,
         phonenumber_format(phone, country, 'E164')  AS phone_e164,
         vat_validate(vat_id)                        AS v,
         vat_format(vat_id, 'iso')                   AS vat_iso,
         TRY(money(TRY_CAST(amount AS DECIMAL(18,3)), currency)) AS amount_money
  FROM crm_import
)
SELECT customer_ref, email_clean, phone_e164, vat_iso, amount_money,
       e.reason AS email_reason, p.reason AS phone_reason, v.reason AS vat_reason,
       CASE WHEN (e.valid IS NULL AND e.reason IS NULL) OR (p.valid IS NULL AND p.reason IS NULL)
                 OR v.valid IS NULL
                 OR 'empty' IN (e.reason, p.reason, v.reason) THEN 'missing'
            WHEN e.valid AND p.valid AND v.valid AND amount_money IS NOT NULL THEN 'clean'
            ELSE 'review' END AS route
FROM checked;

SELECT customer_ref, route, email_reason, phone_reason, vat_reason
FROM crm_checked ORDER BY customer_ref;
-- C-1001/C-1002 clean; C-1003 review (region_missing); C-1004 review (invalid_format,
-- checksum_mismatch, EURO -> NULL money); C-1005 review (missing_prefix); C-1006/C-1007 missing
```

`p.valid IS NULL AND p.reason IS NULL` = phone absent; `p.valid IS NULL` with a reason
(`unsupported_*`) = unjudged → review. In `dns`/`smtp` mode `e.valid IS NULL` can also mean
"lookup unfinished" — re-check later instead of loading.

## Common patterns

- **Column score:** `SELECT compliance_rate FROM compliance('customers', 'phonenumber_is_valid(phone, country)')`
  (a `NULL` expression counts as non-compliant; wrap DNS results in `coalesce(..., true)` to give
  unknowns the benefit of the doubt).
- **Phone dedup key:** `phonenumber_format(phone, country, 'E164')` (NULL for invalid numbers, so
  blanks never collide); or join on `phonenumber_match(a.phone, b.phone, a.country) IN ('EXACT_MATCH', 'NSN_MATCH')`.
- **VAT dedup key:** `vat_format(v, 'iso')` (unifies `GR`/`EL`, spacing, case) after `vat_is_valid(v)`.
- **Address dedup (needs data):** unnest `postal_expand_address(address)` per customer and
  self-join on the variant with `a.id < b.id`; treat matches as review candidates.
- **German decimal strings** (`1.250,00`): rewrite only values matching
  `regexp_full_match(x, '\d{1,3}(\.\d{3})*,\d+|\d+,\d+')` with `replace(replace(x, '.', ''), ',', '.')`
  before `TRY_CAST`; blanket replacement corrupts `1250.00` → `125000.00`.

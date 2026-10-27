# Phone Numbers

> Parse, validate, format and compare phone numbers.

**Use this page to:**
- Check whether a phone number is valid for a region
- Format numbers as E.164, international, national or RFC 3966
- Find the region of a number, or compare two numbers

The phone module is a built-in implementation modeled on Google's libphonenumber. It does not
link libphonenumber. It carries number metadata for 21 regions: US, CA, GB, DE, FR, IT, ES, AU,
JP, CN, IN, BR, MX, AR, RU, NL, SE, NO, DK, FI and CH. Numbers of other regions are not valid.

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`phonenumber_parse`](#phonenumber_parse) | `anofox_tab_phonenumber_parse` | scalar | `STRUCT` with country code, national number, region, type, extension, reason |
| [`phonenumber_is_valid`](#phonenumber_is_valid) | `anofox_tab_phonenumber_is_valid` | scalar | `BOOLEAN` |
| [`phonenumber_is_possible`](#phonenumber_is_possible) | `anofox_tab_phonenumber_is_possible` | scalar | `BOOLEAN` |
| [`phonenumber_is_valid_for_region`](#phonenumber_is_valid_for_region) | `anofox_tab_phonenumber_is_valid_for_region` | scalar | `BOOLEAN` |
| [`phonenumber_format`](#phonenumber_format) | `anofox_tab_phonenumber_format` | scalar | `VARCHAR` |
| [`phonenumber_region`](#phonenumber_region) | `anofox_tab_phonenumber_region` | scalar | `VARCHAR` |
| [`phonenumber_match`](#phonenumber_match) | `anofox_tab_phonenumber_match` | scalar | `VARCHAR` |
| [`phonenumber_example`](#phonenumber_example) | `anofox_tab_phonenumber_example` | scalar | `VARCHAR` |
| [`phonenumber_status`](#phonenumber_status) | `anofox_tab_phonenumber_status` | table | module status and default region |

## The region argument

Most functions take a `region` argument: a two-letter ISO region code such as `'US'` or `'DE'`.
It tells the parser how to read a number without a leading `+` and country code. A number that
starts with `+` carries its own country code. When `region` is `NULL`, the setting
`anofox_tab_phonenumber_default_region` applies (default `US`). The code is case-insensitive and
surrounding spaces are ignored (`' gb '` is `GB`); a `region` of only spaces counts as `NULL`.
`'UK'` is accepted as an alias of `'GB'`, both as `region` and in the default-region setting. A
`region` that is not one of the 21 supported codes (for example `'AT'`) makes a number without
`+` invalid; `phonenumber_parse` reports it as `unsupported_region`. This is deliberate: a
`region` argument is data, often a column, so one row with an unknown country makes that row
invalid but does not stop the query. The [default-region setting](#default-region) is
configuration instead and rejects an unsupported code with an error when it is set.

```sql
SELECT phonenumber_format('020 7946 0958', 'UK', 'E164') AS uk,
       phonenumber_format('020 7946 0958', ' gb ', 'E164') AS gb,
       phonenumber_is_valid_for_region('+44 20 7946 0958', 'UK') AS valid_in_uk;
```

```text
┌───────────────┬───────────────┬─────────────┐
│      uk       │      gb       │ valid_in_uk │
│    varchar    │    varchar    │   boolean   │
├───────────────┼───────────────┼─────────────┤
│ +442079460958 │ +442079460958 │ true        │
└───────────────┴───────────────┴─────────────┘
```

The parser also understands two common ways of writing international numbers:

- **International dialling prefix.** A number that starts with the region's international
  prefix is read like a number with `+`: `0049 30 12345678` with region `'DE'`, or
  `011 49 30 12345678` with region `'US'`. The prefixes are `00` in most regions, `011` in US
  and CA, `0011` in AU, `010` in JP and `810` in RU.
- **Trunk digit after the country code.** `+49 (0)30 12345678` and `+44 020 7946 0018` keep
  the national prefix after the country code. It is dropped when the number is only valid
  without it.

```sql
SELECT phonenumber_format('0049 30 12345678', 'DE', 'E164') AS idd,
       phonenumber_format('+49 (0)30 12345678', NULL, 'E164') AS trunk,
       phonenumber_is_valid('089 123456', 'DE') AS short_de;
```

```text
┌───────────────┬───────────────┬──────────┐
│      idd      │     trunk     │ short_de │
│    varchar    │    varchar    │ boolean  │
├───────────────┼───────────────┼──────────┤
│ +493012345678 │ +493012345678 │ true     │
└───────────────┴───────────────┴──────────┘
```

### Labels, extensions and letters

Phone fields typed by people carry more than digits. The parser reads spaces, `-`, `.`, `/` and
parentheses as separators and handles three more cases:

- **A leading label** is dropped: `Tel`, `Telefon`, `Telephone`, `Phone`, `Mobil`, `Mobile`,
  `Cell` or `Fax` (any case), followed by `.`, `:` or a space. `Tel: 030 12345678`,
  `Mobil 0171 1234567` and the RFC 3966 form `tel:+49-30-12345678` are all valid.
- **A trailing extension** is split off and ignored for validation and formatting: the marker
  `ext`, `extension`, `x`, `Durchwahl` or `DW` (any case, optionally followed by `.` or `:`),
  then only digits up to the end. `phonenumber_parse` returns the digits in the field
  `extension`. A `-` is a separator and never starts an extension, so `030 12345-67` is read as
  one number.
- **Any other letter makes the number invalid** with reason `invalid_characters`:
  `650foo2530000`, `030 12A34567` and `+49 30 1234 FAX 5678` are rejected instead of being read
  as the digits that are left. Vanity numbers such as `1-800-FLOWERS` are not converted. Input
  without any digit (`n/a`) is `not_a_number`.

```sql
SELECT phone,
       (phonenumber_parse(phone, 'DE')).valid AS valid,
       phonenumber_format(phone, 'DE', 'E164') AS e164,
       (phonenumber_parse(phone, 'DE')).extension AS extension,
       (phonenumber_parse(phone, 'DE')).reason AS reason
FROM (VALUES ('Tel.: 030 12345678'), ('030 12345678 Durchwahl 215'), ('+1 650-253-0000 ext. 123'),
             ('030 12A34567'), ('+49 30 1234 FAX 5678'), ('n/a')) t(phone);
```

```text
┌────────────────────────────┬─────────┬───────────────┬───────────┬────────────────────┐
│           phone            │  valid  │     e164      │ extension │       reason       │
│          varchar           │ boolean │    varchar    │  varchar  │      varchar       │
├────────────────────────────┼─────────┼───────────────┼───────────┼────────────────────┤
│ Tel.: 030 12345678         │ true    │ +493012345678 │ NULL      │ NULL               │
│ 030 12345678 Durchwahl 215 │ true    │ +493012345678 │ 215       │ NULL               │
│ +1 650-253-0000 ext. 123   │ true    │ +16502530000  │ 123       │ NULL               │
│ 030 12A34567               │ false   │ NULL          │ NULL      │ invalid_characters │
│ +49 30 1234 FAX 5678       │ false   │ NULL          │ NULL      │ invalid_characters │
│ n/a                        │ false   │ NULL          │ NULL      │ not_a_number       │
└────────────────────────────┴─────────┴───────────────┴───────────┴────────────────────┘
```

```sql
CREATE TABLE contacts AS
SELECT * FROM (VALUES
    (1, '+1 650-253-0000', NULL),
    (2, '(650) 253-0000', 'US'),
    (3, '+44 20 8366 1177', NULL),
    (4, '+44 7400 123456', NULL),
    (5, '+49 30 12345678', NULL),
    (6, '12', 'US')
) t(id, phone, region);
```

---

## `phonenumber_parse`

Canonical name: `anofox_tab_phonenumber_parse`. Scalar function.

Parses a number and returns its parts.

```text
phonenumber_parse(phone VARCHAR, region VARCHAR) -> STRUCT(
    valid           BOOLEAN,
    country_code    INTEGER,
    national_number VARCHAR,
    region          VARCHAR,
    type            VARCHAR,
    reason          VARCHAR,
    extension       VARCHAR
)
```

| Field | Type | Description |
|-------|------|-------------|
| `valid` | `BOOLEAN` | `true` when the number is valid, the same as `phonenumber_is_valid`; the other fields except `reason` are `NULL` when it is not |
| `country_code` | `INTEGER` | Country calling code, e.g. `1`, `44`, `49` |
| `national_number` | `VARCHAR` | The number without country code and formatting |
| `region` | `VARCHAR` | Region code, e.g. `US`, `GB` |
| `type` | `VARCHAR` | `fixed_line`, `mobile`, `fixed_line_or_mobile` or `toll_free` |
| `reason` | `VARCHAR` | `NULL` when the number is valid, else why it is not (see below) |
| `extension` | `VARCHAR` | Digits of a trailing extension (`ext. 123`, `x123`, `Durchwahl 123`), see [Labels, extensions and letters](#labels-extensions-and-letters); `NULL` when there is none or the number is not valid |

| `reason` | Meaning |
|----------|---------|
| `not_a_number` | The input has no digits |
| `invalid_characters` | The number contains letters, other than a leading label or a trailing extension marker |
| `unsupported_region` | A number without `+` and a `region` that is not one of the 21 supported regions |
| `unsupported_country_code` | The country code after `+` (or the international prefix) is not one of the supported regions, e.g. `+43` (Austria) |
| `invalid_length` | The national number has a length that the region does not use |
| `invalid_pattern` | The length fits, but the digits are not in a fixed-line, mobile or toll-free range of the region |

```sql
SELECT id, phone,
       (phonenumber_parse(phone, region)).valid AS valid,
       (phonenumber_parse(phone, region)).country_code AS cc,
       (phonenumber_parse(phone, region)).national_number AS national,
       (phonenumber_parse(phone, region)).type AS type,
       (phonenumber_parse(phone, region)).reason AS reason
FROM contacts ORDER BY id;
```

```text
┌───────┬──────────────────┬─────────┬───────┬────────────┬──────────────────────┬────────────────┐
│  id   │      phone       │  valid  │  cc   │  national  │         type         │     reason     │
│ int32 │     varchar      │ boolean │ int32 │  varchar   │       varchar        │    varchar     │
├───────┼──────────────────┼─────────┼───────┼────────────┼──────────────────────┼────────────────┤
│     1 │ +1 650-253-0000  │ true    │     1 │ 6502530000 │ fixed_line_or_mobile │ NULL           │
│     2 │ (650) 253-0000   │ true    │     1 │ 6502530000 │ fixed_line_or_mobile │ NULL           │
│     3 │ +44 20 8366 1177 │ true    │    44 │ 2083661177 │ fixed_line           │ NULL           │
│     4 │ +44 7400 123456  │ true    │    44 │ 7400123456 │ mobile               │ NULL           │
│     5 │ +49 30 12345678  │ true    │    49 │ 3012345678 │ fixed_line           │ NULL           │
│     6 │ 12               │ false   │  NULL │ NULL       │ NULL                 │ invalid_length │
└───────┴──────────────────┴─────────┴───────┴────────────┴──────────────────────┴────────────────┘
```

---

## `phonenumber_is_valid`

Canonical name: `anofox_tab_phonenumber_is_valid`. Scalar function.

Returns `true` when the number has a valid length and is in a fixed-line, mobile or toll-free
range of its region.

```text
phonenumber_is_valid(phone VARCHAR, region VARCHAR) -> BOOLEAN
```

## `phonenumber_is_possible`

Canonical name: `anofox_tab_phonenumber_is_possible`. Scalar function.

A quicker check that looks at the length only. A possible number need not be valid.

```text
phonenumber_is_possible(phone VARCHAR, region VARCHAR) -> BOOLEAN
```

## `phonenumber_is_valid_for_region`

Canonical name: `anofox_tab_phonenumber_is_valid_for_region`. Scalar function.

Returns `true` when the number is valid and belongs to the given region.

```text
phonenumber_is_valid_for_region(phone VARCHAR, region VARCHAR) -> BOOLEAN
```

```sql
SELECT id, phone,
       phonenumber_is_valid(phone, region) AS valid,
       phonenumber_is_possible(phone, region) AS possible,
       phonenumber_is_valid_for_region(phone, 'GB') AS valid_in_gb
FROM contacts ORDER BY id;
```

```text
┌───────┬──────────────────┬─────────┬──────────┬─────────────┐
│  id   │      phone       │  valid  │ possible │ valid_in_gb │
│ int32 │     varchar      │ boolean │ boolean  │   boolean   │
├───────┼──────────────────┼─────────┼──────────┼─────────────┤
│     1 │ +1 650-253-0000  │ true    │ true     │ false       │
│     2 │ (650) 253-0000   │ true    │ true     │ false       │
│     3 │ +44 20 8366 1177 │ true    │ true     │ true        │
│     4 │ +44 7400 123456  │ true    │ true     │ true        │
│     5 │ +49 30 12345678  │ true    │ true     │ false       │
│     6 │ 12               │ false   │ false    │ false       │
└───────┴──────────────────┴─────────┴──────────┴─────────────┘
```

---

## `phonenumber_format`

Canonical name: `anofox_tab_phonenumber_format`. Scalar function.

Formats a number in one of four styles.

```text
phonenumber_format(phone VARCHAR, region VARCHAR, format VARCHAR) -> VARCHAR
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `phone` | `VARCHAR` | The number |
| `region` | `VARCHAR` | Region hint, or `NULL` for the default region |
| `format` | `VARCHAR` | `E164`, `INTERNATIONAL`, `NATIONAL` or `RFC3966`, case-insensitive |

Returns `NULL` unless the number is valid, the same check as
[`phonenumber_is_valid`](#phonenumber_is_valid): a number that cannot be parsed, has an
impossible length or is not in an assigned range of its region is not formatted.
`phonenumber_parse(...).reason` says why. So every non-`NULL` result is a valid number, and an
`E164` column built with this function holds only valid numbers. An unknown format raises an
error.

| Format | Layout |
|--------|--------|
| `E164` | `+` country code and national number, no separators: `+493012345678` |
| `INTERNATIONAL` | `+` country code, a space, then the region's international grouping: `+49 30 12345678` |
| `NATIONAL` | The region's national grouping, with the national prefix where the region dials one: `030 12345678` |
| `RFC3966` | `tel:` URI: the international grouping with `-` between all blocks: `tel:+49-30-12345678` |

The grouping follows per-region layout rules modelled on libphonenumber's number formats. Each
rule names the leading digits it applies to and how the digits are split into blocks, so a
Berlin number is `030 12345678`, a German mobile `0171 1234567` and a London number
`020 7946 0018`. The rules cover the common fixed-line, mobile and toll-free layouts of the 21
regions; a number that no rule covers is written as one block. US and CA numbers use the
`(650) 253-0000` / `+1 650 253 0000` layout.

```sql
SELECT f AS format, phonenumber_format('6502530000', 'US', f) AS formatted
FROM (VALUES ('E164'), ('INTERNATIONAL'), ('national'), ('RFC3966')) t(f);
```

```text
┌───────────────┬─────────────────────┐
│    format     │      formatted      │
│    varchar    │       varchar       │
├───────────────┼─────────────────────┤
│ E164          │ +16502530000        │
│ INTERNATIONAL │ +1 650 253 0000     │
│ national      │ (650) 253-0000      │
│ RFC3966       │ tel:+1-650-253-0000 │
└───────────────┴─────────────────────┘
```

A number of a possible length that is not in an assigned range is not formatted either:

```sql
SELECT phone, region,
       phonenumber_is_possible(phone, region) AS possible,
       phonenumber_format(phone, region, 'E164') AS e164,
       (phonenumber_parse(phone, region)).reason AS reason
FROM (VALUES ('030 12345678', 'DE'), ('030 1234', 'DE'), ('+49 0180 1234567', NULL),
             ('+31 20 123', NULL)) t(phone, region);
```

```text
┌──────────────────┬─────────┬──────────┬───────────────┬─────────────────┐
│      phone       │ region  │ possible │     e164      │     reason      │
│     varchar      │ varchar │ boolean  │    varchar    │     varchar     │
├──────────────────┼─────────┼──────────┼───────────────┼─────────────────┤
│ 030 12345678     │ DE      │ true     │ +493012345678 │ NULL            │
│ 030 1234         │ DE      │ true     │ NULL          │ invalid_pattern │
│ +49 0180 1234567 │ NULL    │ true     │ NULL          │ invalid_pattern │
│ +31 20 123       │ NULL    │ false    │ NULL          │ invalid_length  │
└──────────────────┴─────────┴──────────┴───────────────┴─────────────────┘
```

```sql
SELECT phonenumber_format('6502530000', 'US', 'dotted');
```

```text
Invalid Input Error: Unknown phone number format 'dotted'. Expected one of E164, INTERNATIONAL, NATIONAL, RFC3966
```

```sql
SELECT phone, region,
       phonenumber_format(phone, region, 'NATIONAL') AS national,
       phonenumber_format(phone, region, 'INTERNATIONAL') AS international
FROM (VALUES ('030 12345678', 'DE'), ('0171 1234567', 'DE'), ('07400 123456', 'GB'),
             ('+44 20 7946 0018', NULL), ('01 23 45 67 89', 'FR'), ('044 668 18 00', 'CH'),
             ('020 123 4567', 'NL'), ('+81 3 1234 5678', NULL)) t(phone, region);
```

```text
┌──────────────────┬─────────┬────────────────┬───────────────────┐
│      phone       │ region  │    national    │   international   │
│     varchar      │ varchar │    varchar     │      varchar      │
├──────────────────┼─────────┼────────────────┼───────────────────┤
│ 030 12345678     │ DE      │ 030 12345678   │ +49 30 12345678   │
│ 0171 1234567     │ DE      │ 0171 1234567   │ +49 171 1234567   │
│ 07400 123456     │ GB      │ 07400 123456   │ +44 7400 123456   │
│ +44 20 7946 0018 │ NULL    │ 020 7946 0018  │ +44 20 7946 0018  │
│ 01 23 45 67 89   │ FR      │ 01 23 45 67 89 │ +33 1 23 45 67 89 │
│ 044 668 18 00    │ CH      │ 044 668 18 00  │ +41 44 668 18 00  │
│ 020 123 4567     │ NL      │ 020 123 4567   │ +31 20 123 4567   │
│ +81 3 1234 5678  │ NULL    │ 03-1234-5678   │ +81 3-1234-5678   │
└──────────────────┴─────────┴────────────────┴───────────────────┘
```

---

## `phonenumber_region`

Canonical name: `anofox_tab_phonenumber_region`. Scalar function.

Returns the region code of a valid number (as [`phonenumber_is_valid`](#phonenumber_is_valid)
decides), or `NULL` for a number that is not valid: `phonenumber_region('030 1234', 'DE')` is
`NULL` although the number has a German prefix.

```text
phonenumber_region(phone VARCHAR, region VARCHAR) -> VARCHAR
```

```sql
SELECT id, phonenumber_region(phone, region) AS region
FROM contacts ORDER BY id;
```

```text
┌───────┬─────────┐
│  id   │ region  │
│ int32 │ varchar │
├───────┼─────────┤
│     1 │ US      │
│     2 │ US      │
│     3 │ GB      │
│     4 │ GB      │
│     5 │ DE      │
│     6 │ NULL    │
└───────┴─────────┘
```

---

## `phonenumber_match`

Canonical name: `anofox_tab_phonenumber_match`. Scalar function.

Compares two numbers.

```text
phonenumber_match(phone1 VARCHAR, phone2 VARCHAR, region VARCHAR) -> VARCHAR
```

Returns one of `EXACT_MATCH`, `NSN_MATCH` (same national number), `SHORT_NSN_MATCH` or
`NO_MATCH`.

```sql
SELECT phonenumber_match('+1 650-253-0000', '(650) 253-0000', 'US') AS same,
       phonenumber_match('+1 650-253-0000', '+44 20 8366 1177', NULL) AS different;
```

```text
┌─────────────┬───────────┐
│    same     │ different │
│   varchar   │  varchar  │
├─────────────┼───────────┤
│ EXACT_MATCH │ NO_MATCH  │
└─────────────┴───────────┘
```

---

## `phonenumber_example`

Canonical name: `anofox_tab_phonenumber_example`. Scalar function.

Returns an example number for a region from the built-in metadata, or `NULL` for an unknown
region.

```text
phonenumber_example(region VARCHAR) -> VARCHAR
```

```sql
SELECT phonenumber_example('US') AS us, phonenumber_example('GB') AS gb,
       phonenumber_example('XX') AS unknown;
```

```text
┌──────────────┬───────────────┬─────────┐
│      us      │      gb       │ unknown │
│   varchar    │    varchar    │ varchar │
├──────────────┼───────────────┼─────────┤
│ +12015550123 │ +441212345678 │ NULL    │
└──────────────┴───────────────┴─────────┘
```

---

## `phonenumber_status`

Canonical name: `anofox_tab_phonenumber_status`. Table function.

```text
phonenumber_status() -> TABLE(initialized BOOLEAN, default_region VARCHAR)
```

```sql
SELECT * FROM phonenumber_status();
```

```text
┌─────────────┬────────────────┐
│ initialized │ default_region │
│   boolean   │    varchar     │
├─────────────┼────────────────┤
│ true        │ US             │
└─────────────┴────────────────┘
```

## Default region

`SET anofox_tab_phonenumber_default_region = 'DE';` changes the region used when the `region`
argument is `NULL`. Only the 21 supported region codes are accepted; unlike an unsupported
`region` argument, which only makes the numbers of that row invalid (`unsupported_region`), an
unsupported setting is an error, so a typo in the configuration does not silently invalidate
every national number:

```sql
SET anofox_tab_phonenumber_default_region = 'XX';
```

```text
Invalid Input Error: Unknown region code 'XX' for anofox_tab_phonenumber_default_region; expected a supported 2-letter ISO region code (e.g. 'US', 'GB', 'DE')
```

The value is read when a query is bound, so a change does not affect a query that is already
running.

## See also

- [Configuration: phone settings](../reference/configuration.md#phone)
- [PII detection](06-pii.md) finds phone numbers inside free text
- [Conventions](00-conventions.md)

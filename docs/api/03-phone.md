# Phone Numbers

> Parse, validate, format and compare phone numbers.

**Use this page to:**
- Check whether a phone number is valid for a region
- Format numbers as E.164, international, national or RFC 3966
- Find the region of a number, or compare two numbers

The phone module is a built-in C++ implementation of Google's libphonenumber rules for parsing,
validating and formatting. It does not link libphonenumber, but carries libphonenumber's number
metadata for all 245 regions it knows (generated from the `phonenumbers` package, version
9.0.43, see [Metadata](#metadata)): every number range (fixed line, mobile, toll-free,
premium rate, shared cost, VoIP, personal, pager, UAN, voicemail), the possible lengths and the
national layouts of every country.

A number the module has no metadata for cannot be judged: a `region` that is no region code
(`'XX'`, `'DEU'`) or a country code that no country uses (`+999`) or that is non-geographic
(`+800` international freephone, `+882` networks). The validity functions return `NULL` for it,
not `false`, and `phonenumber_parse(...).reason` is `unsupported_region` or
`unsupported_country_code`. A data-quality check on `phonenumber_is_valid(...) = false` thus
reports only numbers that are known to be invalid; count the `NULL`s separately.

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

Most functions take a `region` argument: an ISO 3166-1 alpha-2 country code such as `'US'` or
`'DE'`. Three-letter codes (`'DEU'`) and country names are not accepted; they count as an
unsupported region, and the number is not judged.
It tells the parser how to read a number without a leading `+` and country code. A number that
starts with `+` carries its own country code. When `region` is `NULL`, the setting
`anofox_tab_phonenumber_default_region` applies (default `US`). The code is case-insensitive and
surrounding spaces, including the no-break spaces U+00A0, U+2007 and U+202F of spreadsheet
exports, are ignored (`' gb '` is `GB`); a `region` of only spaces counts as `NULL`.
`'UK'` is accepted as an alias of `'GB'`, both as `region` and in the default-region setting. A
`region` that is no region code of the metadata (for example `'XX'`) makes the validity of a
number without `+` `NULL`; `phonenumber_parse` reports it as `unsupported_region`. This is
deliberate: a `region` argument is data, often a column, so one row with an unknown country
leaves that row unjudged but does not stop the query. The
[default-region setting](#default-region) is configuration instead and rejects an unknown code
with an error when it is set.

A number without `+` read in the default region only because `region` is `NULL` (or empty) can
fail just because it is from another country: `030 12345678` is a Berlin number but not a US
one. `phonenumber_parse` then reports reason `region_missing` instead of `invalid_length` or
`invalid_pattern`, and its `region` field shows the default region that was used:

```sql
SELECT phone,
       (phonenumber_parse(phone, NULL)).region AS region,
       (phonenumber_parse(phone, NULL)).reason AS reason,
       (phonenumber_parse(phone, 'DE')).valid AS valid_in_de
FROM (VALUES ('030 12345678'), ('+49 30 12345678')) t(phone);
```

```text
┌─────────────────┬─────────┬────────────────┬─────────────┐
│      phone      │ region  │     reason     │ valid_in_de │
│     varchar     │ varchar │    varchar     │   boolean   │
├─────────────────┼─────────┼────────────────┼─────────────┤
│ 030 12345678    │ US      │ region_missing │ true        │
│ +49 30 12345678 │ DE      │ NULL           │ true        │
└─────────────────┴─────────┴────────────────┴─────────────┘
```

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

A number without `+` may start with the region's national (trunk) prefix, which the parser
drops: `030 12345678` in `DE`, `020 7946 0018` in `GB`, and `1-212-555-0123`,
`1 (212) 555-0123` or `12125550123` in `US` and `CA` (the NANP trunk digit `1`; the bare
10-digit form `212-555-0123` works as well). The national prefix is `0` in most regions, `1` in
the NANP regions (US, CA and the Caribbean), `8` in RU, KZ and LT and `06` in HU; IT, ES, PL,
PT, GR, NO, DK and others have none (an Italian number keeps its leading `0`). Some regions
rewrite the prefix as libphonenumber does: an Argentinian mobile written domestically,
`011 15 2345 6789`, is `+54 9 11 2345 6789`.

The parser also understands two common ways of writing international numbers:

- **International dialling prefix.** A number that starts with the region's international
  prefix is read like a number with `+`: `0049 30 12345678` with region `'DE'`, or
  `011 49 30 12345678` with region `'US'`. The prefixes are `00` in most regions, `011` in US
  and CA, `0011` in AU, `010` in JP and `810` in RU.
- **Trunk digit after the country code.** `+49 (0)30 12345678` and `+44 020 7946 0018` keep
  the national prefix after the country code. It is dropped unless the number fits the
  region's numbering plan only with it (libphonenumber's rule).

```sql
SELECT phonenumber_format('0049 30 12345678', 'DE', 'E164') AS idd,
       phonenumber_format('+49 (0)30 12345678', NULL, 'E164') AS trunk,
       phonenumber_format('1-212-555-0123', 'US', 'E164') AS us_trunk,
       phonenumber_is_valid('089 123456', 'DE') AS short_de;
```

```text
┌───────────────┬───────────────┬──────────────┬──────────┐
│      idd      │     trunk     │   us_trunk   │ short_de │
│    varchar    │    varchar    │   varchar    │ boolean  │
├───────────────┼───────────────┼──────────────┼──────────┤
│ +493012345678 │ +493012345678 │ +12125550123 │ true     │
└───────────────┴───────────────┴──────────────┴──────────┘
```

### Labels, extensions and letters

Phone fields typed by people carry more than digits. The parser reads spaces, `-`, `.`, `/` and
parentheses as separators and handles four more cases:

- **Full-width characters** typed with an East Asian input method are read as their ASCII
  forms (Unicode NFKC): `０３０ １２３４５６７８` is `030 12345678` and `＋４９` is `+49`. Digits of
  other scripts, such as Arabic-Indic digits, are not converted and are ignored like any other
  separator.
- **No-break spaces** that values copied from Excel, Word or a web page carry (U+00A0 no-break
  space, U+202F narrow no-break space, U+2007 figure space) are read as spaces: they are
  trimmed around the number and separate digit groups inside it, so
  `'+49' || chr(160) || '30' || chr(160) || '12345678'` is `+493012345678`.
- **A leading label** is dropped: `Tel`, `Telefon`, `Telephone`, `Phone`, `Mobil`, `Mobile`,
  `Cell` or `Fax` (any case), followed by `.`, `:` or a space. `Tel: 030 12345678`,
  `Mobil 0171 1234567` and the RFC 3966 form `tel:+49-30-12345678` are all valid.
- **A trailing extension** is split off and ignored for validation: the marker `ext`,
  `extension`, `x`, `Durchwahl` or `DW` (any case, optionally followed by `.`, `:` or `=`), then
  only digits up to the end. This includes the RFC 3966 parameter `;ext=`, so
  `tel:+49-30-1234567;ext=123` and `+49 30 1234567;ext=123` have the extension `123`.
  `phonenumber_parse` returns the digits in the field `extension`; only the `RFC3966` format
  writes it back. A `-` is a separator and never starts an extension, so `030 12345-67` is read
  as one number. Other `tel:` URI parameters (`;isub=`, `;phone-context=`) are not supported.
- **Any other letter makes the number invalid** with reason `invalid_characters`:
  `650foo2530000`, `030 12A34567` and `+49 30 1234 FAX 5678` are rejected instead of being read
  as the digits that are left. Vanity numbers such as `1-800-FLOWERS` are not converted. Input
  without any digit (`n/a`, `-`) is `not_a_number`; a blank value (an empty string or whitespace
  only) is `empty`.

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
| `valid` | `BOOLEAN` | `true` when the number is valid, the same as `phonenumber_is_valid` (`NULL` when it cannot be judged: reason `unsupported_region` or `unsupported_country_code`); the other fields except `region` and `reason` are `NULL` when it is not valid |
| `country_code` | `INTEGER` | Country calling code, e.g. `1`, `44`, `49` |
| `national_number` | `VARCHAR` | The number without country code and formatting |
| `region` | `VARCHAR` | Region code, e.g. `US`, `GB`: the region of the number. For a country code that several regions share, the region whose number ranges the number is in, as libphonenumber decides: `+1 416 ...` is `CA`, `+44 1481 ...` is `GG` (Guernsey), `+7 701 ...` is `KZ`, also when `region` names another of them. For an invalid number the region it was read in: the `region` argument or the default region for a number without `+`, the main region of the country code otherwise. `NULL` for `empty`, `not_a_number`, `invalid_characters`, `unsupported_region` and `unsupported_country_code` |
| `type` | `VARCHAR` | `fixed_line`, `mobile`, `fixed_line_or_mobile` (the ranges overlap, as in US), `toll_free`, `premium_rate`, `shared_cost`, `voip`, `personal_number`, `pager`, `uan` or `voicemail` |
| `reason` | `VARCHAR` | `NULL` when the number is valid, else why it is not (see below) |
| `extension` | `VARCHAR` | Digits of a trailing extension (`ext. 123`, `x123`, `Durchwahl 123`), see [Labels, extensions and letters](#labels-extensions-and-letters); `NULL` when there is none or the number is not valid |

| `reason` | Meaning |
|----------|---------|
| `empty` | The value is blank: an empty string or whitespace only (a quoted `''` CSV cell) |
| `not_a_number` | The input has no digits |
| `invalid_characters` | The number contains letters, other than a leading label or a trailing extension marker |
| `unsupported_region` | A number without `+` and a `region` that is no region code of the metadata, e.g. `'XX'`. The number is not judged: `valid` is `NULL` |
| `unsupported_country_code` | The country code after `+` (or the international prefix) is used by no country (`+999`) or is non-geographic (`+800`). The number is not judged: `valid` is `NULL` |
| `invalid_country_code` | No country code can follow the `+`: the digits after it start with `0` (`+0 30 123456`) or there are none. A typo, so the number is invalid (`valid` is `false`) |
| `invalid_length` | The national number has a length that the region does not use, or the input has more than 32 digits |
| `invalid_pattern` | The length fits, but the digits are in no number range of the region |
| `region_missing` | A number without `+` or international prefix, `region` is `NULL` or empty, and the number is invalid (length or range) in the default region. Pass the customer's country as `region`, or store the number with `+` |

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

A number the module has no metadata for is not judged: `valid` is `NULL` and the reason says
why, `unsupported_country_code` for the country code of a number with `+`,
`unsupported_region` for the `region` argument of a number without:

```sql
SELECT phone, region, (phonenumber_parse(phone, region)).valid AS valid,
       phonenumber_is_valid(phone, region) AS is_valid, (phonenumber_parse(phone, region)).reason AS reason
FROM (VALUES ('+43 1 5321234', NULL), ('01 5321234', 'AT'), ('01 5321234', 'XX'),
             ('+800 1234 5678', NULL)) t(phone, region);
```

```text
┌────────────────┬─────────┬─────────┬──────────┬──────────────────────────┐
│     phone      │ region  │  valid  │ is_valid │          reason          │
│    varchar     │ varchar │ boolean │ boolean  │         varchar          │
├────────────────┼─────────┼─────────┼──────────┼──────────────────────────┤
│ +43 1 5321234  │ NULL    │ true    │ true     │ NULL                     │
│ 01 5321234     │ AT      │ true    │ true     │ NULL                     │
│ 01 5321234     │ XX      │ NULL    │ NULL     │ unsupported_region       │
│ +800 1234 5678 │ NULL    │ NULL    │ NULL     │ unsupported_country_code │
└────────────────┴─────────┴─────────┴──────────┴──────────────────────────┘
```

---

## `phonenumber_is_valid`

Canonical name: `anofox_tab_phonenumber_is_valid`. Scalar function.

Returns `true` when the number has a possible length and is in a number range of its region
(any type: fixed line, mobile, toll-free, premium rate, ...), `false` when it is not, and `NULL`
when it cannot be judged (an unknown `region` or country code, see
[the region argument](#the-region-argument)). This is libphonenumber's `isValidNumber`.

```text
phonenumber_is_valid(phone VARCHAR, region VARCHAR) -> BOOLEAN
```

## `phonenumber_is_possible`

Canonical name: `anofox_tab_phonenumber_is_possible`. Scalar function.

A quicker check that looks at the length only (libphonenumber's `isPossibleNumber`; a length
that is only diallable locally, such as a German number without its area code, counts as
possible). A possible number need not be valid. `NULL` when the number cannot be judged.

```text
phonenumber_is_possible(phone VARCHAR, region VARCHAR) -> BOOLEAN
```

## `phonenumber_is_valid_for_region`

Canonical name: `anofox_tab_phonenumber_is_valid_for_region`. Scalar function.

Returns `true` when the number has the country code of the given region and is in one of its
number ranges (libphonenumber's `isValidNumberForRegion`), `false` otherwise. `NULL` when the
region is unknown or the number cannot be judged. A Guernsey number is valid for `GG`, not for
`GB`, although both use `+44`.

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
| `format` | `VARCHAR` | `E164`, `INTERNATIONAL`, `NATIONAL` or `RFC3966`, case-insensitive; surrounding spaces are ignored |

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
| `RFC3966` | `tel:` URI: the international grouping with `-` between all blocks: `tel:+49-30-12345678`. An extension of the input is appended as `;ext=`: `tel:+49-30-1234567;ext=123`, which parses back to the same number and extension |

The grouping follows libphonenumber's number formats. Each rule names the leading digits it
applies to and how the digits are split into blocks, so a Berlin number is `030 12345678`, a
German mobile `0171 1234567` and a London number `020 7946 0018`. As in libphonenumber, the
rules of the country code's main region apply to all regions sharing it (a Guernsey number is
written like a British one), and a number that no rule covers is written as one block. NANP
numbers (US, CA, ...) use the `(650) 253-0000` / `+1 650-253-0000` layout.

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
│ INTERNATIONAL │ +1 650-253-0000     │
│ national      │ (650) 253-0000      │
│ RFC3966       │ tel:+1-650-253-0000 │
└───────────────┴─────────────────────┘
```

An extension of the input appears only in the `RFC3966` output, and reads back from it:

```sql
SELECT phone, phonenumber_format(phone, NULL, 'RFC3966') AS rfc3966,
       (phonenumber_parse(phonenumber_format(phone, NULL, 'RFC3966'), NULL)).extension AS extension
FROM (VALUES ('+49 30 1234567 ext. 123'), ('+49 30 1234567;ext=123')) t(phone);
```

```text
┌─────────────────────────┬────────────────────────────┬───────────┐
│          phone          │          rfc3966           │ extension │
│         varchar         │          varchar           │  varchar  │
├─────────────────────────┼────────────────────────────┼───────────┤
│ +49 30 1234567 ext. 123 │ tel:+49-30-1234567;ext=123 │ 123       │
│ +49 30 1234567;ext=123  │ tel:+49-30-1234567;ext=123 │ 123       │
└─────────────────────────┴────────────────────────────┴───────────┘
```

Mexican numbers follow the 2019 numbering reform: every number has 10 digits and there is no
national prefix, so `NATIONAL` is `55 1234 5678`. The abolished prefixes that older address
books still contain are dropped when parsing: `01` and `02` (long distance), `044` and `045`
(calls to mobiles) and the mobile token `1` after `+52` (`+52 1 55 1234 5678`). libphonenumber
parsed them the same way from v8.10.17 (2019) to v8.13.37; since v8.13.38 (2024) it rejects them
as too long. This is the one deliberate deviation from libphonenumber's current metadata. Mexican
fixed-line and mobile ranges are the same, so the type is `fixed_line_or_mobile`.

```sql
SELECT phone, phonenumber_format(phone, 'MX', 'E164') AS e164,
       phonenumber_format(phone, 'MX', 'NATIONAL') AS national, (phonenumber_parse(phone, 'MX')).type AS type
FROM (VALUES ('55 5208 3000'), ('01 55 5208 3000'), ('044 55 1234 5678'), ('+52 1 33 3614 1500')) t(phone);
```

```text
┌────────────────────┬───────────────┬──────────────┬──────────────────────┐
│       phone        │     e164      │   national   │         type         │
│      varchar       │    varchar    │   varchar    │       varchar        │
├────────────────────┼───────────────┼──────────────┼──────────────────────┤
│ 55 5208 3000       │ +525552083000 │ 55 5208 3000 │ fixed_line_or_mobile │
│ 01 55 5208 3000    │ +525552083000 │ 55 5208 3000 │ fixed_line_or_mobile │
│ 044 55 1234 5678   │ +525512345678 │ 55 1234 5678 │ fixed_line_or_mobile │
│ +52 1 33 3614 1500 │ +523336141500 │ 33 3614 1500 │ fixed_line_or_mobile │
└────────────────────┴───────────────┴──────────────┴──────────────────────┘
```

A number of a possible length that is not in an assigned range is not formatted either:

```sql
SELECT phone, region,
       phonenumber_is_possible(phone, region) AS possible,
       phonenumber_format(phone, region, 'E164') AS e164,
       (phonenumber_parse(phone, region)).reason AS reason
FROM (VALUES ('030 12345678', 'DE'), ('030 12', 'DE'), ('+49 0123 4567890', NULL),
             ('+31 20 123', NULL), ('+31 20 1234567890', NULL)) t(phone, region);
```

```text
┌───────────────────┬─────────┬──────────┬───────────────┬─────────────────┐
│       phone       │ region  │ possible │     e164      │     reason      │
│      varchar      │ varchar │ boolean  │    varchar    │     varchar     │
├───────────────────┼─────────┼──────────┼───────────────┼─────────────────┤
│ 030 12345678      │ DE      │ true     │ +493012345678 │ NULL            │
│ 030 12            │ DE      │ true     │ NULL          │ invalid_pattern │
│ +49 0123 4567890  │ NULL    │ true     │ NULL          │ invalid_pattern │
│ +31 20 123        │ NULL    │ true     │ NULL          │ invalid_pattern │
│ +31 20 1234567890 │ NULL    │ false    │ NULL          │ invalid_length  │
└───────────────────┴─────────┴──────────┴───────────────┴─────────────────┘
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
             ('020 123 4567', 'NL'), ('+81 3 1234 5678', NULL), ('01 5321234', 'AT'),
             ('0470 12 34 56', 'BE'), ('512 345 678', 'PL')) t(phone, region);
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
│ 01 5321234       │ AT      │ 01 5321234     │ +43 1 5321234     │
│ 0470 12 34 56    │ BE      │ 0470 12 34 56  │ +32 470 12 34 56  │
│ 512 345 678      │ PL      │ 512 345 678    │ +48 512 345 678   │
└──────────────────┴─────────┴────────────────┴───────────────────┘
```

---

## `phonenumber_region`

Canonical name: `anofox_tab_phonenumber_region`. Scalar function.

Returns the region code of a valid number (as [`phonenumber_is_valid`](#phonenumber_is_valid)
decides), or `NULL` for a number that is not valid: `phonenumber_region('030 12', 'DE')` is
`NULL` although the number has a German prefix. For a country code shared by several regions it
is the region of the number's range (`+1 416 555 0199` is `CA`). The `region` argument is required, also for
numbers with `+`; pass `NULL` to use the default region (`phonenumber_region(phone)` with one
argument is a binder error).

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

Returns one of `EXACT_MATCH`, `NSN_MATCH` (same national number), `SHORT_NSN_MATCH`,
`NO_MATCH` or `NOT_A_NUMBER`. As in libphonenumber, `NOT_A_NUMBER` means that one of the two
strings cannot be read as a phone number: a blank value, a placeholder such as `-` or `n/a`,
letters, fewer than two digits, or an unknown country code or region. Two identical blank or
placeholder values are therefore no match, so a dedup join on `phonenumber_match` never pairs
customers on a missing phone. A short number that can be read but has an invalid length
(`411`) is compared as written: identical strings are an `EXACT_MATCH`.

```sql
SELECT phonenumber_match('+1 650-253-0000', '(650) 253-0000', 'US') AS same,
       phonenumber_match('+1 650-253-0000', '+44 20 8366 1177', NULL) AS different,
       phonenumber_match('', '', 'US') AS both_blank,
       phonenumber_match('n/a', '+1 650-253-0000', 'US') AS placeholder;
```

```text
┌─────────────┬───────────┬──────────────┬──────────────┐
│    same     │ different │  both_blank  │ placeholder  │
│   varchar   │  varchar  │   varchar    │   varchar    │
├─────────────┼───────────┼──────────────┼──────────────┤
│ EXACT_MATCH │ NO_MATCH  │ NOT_A_NUMBER │ NOT_A_NUMBER │
└─────────────┴───────────┴──────────────┴──────────────┘
```

---

## `phonenumber_example`

Canonical name: `anofox_tab_phonenumber_example`. Scalar function.

Returns libphonenumber's example number for a region (its fixed-line example) in E.164, or
`NULL` for an unknown region.

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
argument is `NULL`. Only region codes of the metadata are accepted; unlike an unknown `region`
argument, which only leaves the numbers of that row unjudged (`unsupported_region`), an unknown
setting is an error, so a typo in the configuration does not silently leave every national
number unjudged:

```sql
SET anofox_tab_phonenumber_default_region = 'XX';
```

```text
Invalid Input Error: Unknown region code 'XX' for anofox_tab_phonenumber_default_region; expected a supported 2-letter ISO region code (e.g. 'US', 'GB', 'DE')
```

The value is read once per statement, when the statement is bound (a prepared statement is
bound again when it is executed), so every row of a statement is read in the same region, a
change takes effect with the next statement, and it does not affect a query that is already
running.

## Metadata

The number metadata is generated from the [`phonenumbers`](https://pypi.org/project/phonenumbers/)
package, the Python port of libphonenumber, by `scripts/generate_phone_metadata.py` into
`src/anofox_phonenumber_metadata_data.inc`. To pick up new number ranges, bump the version pin in
the script, in `python/pyproject.toml` and in `.github/workflows/_extension_smoke_test.yml` (the
parity test `python/tests/test_phone_parity.py`, which CI runs against every built artifact,
compares the example numbers of every region with the same version), then run
`uv run scripts/generate_phone_metadata.py` and rebuild. The metadata of a region is compiled on
its first use.

The only deliberate deviation from upstream is the parsing of Mexico's abolished prefixes (see
[`phonenumber_format`](#phonenumber_format)). Not included are the non-geographic entities of
libphonenumber (region `001`: `+800`, `+808`, `+870`, `+878`, `+881`, `+882`, `+883`, `+888`,
`+979`); such numbers are not judged.

## See also

- [Configuration: phone settings](../reference/configuration.md#phone)
- [PII detection](06-pii.md) finds phone numbers inside free text
- [Conventions](00-conventions.md)

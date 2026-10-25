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
| [`phonenumber_parse`](#phonenumber_parse) | `anofox_tab_phonenumber_parse` | scalar | `STRUCT` with country code, national number, region, type |
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
`anofox_tab_phonenumber_default_region` applies (default `US`).

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
    type            VARCHAR
)
```

| Field | Type | Description |
|-------|------|-------------|
| `valid` | `BOOLEAN` | `true` when the number is valid; the other fields are `NULL` when it is not |
| `country_code` | `INTEGER` | Country calling code, e.g. `1`, `44`, `49` |
| `national_number` | `VARCHAR` | The number without country code and formatting |
| `region` | `VARCHAR` | Region code, e.g. `US`, `GB` |
| `type` | `VARCHAR` | `fixed_line`, `mobile`, `fixed_line_or_mobile` or `toll_free` |

```sql
SELECT id, phone,
       (phonenumber_parse(phone, region)).valid AS valid,
       (phonenumber_parse(phone, region)).country_code AS cc,
       (phonenumber_parse(phone, region)).national_number AS national,
       (phonenumber_parse(phone, region)).type AS type
FROM contacts ORDER BY id;
```

```text
┌───────┬──────────────────┬─────────┬───────┬────────────┬──────────────────────┐
│  id   │      phone       │  valid  │  cc   │  national  │         type         │
│ int32 │     varchar      │ boolean │ int32 │  varchar   │       varchar        │
├───────┼──────────────────┼─────────┼───────┼────────────┼──────────────────────┤
│     1 │ +1 650-253-0000  │ true    │     1 │ 6502530000 │ fixed_line_or_mobile │
│     2 │ (650) 253-0000   │ true    │     1 │ 6502530000 │ fixed_line_or_mobile │
│     3 │ +44 20 8366 1177 │ true    │    44 │ 2083661177 │ fixed_line           │
│     4 │ +44 7400 123456  │ true    │    44 │ 7400123456 │ mobile               │
│     5 │ +49 30 12345678  │ true    │    49 │ 3012345678 │ fixed_line           │
│     6 │ 12               │ false   │  NULL │ NULL       │ NULL                 │
└───────┴──────────────────┴─────────┴───────┴────────────┴──────────────────────┘
```

---

## `phonenumber_is_valid`

Canonical name: `anofox_tab_phonenumber_is_valid`. Scalar function.

Returns `true` when the number has a valid length and prefix for its region.

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

Returns `NULL` when the number is not valid. An unknown format raises an error.

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

```sql
SELECT phonenumber_format('6502530000', 'US', 'dotted');
```

```text
Invalid Input Error: Unknown phone number format 'dotted'. Expected one of E164, INTERNATIONAL, NATIONAL, RFC3966
```

---

## `phonenumber_region`

Canonical name: `anofox_tab_phonenumber_region`. Scalar function.

Returns the region code of a valid number, or `NULL`.

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
argument is `NULL`. Only the 21 supported region codes are accepted:

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

# VAT Numbers

> Parse, normalize, format and validate European VAT identification numbers.

**Use this page to:**
- Check the syntax and check digits of VAT numbers
- Split a VAT number into country and digits
- Normalize and format VAT numbers for display
- Look up country names and EU membership

## Supported countries

28 countries: the 27 EU member states plus the United Kingdom.

| Prefix | Country | Prefix | Country |
|--------|---------|--------|---------|
| `AT` | Austria | `IE` | Ireland |
| `BE` | Belgium | `IT` | Italy |
| `BG` | Bulgaria | `LT` | Lithuania |
| `CY` | Cyprus | `LU` | Luxembourg |
| `CZ` | Czechia | `LV` | Latvia |
| `DE` | Germany | `MT` | Malta |
| `DK` | Denmark | `NL` | Netherlands |
| `EE` | Estonia | `PL` | Poland |
| `EL` (also `GR`) | Greece | `PT` | Portugal |
| `ES` | Spain | `RO` | Romania |
| `FI` | Finland | `SE` | Sweden |
| `FR` | France | `SI` | Slovenia |
| `HR` | Croatia | `SK` | Slovakia |
| `HU` | Hungary | `GB`, `XI` (Northern Ireland) | United Kingdom |

Greece uses the VAT prefix `EL`; the ISO code `GR` is accepted too. `UK` is not a valid prefix:
use `GB`, or `XI` for Northern Ireland. Prefixes are case-insensitive.

`vat_is_valid` verifies check digits for AT, BE, DE, DK, ES, FI, FR, EL/GR, IE, IT, LU, NL, PL,
PT, SE and SI. For the other countries it checks the syntax only. French numbers whose key
contains letters are accepted without a check-digit test.

## Functions

| Function | Canonical name | Input | Returns |
|----------|----------------|-------|---------|
| [`vat_is_valid`](#vat_is_valid) | `anofox_tab_vat_is_valid` | VAT number | `BOOLEAN` |
| [`vat_is_valid_syntax`](#vat_is_valid_syntax) | `anofox_tab_vat_is_valid_syntax` | VAT number | `BOOLEAN` |
| [`vat_exists`](#vat_exists) | `anofox_tab_vat_exists` | VAT number | `BOOLEAN` |
| [`vat`](#vat) | `anofox_tab_vat` | VAT number | `STRUCT(country VARCHAR, digits VARCHAR)` |
| [`vat_split`](#vat_split) | `anofox_tab_vat_split` | VAT number | `STRUCT(country VARCHAR, digits VARCHAR)` |
| [`vat_normalize`](#vat_normalize) | `anofox_tab_vat_normalize` | VAT number | `VARCHAR` |
| [`vat_format`](#vat_format) | `anofox_tab_vat_format` | VAT number, style | `VARCHAR` |
| [`is_valid_vat_country`](#is_valid_vat_country) | `anofox_tab_is_valid_vat_country` | country code | `BOOLEAN` |
| [`vat_is_eu_member`](#vat_is_eu_member) | `anofox_tab_vat_is_eu_member` | country code | `BOOLEAN` |
| [`vat_country_name`](#vat_country_name) | `anofox_tab_vat_country_name` | country code | `VARCHAR` |

All are scalar functions and run locally; they do not query the EU VIES service. A `NULL`
input returns `NULL`.

```sql
CREATE TABLE suppliers AS
SELECT * FROM (VALUES
    (1, 'DE111111125'),
    (2, 'de 111-111-125'),
    (3, 'DE123456789'),
    (4, 'EL123456783'),
    (5, 'XI123456782'),
    (6, 'UK123456789'),
    (7, 'DE12')
) t(id, vat_id);
```

---

## Validation

### `vat_is_valid`

Canonical name: `anofox_tab_vat_is_valid`. Scalar function.

Full check: the number is normalized, its prefix and syntax are checked, and then its check
digits where the country has them.

```text
vat_is_valid(vat_number VARCHAR) -> BOOLEAN
```

### `vat_is_valid_syntax`

Canonical name: `anofox_tab_vat_is_valid_syntax`. Scalar function.

Checks the prefix and the country's number pattern, without check digits.

```text
vat_is_valid_syntax(vat_number VARCHAR) -> BOOLEAN
```

### `vat_exists`

Canonical name: `anofox_tab_vat_exists`. Scalar function.

Checks only that the number starts with a supported prefix followed by a number. It does not
check the length or pattern, and it does not check that the number is registered.

```text
vat_exists(vat_number VARCHAR) -> BOOLEAN
```

```sql
SELECT id, vat_id,
       vat_exists(vat_id) AS has_prefix,
       vat_is_valid_syntax(vat_id) AS syntax_ok,
       vat_is_valid(vat_id) AS valid
FROM suppliers ORDER BY id;
```

```text
┌───────┬────────────────┬────────────┬───────────┬─────────┐
│  id   │     vat_id     │ has_prefix │ syntax_ok │  valid  │
│ int32 │    varchar     │  boolean   │  boolean  │ boolean │
├───────┼────────────────┼────────────┼───────────┼─────────┤
│     1 │ DE111111125    │ true       │ true      │ true    │
│     2 │ de 111-111-125 │ true       │ true      │ true    │
│     3 │ DE123456789    │ true       │ true      │ false   │
│     4 │ EL123456783    │ true       │ true      │ true    │
│     5 │ XI123456782    │ true       │ true      │ true    │
│     6 │ UK123456789    │ false      │ false     │ false   │
│     7 │ DE12           │ true       │ false     │ false   │
└───────┴────────────────┴────────────┴───────────┴─────────┘
```

`DE123456789` has a valid syntax but a wrong check digit.

---

## Parsing and formatting

### `vat`

Canonical name: `anofox_tab_vat`. Scalar function.

### `vat_split`

Canonical name: `anofox_tab_vat_split`. Scalar function.

Both split a VAT number into its ISO country code and its digits. They return the same result.
The country is the ISO code: `EL` becomes `GR` and `XI` becomes `GB`. The result is `NULL` when
the prefix is not supported.

```text
vat(vat_number VARCHAR) -> STRUCT(country VARCHAR, digits VARCHAR)
vat_split(vat_number VARCHAR) -> STRUCT(country VARCHAR, digits VARCHAR)
```

```sql
SELECT id, vat_id,
       (vat(vat_id)).country AS country,
       (vat_split(vat_id)).digits AS digits
FROM suppliers ORDER BY id;
```

```text
┌───────┬────────────────┬─────────┬───────────┐
│  id   │     vat_id     │ country │  digits   │
│ int32 │    varchar     │ varchar │  varchar  │
├───────┼────────────────┼─────────┼───────────┤
│     1 │ DE111111125    │ DE      │ 111111125 │
│     2 │ de 111-111-125 │ DE      │ 111111125 │
│     3 │ DE123456789    │ DE      │ 123456789 │
│     4 │ EL123456783    │ GR      │ 123456783 │
│     5 │ XI123456782    │ GB      │ 123456782 │
│     6 │ UK123456789    │ NULL    │ NULL      │
│     7 │ DE12           │ DE      │ 12        │
└───────┴────────────────┴─────────┴───────────┘
```

### `vat_normalize`

Canonical name: `anofox_tab_vat_normalize`. Scalar function.

Removes spaces and punctuation and converts to upper case. It does not validate.

```text
vat_normalize(vat_number VARCHAR) -> VARCHAR
```

### `vat_format`

Canonical name: `anofox_tab_vat_format`. Scalar function.

```text
vat_format(vat_number VARCHAR, format_style VARCHAR) -> VARCHAR
```

| Style | Result |
|-------|--------|
| `iso` | VAT prefix and digits, e.g. `DE111111125`; Greece as `EL`, Northern Ireland as `XI` |
| `plain` | digits only, without prefix |

The style is case-insensitive. A number that cannot be parsed returns `NULL`. An unknown style
raises an error.

```sql
SELECT vat_normalize('de 111-111-125') AS normalized,
       vat_format('de 111-111-125', 'iso') AS iso,
       vat_format('GR123456783', 'iso') AS greece_iso,
       vat_format('GR123456783', 'plain') AS plain,
       vat_format('not a vat', 'iso') AS unparseable;
```

```text
┌─────────────┬─────────────┬─────────────┬───────────┬─────────────┐
│ normalized  │     iso     │ greece_iso  │   plain   │ unparseable │
│   varchar   │   varchar   │   varchar   │  varchar  │   varchar   │
├─────────────┼─────────────┼─────────────┼───────────┼─────────────┤
│ DE111111125 │ DE111111125 │ EL123456783 │ 123456783 │ NULL        │
└─────────────┴─────────────┴─────────────┴───────────┴─────────────┘
```

```sql
SELECT vat_format('DE111111125', 'dotted');
```

```text
Invalid Input Error: Unsupported VAT format style: dotted (expected 'plain' or 'iso')
```

---

## Country codes

These functions take a two-letter country code, not a VAT number.

### `is_valid_vat_country`

Canonical name: `anofox_tab_is_valid_vat_country`. Scalar function. `true` for one of the 28
supported prefixes, including `EL`, `GR`, `GB` and `XI`.

```text
is_valid_vat_country(country_code VARCHAR) -> BOOLEAN
```

### `vat_is_eu_member`

Canonical name: `anofox_tab_vat_is_eu_member`. Scalar function. `true` for the 27 EU member
states. `GB` and `XI` return `false`.

```text
vat_is_eu_member(country_code VARCHAR) -> BOOLEAN
```

### `vat_country_name`

Canonical name: `anofox_tab_vat_country_name`. Scalar function. The English country name, or
`NULL` for an unsupported code.

```text
vat_country_name(country_code VARCHAR) -> VARCHAR
```

```sql
SELECT code,
       is_valid_vat_country(code) AS vat_country,
       vat_is_eu_member(code) AS eu_member,
       vat_country_name(code) AS country
FROM (VALUES ('DE'), ('el'), ('GR'), ('GB'), ('XI'), ('UK'), ('CH')) t(code);
```

```text
┌─────────┬─────────────┬───────────┬────────────────┐
│  code   │ vat_country │ eu_member │    country     │
│ varchar │   boolean   │  boolean  │    varchar     │
├─────────┼─────────────┼───────────┼────────────────┤
│ DE      │ true        │ true      │ Germany        │
│ el      │ true        │ true      │ Greece         │
│ GR      │ true        │ true      │ Greece         │
│ GB      │ true        │ false     │ United Kingdom │
│ XI      │ true        │ false     │ United Kingdom │
│ UK      │ false       │ false     │ NULL           │
│ CH      │ false       │ false     │ NULL           │
└─────────┴─────────────┴───────────┴────────────────┘
```

To get the country of a VAT number, combine the functions:

```sql
SELECT vat_id, vat_country_name((vat(vat_id)).country) AS country
FROM suppliers WHERE vat_is_valid(vat_id) ORDER BY id;
```

```text
┌────────────────┬────────────────┐
│     vat_id     │    country     │
│    varchar     │    varchar     │
├────────────────┼────────────────┤
│ DE111111125    │ Germany        │
│ de 111-111-125 │ Germany        │
│ EL123456783    │ Greece         │
│ XI123456782    │ United Kingdom │
└────────────────┴────────────────┘
```

## See also

- [Money and currency](04-money.md)
- [Data quality checks](07-data-quality.md): use `compliance` with `vat_is_valid(...)` to check a
  whole column
- [Conventions](00-conventions.md)

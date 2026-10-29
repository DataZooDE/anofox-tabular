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
PT, SE and SI. For the other countries (BG, CY, CZ, EE, HR, HU, LT, LV, MT, RO, SK, GB and XI)
it checks the syntax only. French numbers whose key
contains letters are accepted without a check-digit test.

## Functions

| Function | Canonical name | Input | Returns |
|----------|----------------|-------|---------|
| [`vat_is_valid`](#vat_is_valid) | `anofox_tab_vat_is_valid` | VAT number | `BOOLEAN` |
| [`vat_validate`](#vat_validate) | `anofox_tab_vat_validate` | VAT number | `STRUCT(valid BOOLEAN, country VARCHAR, reason VARCHAR)` |
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

### `vat_validate`

Canonical name: `anofox_tab_vat_validate`. Scalar function.

The same check as `vat_is_valid`, with the reason a number is rejected. Use it to see what is
wrong in a supplier column instead of only counting the invalid rows.

```text
vat_validate(vat VARCHAR) -> STRUCT(valid BOOLEAN, country VARCHAR, reason VARCHAR)
```

| Field | Description |
|-------|-------------|
| `valid` | The same value as `vat_is_valid`; never `NULL` for a non-`NULL` input |
| `country` | The country of a supported prefix as an ISO code, as in [`vat_split`](#vat_split) (`EL` becomes `GR`); `NULL` when the prefix is missing or not supported |
| `reason` | `NULL` when the number is valid, else one of the reasons below |

The checks run in this order, and the first that fails gives the reason:

| `reason` | Meaning |
|----------|---------|
| `empty` | The value is blank: an empty string or whitespace only (a quoted `''` CSV cell) |
| `missing_prefix` | After normalization the value does not start with two letters, e.g. `811128135` |
| `unsupported_country` | Two letters that are not one of the 28 supported prefixes, e.g. `UK` (use `GB`) or `CH` |
| `invalid_format` | The national part does not match the country's pattern: wrong length, a letter where a digit belongs, or nothing after the prefix |
| `checksum_mismatch` | The pattern fits but the check digits are wrong (only for countries with a check-digit test, see [Supported countries](#supported-countries)) |

A `NULL` input returns `NULL`; a blank value is `valid = false` with the reason `empty`, and
`vat_is_valid` returns `false` for it.

```sql
SELECT vat_id, (r).valid, (r).country, (r).reason
FROM (SELECT id, vat_id, vat_validate(vat_id) AS r FROM suppliers)
ORDER BY id;
```

```text
┌────────────────┬─────────────┬─────────────┬─────────────────────┐
│     vat_id     │ (r)."valid" │ (r).country │     (r).reason      │
│    varchar     │   boolean   │   varchar   │       varchar       │
├────────────────┼─────────────┼─────────────┼─────────────────────┤
│ DE111111125    │ true        │ DE          │ NULL                │
│ de 111-111-125 │ true        │ DE          │ NULL                │
│ DE123456789    │ false       │ DE          │ checksum_mismatch   │
│ EL123456783    │ true        │ GR          │ NULL                │
│ XI123456782    │ true        │ XI          │ NULL                │
│ UK123456789    │ false       │ NULL        │ unsupported_country │
│ DE12           │ false       │ DE          │ invalid_format      │
└────────────────┴─────────────┴─────────────┴─────────────────────┘
```

### `vat_is_valid_syntax`

Canonical name: `anofox_tab_vat_is_valid_syntax`. Scalar function.

Checks the prefix and the country's number pattern, without check digits.

```text
vat_is_valid_syntax(vat_number VARCHAR) -> BOOLEAN
```

### `vat_exists`

Canonical name: `anofox_tab_vat_exists`. Scalar function.

`vat_exists` only checks that the value starts with a known country prefix; it does not query
the EU VIES service and does not tell whether the number is registered. The prefix must be
followed by a number (at least one digit). A bare country code such as `'DE'`, or a prefix followed by letters
only, is `false`. It does not check the length or pattern.

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

A column that holds only the country for some suppliers:

```sql
SELECT v, vat_exists(v) AS has_prefix
FROM (VALUES ('DE'), ('de '), ('DE ABC'), ('DE1'), ('ATU13585627')) t(v);
```

```text
┌─────────────┬────────────┐
│      v      │ has_prefix │
│   varchar   │  boolean   │
├─────────────┼────────────┤
│ DE          │ false      │
│ de          │ false      │
│ DE ABC      │ false      │
│ DE1         │ true       │
│ ATU13585627 │ true       │
└─────────────┴────────────┘
```

---

## Parsing and formatting

### `vat`

Canonical name: `anofox_tab_vat`. Scalar function.

### `vat_split`

Canonical name: `anofox_tab_vat_split`. Scalar function.

Both split a VAT number into its country code and its national part, the field `digits`. They
return the same result. Despite its name, `digits` keeps the letters of countries whose numbers
contain them (`U13585627` for `ATU13585627`).
The country is the ISO code for Greece: `EL` becomes `GR`. `GB` and `XI` (Northern Ireland) are
kept as written, so the two stay distinguishable. The result is `NULL` when the prefix is not
supported.

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
│     5 │ XI123456782    │ XI      │ 123456782 │
│     6 │ UK123456789    │ NULL    │ NULL      │
│     7 │ DE12           │ DE      │ 12        │
└───────┴────────────────┴─────────┴───────────┘
```

### `vat_normalize`

Canonical name: `anofox_tab_vat_normalize`. Scalar function.

Removes spaces and punctuation and converts to upper case. It does not validate and keeps the
prefix as written: `GR123456783` stays `GR123456783`. The VAT prefix of Greece is `EL` (the
prefix VIES uses), so to compare or deduplicate VAT numbers use `vat_format(vat, 'iso')`, which
writes Greek numbers with `EL` whichever prefix the input had.

```text
vat_normalize(vat_number VARCHAR) -> VARCHAR
```

Full-width letters, digits and punctuation (`ＤＥ８１１ １２８ １３５`, as typed with an East
Asian input method) and the ideographic space are first folded to ASCII, so they are kept as the
letters and digits they stand for. Every VAT function normalizes its input this way. Other
characters outside ASCII are removed.

```sql
SELECT vat_normalize('ＤＥ８１１　１２８　１３５') AS normalized, vat_is_valid('ＤＥ８１１１２８１３５') AS valid;
```

```text
┌─────────────┬─────────┐
│ normalized  │  valid  │
│   varchar   │ boolean │
├─────────────┼─────────┤
│ DE811128135 │ true    │
└─────────────┴─────────┘
```

### `vat_format`

Canonical name: `anofox_tab_vat_format`. Scalar function.

```text
vat_format(vat_number VARCHAR, format_style VARCHAR) -> VARCHAR
```

| Style | Result |
|-------|--------|
| `iso` | VAT prefix and national part, e.g. `DE111111125`; Greece as `EL` (also for `GR` input); `GB` and `XI` keep their own prefix |
| `plain` | the national part without the prefix, e.g. `111111125`. It is the `digits` field of `vat_split` and keeps the letters some countries use: `U13585627` for `ATU13585627`, `804836951B01` for `NL804836951B01` |

Both styles first normalize the input like `vat_normalize` (no spaces or punctuation, upper
case). The style is case-insensitive. `vat_format` does not validate: a number with a supported
prefix is formatted even when its national part is malformed (`vat_format('DEabc123', 'plain')`
is `ABC123`), so check it with `vat_is_valid` first. A number without a supported prefix returns
`NULL`. An unknown style raises an error. The style has no default: `vat_format(v)` with one
argument is a binder error.

```sql
SELECT vat_normalize('de 111-111-125') AS normalized,
       vat_normalize('gr 123-456-783') AS greece_normalized,
       vat_format('de 111-111-125', 'iso') AS iso,
       vat_format('GR123456783', 'iso') AS greece_iso,
       vat_format('GR123456783', 'plain') AS plain,
       vat_format('ATU13585627', 'plain') AS austria_plain,
       vat_format('not a vat', 'iso') AS unparseable;
```

```text
┌─────────────┬───────────────────┬─────────────┬─────────────┬───────────┬───────────────┬─────────────┐
│ normalized  │ greece_normalized │     iso     │ greece_iso  │   plain   │ austria_plain │ unparseable │
│   varchar   │      varchar      │   varchar   │   varchar   │  varchar  │    varchar    │   varchar   │
├─────────────┼───────────────────┼─────────────┼─────────────┼───────────┼───────────────┼─────────────┤
│ DE111111125 │ GR123456783       │ DE111111125 │ EL123456783 │ 123456783 │ U13585627     │ NULL        │
└─────────────┴───────────────────┴─────────────┴─────────────┴───────────┴───────────────┴─────────────┘
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

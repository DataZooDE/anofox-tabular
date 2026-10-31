# Money and Currency

> Exact money values with a currency, currency-safe arithmetic and formatting.

**Use this page to:**
- Build money values from amounts or from integer cents
- Add, subtract and multiply without mixing currencies
- Total amounts per currency
- Format amounts for display
- Look up currency symbols and names

## The money type

A money value is a struct:

```text
STRUCT(amount DECIMAL(18,3), currency VARCHAR)
```

- `amount` is an exact decimal with 3 places. Arithmetic does not drift. `NaN` and infinity
  cannot be stored, and a result outside the `DECIMAL(18,3)` range raises an
  `Out of Range` error.
- `currency` is an upper-case ISO 4217 code. Input codes are case-insensitive, and surrounding
  whitespace is ignored (`' eur '` is `EUR`).
- 38 currencies are supported (see [Supported currencies](#supported-currencies)). Any other code
  raises `Invalid currency code`; wrap the call in `TRY(...)` to get `NULL` instead, e.g.
  `TRY(money(100, 'XYZ'))` returns `NULL`. Use one `TRY(money(...))` per value and pass its
  result on: two `TRY()`s around the same `money(...)` call still raise, see
  [one raising call in two `TRY()`s](00-conventions.md#one-raising-call-in-two-trys).
- A `NULL` argument returns `NULL`.

## Supported currencies

| Code | Currency | Decimals | Code | Currency | Decimals |
|------|----------|----------|------|----------|----------|
| `USD` | United States Dollar | 2 | `UAH` | Ukrainian Hryvnia | 2 |
| `EUR` | Euro | 2 | `NZD` | New Zealand Dollar | 2 |
| `GBP` | British Pound | 2 | `SGD` | Singapore Dollar | 2 |
| `JPY` | Japanese Yen | 0 | `HKD` | Hong Kong Dollar | 2 |
| `CAD` | Canadian Dollar | 2 | `KRW` | South Korean Won | 0 |
| `AUD` | Australian Dollar | 2 | `MXN` | Mexican Peso | 2 |
| `CHF` | Swiss Franc | 2 | `ZAR` | South African Rand | 2 |
| `CNY` | Chinese Yuan | 2 | `ILS` | Israeli New Shekel | 2 |
| `INR` | Indian Rupee | 2 | `AED` | United Arab Emirates Dirham | 2 |
| `BRL` | Brazilian Real | 2 | `SAR` | Saudi Riyal | 2 |
| `PLN` | Polish Złoty | 2 | `THB` | Thai Baht | 2 |
| `SEK` | Swedish Krona | 2 | `IDR` | Indonesian Rupiah | 2 |
| `DKK` | Danish Krone | 2 | `MYR` | Malaysian Ringgit | 2 |
| `NOK` | Norwegian Krone | 2 | `PHP` | Philippine Peso | 2 |
| `CZK` | Czech Koruna | 2 | `TWD` | New Taiwan Dollar | 2 |
| `HUF` | Hungarian Forint | 2 | `KWD` | Kuwaiti Dinar | 3 |
| `RON` | Romanian Leu | 2 | `BHD` | Bahraini Dinar | 3 |
| `BGN` | Bulgarian Lev | 2 | | | |
| `ISK` | Icelandic Króna | 0 | | | |
| `TRY` | Turkish Lira | 2 | | | |
| `RUB` | Russian Ruble | 2 | | | |

Decimals are the ISO 4217 minor units. They set the divisor of `money_from_cents` and the number
of decimals that `money_format` prints.

## Functions

| Function | Canonical name | Returns |
|----------|----------------|---------|
| [`money`](#money) | `anofox_tab_money` | money |
| [`money_from_cents`](#money_from_cents) | `anofox_tab_money_from_cents` | money |
| [`money_amount`](#money_amount) | `anofox_tab_money_amount` | `DECIMAL(18,3)` |
| [`money_currency`](#money_currency) | `anofox_tab_money_currency` | `VARCHAR` |
| [`money_add`](#money_add) | `anofox_tab_money_add` | money |
| [`money_subtract`](#money_subtract) | `anofox_tab_money_subtract` | money |
| [`money_multiply`](#money_multiply) | `anofox_tab_money_multiply` | money |
| [`money_abs`](#money_abs) | `anofox_tab_money_abs` | money |
| [`money_is_positive`](#money_is_positive) | `anofox_tab_money_is_positive` | `BOOLEAN` |
| [`money_is_negative`](#money_is_negative) | `anofox_tab_money_is_negative` | `BOOLEAN` |
| [`money_is_zero`](#money_is_zero) | `anofox_tab_money_is_zero` | `BOOLEAN` |
| [`money_in_range`](#money_in_range) | `anofox_tab_money_in_range` | `BOOLEAN` |
| [`money_same_currency`](#money_same_currency) | `anofox_tab_money_same_currency` | `BOOLEAN` |
| [`money_format`](#money_format) | `anofox_tab_money_format` | `VARCHAR` |
| [`is_valid_currency`](#is_valid_currency) | `anofox_tab_is_valid_currency` | `BOOLEAN` |
| [`currency_symbol`](#currency_symbol) | `anofox_tab_currency_symbol` | `VARCHAR` |
| [`currency_name`](#currency_name) | `anofox_tab_currency_name` | `VARCHAR` |

All are scalar functions. In the signatures below, `MONEY` stands for
`STRUCT(amount DECIMAL(18,3), currency VARCHAR)`.

---

## Constructing money

### `money`

Canonical name: `anofox_tab_money`. Scalar function.

```text
money(amount DECIMAL(18,3), currency_code VARCHAR) -> MONEY
money(amount DOUBLE, currency_code VARCHAR) -> MONEY
```

- **DECIMAL amounts are exact.** A `DECIMAL` argument of any width and scale, including a literal
  such as `19.99`, uses the `DECIMAL(18,3)` overload: every digit is kept, more than 3 decimal
  places are rounded half away from zero, and a value outside ±999,999,999,999,999.999 raises a
  conversion error.
- **DOUBLE amounts** (and integer amounts, which DuckDB passes as `DOUBLE`) must be finite. They
  are rounded half away from zero to 3 decimal places. A `DOUBLE` holds about 15 significant
  digits, so amounts with more digits than that are not stored exactly: cast them to `DECIMAL`
  first, e.g. `money(amount::DECIMAL(18,3), 'EUR')`. Whole numbers up to the maximum are exact.
  A value that rounds outside the range raises `Out of Range`.
- **String amounts** must be cast. A string literal such as `'1250.00'` fits both overloads, so
  `money('1250.00', 'EUR')` is a binder error, and `TRY()` does not catch it. Write
  `money(CAST('1250.00' AS DECIMAL(18,3)), 'EUR')`, or cast a `VARCHAR` column the same way
  (`TRY_CAST` gives `NULL` for text that is not a number).

```sql
SELECT money_amount(money(123456789012345.678, 'EUR')) AS from_decimal,
       money_amount(money(123456789012345.678::DOUBLE, 'EUR')) AS from_double,
       money_amount(money(999999999999999.999, 'EUR')) AS maximum;
```

```text
┌─────────────────────┬─────────────────────┬─────────────────────┐
│    from_decimal     │     from_double     │       maximum       │
│    decimal(18,3)    │    decimal(18,3)    │    decimal(18,3)    │
├─────────────────────┼─────────────────────┼─────────────────────┤
│ 123456789012345.678 │ 123456789012345.672 │ 999999999999999.999 │
└─────────────────────┴─────────────────────┴─────────────────────┘
```

```sql
SELECT money(1000000000000000.000, 'EUR');
```

```text
Conversion Error: Casting value "1000000000000000.000" to type DECIMAL(18,3) failed: value is out of range!

LINE 1: SELECT money(1000000000000000.000, 'EUR');
                     ^^^^^^^^^^^^^^^^^^^^
```

### `money_from_cents`

Canonical name: `anofox_tab_money_from_cents`. Scalar function.

```text
money_from_cents(cents BIGINT, currency_code VARCHAR) -> MONEY
```

Builds the value exactly from an integer amount in the currency's smallest unit. The amount is
divided by the currency's subunits per unit: 100 for USD and EUR, 1 for JPY, ISK and KRW, 1000
for KWD and BHD.

### `money_amount`

Canonical name: `anofox_tab_money_amount`. Scalar function.

```text
money_amount(money MONEY) -> DECIMAL(18,3)
```

### `money_currency`

Canonical name: `anofox_tab_money_currency`. Scalar function.

```text
money_currency(money MONEY) -> VARCHAR
```

```sql
SELECT money(100.505, 'usd') AS from_double,
       money_from_cents(10050, 'USD') AS from_cents,
       money_from_cents(1000, 'JPY') AS yen;
```

```text
┌────────────────────────────────────────────────┬────────────────────────────────────────────────┬────────────────────────────────────────────────┐
│                  from_double                   │                   from_cents                   │                      yen                       │
│ struct(amount decimal(18,3), currency varchar) │ struct(amount decimal(18,3), currency varchar) │ struct(amount decimal(18,3), currency varchar) │
├────────────────────────────────────────────────┼────────────────────────────────────────────────┼────────────────────────────────────────────────┤
│ {'amount': 100.505, 'currency': USD}           │ {'amount': 100.500, 'currency': USD}           │ {'amount': 1000.000, 'currency': JPY}          │
└────────────────────────────────────────────────┴────────────────────────────────────────────────┴────────────────────────────────────────────────┘
```

```sql
SELECT money_amount(money(2.5, 'eur')) AS amount,
       money_currency(money(2.5, 'eur')) AS currency;
```

```text
┌───────────────┬──────────┐
│    amount     │ currency │
│ decimal(18,3) │ varchar  │
├───────────────┼──────────┤
│         2.500 │ EUR      │
└───────────────┴──────────┘
```

```sql
SELECT money(1.0, 'XYZ');
```

```text
Invalid Input Error: Invalid currency code: XYZ
```

`TRY` turns the error into `NULL`:

```sql
SELECT TRY(money(1.0, 'XYZ')) AS result;
```

```text
┌────────────────────────────────────────────────┐
│                     result                     │
│ struct(amount decimal(18,3), currency varchar) │
├────────────────────────────────────────────────┤
│ NULL                                           │
└────────────────────────────────────────────────┘
```

---

## Arithmetic

### `money_add`

Canonical name: `anofox_tab_money_add`. Scalar function.

```text
money_add(money1 MONEY, money2 MONEY) -> MONEY
```

### `money_subtract`

Canonical name: `anofox_tab_money_subtract`. Scalar function.

```text
money_subtract(money1 MONEY, money2 MONEY) -> MONEY
```

Both arguments must have the same currency; otherwise the call raises an error.

### `money_multiply`

Canonical name: `anofox_tab_money_multiply`. Scalar function.

```text
money_multiply(money MONEY, factor DOUBLE) -> MONEY
```

The result is rounded to 3 decimal places.

### `money_abs`

Canonical name: `anofox_tab_money_abs`. Scalar function.

```text
money_abs(money MONEY) -> MONEY
```

```sql
SELECT money_add(money(100, 'EUR'), money(50.25, 'EUR')) AS added,
       money_subtract(money(1, 'USD'), money(3, 'USD')) AS subtracted,
       money_multiply(money(10, 'USD'), 1.0 / 3) AS multiplied,
       money_abs(money(-5, 'USD')) AS absolute;
```

```text
┌────────────────────────────────────────────────┬────────────────────────────────────────────────┬────────────────────────────────────────────────┬────────────────────────────────────────────────┐
│                     added                      │                   subtracted                   │                   multiplied                   │                    absolute                    │
│ struct(amount decimal(18,3), currency varchar) │ struct(amount decimal(18,3), currency varchar) │ struct(amount decimal(18,3), currency varchar) │ struct(amount decimal(18,3), currency varchar) │
├────────────────────────────────────────────────┼────────────────────────────────────────────────┼────────────────────────────────────────────────┼────────────────────────────────────────────────┤
│ {'amount': 150.250, 'currency': EUR}           │ {'amount': -2.000, 'currency': USD}            │ {'amount': 3.333, 'currency': USD}             │ {'amount': 5.000, 'currency': USD}             │
└────────────────────────────────────────────────┴────────────────────────────────────────────────┴────────────────────────────────────────────────┴────────────────────────────────────────────────┘
```

```sql
SELECT money_add(money(1, 'USD'), money(1, 'EUR'));
```

```text
Invalid Input Error: Cannot operate on money with different currencies: USD and EUR
```

```sql
SELECT money_multiply(money(1e14, 'USD'), 1000);
```

```text
Out of Range Error: Money multiplication result is out of range for DECIMAL(18,3)
```

### Totals per currency

`sum(amount)` over a money column is a binder error: `sum` has no variant for the money struct,
and adding amounts of different currencies would not be meaningful anyway.

```sql
SELECT sum(amount) FROM invoices;
```

```text
Binder Error: No function matches the given name and argument types 'sum(STRUCT(amount DECIMAL(18,3), currency VARCHAR))'. You might need to add explicit type casts.
```

Group by the currency and sum the exact `DECIMAL` amounts instead:

```sql
CREATE TABLE invoices AS
SELECT invoice_no, money(amount, currency) AS amount
FROM (VALUES ('RE-2026-0101', 1250.00, 'EUR'), ('RE-2026-0102', 7800.50, 'EUR'),
             ('RE-2026-0103', 19.90, 'EUR'), ('RE-2026-0104', 2600.00, 'CHF'),
             ('RE-2026-0105', 75.00, 'CHF'), ('RE-2026-0106', 480.25, 'GBP'))
     t(invoice_no, amount, currency);

SELECT money_currency(amount) AS currency,
       count(*) AS invoices,
       sum(money_amount(amount)) AS total
FROM invoices
GROUP BY ALL
ORDER BY currency;
```

```text
┌──────────┬──────────┬───────────────┐
│ currency │ invoices │     total     │
│ varchar  │  int64   │ decimal(38,3) │
├──────────┼──────────┼───────────────┤
│ CHF      │        2 │      2675.000 │
│ EUR      │        3 │      9070.400 │
│ GBP      │        1 │       480.250 │
└──────────┴──────────┴───────────────┘
```

To get money values back, for example to format them, wrap each total in `money`:

```sql
SELECT currency, money_format(money(total, currency), 'symbol') AS total
FROM (SELECT money_currency(amount) AS currency, sum(money_amount(amount)) AS total
      FROM invoices GROUP BY ALL)
ORDER BY currency;
```

```text
┌──────────┬──────────────┐
│ currency │    total     │
│ varchar  │   varchar    │
├──────────┼──────────────┤
│ CHF      │ 2'675.00 CHF │
│ EUR      │ 9.070,40 €   │
│ GBP      │ £480.25      │
└──────────┴──────────────┘
```

A total outside the `DECIMAL(18,3)` range cannot become a money value: the cast of the `DECIMAL(38,3)` sum to `DECIMAL(18,3)` raises a `Conversion Error`.

---

## Tests

### `money_is_positive`

Canonical name: `anofox_tab_money_is_positive`. Scalar function. `true` when the amount is
greater than 0.

```text
money_is_positive(money MONEY) -> BOOLEAN
```

### `money_is_negative`

Canonical name: `anofox_tab_money_is_negative`. Scalar function. `true` when the amount is less
than 0.

```text
money_is_negative(money MONEY) -> BOOLEAN
```

### `money_is_zero`

Canonical name: `anofox_tab_money_is_zero`. Scalar function. `true` when the amount is 0.

```text
money_is_zero(money MONEY) -> BOOLEAN
```

### `money_in_range`

Canonical name: `anofox_tab_money_in_range`. Scalar function. `true` when
`min <= amount <= max`. The bounds follow the same rules as the
[check functions](07-data-quality.md):

```text
money_in_range(money MONEY, min DOUBLE, max DOUBLE) -> BOOLEAN
```

- A `NULL` bound means no bound on that side: `money_in_range(m, 0, NULL)` is `amount >= 0`,
  and with both bounds `NULL` the result is `true` for every non-`NULL` money value.
- A `NULL` money value returns `NULL`.
- `min > max` and a `NaN` bound raise an `Invalid Input Error`. The function is fallible, so
  `TRY(money_in_range(...))` returns `NULL` for that row instead.

Open invoices against optional limits per customer (a customer without a credit limit has
`NULL` in `credit_limit`):

```sql
CREATE TABLE open_invoices AS
SELECT invoice_no, money(amount, currency) AS amount, min_order, credit_limit
FROM (VALUES ('RE-2026-0101', 1250.00, 'EUR', 50.00, 5000.00),
             ('RE-2026-0102', 7800.50, 'EUR', 50.00, 5000.00),
             ('RE-2026-0103',   19.90, 'EUR', 50.00, NULL),
             ('RE-2026-0104', 9999.99, 'EUR', NULL,  NULL))
     t(invoice_no, amount, currency, min_order, credit_limit);

SELECT invoice_no, min_order, credit_limit,
       money_in_range(amount, min_order, credit_limit) AS within_limits
FROM open_invoices ORDER BY invoice_no;
```

```text
┌──────────────┬──────────────┬──────────────┬───────────────┐
│  invoice_no  │  min_order   │ credit_limit │ within_limits │
│   varchar    │ decimal(4,2) │ decimal(6,2) │    boolean    │
├──────────────┼──────────────┼──────────────┼───────────────┤
│ RE-2026-0101 │        50.00 │      5000.00 │ true          │
│ RE-2026-0102 │        50.00 │      5000.00 │ false         │
│ RE-2026-0103 │        50.00 │         NULL │ false         │
│ RE-2026-0104 │         NULL │         NULL │ true          │
└──────────────┴──────────────┴──────────────┴───────────────┘
```

```sql
SELECT money_in_range(money(100, 'EUR'), 5000, 50);
```

```text
Invalid Input Error: money_in_range: min (5000.0) must not be greater than max (50.0)
```

```sql
SELECT money_in_range(money(100, 'EUR'), 'nan'::DOUBLE, 500);
```

```text
Invalid Input Error: money_in_range: min must not be NaN (NULL means no lower bound)
```

### `money_same_currency`

Canonical name: `anofox_tab_money_same_currency`. Scalar function. `true` when both values have
the same currency.

```text
money_same_currency(money1 MONEY, money2 MONEY) -> BOOLEAN
```

```sql
CREATE TABLE payments AS
SELECT id, money(amount, currency) AS amount
FROM (VALUES (1, 120.00, 'EUR'), (2, -15.50, 'EUR'), (3, 0.00, 'USD'), (4, 99999.99, 'GBP'))
     t(id, amount, currency);

SELECT id,
       money_is_positive(amount) AS positive,
       money_is_negative(amount) AS negative,
       money_is_zero(amount) AS zero,
       money_in_range(amount, 0.01, 10000) AS in_range,
       money_same_currency(amount, money(1, 'EUR')) AS is_eur
FROM payments ORDER BY id;
```

```text
┌───────┬──────────┬──────────┬─────────┬──────────┬─────────┐
│  id   │ positive │ negative │  zero   │ in_range │ is_eur  │
│ int32 │ boolean  │ boolean  │ boolean │ boolean  │ boolean │
├───────┼──────────┼──────────┼─────────┼──────────┼─────────┤
│     1 │ true     │ false    │ false   │ true     │ true    │
│     2 │ false    │ true     │ false   │ false    │ true    │
│     3 │ false    │ false    │ true    │ false    │ false   │
│     4 │ true     │ false    │ false   │ false    │ false   │
└───────┴──────────┴──────────┴─────────┴──────────┴─────────┘
```

---

## Formatting

### `money_format`

Canonical name: `anofox_tab_money_format`. Scalar function.

```text
money_format(money MONEY, format_style VARCHAR) -> VARCHAR
```

| Style | Example | Description |
|-------|---------|-------------|
| `symbol` | `$1,234,567.89`, `1.234.567,89 €` | Currency symbol, with the currency's thousands separator and decimal mark. A negative amount puts the minus sign first: `-$42.50`, `-42,50 €` |
| `long` | `1.234,50 Euro` | Currency name, localized number |
| `code` | `1234.50 EUR` | ISO code, dot as decimal mark, no grouping |

The number of decimals follows the currency: 2 for USD and EUR, none for JPY. The style names
are case-insensitive (`'Symbol'` is `symbol`); any other style raises an error. A `NULL` money
value or style returns `NULL`. The style has no default: `money_format(m)` with one argument is a
binder error.

```sql
SELECT money_format(money(1234567.891, 'USD'), 'symbol') AS usd,
       money_format(money(1234567.891, 'EUR'), 'symbol') AS eur,
       money_format(money(1234.5, 'JPY'), 'symbol') AS jpy,
       money_format(money(1234.5, 'EUR'), 'long') AS eur_long,
       money_format(money(1234.5, 'EUR'), 'code') AS eur_code;
```

```text
┌───────────────┬────────────────┬─────────┬───────────────┬─────────────┐
│      usd      │      eur       │   jpy   │   eur_long    │  eur_code   │
│    varchar    │    varchar     │ varchar │    varchar    │   varchar   │
├───────────────┼────────────────┼─────────┼───────────────┼─────────────┤
│ $1,234,567.89 │ 1.234.567,89 € │ ¥1,235  │ 1.234,50 Euro │ 1234.50 EUR │
└───────────────┴────────────────┴─────────┴───────────────┴─────────────┘
```

Negative amounts:

```sql
SELECT money_format(money(-42.5, 'USD'), 'symbol') AS usd,
       money_format(money(-42.5, 'EUR'), 'symbol') AS eur,
       money_format(money(-42.5, 'EUR'), 'long') AS eur_long,
       money_format(money(-42.5, 'EUR'), 'code') AS eur_code;
```

```text
┌─────────┬──────────┬─────────────┬────────────┐
│   usd   │   eur    │  eur_long   │  eur_code  │
│ varchar │ varchar  │   varchar   │  varchar   │
├─────────┼──────────┼─────────────┼────────────┤
│ -$42.50 │ -42,50 € │ -42,50 Euro │ -42.50 EUR │
└─────────┴──────────┴─────────────┴────────────┘
```

An unknown style:

```sql
SELECT money_format(money(19.99, 'EUR'), 'bogus');
```

```text
Invalid Input Error: Unsupported money format style: bogus (expected 'symbol', 'long' or 'code')
```

---

## Currency information

### `is_valid_currency`

Canonical name: `anofox_tab_is_valid_currency`. Scalar function. `true` for one of the 38
[supported codes](#supported-currencies), case-insensitive and ignoring surrounding whitespace.

```text
is_valid_currency(currency_code VARCHAR) -> BOOLEAN
```

### `currency_symbol`

Canonical name: `anofox_tab_currency_symbol`. Scalar function.

```text
currency_symbol(currency_code VARCHAR) -> VARCHAR
```

### `currency_name`

Canonical name: `anofox_tab_currency_name`. Scalar function.

```text
currency_name(currency_code VARCHAR) -> VARCHAR
```

`currency_symbol` and `currency_name` return `NULL` for an unsupported code, like
`vat_country_name`, so they run over unclean columns without `TRY`:

```sql
SELECT currency_symbol('BTC') AS symbol, currency_name('BTC') AS name;
```

```text
┌─────────┬─────────┐
│ symbol  │  name   │
│ varchar │ varchar │
├─────────┼─────────┤
│ NULL    │ NULL    │
└─────────┴─────────┘
```

```sql
SELECT code, currency_symbol(code) AS symbol, currency_name(code) AS currency
FROM (VALUES ('USD'), ('eur'), ('GBP'), ('JPY'), ('CAD'), ('AUD'), ('CHF'), ('CNY'), ('INR'), ('BRL'))
     t(code);
```

```text
┌─────────┬─────────┬──────────────────────┐
│  code   │ symbol  │       currency       │
│ varchar │ varchar │       varchar        │
├─────────┼─────────┼──────────────────────┤
│ USD     │ $       │ United States Dollar │
│ eur     │ €       │ Euro                 │
│ GBP     │ £       │ British Pound        │
│ JPY     │ ¥       │ Japanese Yen         │
│ CAD     │ $       │ Canadian Dollar      │
│ AUD     │ $       │ Australian Dollar    │
│ CHF     │ CHF     │ Swiss Franc          │
│ CNY     │ ¥       │ Chinese Yuan         │
│ INR     │ ₹       │ Indian Rupee         │
│ BRL     │ R$      │ Brazilian Real       │
└─────────┴─────────┴──────────────────────┘
  10 rows                        3 columns
```

```sql
SELECT is_valid_currency('SEK') AS sek, is_valid_currency(' usd ') AS usd, is_valid_currency('XYZ') AS xyz;
```

```text
┌─────────┬─────────┬─────────┐
│   sek   │   usd   │   xyz   │
│ boolean │ boolean │ boolean │
├─────────┼─────────┼─────────┤
│ true    │ true    │ false   │
└─────────┴─────────┴─────────┘
```

## See also

- [VAT numbers](05-vat.md)
- [Conventions](00-conventions.md)

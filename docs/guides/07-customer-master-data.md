# Clean a Customer Master

> Validate and standardise the contact data of a customer table from several countries.

**Use this guide to:**
- Normalise and validate email addresses, and see why one fails
- Parse phone numbers with each customer's country as the region, split off extensions, and format them
- Check EU VAT numbers that are stored without or with their country prefix
- Turn amounts and currency codes into money values without failing on a bad code
- Score each column so you know how clean the table is

Every example creates its own data. Run them in order in one DuckDB session, after
`LOAD anofox_tabular;`.

## 1. The table

A customer master merged from several systems: emails with stray spaces and capitals, phone
numbers in local notation with labels, extensions and a typo, VAT numbers with and without the country prefix, and amounts with
free-text currency codes:

```sql
CREATE TABLE customers AS SELECT * FROM (VALUES
  (1, 'DE', ' Anna.Schmidt@Example.com ', 'Tel. 030 12345678 ext. 12', 'DE136695976',   1250.00, 'EUR'),
  (2, 'GB', 'tom@example.co.uk',          '07400 123456',      'GB123456782',    880.50, 'gbp'),
  (3, 'FR', 'claire@exemple',             '01 23 45 67 89',    'FR40303265045',  410.00, ' EUR'),
  (4, 'CH', 'luca@example.ch',            '044 668 18 0O',     NULL,             99.90, 'CHF'),
  (5, 'NL', 'not-an-email',               '020 123 4567',      'NL004495445B01', 310.00, 'EURO'),
  (6, 'AT', 'eva@example.at',             '+43 1 58800',       'ATU13585627',    720.00, 'EUR')
) t(id, country, email, phone, vat, amount, currency);
```

## 2. Emails

`email_validate` ignores the spaces around an address, so validate the column as it is; store
the trimmed, lower-cased form. `email_validate` returns the stage that decided and a `reason`
when the address fails:

```sql
SELECT id,
       lower(trim(email)) AS email_clean,
       (email_validate(email, 'regex')).valid AS valid,
       (email_validate(email, 'regex')).reason AS reason
FROM customers
ORDER BY id;
```

```text
┌───────┬──────────────────────────┬─────────┬────────────────┐
│  id   │       email_clean        │  valid  │     reason     │
│ int32 │         varchar          │ boolean │    varchar     │
├───────┼──────────────────────────┼─────────┼────────────────┤
│     1 │ anna.schmidt@example.com │ true    │ NULL           │
│     2 │ tom@example.co.uk        │ true    │ NULL           │
│     3 │ claire@exemple           │ true    │ NULL           │
│     4 │ luca@example.ch          │ true    │ NULL           │
│     5 │ not-an-email             │ false   │ invalid_format │
│     6 │ eva@example.at           │ true    │ NULL           │
└───────┴──────────────────────────┴─────────┴────────────────┘
```

The `regex` mode checks the format only, so `claire@exemple` passes although its domain has no
top-level domain. The `dns` mode also looks up the domain's mail servers, which catches such
addresses and typos like `gmail.con`. It needs network access and caches one result
per domain:

```sql no-run
SELECT id, (email_validate(email, 'dns')).valid,
       (email_validate(email, 'dns')).reason
FROM customers;
```

When a lookup cannot finish (a timeout, an unreachable DNS server), `valid` is `NULL` rather than
`false`, and `reason` says what happened (`dns_timeout`, `dns_server_unreachable`): the address is
not known to be bad. Section 6 shows how to score such rows.

See [Email](../api/01-email.md) for all reason codes.

## 3. Phone numbers

Pass each customer's country as the region, so local numbers like `030 12345678` are read
correctly. The region is an ISO 3166 alpha-2 code (`'DE'`, `'GB'`; `'UK'` is accepted as `'GB'`).
The phone module knows 21 regions: US, CA, GB, DE, FR, IT, ES, AU, JP, CN, IN, BR, MX, AR, RU,
NL, SE, NO, DK, FI and CH. A number of any other country, for example Austria (AT), Belgium
(BE), the Czech Republic (CZ) or Poland (PL), is invalid with reason `unsupported_country_code`
when it is written with `+`, or `unsupported_region` when it is written locally with that
country as the region. A leading label such as `Tel.` is dropped and a trailing extension (`ext. 12`, `x12`,
`Durchwahl 12`) is returned in `extension` instead of being read as part of the number. Any
other letter makes a number invalid: the Swiss number has a letter `O` typed for a zero. `reason`
says why a number is rejected; Austria is not one of the supported regions (`+43` is written
with its country code, so the reason is `unsupported_country_code`):

```sql
SELECT id, country,
       (phonenumber_parse(phone, country)).valid AS valid,
       (phonenumber_parse(phone, country)).reason AS reason,
       (phonenumber_parse(phone, country)).extension AS ext,
       phonenumber_format(phone, country, 'E164') AS e164,
       phonenumber_format(phone, country, 'INTERNATIONAL') AS international
FROM customers
ORDER BY id;
```

```text
┌───────┬─────────┬─────────┬──────────────────────────┬─────────┬───────────────┬───────────────────┐
│  id   │ country │  valid  │          reason          │   ext   │     e164      │   international   │
│ int32 │ varchar │ boolean │         varchar          │ varchar │    varchar    │      varchar      │
├───────┼─────────┼─────────┼──────────────────────────┼─────────┼───────────────┼───────────────────┤
│     1 │ DE      │ true    │ NULL                     │ 12      │ +493012345678 │ +49 30 12345678   │
│     2 │ GB      │ true    │ NULL                     │ NULL    │ +447400123456 │ +44 7400 123456   │
│     3 │ FR      │ true    │ NULL                     │ NULL    │ +33123456789  │ +33 1 23 45 67 89 │
│     4 │ CH      │ false   │ invalid_characters       │ NULL    │ NULL          │ NULL              │
│     5 │ NL      │ true    │ NULL                     │ NULL    │ +31201234567  │ +31 20 123 4567   │
│     6 │ AT      │ false   │ unsupported_country_code │ NULL    │ NULL          │ NULL              │
└───────┴─────────┴─────────┴──────────────────────────┴─────────┴───────────────┴───────────────────┘
```

Store the `E164` form; it is unique per number and the other formats can be derived from it.
`phonenumber_format` returns `NULL` for every number that `phonenumber_is_valid` rejects, also
for numbers of a possible length outside an assigned range, so the stored column holds only valid
numbers and `reason` explains the gaps. See [Phone numbers](../api/03-phone.md) for the supported
regions.

Without a region (a `NULL` country), a local number is read in the default region, `US` unless
`anofox_tab_phonenumber_default_region` is set. It then fails with reason `region_missing`, and
the `region` field shows the region it was read in, so a missing country column is not mistaken
for a bad number:

```sql
SELECT (phonenumber_parse('030 12345678', NULL)).region AS region,
       (phonenumber_parse('030 12345678', NULL)).reason AS reason;
```

```text
┌─────────┬────────────────┐
│ region  │     reason     │
│ varchar │    varchar     │
├─────────┼────────────────┤
│ US      │ region_missing │
└─────────┴────────────────┘
```

## 4. VAT numbers

The VAT functions expect the country prefix. Add it where it is missing, then validate:

```sql
SELECT id, vat,
       vat_normalize(CASE WHEN regexp_matches(vat, '^[A-Za-z]{2}') THEN vat ELSE country || vat END) AS vat_clean,
       vat_is_valid(CASE WHEN regexp_matches(vat, '^[A-Za-z]{2}') THEN vat ELSE country || vat END) AS valid
FROM customers
WHERE vat IS NOT NULL
ORDER BY id;
```

```text
┌───────┬────────────────┬────────────────┬─────────┐
│  id   │      vat       │   vat_clean    │  valid  │
│ int32 │    varchar     │    varchar     │ boolean │
├───────┼────────────────┼────────────────┼─────────┤
│     1 │ DE136695976    │ DE136695976    │ true    │
│     2 │ GB123456782    │ GB123456782    │ true    │
│     3 │ FR40303265045  │ FR40303265045  │ true    │
│     5 │ NL004495445B01 │ NL004495445B01 │ true    │
│     6 │ ATU13585627    │ ATU13585627    │ true    │
└───────┴────────────────┴────────────────┴─────────┘
```

`vat_is_valid` checks the format of every country and the check digits of most of them; it does
not ask VIES whether the number is registered. When a number fails, `vat_validate` says why:

```sql
SELECT v AS vat, (vat_validate(v)).reason AS reason
FROM (VALUES ('DE136695977'), ('DE13669597'), ('136695976'), ('UK123456789')) t(v);
```

```text
┌─────────────┬─────────────────────┐
│     vat     │       reason        │
│   varchar   │       varchar       │
├─────────────┼─────────────────────┤
│ DE136695977 │ checksum_mismatch   │
│ DE13669597  │ invalid_format      │
│ 136695976   │ missing_prefix      │
│ UK123456789 │ unsupported_country │
└─────────────┴─────────────────────┘
```

See [VAT](../api/05-vat.md).

## 5. Money

`money` raises an error for a currency code it does not know, which would stop the whole query.
Find those codes first with `is_valid_currency`, which returns `false` instead of raising, or
wrap `money` in `TRY()` to get `NULL` for those rows; surrounding spaces and lower case are
accepted by both:

```sql
SELECT id, currency,
       is_valid_currency(currency) AS known_code,
       TRY(money(amount, currency)) AS amount_money,
       TRY(money(amount, currency)) IS NULL AS needs_review
FROM customers
ORDER BY id;
```

```text
┌───────┬──────────┬────────────┬────────────────────────────────────────────────┬──────────────┐
│  id   │ currency │ known_code │                  amount_money                  │ needs_review │
│ int32 │ varchar  │  boolean   │ struct(amount decimal(18,3), currency varchar) │   boolean    │
├───────┼──────────┼────────────┼────────────────────────────────────────────────┼──────────────┤
│     1 │ EUR      │ true       │ {'amount': 1250.000, 'currency': EUR}          │ false        │
│     2 │ gbp      │ true       │ {'amount': 880.500, 'currency': GBP}           │ false        │
│     3 │  EUR     │ true       │ {'amount': 410.000, 'currency': EUR}           │ false        │
│     4 │ CHF      │ true       │ {'amount': 99.900, 'currency': CHF}            │ false        │
│     5 │ EURO     │ false      │ NULL                                           │ true         │
│     6 │ EUR      │ true       │ {'amount': 720.000, 'currency': EUR}           │ false        │
└───────┴──────────┴────────────┴────────────────────────────────────────────────┴──────────────┘
```

When the amount is a string, for example from a CSV file read as text, cast it to
`DECIMAL(18,3)` first. `money('1250.00', 'EUR')` is a binder error, because a string literal
fits both the `DOUBLE` and the `DECIMAL` variant of `money`, and `TRY()` does not catch binder
errors:

```sql
SELECT TRY(money(CAST('1250.00' AS DECIMAL(18,3)), 'EUR')) AS amount_money;
```

```text
┌────────────────────────────────────────────────┐
│                  amount_money                  │
│ struct(amount decimal(18,3), currency varchar) │
├────────────────────────────────────────────────┤
│ {'amount': 1250.000, 'currency': EUR}          │
└────────────────────────────────────────────────┘
```

See [Money](../api/04-money.md) for the supported currencies.

## 6. Score the table

[`compliance`](../api/07-data-quality.md#compliance) gives the share of rows for which an
expression holds. One row per column shows where the cleanup effort goes:

```sql
SELECT 'email' AS field, round(compliance_rate, 3) AS compliance_rate
FROM compliance('customers', '(email_validate(email, ''regex'')).valid')
UNION ALL
SELECT 'phone', round(compliance_rate, 3)
FROM compliance('customers', 'phonenumber_is_valid(phone, country)')
UNION ALL
SELECT 'currency', round(compliance_rate, 3)
FROM compliance('customers', 'TRY(money(amount, currency)) IS NOT NULL')
ORDER BY field;
```

```text
┌──────────┬─────────────────┐
│  field   │ compliance_rate │
│ varchar  │     double      │
├──────────┼─────────────────┤
│ currency │           0.833 │
│ email    │           0.833 │
│ phone    │           0.667 │
└──────────┴─────────────────┘
```

`compliance` counts a row whose expression is `NULL` as not compliant. That matters for checks
that can end without an answer, such as `email_is_valid(email, 'dns')`, which is `NULL` when the
DNS lookup timed out. Choose what an unknown result counts as in the expression itself:

```sql no-run
-- Uses the network. Unknown outcomes count against the score (strict) ...
SELECT round(compliance_rate, 3)
FROM compliance('customers', 'email_is_valid(email, ''dns'')');
-- ... or are given the benefit of the doubt
SELECT round(compliance_rate, 3)
FROM compliance('customers', 'coalesce(email_is_valid(email, ''dns''), true)');
```

To leave them out of the score entirely, score a view of the rows with a known result, e.g.
`CREATE VIEW checked AS SELECT * FROM customers WHERE email_is_valid(email, 'dns') IS NOT NULL`,
and run `compliance('checked', ...)`. Re-check the excluded rows later.

## 7. Addresses

`postal_parse_address` splits an address into its parts. It needs the libpostal data, and is
not available on Windows.

> **About 500 MB download.** If the data is missing, the first `postal_parse_address` call
> downloads it, inside your query, and the query waits for it. Download it on purpose first,
> once per machine; it goes to `~/.duckdb/extensions/libpostal` (set
> `anofox_tab_postal_data_path` before the first postal call to choose another directory):
>
> ```sql no-run
> SELECT postal_load_data();
> SELECT data_present, data_dir FROM postal_status();
> ```

```sql no-run
SELECT postal_parse_address('Hauptstraße 5, 80331 München, Deutschland');
-- {'house_number': 5, 'road': hauptstraße, 'city': münchen, 'state': NULL,
--  'postcode': 80331, 'country': deutschland}
```

The parts are lower-cased, and `country` is the country name as written in the address
(`deutschland`), not an ISO code; it is `NULL` when the address has no country. Keep the
`country` column of the table as the ISO code, and store the parsed parts next to the original
address rather than replacing it.

See [Postal addresses](../api/02-postal.md).

## Next steps

- [Guide: PII](04-pii.md), to find and mask the personal data in the same table
- [Guide: data quality checks](02-data-quality-checks.md), to run these checks on every load
- [Conventions: errors and `TRY()`](../api/00-conventions.md)

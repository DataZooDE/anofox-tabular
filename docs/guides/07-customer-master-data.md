# Clean a Customer Master

> Validate and standardise the contact data of a customer table from several countries.

**Use this guide to:**
- Normalise and validate email addresses, and see why one fails
- Parse phone numbers with each customer's country as the region, and format them
- Check EU VAT numbers that are stored without or with their country prefix
- Turn amounts and currency codes into money values without failing on a bad code
- Score each column so you know how clean the table is

Every example creates its own data. Run them in order in one DuckDB session, after
`LOAD anofox_tabular;`.

## 1. The table

A customer master merged from several systems: emails with stray spaces and capitals, phone
numbers in local notation, VAT numbers with and without the country prefix, and amounts with
free-text currency codes:

```sql
CREATE TABLE customers AS SELECT * FROM (VALUES
  (1, 'DE', ' Anna.Schmidt@Example.com ', '030 12345678',      'DE136695976',   1250.00, 'EUR'),
  (2, 'GB', 'tom@example.co.uk',          '07400 123456',      'GB123456782',    880.50, 'gbp'),
  (3, 'FR', 'claire@exemple',             '01 23 45 67 89',    'FR40303265045',  410.00, ' EUR'),
  (4, 'CH', 'luca@example.ch',            '044 668 18 00',     NULL,             99.90, 'CHF'),
  (5, 'NL', 'not-an-email',               '020 123 4567',      'NL004495445B01', 310.00, 'EURO'),
  (6, 'AT', 'eva@example.at',             '+43 1 58800',       'ATU13585627',    720.00, 'EUR')
) t(id, country, email, phone, vat, amount, currency);
```

## 2. Emails

Trim and lower-case the addresses first, then validate them. `email_validate` returns the
stage that decided and a `reason` when the address fails:

```sql
SELECT id,
       lower(trim(email)) AS email_clean,
       (email_validate(lower(trim(email)), 'regex')).valid AS valid,
       (email_validate(lower(trim(email)), 'regex')).reason AS reason
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
SELECT id, (email_validate(lower(trim(email)), 'dns')).reason
FROM customers;
```

See [Email](../api/01-email.md) for all reason codes.

## 3. Phone numbers

Pass each customer's country as the region, so local numbers like `030 12345678` are read
correctly. `reason` says why a number is rejected; Austria is not one of the supported regions:

```sql
SELECT id, country,
       (phonenumber_parse(phone, country)).valid AS valid,
       (phonenumber_parse(phone, country)).reason AS reason,
       phonenumber_format(phone, country, 'E164') AS e164,
       phonenumber_format(phone, country, 'INTERNATIONAL') AS international
FROM customers
ORDER BY id;
```

```text
┌───────┬─────────┬─────────┬──────────────────────────┬───────────────┬───────────────────┐
│  id   │ country │  valid  │          reason          │     e164      │   international   │
│ int32 │ varchar │ boolean │         varchar          │    varchar    │      varchar      │
├───────┼─────────┼─────────┼──────────────────────────┼───────────────┼───────────────────┤
│     1 │ DE      │ true    │ NULL                     │ +493012345678 │ +49 30 12345678   │
│     2 │ GB      │ true    │ NULL                     │ +447400123456 │ +44 7400 123456   │
│     3 │ FR      │ true    │ NULL                     │ +33123456789  │ +33 1 23 45 67 89 │
│     4 │ CH      │ true    │ NULL                     │ +41446681800  │ +41 44 668 18 00  │
│     5 │ NL      │ true    │ NULL                     │ +31201234567  │ +31 20 123 4567   │
│     6 │ AT      │ false   │ unsupported_country_code │ NULL          │ NULL              │
└───────┴─────────┴─────────┴──────────────────────────┴───────────────┴───────────────────┘
```

Store the `E164` form; it is unique per number and the other formats can be derived from it. See
[Phone numbers](../api/03-phone.md) for the supported regions.

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
not ask VIES whether the number is registered. See [VAT](../api/05-vat.md).

## 5. Money

`money` raises an error for a currency code it does not know, which would stop the whole query.
Wrap it in `TRY()` to get `NULL` for those rows instead; surrounding spaces and lower case are
accepted:

```sql
SELECT id, currency,
       TRY(money(amount, currency)) AS amount_money,
       TRY(money(amount, currency)) IS NULL AS needs_review
FROM customers
ORDER BY id;
```

```text
┌───────┬──────────┬────────────────────────────────────────────────┬──────────────┐
│  id   │ currency │                  amount_money                  │ needs_review │
│ int32 │ varchar  │ struct(amount decimal(18,3), currency varchar) │   boolean    │
├───────┼──────────┼────────────────────────────────────────────────┼──────────────┤
│     1 │ EUR      │ {'amount': 1250.000, 'currency': EUR}          │ false        │
│     2 │ gbp      │ {'amount': 880.500, 'currency': GBP}           │ false        │
│     3 │  EUR     │ {'amount': 410.000, 'currency': EUR}           │ false        │
│     4 │ CHF      │ {'amount': 99.900, 'currency': CHF}            │ false        │
│     5 │ EURO     │ NULL                                           │ true         │
│     6 │ EUR      │ {'amount': 720.000, 'currency': EUR}           │ false        │
└───────┴──────────┴────────────────────────────────────────────────┴──────────────┘
```

See [Money](../api/04-money.md) for the supported currencies.

## 6. Score the table

[`compliance`](../api/07-data-quality.md#compliance) gives the share of rows for which an
expression holds. One row per column shows where the cleanup effort goes:

```sql
SELECT 'email' AS field, round(compliance_rate, 3) AS compliance_rate
FROM compliance('customers', '(email_validate(lower(trim(email)), ''regex'')).valid')
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
│ phone    │           0.833 │
└──────────┴─────────────────┘
```

## 7. Addresses

`postal_parse_address` splits an address into its parts. It needs the libpostal data (about
500 MB, downloaded on first use) and is not available on Windows:

```sql no-run
SELECT postal_parse_address('Hauptstraße 5, 80331 München');
```

See [Postal addresses](../api/02-postal.md).

## Next steps

- [Guide: PII](04-pii.md), to find and mask the personal data in the same table
- [Guide: data quality checks](02-data-quality-checks.md), to run these checks on every load
- [Conventions: errors and `TRY()`](../api/00-conventions.md)

# Find and Mask PII

> Locate personal data in a table, inspect it row by row, and mask it before you share the data.

**Use this guide to:**
- Find which columns of a table contain PII
- Inspect each finding with the key of its row
- Prove which columns a scan covered
- Mask PII for an export, with the strategy that fits each column
- Restrict detection to the types you care about
- Run without network access

Every step runs in a fresh DuckDB session. Start with the extension loaded:

```sql no-run
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;
```

## 1. Create sample data

`pii_scan_table` and `pii_audit_table` read regular tables and views, not TEMP tables.

```sql
CREATE TABLE customers AS
SELECT * FROM (VALUES
    (101, 'Anna',  'anna@example.com',  '123-45-6789', '4111 1111 1111 1111', '01511234567', DATE '2024-02-01', 'B2C', 'Call me on +1 (415) 555-1234'),
    (102, 'Li',    'bob@example.org',   NULL,          NULL,                  NULL,          DATE '2024-05-17', 'B2B', 'No contact details here'),
    (103, 'Bo',    'not an email',      '219-09-9999', NULL,                  '030 1234567', DATE '2025-01-09', 'B2C', 'Wire to DE89370400440532013000'),
    (104, 'Carol', 'carol@example.net', NULL,          '5500 0000 0000 0004', NULL,          DATE '2025-03-30', 'B2B', 'Server 192.168.1.20 is down')
) t(customer_id, first_name, email, ssn, card, phone, signup_date, segment, note);
```

## 2. Find the PII columns

`pii_scan_table` checks every column that holds text (`VARCHAR`, `JSON`, and `STRUCT`, `LIST` or
`MAP` values with text inside) and returns one row per column and PII type:

```sql
SELECT column_name, pii_type, match_count
FROM pii_scan_table('customers')
WHERE pii_type IS NOT NULL
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬─────────────┬─────────────┐
│ column_name │  pii_type   │ match_count │
│   varchar   │   varchar   │    int64    │
├─────────────┼─────────────┼─────────────┤
│ card        │ CREDIT_CARD │           2 │
│ email       │ EMAIL       │           3 │
│ first_name  │ NAME        │           3 │
│ note        │ IBAN        │           1 │
│ note        │ IP_ADDRESS  │           1 │
│ note        │ PHONE       │           1 │
│ phone       │ PHONE       │           1 │
│ ssn         │ US_SSN      │           2 │
└─────────────┴─────────────┴─────────────┘
```

The free-text column `note` holds three different types. Those are the columns to treat with
care.

The scan also returns `sample_values`, up to five matched values per row. They are raw PII, so
keep the scan result as protected as the table.

### Prove what the scan covered

Without the `WHERE` clause the scan accounts for every column: `status` says whether a column was
scanned and found to hold PII (`scanned`), scanned without a match (`no_match`) or not scanned
because it holds no text (`skipped (INTEGER)`, `skipped (DATE)`). `values_scanned` counts the
non-`NULL` values that were checked:

```sql
SELECT column_name, pii_type, match_count, values_scanned, status
FROM pii_scan_table('customers');
```

```text
┌─────────────┬─────────────┬─────────────┬────────────────┬───────────────────┐
│ column_name │  pii_type   │ match_count │ values_scanned │      status       │
│   varchar   │   varchar   │    int64    │     int64      │      varchar      │
├─────────────┼─────────────┼─────────────┼────────────────┼───────────────────┤
│ customer_id │ NULL        │           0 │              0 │ skipped (INTEGER) │
│ first_name  │ NAME        │           3 │              4 │ scanned           │
│ email       │ EMAIL       │           3 │              4 │ scanned           │
│ ssn         │ US_SSN      │           2 │              2 │ scanned           │
│ card        │ CREDIT_CARD │           2 │              2 │ scanned           │
│ phone       │ PHONE       │           1 │              2 │ scanned           │
│ signup_date │ NULL        │           0 │              0 │ skipped (DATE)    │
│ segment     │ NULL        │           0 │              4 │ no_match          │
│ note        │ IBAN        │           1 │              4 │ scanned           │
│ note        │ IP_ADDRESS  │           1 │              4 │ scanned           │
│ note        │ PHONE       │           1 │              4 │ scanned           │
└─────────────┴─────────────┴─────────────┴────────────────┴───────────────────┘
  11 rows                                                            5 columns
```

This is the record to keep for an audit: every column of the table is listed. Name a skipped
column in the `columns` argument to scan it as text.

Two more columns of the record say what the scan checked: `checked_types` lists the PII types
that were applied to the column, and `ner_model` names the NER model that ran on it (`NULL` when
none did). Keep them with the record, because a `no_match` only covers the types that were
checked. With `anofox_pii_enabled_types` restricted to `EMAIL`, the phone number, IBAN and IP address in
`note` are not found, and the record shows why:

```sql
SET anofox_pii_enabled_types = 'EMAIL';
SELECT column_name, status, checked_types, ner_model
FROM pii_scan_table('customers', columns := 'phone, note');
```

```text
┌─────────────┬──────────┬───────────────┬───────────┐
│ column_name │  status  │ checked_types │ ner_model │
│   varchar   │ varchar  │   varchar[]   │  varchar  │
├─────────────┼──────────┼───────────────┼───────────┤
│ phone       │ no_match │ [EMAIL]       │ NULL      │
│ note        │ no_match │ [EMAIL]       │ NULL      │
└─────────────┴──────────┴───────────────┴───────────┘
```

```sql
RESET anofox_pii_enabled_types;
```

`match_count` counts matches, not rows: a note with two phone numbers counts twice, so a count
can even exceed `values_scanned`. Fewer matches than values, or `no_match`, means that detection
found nothing in the other values, not that they are free of personal data. `first_name` holds
four names, but the NER model does not take `Li` on its own for a name, and `phone` holds
`01511234567`, a number without a separator or a phone keyword (`Tel`, `Mobil`, ...) before it,
which the phone pattern does not match. Columns like these are masked by what you know about them,
not by detection (step 4).

## 3. Inspect the findings row by row

`pii_audit_table` returns one row per match. Pass `id_column := 'customer_id'` so each finding
carries the key of its row:

```sql
SELECT customer_id, column_name, pii_type, original_value
FROM pii_audit_table('customers', id_column := 'customer_id')
ORDER BY customer_id, column_name;
```

```text
┌─────────────┬─────────────┬─────────────┬────────────────────────────────┐
│ customer_id │ column_name │  pii_type   │         original_value         │
│    int32    │   varchar   │   varchar   │            varchar             │
├─────────────┼─────────────┼─────────────┼────────────────────────────────┤
│         101 │ card        │ CREDIT_CARD │ 4111 1111 1111 1111            │
│         101 │ email       │ EMAIL       │ anna@example.com               │
│         101 │ first_name  │ NAME        │ Anna                           │
│         101 │ note        │ PHONE       │ Call me on +1 (415) 555-1234   │
│         101 │ ssn         │ US_SSN      │ 123-45-6789                    │
│         102 │ email       │ EMAIL       │ bob@example.org                │
│         103 │ first_name  │ NAME        │ Bo                             │
│         103 │ note        │ IBAN        │ Wire to DE89370400440532013000 │
│         103 │ phone       │ PHONE       │ 030 1234567                    │
│         103 │ ssn         │ US_SSN      │ 219-09-9999                    │
│         104 │ card        │ CREDIT_CARD │ 5500 0000 0000 0004            │
│         104 │ email       │ EMAIL       │ carol@example.net              │
│         104 │ first_name  │ NAME        │ Carol                          │
│         104 │ note        │ IP_ADDRESS  │ Server 192.168.1.20 is down    │
└─────────────┴─────────────┴─────────────┴────────────────────────────────┘
  14 rows                                                        4 columns
```

`original_value` is the unmasked cell: the audit result holds the PII it reports. When the
findings leave the team that owns the data, share `masked_value` rather than `original_value`,
but do not take it as safe. `masked_value` is the whole cell with only the detected PII masked:
PII that detection misses stays in it, such as a compact phone number without a phone keyword
(`01511234567`, a documented limit of `PHONE`). A second scan of the masked values cannot prove
them clean either, as it misses the same values. For columns known to hold PII, mask the whole
value with `pii_mask_value` (section 4), and review free-text columns before you export them.

Count the findings per customer to see who has the most personal data on file:

```sql
SELECT customer_id, count(*) AS findings
FROM pii_audit_table('customers', id_column := 'customer_id')
GROUP BY customer_id
ORDER BY findings DESC, customer_id;
```

```text
┌─────────────┬──────────┐
│ customer_id │ findings │
│    int32    │  int64   │
├─────────────┼──────────┤
│         101 │        5 │
│         103 │        4 │
│         104 │        4 │
│         102 │        1 │
└─────────────┴──────────┘
```

## 4. Mask the data for an export

Pick a strategy per column. Columns that are known to hold one kind of PII are masked with
`pii_mask_value`: it masks every non-`NULL` value as the given type, whether detection finds it
or not, so `Li` and `01511234567` are masked too. Free text, where you do not know what is in
it, goes through detection with `pii_mask`.

Here the first name is redacted, the email keeps its first character and top-level domain, the
SSN its last four digits, the card number and phone number are hashed so equal values still
match, and the free text is redacted:

```sql
CREATE TABLE customers_export AS
SELECT customer_id,
       pii_mask_value(first_name, 'NAME', 'redact')   AS first_name,
       pii_mask_value(email, 'EMAIL', 'partial')      AS email,
       pii_mask_value(ssn, 'US_SSN', 'partial')       AS ssn,
       pii_mask_value(card, 'CREDIT_CARD', 'hash')    AS card,
       pii_mask_value(phone, 'PHONE', 'hash')         AS phone,
       pii_mask(note, 'redact')                       AS note
FROM customers;

SELECT * FROM customers_export ORDER BY customer_id;
```

```text
┌─────────────┬────────────┬───────────────────┬─────────────┬──────────────────┬──────────────────┬─────────────────────────────┐
│ customer_id │ first_name │       email       │     ssn     │       card       │      phone       │            note             │
│    int32    │  varchar   │      varchar      │   varchar   │     varchar      │     varchar      │           varchar           │
├─────────────┼────────────┼───────────────────┼─────────────┼──────────────────┼──────────────────┼─────────────────────────────┤
│         101 │ [NAME]     │ a***@*******.com  │ ***-**-6789 │ 9bbef19476623ca5 │ 2d27d42a5cd7c21f │ Call me on [PHONE]          │
│         102 │ [NAME]     │ b**@*******.org   │ NULL        │ NULL             │ NULL             │ No contact details here     │
│         103 │ [NAME]     │ ************      │ ***-**-9999 │ NULL             │ 27423fa11f870569 │ Wire to [IBAN]              │
│         104 │ [NAME]     │ c****@*******.net │ NULL        │ dd1ed74bd626c6fa │ NULL             │ Server [IP_ADDRESS] is down │
└─────────────┴────────────┴───────────────────┴─────────────┴──────────────────┴──────────────────┴─────────────────────────────┘
```

`email` of customer 103 is `not an email`: `pii_mask_value` masks it anyway, because the column
is declared to hold email addresses. Use `pii_mask_column` instead where only detected values
should change.

Check what is left. Scan the export again:

```sql
SELECT column_name, pii_type, match_count, sample_values
FROM pii_scan_table('customers_export')
WHERE pii_type IS NOT NULL
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬──────────┬─────────────┬───────────────┐
│ column_name │ pii_type │ match_count │ sample_values │
│   varchar   │ varchar  │    int64    │   varchar[]   │
└─────────────┴──────────┴─────────────┴───────────────┘
                         0 rows
```

The re-scan only confirms that what detection finds was masked. It cannot show that values
detection never found are gone: had `first_name` and `phone` been masked with `pii_mask_column`,
`Li` and `01511234567` would still be in the export and this scan would not report them.
Mask columns that are known to hold PII with `pii_mask_value`.

This matters most for name columns. `pii_mask_column(last_name, 'NAME', 'redact')` masks only
the cells where the NER model labels the surname `NAME`; a surname it reads as a company or
place (`ORGANIZATION`, `LOCATION`) stays in clear text. For a column that holds names, use
`pii_mask_value(last_name, 'NAME', 'redact')`.

Street addresses are no single PII type. `POSTAL_CODE` (see section 5) finds postcodes, but
the street and house number stay. Where a column holds addresses, split it with
`postal_parse_address` from the [postal module](../api/02-postal.md#postal_parse_address) and
keep only the parts you need. This block is not run as part of the documentation tests, as it
needs the libpostal data (about 500 MB, see the postal page):

```sql no-run
SELECT customer_id,
       pii_mask_value(concat_ws(' ', (a).road, (a).house_number), 'LOCATION', 'redact') AS street,
       left((a).postcode, 2) || '***'                                                AS postcode_area,
       (a).city                                                                       AS city
FROM (SELECT customer_id, postal_parse_address(address) AS a FROM customer_addresses);
-- 'Hauptstraße 5, 80331 München' -> street = '[LOCATION]', postcode_area = '80***', city = 'münchen'
```

libpostal lower-cases the parts it returns. A row where libpostal finds no road keeps a `NULL`
street; `concat_ws` of only `NULL` parts is an empty string, which `pii_mask_value` leaves empty.

Names, organizations and places come from the NER model. Its results are statistical, so
review them before you act on them. The default model `bert-multi` covers German, English and
eight other languages; the English-only `distilbert-en` misses many names and places in German
or other non-English free text (see [NER model](../api/06-pii.md#ner-model)).

`hash` is the first 16 hex characters of the SHA-256 of the card number with spaces and dashes
removed, so `4111 1111 1111 1111` and `4111-1111-1111-1111` give the same value; a no-break
space or another Unicode space separator (common in values pasted from PDFs and spreadsheets)
counts as a space, in detection as in the hash, and so does a single line break in a card number
or IBAN that a mail client wrapped over two lines. It has no key:
it is a pseudonym, not anonymisation. Card numbers, phone numbers and SSNs can be enumerated, so
a determined reader can recover them from their hashes. See
[Hashing](../api/06-pii.md#hashing).

A strategy name that is not `redact`, `partial`, `asterisk`, `hash` or `none` is an error
(`Invalid mask strategy: …`), so a typo never leaves the text unmasked.
`SET anofox_pii_default_mask_strategy = 'none'` is rejected as well.

## 5. Detect only some types

`anofox_pii_enabled_types` limits detection to a comma-separated list of types. It applies to all
PII functions, including the table scans:

```sql
SET anofox_pii_enabled_types = 'EMAIL,US_SSN';

SELECT column_name, pii_type, match_count
FROM pii_scan_table('customers')
WHERE pii_type IS NOT NULL
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬──────────┬─────────────┐
│ column_name │ pii_type │ match_count │
│   varchar   │ varchar  │    int64    │
├─────────────┼──────────┼─────────────┤
│ email       │ EMAIL    │           3 │
│ ssn         │ US_SSN   │           2 │
└─────────────┴──────────┴─────────────┘
```

Leaving out `NAME`, `ORGANIZATION`, `LOCATION` and `MISC` also makes scans much faster: the NER
model takes in the order of tens of milliseconds per row, pattern matching a tiny fraction of
that (see [Throughput](../api/06-pii.md#throughput)). Columns that hold only addresses or phone
numbers stay fast with the NER types on: a value whose letters all lie inside pattern matches
skips the model.

Set it back to the default types:

```sql
RESET anofox_pii_enabled_types;
```

`POSTAL_CODE` is not among the default types. Add it with
`SET anofox_pii_enabled_types = 'DEFAULT,POSTAL_CODE'` when addresses have to be masked too.

For German customer and claims data, `DATE_OF_BIRTH` (a date after `geb.`, `Geburtsdatum`,
`DOB`, ..., also with a two-digit year such as `geb. 5.5.80`), `DE_SVNR`
(Rentenversicherungsnummer), `DE_KVNR` (Krankenversichertennummer after a keyword such as
`KVNR` or `Versichertennummer`, with a valid check digit) and `DE_ID_CARD` (Personalausweis and
Reisepass numbers) are detected by default, as are compact phone numbers after a phone keyword
(`Tel 01701234567`) and IBANs with irregular grouping (`DE89 37040044 0532013000`). Invoice dates
and order numbers without such a keyword or check digit are left alone. If mistyped IBANs and card numbers must be masked as well,
`SET anofox_pii_require_checksum = false` reports them at confidence 0.6 (see
[Settings](../api/06-pii.md#settings)).

Special categories of personal data (GDPR Art. 9: health, religion or beliefs, racial or ethnic
origin, political opinions, trade-union membership, sex life or sexual orientation, genetic and
biometric data) are not detected. A claims note that says `Diagnose: Diabetes Typ 2` is not
flagged for its diagnosis; free-text columns that can hold such data need their own review and
are best masked as a whole with `pii_mask_value`.

## 6. Run without network access

Names, organizations and places are found by a NER model. The first detection that needs it
downloads the model (`bert-multi`, about 178 MB, for German, English and other European
languages; see `anofox_ner_model`) from Hugging Face into
`~/.duckdb/extensions/anofox/ner/<model>/`.
To prevent the download:

```sql no-run
SET anofox_ner_auto_download = false;
SELECT model_status FROM anofox_ner_status();
-- NOT_DOWNLOADED when the model is not on disk yet
```

`anofox_ner_status()` reports `NOT_DOWNLOADED` as soon as the setting is off and the model is
missing, before any detection runs. The pattern-based types keep working, and `NAME` falls back
to a dictionary of common given names (with German, Austrian and Swiss names; `Sabine Weiß` and
`Peter Müller` are found as whole names).
`ORGANIZATION`, `LOCATION` and `MISC` are not detected. `pii_scan_table` says so in its status:
`scanned (NER unavailable)` or `no_match (NER unavailable)` instead of `scanned` or `no_match`,
so an audit can see that the NER types were not checked. This snippet is not run as part of the
docs because its result depends on whether the model is already on disk.

To keep detection fully pattern-based, restrict the types instead:

```sql
SET anofox_pii_enabled_types = 'EMAIL,PHONE,CREDIT_CARD,US_SSN,IBAN,IP_ADDRESS';
SELECT pii_mask('Anna Schmidt, anna@example.com, server 10.0.0.7') AS masked;
```

```text
┌────────────────────────────────────────────┐
│                   masked                   │
│                  varchar                   │
├────────────────────────────────────────────┤
│ Anna Schmidt, [EMAIL], server [IP_ADDRESS] │
└────────────────────────────────────────────┘
```

## Next steps

- [PII API reference](../api/06-pii.md): every function, type and strategy
- [Configuration: PII](../reference/configuration.md#pii): all PII and NER settings
- [Network and privacy](../reference/network-and-privacy.md): what is downloaded and when
- [Data quality checks](02-data-quality-checks.md): validate the rest of the table

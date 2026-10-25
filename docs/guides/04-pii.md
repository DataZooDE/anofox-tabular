# Find and Mask PII

> Locate personal data in a table, inspect it row by row, and mask it before you share the data.

**Use this guide to:**
- Find which columns of a table contain PII
- Inspect each finding with the key of its row
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
    (101, 'anna@example.com', '123-45-6789', '4111 1111 1111 1111', 'Call me on +1 (415) 555-1234'),
    (102, 'bob@example.org',  NULL,          NULL,                  'No contact details here'),
    (103, 'not an email',     '219-09-9999', NULL,                  'Wire to DE89370400440532013000'),
    (104, 'carol@example.net', NULL,         '5500 0000 0000 0004', 'Server 192.168.1.20 is down')
) t(customer_id, email, ssn, card, note);
```

## 2. Find the PII columns

`pii_scan_table` checks every `VARCHAR` column and returns one row per column and PII type:

```sql
SELECT column_name, pii_type, match_count
FROM pii_scan_table('customers')
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬─────────────┬─────────────┐
│ column_name │  pii_type   │ match_count │
│   varchar   │   varchar   │    int64    │
├─────────────┼─────────────┼─────────────┤
│ card        │ CREDIT_CARD │           2 │
│ email       │ EMAIL       │           3 │
│ note        │ IBAN        │           1 │
│ note        │ IP_ADDRESS  │           1 │
│ note        │ PHONE       │           1 │
│ ssn         │ US_SSN      │           2 │
└─────────────┴─────────────┴─────────────┘
```

The free-text column `note` holds three different types. Those are the columns to treat with
care.

The scan also returns `sample_values`, up to five matched values per row. They are raw PII, so
keep the scan result as protected as the table.

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
│         101 │ note        │ PHONE       │ Call me on +1 (415) 555-1234   │
│         101 │ ssn         │ US_SSN      │ 123-45-6789                    │
│         102 │ email       │ EMAIL       │ bob@example.org                │
│         103 │ note        │ IBAN        │ Wire to DE89370400440532013000 │
│         103 │ ssn         │ US_SSN      │ 219-09-9999                    │
│         104 │ card        │ CREDIT_CARD │ 5500 0000 0000 0004            │
│         104 │ email       │ EMAIL       │ carol@example.net              │
│         104 │ note        │ IP_ADDRESS  │ Server 192.168.1.20 is down    │
└─────────────┴─────────────┴─────────────┴────────────────────────────────┘
  10 rows                                                        4 columns
```

`original_value` is the unmasked cell: the audit result holds the PII it reports. Share
`masked_value` instead when the findings leave the team that owns the data.

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
│         101 │        4 │
│         104 │        3 │
│         103 │        2 │
│         102 │        1 │
└─────────────┴──────────┘
```

## 4. Mask the data for an export

Pick a strategy per column. Here the email domain stays readable, the SSN keeps its last four
digits, the card number is hashed so equal cards still match, and the free text is redacted:

```sql
CREATE TABLE customers_export AS
SELECT customer_id,
       pii_mask_column(email, 'EMAIL', 'partial')     AS email,
       pii_mask_column(ssn, 'US_SSN', 'partial')      AS ssn,
       pii_mask_column(card, 'CREDIT_CARD', 'hash')   AS card,
       pii_mask(note, 'redact')                       AS note
FROM customers;

SELECT * FROM customers_export ORDER BY customer_id;
```

```text
┌─────────────┬───────────────────┬─────────────┬──────────────────┬─────────────────────────────┐
│ customer_id │       email       │     ssn     │       card       │            note             │
│    int32    │      varchar      │   varchar   │     varchar      │           varchar           │
├─────────────┼───────────────────┼─────────────┼──────────────────┼─────────────────────────────┤
│         101 │ an**@example.com  │ ***-**-6789 │ 9bbef19476623ca5 │ Call me on [PHONE]          │
│         102 │ bo*@example.org   │ NULL        │ NULL             │ No contact details here     │
│         103 │ not an email      │ ***-**-9999 │ NULL             │ Wire to [IBAN]              │
│         104 │ ca***@example.net │ NULL        │ dd1ed74bd626c6fa │ Server [IP_ADDRESS] is down │
└─────────────┴───────────────────┴─────────────┴──────────────────┴─────────────────────────────┘
```

Check what is left. Scan the export again:

```sql
SELECT column_name, pii_type, match_count, sample_values
FROM pii_scan_table('customers_export')
ORDER BY column_name, pii_type;
```

```text
┌─────────────┬──────────────┬─────────────┬────────────────────────────────────────────────────────┐
│ column_name │   pii_type   │ match_count │                     sample_values                      │
│   varchar   │   varchar    │    int64    │                       varchar[]                        │
├─────────────┼──────────────┼─────────────┼────────────────────────────────────────────────────────┤
│ email       │ EMAIL        │           3 │ [an**@example.com, bo*@example.org, ca***@example.net] │
│ note        │ ORGANIZATION │           1 │ [IP_ADDRESS]                                           │
└─────────────┴──────────────┴─────────────┴────────────────────────────────────────────────────────┘
```

The partially masked emails still look like email addresses, so the scan still reports them.
That is expected for `partial`. Use `redact` or `hash` for columns that must not match any PII
pattern after export.

The `ORGANIZATION` row comes from the NER model, which reads the label `[IP_ADDRESS]` as an
organization name. NER results are statistical, so review them before you act on them. The
model is trained on English: on German or other non-English free text it misses many names and
places, so do not rely on it alone there.

`hash` is the first 16 hex characters of the SHA-256 of the card number with spaces and dashes
removed, so `4111 1111 1111 1111` and `4111-1111-1111-1111` give the same value. It has no key:
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
that (see [Throughput](../api/06-pii.md#throughput)).

Set it back to the default types:

```sql
RESET anofox_pii_enabled_types;
```

`POSTAL_CODE` is not among the default types. Add it with
`SET anofox_pii_enabled_types = 'DEFAULT,POSTAL_CODE'` when addresses have to be masked too.

For German customer and claims data, `DATE_OF_BIRTH` (a date after `geb.`, `Geburtsdatum`,
`DOB`, ...), `DE_SVNR` (Rentenversicherungsnummer) and `DE_ID_CARD` (Personalausweis and
Reisepass numbers) are detected by default. Invoice dates and order numbers without such a
keyword or check digit are left alone. If mistyped IBANs and card numbers must be masked as well,
`SET anofox_pii_require_checksum = false` reports them at confidence 0.6 (see
[Settings](../api/06-pii.md#settings)).

## 6. Run without network access

Names, organizations and places are found by a NER model. The first detection that needs it
downloads the model (about 66 MB) from Hugging Face into `~/.duckdb/extensions/anofox/ner/`.
To prevent the download:

```sql no-run
SET anofox_ner_auto_download = false;
SELECT model_status FROM anofox_ner_status();
-- NOT_DOWNLOADED when the model is not on disk yet
```

`anofox_ner_status()` reports `NOT_DOWNLOADED` as soon as the setting is off and the model is
missing, before any detection runs. The pattern-based types keep working, and `NAME` falls back
to a name dictionary.
`ORGANIZATION`, `LOCATION` and `MISC` are not detected. This snippet is not run as part of the
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

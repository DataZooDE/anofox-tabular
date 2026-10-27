# PII Detection and Masking

> Find, validate and mask personal data such as emails, card numbers, IBANs, dates of birth,
> German social insurance and ID card numbers, and names in text.

**Use this page to:**
- Detect PII in a text value and get its type, position and confidence
- Mask PII with a redact, partial, asterisk or hash strategy
- Scan whole tables for PII columns and audit single rows
- Check card numbers, IBANs, SSNs and other identifiers by checksum
- Check whether the NER model for names, organizations and locations is available

Pattern-based detection runs locally. Detection of `NAME`, `ORGANIZATION`, `LOCATION` and `MISC`
uses a NER model that is downloaded from Hugging Face on first use (see [NER model](#ner-model)
and [Network and privacy](../reference/network-and-privacy.md)).

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`pii_detect`](#pii_detect) | `anofox_tab_pii_detect` | scalar | list of matches |
| [`pii_detect_batch`](#pii_detect_batch) | `anofox_tab_pii_detect_batch` | scalar | one list of matches per list element |
| [`pii_detect_emails`](#type-specific-detection) | `anofox_tab_pii_detect_emails` | scalar | `EMAIL` matches |
| [`pii_detect_phones`](#type-specific-detection) | `anofox_tab_pii_detect_phones` | scalar | `PHONE` matches |
| [`pii_detect_credit_cards`](#type-specific-detection) | `anofox_tab_pii_detect_credit_cards` | scalar | `CREDIT_CARD` matches |
| [`pii_detect_ssns`](#type-specific-detection) | `anofox_tab_pii_detect_ssns` | scalar | `US_SSN` matches |
| [`pii_detect_ibans`](#type-specific-detection) | `anofox_tab_pii_detect_ibans` | scalar | `IBAN` matches |
| [`pii_detect_names`](#type-specific-detection) | `anofox_tab_pii_detect_names` | scalar | `NAME` matches |
| [`pii_mask`](#pii_mask) | `anofox_tab_pii_mask` | scalar | text with all PII masked |
| [`pii_mask_column`](#pii_mask_column) | `anofox_tab_pii_mask_column` | scalar | text with one PII type masked |
| [`pii_mask_value`](#pii_mask_value) | `anofox_tab_pii_mask_value` | scalar | the whole value masked as one PII type, without detection |
| [`pii_redact_column`](#pii_redact_column) | `anofox_tab_pii_redact_column` | scalar | text with all PII masked |
| [`pii_contains`](#pii_contains) | `anofox_tab_pii_contains` | scalar | `BOOLEAN` |
| [`pii_count`](#pii_count) | `anofox_tab_pii_count` | scalar | `BIGINT` |
| [`pii_is_valid_ssn`](#validation-functions) | `anofox_tab_pii_is_valid_ssn` | scalar | `BOOLEAN` |
| [`pii_is_valid_iban`](#validation-functions) | `anofox_tab_pii_is_valid_iban` | scalar | `BOOLEAN` |
| [`pii_is_valid_credit_card`](#validation-functions) | `anofox_tab_pii_is_valid_credit_card` | scalar | `BOOLEAN` |
| [`pii_is_valid_nino`](#validation-functions) | `anofox_tab_pii_is_valid_nino` | scalar | `BOOLEAN` |
| [`pii_is_valid_de_tax_id`](#validation-functions) | `anofox_tab_pii_is_valid_de_tax_id` | scalar | `BOOLEAN` |
| [`pii_is_valid_crypto_address`](#validation-functions) | `anofox_tab_pii_is_valid_crypto_address` | scalar | `BOOLEAN` |
| [`pii_scan_table`](#pii_scan_table) | `anofox_tab_pii_scan_table` | table | one row per column and PII type, plus one per column without a match |
| [`pii_audit_table`](#pii_audit_table) | `anofox_tab_pii_audit_table` | table | one row per match |
| [`pii_status`](#pii_status) | `anofox_tab_pii_status` | table | the registered recognizers |
| [`pii_config`](#pii_config) | `anofox_tab_pii_config` | table | the current PII settings |
| [`anofox_ner_status`](#anofox_ner_status) | `anofox_ner_status` (no alias) | table | the state of the NER model |

## PII types

`pii_status()` lists the types. Detection uses patterns plus a checksum where one exists. The four
NER types need the NER model.

| Type | Detects | Method |
|------|---------|--------|
| `EMAIL` | Email addresses, also with non-ASCII letters (`jörg.k@müller-gmbh.de`); a local part of at most 64 characters and domain labels of at most 63 | pattern |
| `PHONE` | International numbers with a `+` or `00` prefix (`+49 (0) 221 4711-0`, `0049 171 1234567`), national numbers with an area code and a space, dash or slash (`0171 1234567`, `089/12345678`, `(089) 123 456 78`) and US `(555) 123-4567`; 7 to 15 digits. A number starts and ends at the text boundary or at any character that is no letter, digit or `+`, so numbers in JSON values (`"phone":"+49 171 2345678"`), CSV fields, HTML cells (`<td>+49 30 1234567</td>`), `tel=+49...` and `Tel.+49...` are found. A number joined by a dash to letters or digits (`INC-0221-4711`, the groups of a UUID), or following a digit and a space, `/` or `.` (the groups of an IBAN), is part of a longer token and not matched | pattern |
| `CREDIT_CARD` | Card numbers of Visa, Mastercard (51-55, 2221-2720), American Express, Discover, Maestro, JCB, Diners Club and UnionPay: 13 to 19 digits, compact or in groups joined by one space, dash or dot (`4111 1111 1111 1111`, `3782-822463-10005`, `4111.1111.1111.1111`). The digits must carry a brand prefix with a length the brand issues; digits that continue a longer token (`DE89 3704 ...`, `SN4111...`, `4111...-01`) are not matched | pattern + Luhn checksum + brand prefix |
| `US_SSN` | US Social Security Numbers `XXX-XX-XXXX` or `XXX XX XXXX`; bare 9-digit numbers are not matched | pattern + area validation |
| `IBAN` | International Bank Account Numbers, compact or in groups of four separated by single spaces or dashes (`DE89 3704 0044 0532 0130 00`). The country code must be in the SWIFT IBAN registry and the IBAN must have that country's length (`DE` 22, `NL` 18, ...), so a hex id such as `ce78d1da254c0843eb23951ae077ff5f` whose MOD-97 checksum holds by chance is no IBAN | pattern + registered country and length + MOD-97 checksum |
| `IP_ADDRESS` | IPv4 and IPv6 addresses. IPv6 in all RFC 4291 text forms: full, compressed with `::` (`2001:db8::1`, `::1`), with an IPv4 tail (`::ffff:192.0.2.1`) and with a zone (`fe80::1%eth0`); an IPv6 address needs at least one decimal digit, so `cafe::beef` and `std::vector` are not matched | pattern |
| `URL` | HTTP and HTTPS URLs | pattern |
| `MAC_ADDRESS` | MAC addresses with colons or hyphens (`00:1A:2B:3C:4D:5E`) or dots (`0026.5e8b.1c2a`: exactly three groups of four with at least one letter A-F, so `4111.1111.1111.1111` is no MAC); without separators only a standalone 12-character hex token with at least one letter A-F and one digit that is not the last group of a UUID | pattern |
| `UK_NINO` | UK National Insurance Numbers | pattern |
| `US_PASSPORT` | A letter and 8 digits (`C12345678`) | pattern |
| `DE_TAX_ID` | German tax ID (Steueridentifikationsnummer, 11 digits) | pattern + checksum |
| `API_KEY` | AWS and GitHub tokens, high-entropy strings of 32 to 64 characters; a run next to `+` or `/` is a piece of a base64 or path token (an inline attachment) and not matched. UUIDs (`8-4-4-4-12` hex groups in any case, with or without braces) and md5, sha1 and sha256 hex digests (32, 40 or 64 hex characters, all lower or all upper case) are identifiers, not keys, and not matched | pattern + entropy |
| `CRYPTO_ADDRESS` | Bitcoin (P2PKH, P2SH, SegWit) and Ethereum addresses | pattern + checksum where available |
| `DATE_OF_BIRTH` | A date (`12.03.1985`, `3.7.1962`, `1985-03-12`, `12/03/1985`, `12. März 1985`, `March 12, 1985`) that follows a birth keyword (`geboren`, `geb.`, `Geburtsdatum`, `Geburtstag`, `DOB`, `D.O.B.`, `date of birth`, `birthday`, `born`) with at most three words and no comma or sentence end in between. A date without a keyword is not matched. It must be a real calendar date from 1900 up to today | pattern + keyword + date check |
| `DE_SVNR` | German pension insurance number (Rentenversicherungsnummer, often called Sozialversicherungsnummer): area, birth date `DDMMYY`, initial letter, serial, check digit, with or without spaces (`65 170839 J 003`, `65170839J003`) | pattern + check digit |
| `DE_ID_CARD` | German Personalausweis or Reisepass number: a letter and 8 characters from `CFGHJKLMNPRTVWXYZ0-9` with at least one digit, optionally followed by the check digit (`T220001293`). With a valid ICAO 9303 check digit it is matched anywhere; without it only after a keyword (`Personalausweis`, `Ausweis`, `Ausweis-Nr.`, `Reisepass`, `Passnummer`, `ID card`, `passport`) | pattern + check digit or keyword |
| `POSTAL_CODE` | Off by default (see below). DE 5-digit, AT/CH 4-digit codes followed by a town (`80331 München`) after a street and house number, at the start of a line or after a comma, or with a `D-`/`A-`/`CH-` prefix; NL `1015 CJ` followed by a town; UK postcodes (`SW1A 2AA`) at the end of an address line; any of these after `PLZ`, `Postleitzahl`, `postcode` or `zip` | pattern + address context |
| `NAME` | Person names | NER; a name dictionary when the model is not available |
| `ORGANIZATION` | Organization names | NER only |
| `LOCATION` | Places | NER only |
| `MISC` | Other named entities | NER only |

Detection skips a bare 9-digit number such as `123456789`. It is neither a `US_SSN` nor a
`US_PASSPORT`, because contract, order and claim numbers have the same shape.

The German types need context or a check digit, because claims notes and invoices are full of
dates, order numbers and amounts. Invoice and claim dates, amounts, quantities (`12345 Stück`)
and plain 5-digit numbers are not matched. A 9-character document number such as `T22000129`
is a `DE_ID_CARD` after an ID keyword and a `US_PASSPORT` (a letter and 8 digits) without one.

```sql
SET anofox_pii_enabled_types = 'DATE_OF_BIRTH,DE_SVNR,DE_ID_CARD';
SELECT pii_mask('VN geb. 12.03.1985, Ausweis-Nr. T220001293, SV-Nr. 65 170839 J 003; Schaden vom 15.08.2024, Rechnung 4711081512') AS masked;
```

```text
┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                      masked                                                      │
│                                                     varchar                                                      │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ VN geb. [DATE_OF_BIRTH], Ausweis-Nr. [DE_ID_CARD], SV-Nr. [DE_SVNR]; Schaden vom 15.08.2024, Rechnung 4711081512 │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

```sql
SELECT pii_mask_column('VN geb. 12.03.1985', 'DATE_OF_BIRTH', 'partial') AS dob,
       pii_mask_column('SV-Nr. 65 170839 J 003', 'DE_SVNR', 'partial') AS svnr,
       pii_mask_column('Ausweis-Nr. T220001293', 'DE_ID_CARD', 'partial') AS id_card;
```

```text
┌────────────────────┬────────────────────────┬────────────────────────┐
│        dob         │          svnr          │        id_card         │
│      varchar       │        varchar         │        varchar         │
├────────────────────┼────────────────────────┼────────────────────────┤
│ VN geb. **.**.1985 │ SV-Nr. 65 ****** * *** │ Ausweis-Nr. T********* │
└────────────────────┴────────────────────────┴────────────────────────┘
```

```sql
RESET anofox_pii_enabled_types;
```

`POSTAL_CODE` is off by default: even with the address rules a postal code is rarely the PII
you need to find, and it is weak evidence on its own. It is detected only when you name it,
in `anofox_pii_enabled_types` or as the type argument of `pii_mask_column`. `DEFAULT` in
`anofox_pii_enabled_types` stands for all default types, so `'DEFAULT,POSTAL_CODE'` adds postal
codes to them:

```sql
SET anofox_pii_enabled_types = 'POSTAL_CODE';
SELECT a AS address, pii_mask(a) AS masked
FROM (VALUES ('Musterstraße 12, 80331 München'), ('Keizersgracht 123, 1015 CJ Amsterdam'),
             ('10 Downing Street, London SW1A 2AA'), ('Auftrag 12345 Stück, Vertragsnummer 80331')) t(a);
```

```text
┌───────────────────────────────────────────┬────────────────────────────────────────────┐
│                  address                  │                   masked                   │
│                  varchar                  │                  varchar                   │
├───────────────────────────────────────────┼────────────────────────────────────────────┤
│ Musterstraße 12, 80331 München            │ Musterstraße 12, [POSTAL_CODE] München     │
│ Keizersgracht 123, 1015 CJ Amsterdam      │ Keizersgracht 123, [POSTAL_CODE] Amsterdam │
│ 10 Downing Street, London SW1A 2AA        │ 10 Downing Street, London [POSTAL_CODE]    │
│ Auftrag 12345 Stück, Vertragsnummer 80331 │ Auftrag 12345 Stück, Vertragsnummer 80331  │
└───────────────────────────────────────────┴────────────────────────────────────────────┘
```

```sql
RESET anofox_pii_enabled_types;
```

## Masking strategies

The strategy argument is case-insensitive.

| Strategy | Effect |
|----------|--------|
| `redact` (default) | Replace each match with its type label, e.g. `[EMAIL]` |
| `partial` | Keep a few characters, e.g. `***-**-6789`; never more than half of the letters and digits (see [Partial masks](#partial-masks)) |
| `asterisk` | Replace every character with `*` |
| `hash` | Replace with a pseudonym: the first 16 hex characters of the SHA-256 of the normalised value (see [Hashing](#hashing)) |
| `none` | Leave the text unchanged |

`partial` and `asterisk` count characters, not bytes: `Müller` becomes six stars and a partial
mask never cuts a multi-byte UTF-8 character in half, so masked values can be stored in any
`VARCHAR` column.

### Partial masks

A partial mask keeps at most half of the letters and digits of a value and masks short values
completely. Each type has its own rule:

| Type | Rule | Example |
|------|------|---------|
| `EMAIL` | First character of the local part and the top-level domain; every other character becomes `*`, dots between domain labels stay. The domain is masked as well, because a company or personal domain identifies a person as well as the local part does | `jane.doe@acme-corp.com` → `j*******@*********.com`, `ab@cd.de` → `a*@**.de` |
| `PHONE` | `***-***-` and the last 4 digits (`+***-***-` with a leading `+`); never more than half of the digits, up to 4 digits are masked completely | `+49 151 12345678` → `+***-***-5678` |
| `CREDIT_CARD` | Last 4 digits; the first 4 too from 16 digits on | `4111 1111 1111 1111` → `4111********1111`, `3782 822463 10005` → `***********0005` |
| `US_SSN` | Last 4 digits | `123-45-6789` → `***-**-6789` |
| `IBAN` | Country code and check digits, plus up to 4 trailing characters while at most half stays readable | `DE89 3704 0044 0532 0130 00` → `DE89**************3000`, `NO9386011117947` → `NO93********947` |
| `DE_TAX_ID` | Last 3 digits | `86095742719` → `**-***-***-719` |
| `IP_ADDRESS` | First IPv4 octet or first IPv6 group | `192.168.1.100` → `192.***.***.**` |
| `URL` | Scheme and top-level domain; every other host label is masked and a path, query or fragment becomes `/***` | `https://crm.example.com/customers/4711` → `https://***.*******.com/***` |
| `MAC_ADDRESS` | Manufacturer part (first two octets, or first four hex digits) | `00:1A:2B:3C:4D:5E` → `00:1A:**:**:**:**` |
| `UK_NINO` | Prefix letters and suffix letter | `AB123456C` → `AB ** ** ** C` |
| `US_PASSPORT` | The leading letter | `A12345678` → `A********` |
| `API_KEY` | The vendor prefix (`AKIA`, `ghp_`, ...) or the first 4 characters | `AKIAIOSFODNN7EXAMPLE` → `AKIA****************` |
| `CRYPTO_ADDRESS` | First and last 4 characters | `bc1qar0s…5mdq` → `bc1q****…5mdq` |
| `NAME` | Per word: the first character; words of up to 4 characters are masked completely | `Anna Schmidt` → `**** S******`, `Li Na` → `** **` |
| `ORGANIZATION`, `MISC` | Per word: up to the first 3 characters, at most half of the word; words of up to 4 characters are masked completely | `Microsoft` → `Mic******`, `Deutsche Bank AG` → `Deu***** **** **` |
| `LOCATION` | The first word like `ORGANIZATION`, all further words masked | `Berlin` → `Ber***`, `Rome` → `****`, `Frankfurt am Main` → `Fra****** ** ****` |
| Any other type | The first and last character from 5 characters on, the first and last 2 from 8 characters on; up to 4 characters are masked completely | |

The half rule is checked for every match: a type-specific mask that would reveal more than half of
the letters and digits of the value (possible for unusual values passed to
[`pii_mask_value`](#pii_mask_value)) is replaced by one `*` per character.

`pii_mask(text)` without a strategy always redacts. The setting `anofox_pii_default_mask_strategy`
applies only to `pii_redact_column(value)` and `pii_audit_table`. A strategy argument that
is not one of these names is an error (`Invalid mask strategy: …`), in `pii_mask`,
`pii_mask_column`, `pii_redact_column` and in `SET anofox_pii_default_mask_strategy`. The
setting also rejects `none` (`anofox_pii_default_mask_strategy cannot be 'none'`), because a
default that turns masking off would expose raw PII; `none` stays valid as an explicit argument.

```sql
SELECT s AS strategy, pii_mask('SSN: 123-45-6789, mail a.b@example.com', s) AS masked
FROM (VALUES ('redact'), ('partial'), ('asterisk'), ('hash'), ('none')) t(s);
```

```text
┌──────────┬──────────────────────────────────────────────┐
│ strategy │                    masked                    │
│ varchar  │                   varchar                    │
├──────────┼──────────────────────────────────────────────┤
│ redact   │ SSN: [US_SSN], mail [EMAIL]                  │
│ partial  │ SSN: ***-**-6789, mail a**@*******.com       │
│ asterisk │ SSN: ***********, mail ***************       │
│ hash     │ SSN: 15e2b0d3c33891eb, mail a6f1bad172104d25 │
│ none     │ SSN: 123-45-6789, mail a.b@example.com       │
└──────────┴──────────────────────────────────────────────┘
```

### Hashing

`hash` replaces a match with the first 16 hex characters (64 bits) of the SHA-256 of the
normalised value. Normalising first makes differently formatted copies of one value hash alike,
so hashed columns can still be joined and counted:

| Type | Normalisation before hashing |
|------|------------------------------|
| `PHONE`, `CREDIT_CARD`, `US_SSN`, `DE_TAX_ID` | Spaces, `-`, `.`, `/`, `(` and `)` removed (a leading `+` is kept) |
| `IBAN`, `UK_NINO`, `DE_SVNR`, `DE_ID_CARD` | Same separators removed, upper-cased |
| `EMAIL` | Lower-cased |
| all other types | None: the matched text is hashed as it is |

```sql
SELECT pii_mask_column(card, 'CREDIT_CARD', 'hash') AS hashed,
       pii_mask_column(card, 'CREDIT_CARD', 'hash') = left(sha256('4111111111111111'), 16) AS same_as_sha256
FROM (VALUES ('4111 1111 1111 1111'), ('4111-1111-1111-1111')) t(card);
```

```text
┌──────────────────┬────────────────┐
│      hashed      │ same_as_sha256 │
│     varchar      │    boolean     │
├──────────────────┼────────────────┤
│ 9bbef19476623ca5 │ true           │
│ 9bbef19476623ca5 │ true           │
└──────────────────┴────────────────┘
```

The hash has no key or salt. That makes it a pseudonym, not anonymisation: anyone with the
hash can test candidate values, and small value spaces such as phone numbers, SSNs or card
numbers of a known issuer can be enumerated and so reversed by brute force. Treat hashed data
as personal data; use `redact` when the values must not be recoverable.

## Match structure

The detection functions return a list of structs:

```text
STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | `VARCHAR` | The PII type, e.g. `EMAIL` |
| `text` | `VARCHAR` | The matched text |
| `start_pos` | `BIGINT` | Start offset in the input, 0-based, in UTF-8 bytes |
| `end_pos` | `BIGINT` | End offset in the input, exclusive, in UTF-8 bytes |
| `confidence` | `DOUBLE` | `1.0` for pattern matches; `0.6` for an IBAN or card number that fails its checksum (only with `anofox_pii_require_checksum = false`); `0.5` for a name-dictionary match; the model score for NER matches |

The offsets count bytes of the UTF-8 encoding, not characters, so after a non-ASCII character
they are larger than the character position that `substr` expects. For ASCII text the two are
the same.

---

## `pii_detect`

Canonical name: `anofox_tab_pii_detect`. Scalar function.

Returns every PII match in the text. Types disabled by `anofox_pii_enabled_types` are skipped.

```text
pii_detect(text VARCHAR) -> STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `text` | `VARCHAR` | required | The text to scan |

```sql
SELECT m.type, m.text, m.start_pos, m.end_pos, m.confidence
FROM (SELECT unnest(pii_detect('Contact john.doe@example.com, SSN 123-45-6789')) AS m);
```

```text
┌─────────┬──────────────────────┬───────────┬─────────┬────────────┐
│  type   │         text         │ start_pos │ end_pos │ confidence │
│ varchar │       varchar        │   int64   │  int64  │   double   │
├─────────┼──────────────────────┼───────────┼─────────┼────────────┤
│ EMAIL   │ john.doe@example.com │         8 │      28 │        1.0 │
│ US_SSN  │ 123-45-6789          │        34 │      45 │        1.0 │
└─────────┴──────────────────────┴───────────┴─────────┴────────────┘
```

With the NER model available, names, organizations and places are found too. The confidence
of a NER match is the model score (from 0.7 up to just below `1.0`; rounded here):

```sql
SELECT m.type, m.text, round(m.confidence, 3) AS confidence
FROM (SELECT unnest(pii_detect('John Smith from Microsoft visited Paris')) AS m);
```

```text
┌──────────────┬────────────┬────────────┐
│     type     │    text    │ confidence │
│   varchar    │  varchar   │   double   │
├──────────────┼────────────┼────────────┤
│ NAME         │ John Smith │        1.0 │
│ ORGANIZATION │ Microsoft  │        1.0 │
│ LOCATION     │ Paris      │        1.0 │
└──────────────┴────────────┴────────────┘
```

---

## `pii_detect_batch`

Canonical name: `anofox_tab_pii_detect_batch`. Scalar function.

Runs `pii_detect` on every element of a list and returns one match list per element. A `NULL`
element gives `NULL`.

```text
pii_detect_batch(texts VARCHAR[]) -> STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[][]
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `texts` | `VARCHAR[]` | required | The texts to scan |

```sql
SELECT len(r) AS elements, [len(x) FOR x IN r] AS matches_per_element
FROM (SELECT pii_detect_batch(['a@b.com', 'no pii here', 'SSN 123-45-6789 and c@d.org']) AS r);
```

```text
┌──────────┬─────────────────────┐
│ elements │ matches_per_element │
│  int64   │       int64[]       │
├──────────┼─────────────────────┤
│        3 │ [1, 0, 2]           │
└──────────┴─────────────────────┘
```

---

## Type-specific detection

Each function returns the matches of one type, with the same structure as `pii_detect`.

| Function | Canonical name | Type |
|----------|----------------|------|
| `pii_detect_emails` | `anofox_tab_pii_detect_emails` | `EMAIL` |
| `pii_detect_phones` | `anofox_tab_pii_detect_phones` | `PHONE` |
| `pii_detect_credit_cards` | `anofox_tab_pii_detect_credit_cards` | `CREDIT_CARD` |
| `pii_detect_ssns` | `anofox_tab_pii_detect_ssns` | `US_SSN` |
| `pii_detect_ibans` | `anofox_tab_pii_detect_ibans` | `IBAN` |
| `pii_detect_names` | `anofox_tab_pii_detect_names` | `NAME` (NER model, or the name dictionary without it) |

```text
pii_detect_<type>(text VARCHAR) -> STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)[]
```

```sql
SELECT [m.text FOR m IN pii_detect_emails(t)]       AS emails,
       [m.text FOR m IN pii_detect_phones(t)]       AS phones,
       [m.text FOR m IN pii_detect_credit_cards(t)] AS cards,
       [m.text FOR m IN pii_detect_ssns(t)]         AS ssns,
       [m.text FOR m IN pii_detect_ibans(t)]        AS ibans
FROM (VALUES ('a@b.com, +1 (415) 555-1234, 4111 1111 1111 1111, 123-45-6789, DE89370400440532013000')) v(t);
```

```text
┌───────────┬───────────────────────┬───────────────────────┬───────────────┬──────────────────────────┐
│  emails   │        phones         │         cards         │     ssns      │          ibans           │
│ varchar[] │       varchar[]       │       varchar[]       │   varchar[]   │        varchar[]         │
├───────────┼───────────────────────┼───────────────────────┼───────────────┼──────────────────────────┤
│ [a@b.com] │ ['+1 (415) 555-1234'] │ [4111 1111 1111 1111] │ [123-45-6789] │ [DE89370400440532013000] │
└───────────┴───────────────────────┴───────────────────────┴───────────────┴──────────────────────────┘
```

```sql
SELECT [m.text FOR m IN pii_detect_names('Angela Merkel met Barack Obama')] AS names;
```

```text
┌───────────────────────────────┐
│             names             │
│           varchar[]           │
├───────────────────────────────┤
│ [Angela Merkel, Barack Obama] │
└───────────────────────────────┘
```

---

## `pii_mask`

Canonical name: `anofox_tab_pii_mask`. Scalar function.

Replaces every PII match in the text according to the strategy.

```text
pii_mask(text VARCHAR [, strategy VARCHAR]) -> VARCHAR
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `text` | `VARCHAR` | required | The text to mask |
| `strategy` | `VARCHAR` | `redact` (the setting `anofox_pii_default_mask_strategy` does not apply) | See [Masking strategies](#masking-strategies). `NULL` returns `NULL`. |

```sql
SELECT pii_mask('Email: test@example.com')            AS redacted,
       pii_mask('SSN: 123-45-6789', 'partial')        AS partial,
       pii_mask('Card: 4111111111111111', 'asterisk') AS asterisk;
```

```text
┌────────────────┬──────────────────┬────────────────────────┐
│    redacted    │     partial      │        asterisk        │
│    varchar     │     varchar      │        varchar         │
├────────────────┼──────────────────┼────────────────────────┤
│ Email: [EMAIL] │ SSN: ***-**-6789 │ Card: **************** │
└────────────────┴──────────────────┴────────────────────────┘
```

---

## `pii_mask_column`

Canonical name: `anofox_tab_pii_mask_column`. Scalar function.

Masks only the matches of one PII type. Other PII in the text stays as it is. An unknown type
raises an error.

```text
pii_mask_column(value VARCHAR, pii_type VARCHAR, strategy VARCHAR) -> VARCHAR
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `value` | `VARCHAR` | required | The text to mask |
| `pii_type` | `VARCHAR` | required | A type from [PII types](#pii-types), e.g. `US_SSN` |
| `strategy` | `VARCHAR` | required | See [Masking strategies](#masking-strategies) |

The type and strategy can differ from row to row, for example when they come from columns. A
`NULL` argument returns `NULL` for that row.

```sql
SELECT pii_mask_column('SSN 123-45-6789, mail a@b.com', 'US_SSN', 'partial') AS masked;
```

```text
┌───────────────────────────────┐
│            masked             │
│            varchar            │
├───────────────────────────────┤
│ SSN ***-**-6789, mail a@b.com │
└───────────────────────────────┘
```

---

## `pii_mask_value`

Canonical name: `anofox_tab_pii_mask_value`. Scalar function.

Masks the whole value as the given PII type, without running detection. Use it for columns that
are known to hold PII, where detection finds nothing in some cells: a two-letter first name such
as `Li`, a phone number without separators such as `01511234567`.

```text
pii_mask_value(value VARCHAR, pii_type VARCHAR, strategy VARCHAR) -> VARCHAR
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `value` | `VARCHAR` | required | The value to mask; all of it is treated as one match of `pii_type` |
| `pii_type` | `VARCHAR` | required | A type from [PII types](#pii-types), e.g. `NAME`; the aliases of `pii_mask_column` (`person`, `ssn`, ...) work too |
| `strategy` | `VARCHAR` | required | See [Masking strategies](#masking-strategies) |

What each strategy returns:

| Strategy | Result |
|----------|--------|
| `redact` | The type label, e.g. `[NAME]` |
| `asterisk` | One `*` per character |
| `partial` | The type's [partial mask](#partial-masks) |
| `hash` | The type's normalised hash, the same value `pii_mask` gives a detected match (see [Hashing](#hashing)) |
| `none` | The value unchanged |

A `NULL` argument returns `NULL` for that row; the type and strategy can differ from row to row.
An empty value stays empty. An unknown type (`Unknown PII type: …`) or strategy
(`Invalid mask strategy: …`) is an error.

```sql
SELECT pii_mask_value('Li', 'NAME', 'redact')                    AS first_name,
       pii_mask_value('01511234567', 'PHONE', 'partial')         AS phone,
       pii_mask_value('jane.doe@acme-corp.com', 'EMAIL', 'partial') AS email,
       pii_mask_value('Schmidt', 'NAME', 'asterisk')             AS last_name;
```

```text
┌────────────┬──────────────┬────────────────────────┬───────────┐
│ first_name │    phone     │         email          │ last_name │
│  varchar   │   varchar    │        varchar         │  varchar  │
├────────────┼──────────────┼────────────────────────┼───────────┤
│ [NAME]     │ ***-***-4567 │ j*******@*********.com │ *******   │
└────────────┴──────────────┴────────────────────────┴───────────┘
```

`pii_mask_column` only masks what detection finds, so `Li` and `01511234567` would stay as they
are:

```sql
SELECT pii_mask_column('Li', 'NAME', 'redact') AS first_name,
       pii_mask_column('01511234567', 'PHONE', 'redact') AS phone;
```

```text
┌────────────┬─────────────┐
│ first_name │    phone    │
│  varchar   │   varchar   │
├────────────┼─────────────┤
│ Li         │ 01511234567 │
└────────────┴─────────────┘
```

The same holds for surnames: `pii_mask_column(last_name, 'NAME', ...)` masks a surname only
where the NER model labels it `NAME`. A surname it reads as `ORGANIZATION` or `LOCATION`
(many surnames are also company or place names) stays in clear text. Mask a column that holds
names with `pii_mask_value(last_name, 'NAME', ...)`. For street addresses, split the column with
[`postal_parse_address`](02-postal.md#postal_parse_address) and mask the road and house number
with `pii_mask_value`; the [PII guide](../guides/04-pii.md#4-mask-the-data-for-an-export) has a
recipe.

---

## `pii_redact_column`

Canonical name: `anofox_tab_pii_redact_column`. Scalar function.

Masks all PII in the text, like `pii_mask`.

```text
pii_redact_column(value VARCHAR [, strategy VARCHAR]) -> VARCHAR
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `value` | `VARCHAR` | required | The text to mask |
| `strategy` | `VARCHAR` | `anofox_pii_default_mask_strategy` (`redact`) | See [Masking strategies](#masking-strategies). It can differ from row to row; `NULL` returns `NULL` |

```sql
SELECT pii_redact_column('SSN 123-45-6789, mail a@b.com')         AS redacted,
       pii_redact_column('SSN 123-45-6789, mail a@b.com', 'hash') AS hashed;
```

```text
┌────────────────────────────┬─────────────────────────────────────────────┐
│          redacted          │                   hashed                    │
│          varchar           │                   varchar                   │
├────────────────────────────┼─────────────────────────────────────────────┤
│ SSN [US_SSN], mail [EMAIL] │ SSN 15e2b0d3c33891eb, mail fb98d44ad7501a95 │
└────────────────────────────┴─────────────────────────────────────────────┘
```

---

## `pii_contains`

Canonical name: `anofox_tab_pii_contains`. Scalar function.

Returns `true` when the text contains at least one match.

```text
pii_contains(text VARCHAR) -> BOOLEAN
```

```sql
SELECT pii_contains('Contact us at support@company.com') AS with_email,
       pii_contains('Hello, world!') AS without;
```

```text
┌────────────┬─────────┐
│ with_email │ without │
│  boolean   │ boolean │
├────────────┼─────────┤
│ true       │ false   │
└────────────┴─────────┘
```

---

## `pii_count`

Canonical name: `anofox_tab_pii_count`. Scalar function.

Returns the number of matches in the text.

```text
pii_count(text VARCHAR) -> BIGINT
```

```sql
SELECT pii_count('Email: a@b.com, SSN: 123-45-6789, Card: 4111111111111111') AS matches;
```

```text
┌─────────┐
│ matches │
│  int64  │
├─────────┤
│       3 │
└─────────┘
```

---

## Validation functions

These functions check a single identifier, not free text. They return `true` when the value has
the right format and, where one exists, a valid checksum.

| Function | Canonical name | Checks |
|----------|----------------|--------|
| `pii_is_valid_ssn` | `anofox_tab_pii_is_valid_ssn` | US SSN format and area number |
| `pii_is_valid_iban` | `anofox_tab_pii_is_valid_iban` | IBAN: registered country code, that country's length and MOD-97 checksum |
| `pii_is_valid_credit_card` | `anofox_tab_pii_is_valid_credit_card` | Luhn checksum |
| `pii_is_valid_nino` | `anofox_tab_pii_is_valid_nino` | UK National Insurance Number format |
| `pii_is_valid_de_tax_id` | `anofox_tab_pii_is_valid_de_tax_id` | German tax ID: 11 digits and check digit |
| `pii_is_valid_crypto_address` | `anofox_tab_pii_is_valid_crypto_address` | Bitcoin (Base58 checksum for legacy and P2SH) or Ethereum (hex format) |

```text
pii_is_valid_ssn(ssn VARCHAR) -> BOOLEAN
pii_is_valid_iban(iban VARCHAR) -> BOOLEAN
pii_is_valid_credit_card(card_number VARCHAR) -> BOOLEAN
pii_is_valid_nino(nino VARCHAR) -> BOOLEAN
pii_is_valid_de_tax_id(tax_id VARCHAR) -> BOOLEAN
pii_is_valid_crypto_address(address VARCHAR) -> BOOLEAN
```

```sql
SELECT pii_is_valid_credit_card('4111111111111111') AS card_ok,
       pii_is_valid_credit_card('4111111111111112') AS card_bad,
       pii_is_valid_iban('DE89370400440532013000')  AS iban_ok,
       pii_is_valid_iban('DE89370400440532013001')  AS iban_bad,
       pii_is_valid_ssn('000-12-3456')              AS ssn_area_000;
```

```text
┌─────────┬──────────┬─────────┬──────────┬──────────────┐
│ card_ok │ card_bad │ iban_ok │ iban_bad │ ssn_area_000 │
│ boolean │ boolean  │ boolean │ boolean  │   boolean    │
├─────────┼──────────┼─────────┼──────────┼──────────────┤
│ true    │ false    │ true    │ false    │ false        │
└─────────┴──────────┴─────────┴──────────┴──────────────┘
```

```sql
SELECT pii_is_valid_nino('AB123456C')                                  AS nino,
       pii_is_valid_de_tax_id('65929970489')                           AS de_tax_id,
       pii_is_valid_crypto_address('1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa') AS bitcoin;
```

```text
┌─────────┬───────────┬─────────┐
│  nino   │ de_tax_id │ bitcoin │
│ boolean │  boolean  │ boolean │
├─────────┼───────────┼─────────┤
│ true    │ true      │ true    │
└─────────┴───────────┴─────────┘
```

---

## `pii_scan_table`

Canonical name: `anofox_tab_pii_scan_table`. Table function.

Scans the `VARCHAR` columns of a table and returns one row per column and PII type found, plus
one row for every column without a match, so the result accounts for every column: an audit can
show which columns were scanned and found clean, and which were not scanned at all. It accepts
regular tables and views, not TEMP tables (see
[Conventions](00-conventions.md#table-and-column-arguments)); a TEMP table is an error that says
so and suggests copying it into a regular table.

```text
pii_scan_table(table_name VARCHAR [, columns VARCHAR]) -> TABLE(
    column_name    VARCHAR,
    pii_type       VARCHAR,
    match_count    BIGINT,
    sample_values  VARCHAR[],
    confidence     DOUBLE,
    values_scanned BIGINT,
    status         VARCHAR
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | The table to scan |
| `columns` | `VARCHAR` | all `VARCHAR` columns (`JSON` included) | Comma-separated column names. Names match case-insensitively; an unknown name is an error. A named column of another type (`BIGINT`, `STRUCT`, `LIST`, ...) is cast to `VARCHAR` and scanned |

| Column | Type | Description |
|--------|------|-------------|
| `column_name` | `VARCHAR` | The column |
| `pii_type` | `VARCHAR` | The PII type found; `NULL` on a row without a match |
| `match_count` | `BIGINT` | Number of matches; `0` on a row without a match |
| `sample_values` | `VARCHAR[]` | Up to 5 matched values; empty on a row without a match |
| `confidence` | `DOUBLE` | Mean confidence of the matches: `1.0` for pattern types (lower when `anofox_pii_require_checksum = false` adds checksum failures at `0.6`), the mean model score for NER types; `NULL` on a row without a match |
| `values_scanned` | `BIGINT` | Number of non-`NULL` values of the column that were scanned; `0` for a skipped column |
| `status` | `VARCHAR` | `scanned`: the row reports a PII type found in the column. `no_match`: the column was scanned and nothing was found. `skipped (<type>)`, e.g. `skipped (DATE)`: the column was not scanned because it is not `VARCHAR`; name it in `columns` to scan it. `error (<message>)`: the column could not be read. `scanned (NER unavailable)` and `no_match (NER unavailable)`: as `scanned` and `no_match`, but the NER types (`NAME`, `ORGANIZATION`, `LOCATION`, `MISC`) are enabled and the model could not run (status `NOT_DOWNLOADED`, `FAILED` or `NOT_AVAILABLE` in [`anofox_ner_status`](#anofox_ner_status)): `NAME` was checked with the name dictionary only, and organizations, locations and other entities were not checked at all |

Rows come in table column order and, within a column, ordered by `pii_type`. Without `columns`
every column of the table appears; with `columns` exactly the named columns appear.
`WHERE pii_type IS NOT NULL` keeps only the PII findings.

`no_match` means that detection found nothing, not that the column holds no personal data:
detection misses values it cannot recognise, such as a two-letter first name. Without the NER
model the status says `(NER unavailable)`, so a scan run offline is not mistaken for a full one. Mask columns known
to hold PII with [`pii_mask_value`](#pii_mask_value).

`sample_values` holds matched values as they are, that is raw PII. Treat the scan result like
the table itself, or mask it before you share it.

The example tables of this page:

```sql
CREATE TABLE customers AS
SELECT * FROM (VALUES
    (101, 'anna@example.com', '123-45-6789', '4111 1111 1111 1111', 'Call me on +1 (415) 555-1234'),
    (102, 'bob@example.org',  NULL,          NULL,                  'No contact details here'),
    (103, 'not an email',     '987-65-4320', NULL,                  'Wire to DE89370400440532013000')
) t(customer_id, email, ssn, card, note);
```

```sql
SELECT column_name, pii_type, match_count, sample_values, values_scanned, status
FROM pii_scan_table('customers');
```

```text
┌─────────────┬─────────────┬─────────────┬─────────────────────────────────────┬────────────────┬───────────────────┐
│ column_name │  pii_type   │ match_count │            sample_values            │ values_scanned │      status       │
│   varchar   │   varchar   │    int64    │              varchar[]              │     int64      │      varchar      │
├─────────────┼─────────────┼─────────────┼─────────────────────────────────────┼────────────────┼───────────────────┤
│ customer_id │ NULL        │           0 │ []                                  │              0 │ skipped (INTEGER) │
│ email       │ EMAIL       │           2 │ [anna@example.com, bob@example.org] │              3 │ scanned           │
│ ssn         │ US_SSN      │           1 │ [123-45-6789]                       │              2 │ scanned           │
│ card        │ CREDIT_CARD │           1 │ [4111 1111 1111 1111]               │              1 │ scanned           │
│ note        │ IBAN        │           1 │ [DE89370400440532013000]            │              3 │ scanned           │
│ note        │ PHONE       │           1 │ ['+1 (415) 555-1234']               │              3 │ scanned           │
└─────────────┴─────────────┴─────────────┴─────────────────────────────────────┴────────────────┴───────────────────┘
```

Scan only some columns:

```sql
SELECT column_name, pii_type, match_count, status
FROM pii_scan_table('customers', 'email,note');
```

```text
┌─────────────┬──────────┬─────────────┬─────────┐
│ column_name │ pii_type │ match_count │ status  │
│   varchar   │ varchar  │    int64    │ varchar │
├─────────────┼──────────┼─────────────┼─────────┤
│ email       │ EMAIL    │           2 │ scanned │
│ note        │ IBAN     │           1 │ scanned │
│ note        │ PHONE    │           1 │ scanned │
└─────────────┴──────────┴─────────────┴─────────┘
```

---

## `pii_audit_table`

Canonical name: `anofox_tab_pii_audit_table`. Table function.

Returns one row per PII match, with the original and the masked value. Like `pii_scan_table` it
accepts regular tables and views, not TEMP tables; a TEMP table is an error that says so.

```text
pii_audit_table(table_name VARCHAR [, columns VARCHAR] [, id_column := VARCHAR]) -> TABLE(
    row_id         BIGINT,
    [<id_column>,]
    column_name    VARCHAR,
    pii_type       VARCHAR,
    original_value VARCHAR,
    masked_value   VARCHAR,
    start_pos      BIGINT,
    end_pos        BIGINT,
    confidence     DOUBLE,
    matched_text   VARCHAR
)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | The table to audit |
| `columns` | `VARCHAR` | all `VARCHAR` columns (`JSON` included) | Comma-separated column names. Names match case-insensitively; an unknown name is an error. A named column of another type (`BIGINT`, `STRUCT`, `LIST`, ...) is cast to `VARCHAR` and scanned |
| `id_column` | `VARCHAR`, named only | none | A column whose value is added after `row_id`, under its own name and type |

| Column | Type | Description |
|--------|------|-------------|
| `row_id` | `BIGINT` | 1-based row number in the table |
| `<id_column>` | the column's type | Only with `id_column`: the row's identifier |
| `column_name` | `VARCHAR` | The column with the match |
| `pii_type` | `VARCHAR` | The PII type |
| `original_value` | `VARCHAR` | The full cell value, unmasked (raw PII) |
| `masked_value` | `VARCHAR` | The cell value masked with the default strategy |
| `start_pos`, `end_pos` | `BIGINT` | Position of the match in the cell, in UTF-8 bytes |
| `confidence` | `DOUBLE` | Match confidence |
| `matched_text` | `VARCHAR` | The detected text, the part of `original_value` between `start_pos` and `end_pos` (raw PII) |

`original_value` and `matched_text` are raw PII, so the audit result contains the PII it reports. Select only
`masked_value` when the result leaves the trusted environment.

```sql
SELECT row_id, column_name, pii_type, matched_text, masked_value, start_pos, end_pos
FROM pii_audit_table('customers', 'ssn, note')
ORDER BY row_id, column_name;
```

```text
┌────────┬─────────────┬──────────┬────────────────────────┬────────────────────┬───────────┬─────────┐
│ row_id │ column_name │ pii_type │      matched_text      │    masked_value    │ start_pos │ end_pos │
│ int64  │   varchar   │ varchar  │        varchar         │      varchar       │   int64   │  int64  │
├────────┼─────────────┼──────────┼────────────────────────┼────────────────────┼───────────┼─────────┤
│      1 │ note        │ PHONE    │ +1 (415) 555-1234      │ Call me on [PHONE] │        11 │      28 │
│      1 │ ssn         │ US_SSN   │ 123-45-6789            │ [US_SSN]           │         0 │      11 │
│      3 │ note        │ IBAN     │ DE89370400440532013000 │ Wire to [IBAN]     │         8 │      30 │
└────────┴─────────────┴──────────┴────────────────────────┴────────────────────┴───────────┴─────────┘
```

With `id_column`, every finding carries the customer key, so it joins back to the table:

```sql
SELECT customer_id, column_name, pii_type, masked_value
FROM pii_audit_table('customers', id_column := 'customer_id')
ORDER BY customer_id, column_name;
```

```text
┌─────────────┬─────────────┬─────────────┬────────────────────┐
│ customer_id │ column_name │  pii_type   │    masked_value    │
│    int32    │   varchar   │   varchar   │      varchar       │
├─────────────┼─────────────┼─────────────┼────────────────────┤
│         101 │ card        │ CREDIT_CARD │ [CREDIT_CARD]      │
│         101 │ email       │ EMAIL       │ [EMAIL]            │
│         101 │ note        │ PHONE       │ Call me on [PHONE] │
│         101 │ ssn         │ US_SSN      │ [US_SSN]           │
│         102 │ email       │ EMAIL       │ [EMAIL]            │
│         103 │ note        │ IBAN        │ Wire to [IBAN]     │
└─────────────┴─────────────┴─────────────┴────────────────────┘
```

---

## `pii_status`

Canonical name: `anofox_tab_pii_status`. Table function.

Lists the registered recognizers. `enabled` tells whether the type is detected under the
current `anofox_pii_enabled_types`; with the default (empty) setting every type except
`POSTAL_CODE` is enabled.

```text
pii_status() -> TABLE(pii_type VARCHAR, recognizer_name VARCHAR, enabled BOOLEAN, pattern_info VARCHAR)
```

```sql
SELECT pii_type, recognizer_name, pattern_info FROM pii_status() ORDER BY pii_type;
```

```text
┌────────────────┬────────────────────────────────┬───────────────────────────────────────────────────────────────────────────────────────────┐
│    pii_type    │        recognizer_name         │                                       pattern_info                                        │
│    varchar     │            varchar             │                                          varchar                                          │
├────────────────┼────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────┤
│ API_KEY        │ API Key                        │ AWS/GitHub tokens and high-entropy strings                                                │
│ CREDIT_CARD    │ Credit Card                    │ Card brand prefixes (IIN) with Luhn validation                                            │
│ CRYPTO_ADDRESS │ Crypto Address                 │ Bitcoin (P2PKH/P2SH/SegWit) and Ethereum                                                  │
│ DATE_OF_BIRTH  │ Date of Birth                  │ Date after a birth keyword (geb., Geburtsdatum, DOB, born on), real calendar date         │
│ DE_ID_CARD     │ German ID Card / Passport      │ 9-character document number; check digit or keyword (Personalausweis, Reisepass) required │
│ DE_SVNR        │ German Social Insurance Number │ Rentenversicherungsnummer with check digit                                                │
│ DE_TAX_ID      │ German Tax ID                  │ 11-digit Steueridentifikationsnummer                                                      │
│ EMAIL          │ Email                          │ RFC 5322 email pattern                                                                    │
│ IBAN           │ IBAN                           │ ISO 13616: registered country, its length and MOD-97 checksum                             │
│ IP_ADDRESS     │ IP Address                     │ IPv4 and IPv6 formats                                                                     │
│ LOCATION       │ Location                       │ OpenVINO NER model (anofox_ner_model), LOC entities (cities, countries, regions)          │
│ MAC_ADDRESS    │ MAC Address                    │ Colon/hyphen/dot formats; bare hex needs letter+digit                                     │
│ MISC           │ Miscellaneous Entity           │ OpenVINO NER model distilbert-en only, MISC entities (events, nationalities, products)    │
│ NAME           │ Person Name                    │ OpenVINO NER model (anofox_ner_model), PER entities; name dictionary fallback             │
│ ORGANIZATION   │ Organization Name              │ OpenVINO NER model (anofox_ner_model), ORG entities (companies, institutions)             │
│ PHONE          │ Phone                          │ International (+XX, 00XX) and national (0XX) formats                                      │
│ POSTAL_CODE    │ Postal Code                    │ DE/AT/CH, NL and UK postcodes next to an address (off by default)                         │
│ UK_NINO        │ UK NINO                        │ UK National Insurance Number format                                                       │
│ URL            │ URL                            │ HTTP/HTTPS URLs                                                                           │
│ US_PASSPORT    │ US Passport                    │ Letter + 8 digits                                                                         │
│ US_SSN         │ US SSN                         │ XXX-XX-XXXX format with area validation                                                   │
└────────────────┴────────────────────────────────┴───────────────────────────────────────────────────────────────────────────────────────────┘
  21 rows                                                                                                                           3 columns
```

---

## `pii_config`

Canonical name: `anofox_tab_pii_config`. Table function.

Shows the current values of the PII settings.

```text
pii_config() -> TABLE(option_name VARCHAR, option_value VARCHAR, description VARCHAR)
```

```sql
SELECT option_name, option_value FROM pii_config();
```

```text
┌──────────────────────────────────┬────────────────────────────────┐
│           option_name            │          option_value          │
│             varchar              │            varchar             │
├──────────────────────────────────┼────────────────────────────────┤
│ anofox_pii_min_confidence        │ 0.500000                       │
│ anofox_pii_default_mask_strategy │ REDACT                         │
│ anofox_pii_enabled_types         │ (all types except POSTAL_CODE) │
│ anofox_pii_deep_validation       │ false                          │
│ anofox_pii_require_checksum      │ true                           │
└──────────────────────────────────┴────────────────────────────────┘
```

---

## `anofox_ner_status`

Canonical name: `anofox_ner_status`. Table function. It has no short alias.

Shows whether the NER model is available and loaded.

```text
anofox_ner_status() -> TABLE(
    onnx_available BOOLEAN,
    model_status   VARCHAR,
    model_path     VARCHAR,
    model_size_mb  DOUBLE,
    status_message VARCHAR
)
```

| Column | Type | Description |
|--------|------|-------------|
| `onnx_available` | `BOOLEAN` | `true` when the build includes OpenVINO (the name is kept for compatibility) |
| `model_status` | `VARCHAR` | See the status table below |
| `model_path` | `VARCHAR` | Path of the model file |
| `model_size_mb` | `DOUBLE` | Size of the model file |
| `status_message` | `VARCHAR` | A readable description of the state |

| `model_status` | Meaning |
|----------------|---------|
| `NOT_LOADED` | The model is not loaded yet; the first detection that needs it loads it (and downloads it first when it is not on disk) |
| `DOWNLOADING` | The model is being downloaded |
| `LOADED` | The model is loaded and NER detection runs |
| `FAILED` | Download or loading failed; `status_message` says why |
| `NOT_DOWNLOADED` | The model is not on disk and `anofox_ner_auto_download` is false, so NER detection is skipped. Reported right away, before any detection runs |
| `NOT_AVAILABLE` | The build has no OpenVINO |

```sql no-run
-- The result depends on the build and on whether the model was downloaded.
SELECT onnx_available, model_status, status_message FROM anofox_ner_status();
```

On a build with OpenVINO, after the model has been loaded, it reports `true`, `LOADED` and
`Model loaded successfully (bert-multi, device: CPU)`.

## NER model

`NAME`, `ORGANIZATION`, `LOCATION` and `MISC` are found by a NER model that runs on OpenVINO.
`anofox_ner_model` selects it:

| Model | Languages | Entity types | Download | Licence | Hugging Face repository |
|-------|-----------|--------------|----------|---------|-------------------------|
| `bert-multi` (default) | German, English, French, Spanish, Italian, Dutch, Portuguese, Latvian, Arabic, Chinese | PER, ORG, LOC | 178 MB | AFL-3.0 | `Xenova/bert-base-multilingual-cased-ner-hrl` |
| `distilbert-en` | English | PER, ORG, LOC, MISC | 66 MB | Apache-2.0 | `onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX` |
| `distilbert-multi` | as `bert-multi` | PER, ORG, LOC | 135 MB | AFL-3.0 | `Xenova/distilbert-base-multilingual-cased-ner-hrl` |

The multilingual models are the ONNX exports of Davlan's `*-multilingual-cased-ner-hrl` models.
They tag no `MISC` entities, so `MISC` is found only with `distilbert-en`; they also tag dates,
which are not reported (dates of birth are the pattern type `DATE_OF_BIRTH`).

```sql no-run
-- needs a build with OpenVINO; the default model bert-multi is downloaded on first use
SELECT pii_mask('Mit freundlichen Grüßen Jürgen Schäfer, Allianz Versicherungs-AG, München');
-- Mit freundlichen Grüßen [NAME], [ORGANIZATION], [LOCATION]

-- English-only model (66 MB, also tags MISC): set it before the first detection of the process
SET anofox_ner_model = 'distilbert-en';
```

- **Language.** `bert-multi` covers German and the other languages of the table. The English
  `distilbert-en` finds names and places in other languages only partly: on German free text it
  takes capitalised nouns for entities and misses German names and companies. On the benchmark in `test/ner_eval` (42 hand-annotated tickets and
  letters, 24 of them German) `bert-multi` finds 93% of the person names at a precision of
  0.94, `distilbert-en` 65% at 0.55; on the German texts alone 87% at 1.00 against 55% at 0.38.
  Review NER results anyway, and do not rely on them alone.
- **Choosing the model.** Set `anofox_ner_model` before the first detection that needs the
  model. Once the model is loaded, switching to another one is an error (`anofox_ner_model
  cannot be changed after the NER model has been loaded`) until the process restarts.
- **Speed.** On a 16-core desktop CPU, `bert-multi` takes about 40 ms per row of the
  benchmark, `distilbert-en` about 20 ms. `distilbert-multi` needs a fixed input length of 256
  tokens with the OpenVINO version the extension is built with (the extension detects this when
  it loads the model and logs a warning), which makes it slower than `bert-multi`: about 50 ms.
- **Download.** The first detection that needs the model downloads it (178 MB for the default
  `bert-multi`, see above) from Hugging Face into `~/.duckdb/extensions/anofox/ner/<model>/`. This applies to `pii_detect`,
  `pii_mask`, `pii_scan_table`, `pii_audit_table` and the other detection functions, unless
  `anofox_pii_enabled_types` lists only pattern-based types.
- **Offline use.** `SET anofox_ner_auto_download = false` turns the download off. NER detection
  is then skipped, the status becomes `NOT_DOWNLOADED`, `NAME` falls back to the name dictionary,
  and the pattern-based types keep working. `pii_scan_table` then reports
  `scanned (NER unavailable)` or `no_match (NER unavailable)` (see [`pii_scan_table`](#pii_scan_table)). To install the model by hand, place
  `model_quantized.onnx` and `tokenizer.json` in the model directory.
- **Platforms.** Per `vcpkg.json`, OpenVINO is built only for x64 Linux and x64 macOS, and
  `CMakeLists.txt` disables it on musl. On other builds `onnx_available` is `false` and the status
  is `NOT_AVAILABLE`. Check with `anofox_ner_status()`.
- **Whole words and priority.** A NER match always covers whole words: a span that the model ends
  inside a word is widened to the word, so masking never leaves part of a word in clear text.
  Where a NER match overlaps a pattern-based match (an IBAN, a card number, an email address, ...),
  the pattern-based match wins, so a guess like `IBAN DE` cannot leave the account number
  unmasked.
- **Codes.** A NER match that is only an ISO 3166-1 country code or ISO 4217 currency code in
  capitals (`DE`, `GB`, `EUR`, `USD`) is dropped: the model tags such codes in master data as
  organizations, and on their own they identify nobody.
- **Confidence.** NER matches below 0.7 are never reported. `anofox_pii_min_confidence`
  (default 0.5) filters on top of that.
- **Long texts.** The model accepts at most 512 tokens, and it misses entities at late
  positions well before that limit. A value longer than 256 tokens (roughly 1,200 characters of
  English text) is therefore split into windows of 256 tokens that overlap by 64 tokens, and the
  entities of all windows are combined, so a name in the signature of a long ticket is found like
  one in its first line. The cost grows linearly with the length: each further 190 tokens
  (roughly 900 characters) is one more model run.
- **Values without words.** NER skips a value that contains no word, where a word is a run of at
  least two letters between spaces or ASCII punctuation, with no digit in it (non-ASCII letters
  such as `ü` count as letters). Hex digests, UUIDs, numbers, amounts and codes such as `A-12`
  are therefore not sent through the model; `Li`, `Müller` and `J. Smith` are.
- **Words and punctuation.** The tokenizer splits words like the reference BERT tokenizer: at
  spaces (also no-break spaces), at ASCII and Unicode punctuation (`„“`, `«»`, `–`, `…`, `¿`)
  and around CJK characters. Quotes and brackets around a name are not part of the match:
  `„Sabine Weiß“` is masked as `„[NAME]“`.
- **Limits.** All-caps text can give fragmented or no results.
- **Name dictionary.** Without the model, and for a text in which the model finds no person,
  `NAME` uses a dictionary of common given names (English and Western European, and several
  hundred German, Austrian and Swiss names). A capitalised given name from the dictionary, plus up
  to two following capitalised words (`Sabine Weiß`, `Klaus-Peter Öztürk`, `Ursula von der Leyen`
  with name particles), is a match with confidence 0.5. Matching is on whole words and handles
  umlauts and accents; a surname alone (`Müller`) or a given name missing from the dictionary is
  not found.

## Throughput

Pattern-based detection is fast: plain regular expressions and checksums. NER is not: each
distinct text runs through the model, which takes in the order of tens of milliseconds per row on
a CPU, so a scan of a million free-text rows takes hours. Results are cached per distinct text
(`anofox_ner_cache_size`), which helps only for repeated values. Values without any word (ids,
hashes, numbers) skip the model, and texts longer than 256 tokens take one model run per window.
When `pii_scan_table` runs the NER types over more than 100,000 values, it logs a warning once
with this hint.

To keep large scans fast, leave the NER types out: set `anofox_pii_enabled_types` to the
pattern-based types you need (no `NAME`, `ORGANIZATION`, `LOCATION`, `MISC`), or pass the type to
`pii_mask_column`. Then no model inference runs at all. Use NER on the free-text columns that
need it, or on a sample first.

## Settings

| Setting | Purpose |
|---------|---------|
| `anofox_pii_enabled_types` | Comma-separated list of types to detect; empty means all types except `POSTAL_CODE`. `DEFAULT` stands for that default set (`'DEFAULT,POSTAL_CODE'`). An unknown type is an error. |
| `anofox_pii_require_checksum` | `true` (default): an IBAN or card number must pass its checksum. `false`: values of the right shape that fail it are reported too, at confidence `0.6`, so mistyped account and card numbers are masked as well |
| `anofox_pii_default_mask_strategy` | The strategy of `pii_redact_column(value)` and of `masked_value` in `pii_audit_table`; `none` is rejected |
| `anofox_pii_min_confidence` | Minimum confidence of a match; dictionary name matches have 0.5, pattern matches 1.0 |
| `anofox_pii_deep_validation` | Validate detected phone numbers with the built-in phone module. A match is dropped only when the module can judge the number and finds its length impossible: a number with `+CC` (or the default region's international prefix) for that country, a national number for `anofox_tab_phonenumber_default_region`. A national number in another country's format, such as `0171 1234567` with the default region `US`, cannot be judged and stays a match. |
| `anofox_ner_auto_download` | Download the NER model on first use |
| `anofox_ner_model` | The NER model: `distilbert-en`, `distilbert-multi` or `bert-multi` (see [NER model](#ner-model)); set it before the model is loaded |
| `anofox_ner_device` | OpenVINO device (`AUTO`, `CPU`, `GPU`, ...) |
| `anofox_ner_cache_size` | LRU cache size for NER results; 0 disables it |

Types, defaults and details are in [Configuration: PII](../reference/configuration.md#pii).

```sql
SET anofox_pii_enabled_types = 'EMAIL,US_SSN';
SELECT pii_mask('anna@example.com, 123-45-6789, +1 (415) 555-1234') AS masked;
```

```text
┌──────────────────────────────────────┐
│                masked                │
│               varchar                │
├──────────────────────────────────────┤
│ [EMAIL], [US_SSN], +1 (415) 555-1234 │
└──────────────────────────────────────┘
```

```sql
RESET anofox_pii_enabled_types;
```

A mistyped IBAN (last digit wrong) or card number fails its checksum and is left in clear text.
For masking before an export, turn the checksum requirement off. An IBAN must then still have
the length registered for its country, which also decides where it ends; the validation
functions (`pii_is_valid_iban`, ...) stay strict:

```sql
SET anofox_pii_enabled_types = 'IBAN,CREDIT_CARD';
SET anofox_pii_require_checksum = false;
SELECT m.type, m.text, m.confidence
FROM (SELECT unnest(pii_detect('Erstattung auf DE89 3704 0044 0532 0130 01, Karte 4111 1111 1111 1112')) AS m);
```

```text
┌─────────────┬─────────────────────────────┬────────────┐
│    type     │            text             │ confidence │
│   varchar   │           varchar           │   double   │
├─────────────┼─────────────────────────────┼────────────┤
│ IBAN        │ DE89 3704 0044 0532 0130 01 │        0.6 │
│ CREDIT_CARD │ 4111 1111 1111 1112         │        0.6 │
└─────────────┴─────────────────────────────┴────────────┘
```

```sql
RESET anofox_pii_require_checksum;
RESET anofox_pii_enabled_types;
```

## See also

- [Guide: find and mask PII](../guides/04-pii.md)
- [Configuration: PII](../reference/configuration.md#pii)
- [Network and privacy](../reference/network-and-privacy.md)
- [Conventions](00-conventions.md)

# Network and Privacy

> What anofox-tabular sends or fetches over the network, and how to turn each part off.

**Use this page to:**
- Run the extension without network access
- Know which functions download data, and how much
- Opt out of telemetry and the feedback banner

The core functions run locally inside DuckDB: validation, money, VAT, the quality checks,
anomaly detection, diffing, profiling and the pattern-based PII recognizers. They read only
the tables you pass and send nothing.

## What uses the network

| Feature | What it fetches or sends | When | Opt out |
|---------|--------------------------|------|---------|
| PII name, organization and location detection (NER) | The NER model chosen with `anofox_ner_model` (66 MB for `distilbert-en`, 135 MB for `distilbert-multi`, 178 MB for `bert-multi`), from Hugging Face, into `~/.duckdb/extensions/anofox/ner/<model>/` | First NER-based detection, e.g. the first `pii_detect` call on text, when the model is not on disk | `SET anofox_ner_auto_download = false`: NER detection is skipped (status `NOT_DOWNLOADED`), the pattern recognizers keep working |
| Postal address data | libpostal data, about 500 MB, from `public-read-libpostal-data.s3.amazonaws.com`, into `anofox_tab_postal_data_path` | `postal_load_data()`, or the first `postal_parse_address` / `postal_expand_address` call when the data is missing | Do not call the postal functions |
| Email modes `dns` and `smtp` | DNS MX query for the domain; for `smtp` also a TCP connection to the mail host (port 25 by default) | `email_validate` / `email_is_valid` with mode `dns` or `smtp` | Use mode `regex`, the default |
| Telemetry | Anonymous usage events to PostHog EU | On by default: one event when the extension loads, then counts of function calls | `SET anofox_telemetry_enabled = false`, or the environment variable `DATAZOO_DISABLE_TELEMETRY=1` (also stops the load event) |
| Feedback banner | Nothing; it prints a local message | In an interactive terminal, at most once a day | `SET datazoo_banner = false`, or `DATAZOO_NO_BANNER=1` |

## Offline use

To run without any network access, set these before you use the PII or email functions:

```sql
SET anofox_ner_auto_download = false;
SET anofox_telemetry_enabled = false;
SET anofox_tab_email_default_validation = 'regex';
```

Then do not pass `dns` or `smtp` to the email functions, and do not call the postal functions
unless the libpostal data is already in `anofox_tab_postal_data_path`. Start DuckDB with
`DATAZOO_DISABLE_TELEMETRY=1` to also suppress the telemetry event that is sent when the
extension loads.

With NER off, the pattern-based PII types (email, phone, IBAN, credit card, SSN and others)
work as before:

```sql
SET anofox_ner_auto_download = false;
SELECT pii_mask('Mail jane@example.com, IBAN DE89370400440532013000') AS masked;
```

```text
┌───────────────────────────┐
│          masked           │
│          varchar          │
├───────────────────────────┤
│ Mail [EMAIL], IBAN [IBAN] │
└───────────────────────────┘
```

## NER model

The model is only used for the NER-based PII types: `NAME`, `ORGANIZATION`, `LOCATION` and
`MISC`. It is downloaded once and then loaded from disk. Check its state with
[`anofox_ner_status()`](../api/06-pii.md#anofox_ner_status).

NER needs OpenVINO, which is part of the build only on some platforms. According to
`vcpkg.json` it is linked for Linux x64 and macOS x64. On other platforms (macOS arm64,
Windows) `anofox_ner_status()` reports `onnx_available = false`, nothing is downloaded, and
`NAME` falls back to a built-in name dictionary.

## Telemetry

Telemetry is on by default. It sends:

- an `extension_loaded` event when the extension loads;
- one `function_executed` event per function and session, with the function name, the number
  of calls and the median duration.

Every event carries the extension and DuckDB version, the platform, flags for CI and
containers, and a pseudonymous machine id (a SHA-256 hash). It never contains your data: no
input values, table or column names, SQL text or error messages.

Turn it off with either of:

```sql no-run
SET anofox_telemetry_enabled = false;
```

```bash
export DATAZOO_DISABLE_TELEMETRY=1
```

A `SET` runs after the extension has loaded, so it stops every later event but not the load
event. The environment variable stops both. The full description is in
[TELEMETRY.md](../../TELEMETRY.md).

## Feedback banner

When you load the extension in an interactive terminal, a short banner asks for feedback, at
most once a day. It is printed locally and sends nothing. It does not print when the output is
piped, in notebooks or in CI. Hide it with `SET datazoo_banner = false;` or
`DATAZOO_NO_BANNER=1`.

## See also

- [Configuration](configuration.md)
- [PII detection](../api/06-pii.md)
- [Postal addresses](../api/02-postal.md)
- [Email validation](../api/01-email.md)

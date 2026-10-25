# Configuration

> Every setting of anofox-tabular, with its type, default and effect.

**Use this page to:**
- Find the setting that controls a module
- Change a setting with `SET`
- Read the current values

Set a value with `SET name = value;` and read it with `current_setting('name')` or
`duckdb_settings()`. All settings have global scope.

The names use four prefixes: `anofox_tab_` for the email, phone, postal and tracing settings,
`anofox_pii_` and `anofox_ner_` for PII detection, `anofox_telemetry_` for telemetry, and
`datazoo_banner` for the feedback banner that DataZoo extensions share.

```sql
SELECT name, input_type
FROM duckdb_settings()
WHERE name LIKE 'anofox_tab_%'
ORDER BY name;
```

```text
┌──────────────────────────────────────────┬────────────┐
│                   name                   │ input_type │
│                 varchar                  │  varchar   │
├──────────────────────────────────────────┼────────────┤
│ anofox_tab_email_default_validation      │ VARCHAR    │
│ anofox_tab_email_dns_timeout_ms          │ BIGINT     │
│ anofox_tab_email_dns_tries               │ INTEGER    │
│ anofox_tab_email_regex_pattern           │ VARCHAR    │
│ anofox_tab_email_smtp_connect_timeout_ms │ BIGINT     │
│ anofox_tab_email_smtp_helo_domain        │ VARCHAR    │
│ anofox_tab_email_smtp_mail_from          │ VARCHAR    │
│ anofox_tab_email_smtp_port               │ INTEGER    │
│ anofox_tab_email_smtp_read_timeout_ms    │ BIGINT     │
│ anofox_tab_phonenumber_default_region    │ VARCHAR    │
│ anofox_tab_postal_data_path              │ VARCHAR    │
│ anofox_tab_trace_enabled                 │ BOOLEAN    │
│ anofox_tab_trace_level                   │ VARCHAR    │
└──────────────────────────────────────────┴────────────┘
  13 rows                                     2 columns
```

## All settings

| Setting | Type | Default | Module |
|---------|------|---------|--------|
| `anofox_tab_email_default_validation` | `VARCHAR` | `regex` | [Email](#email) |
| `anofox_tab_email_regex_pattern` | `VARCHAR` | see [Email](#email) | [Email](#email) |
| `anofox_tab_email_dns_timeout_ms` | `BIGINT` | `1000` | [Email](#email) |
| `anofox_tab_email_dns_tries` | `INTEGER` | `1` | [Email](#email) |
| `anofox_tab_email_smtp_port` | `INTEGER` | `25` | [Email](#email) |
| `anofox_tab_email_smtp_connect_timeout_ms` | `BIGINT` | `5000` | [Email](#email) |
| `anofox_tab_email_smtp_read_timeout_ms` | `BIGINT` | `5000` | [Email](#email) |
| `anofox_tab_email_smtp_helo_domain` | `VARCHAR` | `duckdb.local` | [Email](#email) |
| `anofox_tab_email_smtp_mail_from` | `VARCHAR` | `validator@duckdb.local` | [Email](#email) |
| `anofox_tab_phonenumber_default_region` | `VARCHAR` | `US` | [Phone](#phone) |
| `anofox_tab_postal_data_path` | `VARCHAR` | `.duckdb/extensions/libpostal` | [Postal](#postal) |
| `anofox_tab_trace_enabled` | `BOOLEAN` | `true` | [Tracing](#tracing) |
| `anofox_tab_trace_level` | `VARCHAR` | `info` | [Tracing](#tracing) |
| `anofox_pii_enabled_types` | `VARCHAR` | empty (all types except `POSTAL_CODE`) | [PII](#pii) |
| `anofox_pii_min_confidence` | `DOUBLE` | `0.5` | [PII](#pii) |
| `anofox_pii_default_mask_strategy` | `VARCHAR` | `redact` | [PII](#pii) |
| `anofox_pii_deep_validation` | `BOOLEAN` | `false` | [PII](#pii) |
| `anofox_pii_require_checksum` | `BOOLEAN` | `true` | [PII](#pii) |
| `anofox_ner_auto_download` | `BOOLEAN` | `true` | [NER](#ner) |
| `anofox_ner_model` | `VARCHAR` | `distilbert-en` | [NER](#ner) |
| `anofox_ner_device` | `VARCHAR` | `AUTO` | [NER](#ner) |
| `anofox_ner_cache_size` | `BIGINT` | `10000` | [NER](#ner) |
| `anofox_telemetry_enabled` | `BOOLEAN` | `true` (`false` when `DATAZOO_DISABLE_TELEMETRY` is set) | [Telemetry](#telemetry) |
| `anofox_telemetry_key` | `VARCHAR` | the project's PostHog key | [Telemetry](#telemetry) |
| `datazoo_banner` | `BOOLEAN` | `true` | [Banner](#banner) |

The postal setting exists only in builds with the postal module (not on Windows). The defaults above are
the values `duckdb_settings()` reports in a new session.

## Email

Used by [`email_is_valid` and `email_validate`](../api/01-email.md).

| Setting | Effect |
|---------|--------|
| `anofox_tab_email_default_validation` | Mode used when the `mode` argument is omitted or `NULL`: `regex`, `dns` or `smtp` |
| `anofox_tab_email_regex_pattern` | Regular expression of the `regex` stage (ECMAScript syntax). Default: ``^[A-Za-z0-9!#$%&'*+/=?^_`{\|}~-]+(?:\.[A-Za-z0-9!#$%&'*+/=?^_`{\|}~-]+)*@(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)*(?![0-9]+$)[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$``; see [Syntax rules](../api/01-email.md#syntax-rules-of-the-regex-stage). The 254/64-character length limits apply to any pattern. `RESET` restores the default |
| `anofox_tab_email_dns_timeout_ms` | Timeout of one DNS query, in milliseconds (1 to 5000) |
| `anofox_tab_email_dns_tries` | Number of DNS attempts before the stage fails (1 to 10) |
| `anofox_tab_email_smtp_port` | Port of the MX host for the `smtp` stage (1 to 65535) |
| `anofox_tab_email_smtp_connect_timeout_ms` | TCP connect timeout of the `smtp` stage, in milliseconds (1 to 5000) |
| `anofox_tab_email_smtp_read_timeout_ms` | Read and write timeout of the `smtp` stage, in milliseconds (1 to 5000) |
| `anofox_tab_email_smtp_helo_domain` | Domain sent with `EHLO` |
| `anofox_tab_email_smtp_mail_from` | Address sent with `MAIL FROM` |

`email_config()` lists the current email values:

```sql
SET anofox_tab_email_dns_timeout_ms = 2000;
SELECT key, value FROM email_config() WHERE key LIKE 'dns%';
```

```text
┌───────────────────────┬─────────┐
│          key          │  value  │
│        varchar        │ varchar │
├───────────────────────┼─────────┤
│ dns_timeout_ms        │ 2000    │
│ dns_tries             │ 1       │
│ dns_cache_ttl_s       │ 300     │
│ dns_cache_max_entries │ 10000   │
└───────────────────────┴─────────┘
```

## Phone

| Setting | Effect |
|---------|--------|
| `anofox_tab_phonenumber_default_region` | Region used by the [phone functions](../api/03-phone.md) when the `region` argument is `NULL`. Must be one of the 21 supported region codes; case-insensitive, surrounding spaces are ignored and `UK` is stored as `GB`. Read when a query is bound. |

## Postal

| Setting | Effect |
|---------|--------|
| `anofox_tab_postal_data_path` | Directory of the libpostal data. The default is relative to the working directory of the process; `~` is expanded. See [Postal addresses](../api/02-postal.md#where-the-data-goes). |

## Tracing

The extension logs through spdlog to stderr. Lines start with `[anofox] [<level>]`.

| Setting | Effect |
|---------|--------|
| `anofox_tab_trace_enabled` | `false` turns all tracing output off |
| `anofox_tab_trace_level` | Lowest level that is printed: `trace`, `debug`, `info`, `warn`, `error`, `critical` or `off` |

Tracing is on at level `info` by default, so the CLI prints a few `[anofox] [info]` lines when the
extension loads and when some functions run. To keep only warnings and errors:

```sql
SET anofox_tab_trace_level = 'warn';
SELECT current_setting('anofox_tab_trace_level') AS level;
```

```text
┌─────────┐
│  level  │
│ varchar │
├─────────┤
│ warn    │
└─────────┘
```

## PII

Used by the [PII functions](../api/06-pii.md).

| Setting | Effect |
|---------|--------|
| `anofox_pii_enabled_types` | Comma-separated list of PII types to detect, e.g. `'EMAIL,US_SSN'`. Empty means all types except `POSTAL_CODE`, which is detected only when listed. `DEFAULT` stands for the default set, so `'DEFAULT,POSTAL_CODE'` adds postal codes. All PII functions honor it, including `pii_mask`, `pii_scan_table` and `pii_audit_table`; an explicit type argument (`pii_mask_column`) overrides it. |
| `anofox_pii_min_confidence` | Matches below this confidence (0.0 to 1.0) are dropped. Pattern matches have confidence 1.0; dictionary name matches have 0.5. |
| `anofox_pii_default_mask_strategy` | Strategy of `pii_redact_column(value)` without a strategy argument and of the `masked_value` column of `pii_audit_table`: `redact`, `hash`, `partial` or `asterisk` (case-insensitive). `none` is rejected, because a default that turns masking off would expose raw PII; pass `'none'` as an explicit argument instead. `pii_mask(text)` always redacts. |
| `anofox_pii_deep_validation` | `true` validates detected phone numbers with the phone module and drops a match only when the module can judge the number and finds its length impossible. A number with `+CC`, or with the international prefix of `anofox_tab_phonenumber_default_region` (`00` for `DE`, `011` for `US`), is judged for that country; a national number is judged for `anofox_tab_phonenumber_default_region` when it is written in that region's format. A national number in another country's format (`0171 1234567` or `0049 171 1234567` with the default region `US`) cannot be judged and is kept, so deep validation never unmasks a number because the default region differs. |
| `anofox_pii_require_checksum` | `true` (default) reports an IBAN or credit card number only when its checksum (MOD-97, Luhn) holds. `false` also reports values of the right shape whose checksum fails, at confidence `0.6`, so mistyped numbers are masked too; an IBAN must still have its country's length. `pii_is_valid_iban` and `pii_is_valid_credit_card` stay strict. A `anofox_pii_min_confidence` above 0.6 drops these matches again. |

`pii_config()` lists the current PII values.

```sql
SET anofox_pii_enabled_types = 'EMAIL';
SELECT pii_mask('Mail a@b.com or call (555) 123-4567') AS masked;
```

```text
┌─────────────────────────────────────┐
│               masked                │
│               varchar               │
├─────────────────────────────────────┤
│ Mail [EMAIL] or call (555) 123-4567 │
└─────────────────────────────────────┘
```

## NER

Named-entity recognition finds person names, organizations, locations and other entities. It
uses an OpenVINO model that is downloaded on first use. See
[PII detection](../api/06-pii.md#anofox_ner_status).

| Setting | Effect |
|---------|--------|
| `anofox_ner_auto_download` | `true` downloads the model (~66 MB) from Hugging Face when it is not on disk. `false` never downloads: NER detection is skipped, the pattern recognizers keep working, and `anofox_ner_status()` reports `NOT_DOWNLOADED` while the model is not on disk. |
| `anofox_ner_model` | Model name. Only `distilbert-en` is supported. Set it before the model is first loaded. |
| `anofox_ner_device` | OpenVINO device: `AUTO`, `CPU`, `GPU`, `GPU.0`, ... Takes effect at the next model load. |
| `anofox_ner_cache_size` | Number of NER results kept in an LRU cache; `0` disables the cache |

## Telemetry

| Setting | Effect |
|---------|--------|
| `anofox_telemetry_enabled` | `false` stops all later telemetry events. Its default is `true`, or `false` when the environment variable `DATAZOO_DISABLE_TELEMETRY` is set. |
| `anofox_telemetry_key` | PostHog project key the events are sent with |

A `SET` runs after the extension has loaded, so it cannot stop the one `extension_loaded` event.
Set `DATAZOO_DISABLE_TELEMETRY=1` in the environment to stop that one too. See
[Network and privacy](network-and-privacy.md#telemetry) and [TELEMETRY.md](../../TELEMETRY.md).

## Banner

| Setting | Effect |
|---------|--------|
| `datazoo_banner` | `false` hides the feedback banner. The banner prints at most once a day per extension, and only in an interactive terminal. The environment variable `DATAZOO_NO_BANNER=1` hides it too. |

## See also

- [Network and privacy](network-and-privacy.md)
- [Troubleshooting](troubleshooting.md)
- [API conventions](../api/00-conventions.md)

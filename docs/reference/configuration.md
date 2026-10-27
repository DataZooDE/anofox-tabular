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
| `anofox_tab_postal_data_path` | `VARCHAR` | `~/.duckdb/extensions/libpostal` | [Postal](#postal) |
| `anofox_tab_trace_enabled` | `BOOLEAN` | `true` | [Tracing](#tracing) |
| `anofox_tab_trace_level` | `VARCHAR` | `info` | [Tracing](#tracing) |
| `anofox_pii_enabled_types` | `VARCHAR` | empty (all types except `POSTAL_CODE`) | [PII](#pii) |
| `anofox_pii_min_confidence` | `DOUBLE` | `0.5` | [PII](#pii) |
| `anofox_pii_default_mask_strategy` | `VARCHAR` | `redact` | [PII](#pii) |
| `anofox_pii_deep_validation` | `BOOLEAN` | `false` | [PII](#pii) |
| `anofox_pii_require_checksum` | `BOOLEAN` | `true` | [PII](#pii) |
| `anofox_ner_auto_download` | `BOOLEAN` | `true` | [NER](#ner) |
| `anofox_ner_model` | `VARCHAR` | `bert-multi` | [NER](#ner) |
| `anofox_ner_device` | `VARCHAR` | `AUTO` | [NER](#ner) |
| `anofox_ner_cache_size` | `BIGINT` | `10000` | [NER](#ner) |
| `anofox_ner_required` | `BOOLEAN` | `false` | [NER](#ner) |
| `anofox_telemetry_enabled` | `BOOLEAN` | `true` (`false` when `DATAZOO_DISABLE_TELEMETRY` is set) | [Telemetry](#telemetry) |
| `anofox_telemetry_key` | `VARCHAR` | the project's PostHog key | [Telemetry](#telemetry) |
| `datazoo_banner` | `BOOLEAN` | `true` | [Banner](#banner) |

The postal setting exists only in builds with the postal module (not on Windows). The defaults above are
the values `duckdb_settings()` reports in a new session.

## Email

Used by [`email_is_valid` and `email_validate`](../api/01-email.md).

| Setting | Effect |
|---------|--------|
| `anofox_tab_email_default_validation` | Mode used when the `mode` argument is omitted or `NULL`: `regex`, `dns` or `smtp` (`full` is stored as `smtp`); case-insensitive, an unknown mode raises an error |
| `anofox_tab_email_regex_pattern` | Regular expression of the `regex` stage, in [RE2 syntax](https://github.com/google/re2/wiki/Syntax) (linear-time matching; no backreferences, lookahead or lookbehind). It must match the whole address. An invalid or unsupported pattern is an error at `SET`, and the previous pattern stays. Default: ``^[A-Za-z0-9!#$%&'*+/=?^_`{\|}~-]+(?:\.[A-Za-z0-9!#$%&'*+/=?^_`{\|}~-]+)*@(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)*[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$``, plus the rule that the last domain label is not all digits; see [Syntax rules](../api/01-email.md#syntax-rules-of-the-regex-stage) and [Custom patterns](../api/01-email.md#custom-patterns). The 254/64-character length limits apply to any pattern. `RESET` restores the default |
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
| `anofox_tab_phonenumber_default_region` | Region used by the [phone functions](../api/03-phone.md) when the `region` argument is `NULL`. Must be one of the 21 supported region codes, else `SET` raises an error (a per-call `region` argument with an unsupported code instead makes only that row invalid, reason `unsupported_region`); case-insensitive, surrounding spaces are ignored and `UK` is stored as `GB`. Read when a query is bound. A number without `+` that is invalid in this region when no `region` argument is given has the `phonenumber_parse` reason `region_missing`. The PII `hash` strategy also uses it: a detected national phone number is hashed in its E.164 form for this region (see [Hashing](../api/06-pii.md#hashing)). |

## Postal

| Setting | Effect |
|---------|--------|
| `anofox_tab_postal_data_path` | Directory of the libpostal data. The default is in the home directory (`~` is expanded with DuckDB's `home_directory` setting, else `HOME`, or `USERPROFILE` on Windows). At the default, a complete copy in `.duckdb/extensions/libpostal` under the working directory, the default of v2026.10.25 and older, is still used when the home directory has none. `RESET` returns to the default. See [Postal addresses](../api/02-postal.md#where-the-data-goes). |

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
| `anofox_pii_min_confidence` | Matches below this confidence (0.0 to 1.0) are dropped, whatever their type. Pattern matches have confidence 1.0, checksum failures (`anofox_pii_require_checksum = false`) 0.6, dictionary name matches 0.5, and NER matches the model score (at least 0.7). |
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

### An export preset

The PII defaults are set for discovery: `anofox_pii_require_checksum = true` is deliberate, so a
scan does not report every order or invoice number of the right length as an IBAN or card number.
A masking run before an export has the opposite priority: a mistyped account number must be
masked, and a false positive only masks a digit run. Set the run's settings explicitly instead
of relying on whatever the session holds, including `anofox_pii_min_confidence`, which must stay
at or below `0.6` or it drops the checksum-failing matches again:

```sql
SET anofox_pii_require_checksum = false;
SET anofox_pii_min_confidence = 0.5;
SET anofox_pii_enabled_types = 'EMAIL,PHONE,IBAN,CREDIT_CARD,NAME';
SET anofox_pii_default_mask_strategy = 'redact';
SELECT pii_mask('Refund to DE89 3704 0044 0532 0130 01, card 4111 1111 1111 1112') AS masked;
```

```text
┌──────────────────────────────────────┐
│                masked                │
│               varchar                │
├──────────────────────────────────────┤
│ Refund to [IBAN], card [CREDIT_CARD] │
└──────────────────────────────────────┘
```

Settings are process-wide and a later `SET` changes them for every connection, so record the
effective values next to the scan or export they belong to:

```sql
SELECT current_setting('anofox_pii_require_checksum') AS require_checksum,
       current_setting('anofox_pii_min_confidence') AS min_confidence,
       current_setting('anofox_pii_enabled_types') AS enabled_types,
       current_setting('anofox_pii_default_mask_strategy') AS mask_strategy,
       current_setting('anofox_ner_model') AS ner_model;
```

```text
┌──────────────────┬────────────────┬───────────────────────────────────┬───────────────┬────────────┐
│ require_checksum │ min_confidence │           enabled_types           │ mask_strategy │ ner_model  │
│     boolean      │     double     │              varchar              │    varchar    │  varchar   │
├──────────────────┼────────────────┼───────────────────────────────────┼───────────────┼────────────┤
│ false            │            0.5 │ EMAIL,PHONE,IBAN,CREDIT_CARD,NAME │ REDACT        │ bert-multi │
└──────────────────┴────────────────┴───────────────────────────────────┴───────────────┴────────────┘
```

`pii_config()` returns the same values as rows. Reset the preset when the run is done:

```sql
RESET anofox_pii_require_checksum;
RESET anofox_pii_min_confidence;
RESET anofox_pii_enabled_types;
RESET anofox_pii_default_mask_strategy;
```

Masking with this preset still leaves the PII that detection misses in clear text; see the
residual-risk note in [`pii_audit_table`](../api/06-pii.md#pii_audit_table).

## NER

Named-entity recognition finds person names, organizations, locations and other entities. It
uses an OpenVINO model that is downloaded on first use. See
[PII detection](../api/06-pii.md#anofox_ner_status).

| Setting | Effect |
|---------|--------|
| `anofox_ner_auto_download` | `true` downloads the model (178 MB for the default `bert-multi`, see `anofox_ner_model`) from Hugging Face when it is not on disk. `false` never downloads: NER detection is skipped, the pattern recognizers keep working, `NAME` falls back to a name dictionary, `anofox_ner_status()` reports `NOT_DOWNLOADED` while the model is not on disk, and `pii_scan_table` marks its status with `(NER unavailable)`. |
| `anofox_ner_model` | Model name: `bert-multi` (default) or `distilbert-multi` (German, English, French, Spanish, Italian, Dutch, Portuguese, Latvian, Arabic, Chinese), or `distilbert-en` (English only, 66 MB, the only model that tags `MISC`). Set it before the model is first loaded; switching afterwards is an error until the process restarts. Setting it after a load that failed (`FAILED`) or found no model (`NOT_DOWNLOADED`) makes the next detection load again. See [NER model](../api/06-pii.md#ner-model). |
| `anofox_ner_device` | OpenVINO device: `AUTO` (picks a GPU if there is one, else the CPU), `CPU`, `GPU`, `NPU`, an indexed device such as `GPU.0` or `NPU.1`, or `AUTO:`, `HETERO:` or `MULTI:` followed by a comma-separated list of these devices (`HETERO:GPU,CPU`). Other names are an error at `SET`; names are stored in upper case. The device takes effect at the next model load: setting it after a load that failed (for example on `GPU` without a GPU) makes the next detection load the model again. Once the model is loaded, a new device takes effect only in a new process. |
| `anofox_ner_cache_size` | Number of NER results kept in an LRU cache; `0` disables the cache |
| `anofox_ner_required` | `false` (default): when the model is not available (`NOT_DOWNLOADED`, `FAILED`, `NOT_AVAILABLE`), detection goes on without it: `NAME` falls back to the name dictionary, and `ORGANIZATION`, `LOCATION` and `MISC` are not detected. `true`: a detection that needs the model (the default types, or a type list with `NAME`, `ORGANIZATION`, `LOCATION` or `MISC`) raises an error that names the status and how to fix it. Detections of pattern-based types only are not affected. |

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

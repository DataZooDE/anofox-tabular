# Anofox Tabular DuckDB Extension

Anofox Tabular enriches DuckDB with production‑ready validation primitives for customer data. It provides:

- **Email validation** with regex, DNS and SMTP checks, including configurable timeouts and structured results.
- **Postal parsing & expansion** powered by libpostal, plus helpers to manage the required data bundles.
- **Phone number parsing/formatting** backed by libphonenumber for international coverage.
- **Centralised tracing** (via spdlog) that can be toggled and levelled from SQL to aid debugging.

The project builds on DuckDB’s extension template but replaces the placeholder code with a realistic data quality toolkit.

---

## Quick Start

```bash
git clone https://github.com/datazoo/anofox-tabular.git
cd anofox-tabular
git submodule update --init --recursive
make

# launch DuckDB with the extension preloaded
./build/release/duckdb
```

From inside DuckDB:

```sql
LOAD anofox_tabular;
SELECT * FROM anofox_email_config();
```

The build produces three primary artefacts:

- `build/release/duckdb` – DuckDB CLI with the extension statically linked.
- `build/release/test/unittest` – DuckDB’s test runner including the extension.
- `build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension` – the distributable loadable binary.

### Dependencies

Vcpkg is used to fetch libpostal, libphonenumber, c-ares and spdlog. If you do not have a vcpkg checkout yet:

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

The extension’s `vcpkg.json` pins the required libraries – building the project will install them automatically.

## Feature Overview

### Email Verification
- `anofox_email_is_valid(email [, mode])` → boolean.
- `anofox_email_validate(email [, mode])` → struct report.
- `anofox_email_config()` → table of active configuration.

Validation modes: `regex` (default), `dns`, `smtp`, or `full` (alias for `smtp`). SMTP verification respects per-host and overall timeouts and records a transcript.

### Postal Utilities
- `anofox_postal_parse_address(address)` → struct with house_number, road, city, state, postcode, country.
- `anofox_postal_expand_address(address)` → list of normalised address variants.
- `anofox_postal_load_data()` → downloads and extracts the libpostal assets into the configured directory.
- `anofox_postal_status()` → table describing library status and data directory.

### Phone Number Utilities
- `anofox_phonenumber_parse(number, region_hint)` → struct with validity, country code, national number, inferred region and type.
- `anofox_phonenumber_format(number, region_hint, format)` → formatted string (`E164`, `INTERNATIONAL`, `NATIONAL`, `RFC3966`).
- `anofox_phonenumber_region(number, region_hint)` → ISO region code.
- `anofox_phonenumber_status()` → table reporting libphonenumber initialisation state and default region.

### Tracing
Logs are routed through spdlog and gated by two options:

```sql
SET anofox_trace_enabled = true;
SET anofox_trace_level = 'debug';  -- trace|debug|info|warn|error|critical|off
```

Messages are prefixed with the originating subsystem (`[anofox] email: …`, etc.) to simplify parsing in CI logs.

## SQL Function Reference

| Function | Kind | Signature | Notes |
|----------|------|-----------|-------|
| `anofox_email_is_valid` | Scalar | `(VARCHAR email [, VARCHAR mode]) → BOOLEAN` | Quick boolean verdict. |
| `anofox_email_validate` | Scalar | `(VARCHAR email [, VARCHAR mode]) → STRUCT(valid BOOLEAN, stage VARCHAR, reason VARCHAR, mx_hosts LIST<VARCHAR>, smtp_debug STRUCT(transcript LIST<VARCHAR>))` | Detailed validation with MX list and SMTP transcript. |
| `anofox_email_config` | Table | `() → TABLE(key VARCHAR, value VARCHAR)` | Current configuration snapshot. |
| `anofox_postal_parse_address` | Scalar | `(VARCHAR address) → STRUCT(house_number, road, city, state, postcode, country)` | Wraps libpostal parser. |
| `anofox_postal_expand_address` | Scalar | `(VARCHAR address) → LIST<VARCHAR>` | Generates expanded address candidates. |
| `anofox_postal_status` | Table | `() → TABLE(initialized BOOLEAN, data_present BOOLEAN, data_dir VARCHAR)` | Verifies libpostal availability. |
| `anofox_postal_load_data` | Scalar | `() → BOOLEAN` | Downloads/extracts assets; returns true on success. |
| `anofox_phonenumber_parse` | Scalar | `(VARCHAR number, VARCHAR region_hint) → STRUCT(valid BOOLEAN, country_code INTEGER, national_number VARCHAR, region VARCHAR, type VARCHAR)` | Region hint defaults to configured option when NULL. |
| `anofox_phonenumber_format` | Scalar | `(VARCHAR number, VARCHAR region_hint, VARCHAR format) → VARCHAR` | Throws on invalid inputs. |
| `anofox_phonenumber_region` | Scalar | `(VARCHAR number, VARCHAR region_hint) → VARCHAR` | Returns best-effort ISO code. |
| `anofox_phonenumber_status` | Table | `() → TABLE(initialized BOOLEAN, default_region VARCHAR)` | Reports manager state. |

## Configuration Options

Set options with `SET …` or persist via DuckDB’s configuration mechanisms.

| Option | Description | Default |
|--------|-------------|---------|
| `anofox_trace_enabled` | Enable/disable tracing output globally. | `true` |
| `anofox_trace_level` | Minimum trace level (`trace/debug/info/warn/error/critical/off`). | `info` |
| `anofox_email_default_validation` | Default mode for `anofox_email_is_valid`. | `regex` |
| `anofox_email_regex_pattern` | ECMAScript regex used in the first stage. | RFC 5322-inspired pattern |
| `anofox_email_dns_timeout_ms` | Per-try DNS timeout (1–5000 ms). | `1000` |
| `anofox_email_dns_tries` | DNS retries before giving up. | `1` |
| `anofox_email_smtp_port` | SMTP port used for MX hosts. | `25` |
| `anofox_email_smtp_connect_timeout_ms` | TCP connect timeout (1–5000 ms). | `5000` |
| `anofox_email_smtp_read_timeout_ms` | Read/write timeout (1–5000 ms). | `5000` |
| `anofox_email_smtp_helo_domain` | HELO/EHLO domain presented to servers. | `duckdb.local` |
| `anofox_email_smtp_mail_from` | MAIL FROM address used during verification. | `validator@duckdb.local` |
| `anofox_postal_data_path` | Directory where libpostal assets are stored. | `.duckdb/extensions/libpostal` |
| `anofox_phonenumber_default_region` | ISO region used when the hint is NULL. | `US` |

### Libpostal Assets

Libpostal requires data files (~500 MB). Either download them manually to `anofox_postal_data_path` or let the extension do it:

```sql
CALL anofox_postal_load_data(); -- returns true on success
```

Ensure the DuckDB process has write permissions in the target directory.

## Development & Testing

- `make` – configure and build release binaries with the extension.
- `make test` – run the SQL test suite under `test/sql`.
- `cmake --build build/release --config Debug` – build a debug flavour if needed.

All tracing is disabled via `SET anofox_trace_enabled = false` during tests if noise becomes an issue.

## Distribution Checklist

1. Build the loadable bundle: `cmake --build build/release --target anofox_tabular_loadable_extension`.
2. Upload `anofox_tabular.duckdb_extension` alongside the versioned metadata generated by DuckDB’s tooling.
3. Users install via DuckDB’s standard `INSTALL/LOAD` commands once the custom repository path is set.

---

This repository started from the DuckDB extension template but now serves as a reference implementation for data-quality focused tooling inside DuckDB. Contributions and bug reports are welcome.

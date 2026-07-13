# Anofox Tabular Telemetry

Anofox Tabular collects **anonymous, privacy-preserving usage telemetry** so we
can see which data-quality functions are used, on which platforms, and where
they fail — and prioritise accordingly. It is **on by default** and **trivial to
turn off**.

Telemetry is emitted through the shared
[`DataZooDE/posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library and follows the cross-product **`telemetry_schema: 2`** envelope
(`posthog-telemetry/TELEMETRY-SCHEMA.md`). Ingestion is the EU PostHog cloud.

## How to turn it off

Any one of these fully short-circuits telemetry — when disabled, **nothing
leaves the machine** (the opt-out is enforced at the transport, not just at the
call sites):

```sql
SET anofox_telemetry_enabled = false;   -- DuckDB setting (per session)
```

```bash
export DATAZOO_DISABLE_TELEMETRY=1       -- environment (1|true|yes)
```

The environment variable is also honoured **before** the extension emits its
load event: `DATAZOO_DISABLE_TELEMETRY` (or pre-setting
`anofox_telemetry_enabled=false` via DBConfig) suppresses even the
`extension_loaded` event. A SQL `SET` can only run after load, so it disables
everything from that point on.

## The guarantee: bounded, enumerated, non-PII

Every property we send is **either** a constant drawn from a small,
code-controlled enumeration **or** a pure number (durations, counts). The
library additionally clamps every outgoing string to 512 bytes as a backstop.

We **never** send: input values or column data of any kind — no email
addresses, phone numbers, postal addresses, VAT/tax numbers, IBANs, credit-card
numbers, SSNs, names, or any other PII the functions inspect; no table names,
column names, SQL text, or error messages. `function_name` is drawn from the
fixed, code-controlled set of this extension's own function names — never a
user-supplied string.

## What is collected

### Envelope (attached to every event)

`product` (`anofox_tabular`), `product_version`, `product_edition` (`oss`),
`telemetry_schema` (`2`), `duckdb_version`, `os`, `arch`, `platform`, `is_ci`,
`is_container`, a per-process `$session_id`, and — once associated — the
`deployment` group. `distinct_id` is the SHA-256 of a machine id: a **stable,
pseudonymous** identifier, not tied to any personal data.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `extension_loaded` | the `anofox_tabular` extension loads | — |
| `function_executed` | a DuckDB function runs — **aggregated** per function per session (not per row) | `function_name`, `call_count`, `duration_ms_p50` |

This repository emits **only** these two events. The shared schema also defines
`feature_used` and `$exception`; those are **not** emitted here.

## Function-call aggregation

DuckDB function calls are recorded via `RecordFunctionCall(function_name)`, which
aggregates in-process into a single `function_executed` event per function per
session (carrying `call_count` and `duration_ms_p50`). The call sites live in the
**bind** / **bind-replace** paths of each function (e.g. `MoneyBind`,
`PIIDetectBind`, `MetricVolumeBindReplace`, `ProfileSummaryBindReplace`), which
run once per query — never on a per-row `GetChunk` path. A million-row scan
therefore produces O(1) telemetry rows, not a firehose.

The instrumented function families are: `money_*` / currency helpers, `vat_*`,
`email_*`, `phonenumber_*`, `postal_*`, `pii_*` / `anofox_ner_*`, `metric_*` /
`isolation_forest*` / `dbscan*`, `profile_*`, `diff_*`, and `outlier_tree`.

## Enterprise / account analytics

Anofox Tabular (OSS) associates only the `deployment` group. It has no license
key, so no `account` group is associated.

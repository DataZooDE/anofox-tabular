# anofox-tabular Python package

Python wrapper for the [anofox-tabular](https://github.com/DataZooDE/anofox-tabular) DuckDB extension — data quality, PII detection, email/phone validation, anomaly detection, diffing, money, and VAT primitives.

## Installation

```bash
pip install anofox-tabular
```

Optional extras for DataFrame support:

```bash
pip install "anofox-tabular[pandas]"   # adds pandas
pip install "anofox-tabular[polars]"   # adds polars
pip install "anofox-tabular[pandas,polars]"
```

## Quick start

```python
import anofox

# In-memory database (extension downloaded automatically)
with anofox.connect() as conn:
    # Email validation
    print(conn.execute("SELECT anofox_tab_email_is_valid('hi@example.com', 'regex')").fetchone())

# Or use a locally built extension
conn = anofox.connect(
    extension_path="/path/to/anofox_tabular.duckdb_extension"
)
```

With `duckdb` 2.0, a result of `conn.execute()` stays open until a fetch returns no more rows.
The `anofox` functions and `conn.register()`, `conn.unregister()` and `conn.sql()` run an open
result to its end first; `conn.complete()` does it explicitly (it does nothing on `duckdb` 1.5.x).

### Python-native API

This block runs as is: it creates small sample tables first. Replace them with your own tables
or DataFrames.

```python
# needs the pandas extra: pip install "anofox-tabular[pandas]"
import pandas as pd

import anofox
from anofox import diff, pii, quality, validate
from anofox.validate import EmailRule, PhoneRule

conn = anofox.connect()

# ── Sample data (replace with your own tables) ───────────────────────
conn.execute("""
    CREATE TABLE orders AS
    SELECT i AS id, 20 + i % 13 AS amount,
           CASE WHEN i % 4 = 0 THEN 'open' ELSE 'shipped' END AS status,
           TIMESTAMP '2026-01-01' + INTERVAL (i) HOUR AS created_at
    FROM range(1, 201) t(i)
""")
conn.execute("CREATE TABLE orders_v2 AS SELECT * FROM orders WHERE id <> 7")

# ── Email validation ──────────────────────────────────────────────────
print(validate.email_is_valid(conn, "hi@example.com"))                # True

df = pd.DataFrame({"email": ["a@b.com", "bad-email", "c@d.org"]}, index=[10, 20, 30])
print(validate.email_is_valid(conn, df, column="email"))
# copy of df with an added 'email_is_valid' column; index and row order are kept,
# so the column can be assigned back:
df["ok"] = validate.email_is_valid(conn, df, column="email")["email_is_valid"]

# ── Phone validation ──────────────────────────────────────────────────
print(validate.phone_is_valid(conn, "+14155552671", region="US"))     # True
print(validate.phone_format(conn, "+14155552671", "US", "INTERNATIONAL"))
phones = pd.DataFrame({"phone": ["4155552671", "01 42 68 53 00"], "country": ["US", "FR"]})
print(validate.phone_is_valid(conn, phones, column="phone", region_column="country"))
# region_column: the region of each row (instead of one region= for all rows).
# Without region= the setting anofox_tab_phonenumber_default_region applies (US by default).
print(validate.phone_format(conn, "12", "US"))                        # None: cannot be parsed

# ── Data quality ──────────────────────────────────────────────────────
print(quality.volume(conn, "orders", min_rows=100))
print(quality.null_rate(conn, "orders", "amount", max_null_rate=0.05)["message"])
print(quality.distinct_count(conn, "orders", "status", min_distinct=2, max_distinct=10))
print(quality.schema_check(conn, "orders", ["id", "amount", "created_at"]))
print(quality.volume(conn, df, min_rows=1)["status"])   # a DataFrame works too

# ── High-level profile ────────────────────────────────────────────────
print(conn.profile(df))   # pd.DataFrame with per-column metrics

# ── PII detection ─────────────────────────────────────────────────────
print(pii.pii_contains(conn, "Call me at +1-415-555-2671"))  # True
print(pii.pii_detect(conn, "Email: test@example.com"))        # [{"type": "EMAIL", ...}]
print(pii.pii_mask(conn, "test@example.com", strategy="redact"))
print(pii.pii_mask(conn, df, column="email"))                 # adds a 'pii_mask' column
print(pii.pii_scan_table(conn, "orders"))

# ── Diff ──────────────────────────────────────────────────────────────
print(diff.joindiff(conn, "orders", "orders_v2", primary_keys="id"))
# diff_type: 'added', 'removed', 'changed' (and 'unchanged' with include_unchanged=True)

# ── Schema validation ─────────────────────────────────────────────────
contacts = pd.DataFrame({"email": ["a@b.com", "bad-email"], "phone": ["+49 30 901820", "123"]})
result = conn.validate(contacts, schema={
    "email": EmailRule(mode="regex"),
    "phone": PhoneRule(region="DE"),
})
print(result.passed)      # False
print(result.failures)    # DataFrame: column, row_id, value, rule
```

## DataFrames

- **pandas and polars.** Every function that takes a DataFrame takes either. Results that are
  DataFrames use the input's library: a polars input gives a polars result, a pandas input
  or a table name gives pandas. A value the extension cannot parse comes back as `None`
  (a missing value in a DataFrame), like SQL `NULL`. A Series is not a DataFrame and raises `TypeError`: pass
  `series.to_frame()` (with `column=series.name` for the row-wise helpers).
- **Row-wise results** (`email_is_valid`, `email_validate`, `phone_is_valid`, `phone_parse`,
  `phone_format`, `phone_region`, the `anofox.vat` functions, `make_money`, `pii_detect`,
  `pii_mask` with `column=`) are a copy of the input plus one column. They keep
  the input's index (pandas) and row order, so `df["ok"] = ...["email_is_valid"]` lines up.
- **Table functions** (`anofox.quality`, `anofox.anomaly`, `anofox.profile`, `anofox.diff`,
  `pii_scan_table`, `pii_audit_table`, `conn.profile`, `conn.quality_check`) take a table name
  or a DataFrame. A DataFrame is copied into a temporary table for the call and dropped
  afterwards. On a connection to a database file (`anofox.connect("my.db")`) that copy is
  written to the file while the call runs. Any other object raises `TypeError`.
- **`conn.profile(name)`** resolves a table name like a query does: with tables of the same
  name in several schemas it profiles the one the search path finds. Pass `"schema.table"`
  for another schema; an unknown table raises `duckdb.CatalogException`.
- **`conn.validate(df, schema)`** returns `ValidationResult(passed, failures)`. `failures` has
  one row per failing value with the columns `column`, `row_id`, `value` and `rule`. `row_id`
  is the index label of the failing row for a pandas DataFrame and its 0-based row position
  for a polars DataFrame. Each rule is an `EmailRule(mode=...)` or a `PhoneRule(region=...)`
  from `anofox.validate`; any other rule, such as the string `"email"`, raises `TypeError`.
- **`conn.quality_check(df, rules)`** supports the rules `volume`, `null_rate` and
  `distinct_count`; any other rule name raises `ValueError`.

## Results

- The `anofox.quality` checks return a dict with the call's parameters and every result
  column of the SQL function by name (`status`, the metric, the thresholds, `message`).
  `DECIMAL` values are returned as `float`; `_raw` still holds the raw result row.
- Values are passed to DuckDB as bound parameters and column names are quoted, so any text
  (`o'brien@example.ie`) is safe to pass.
- The anomaly wrappers use the SQL defaults: a parameter you leave out is passed as `NULL`
  and the extension applies its default. `isolation_forest` and `isolation_forest_mv` take
  `seed=` for reproducible scores (`isolation_forest_mv` also `ndim=`). The per-row modes
  are `"scores"` (isolation forest), `"clusters"` (DBSCAN; `"scores"` is accepted too) and
  `"outliers"` (outlier tree); an unknown `output_mode` raises `ValueError`. Column lists
  can be a list or a comma-separated string (`"amount, items"`). A column whose name contains
  a comma cannot be selected and raises `ValueError`.

## Module overview

| Module | Functions |
|--------|-----------|
| `anofox.validate` | `email_is_valid`, `email_validate`, `phone_is_valid`, `phone_parse`, `phone_format`, `phone_region` |
| `anofox.quality` | `volume`, `null_rate`, `distinct_count`, `freshness`, `zscore`, `iqr`, `schema_check`, `regex_match`, `values_in_set`, `agg_check`, `duplicate_count`, `occurrence`, `match_rate`, `compliance`, `rel_count_change`, `metric_anomaly_iqr`, `rolling_values_in_set`, `define_checks`, `run_checks` |
| `anofox.anomaly` | `isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`, `outlier_tree` |
| `anofox.pii` | `pii_detect`, `pii_mask`, `pii_mask_value`, `pii_contains`, `pii_scan_table`, `pii_audit_table` |
| `anofox.diff` | `joindiff`, `hashdiff` |
| `anofox.profile` | `profile_table`, `profile_summary`, `profile_correlations` |
| `anofox.money` | `make_money(conn, amount, currency)` (or a DataFrame with `column=` and `currency=` / `currency_column=`), `money_from_cents`, `is_valid_currency`, `currency_symbol`, `money_add`, etc. |
| `anofox.vat` | `make_vat`, `vat_is_valid`, `vat_is_valid_syntax`, `vat_exists` (prefix check only), `vat_split`, `vat_normalize`, `vat_format(conn, vat, style="plain"\|"iso")`, `is_valid_vat_country`, `vat_is_eu_member`, `vat_country_name` |

## CLI

```bash
# Profile any file (colored table output)
anofox profile data.parquet
anofox profile data.csv --format json

# Quality checks
anofox quality data.parquet --volume-min 1000
anofox quality data.csv --null-max 0.05 --column email --format json
```

`anofox quality` exits with `0` when every check passes, `1` when a check fails and `2` when
the checks cannot run (invalid arguments, an unreadable file, an unknown column, a missing
extension binary); the error goes to stderr. With `--format json`, `DECIMAL` values are
written as JSON numbers and dates as ISO 8601 strings.

Supported formats: `.parquet`, `.csv`, `.tsv`, `.json`, `.ndjson`

## pytest plugin

```python
# Run with: pytest --anofox-check
import pytest

@pytest.mark.anofox_quality("orders", volume_min=100)
def test_orders_table_has_data(anofox_conn):
    ...
```

The `anofox_conn` session-scoped fixture is provided automatically. Tests skip if the extension is unavailable.

## Extension resolution

The package resolves the extension binary in this order:

1. `extension_path` argument to `connect()` (path to a local binary)
2. `ANOFOX_EXT_PATH` environment variable
3. Cached binary in `~/.anofox/extensions/<duckdb version>/<platform>/`
4. Download from the DuckDB community registry, then the S3 mirror
   (`https://get.erpl.io`), both laid out as
   `<base>/v<duckdb version>/<platform>/anofox_tabular.duckdb_extension.gz`
   (`duckdb` 1.5.6 downloads from `v1.5.6/`, `duckdb` 2.0.0 from `v2.0.0/`)

Downloads exist only for DuckDB releases. With a pre-release, development or
post-release `duckdb` package (`2.0.0rc1`, `1.5.7.dev42`, `1.5.6.post1`)
`connect()` raises `anofox.UnsupportedDuckDBVersionError` (a `RuntimeError`)
before any download is attempted; install a released `duckdb`, or set
`ANOFOX_EXT_PATH` / `extension_path` to an extension built for that DuckDB.
The package declares `duckdb>=1.0.0` without an upper bound: the extension is
published for each supported DuckDB release line (currently 1.5.x and 2.0).

Downloads are verified against a `.sha256` sidecar published next to the
archive. The S3 mirror publishes sidecars for every build; the community
registry does not, so by default downloads come from the mirror. Set
`ANOFOX_ALLOW_INSECURE_DOWNLOAD=1` to accept archives without a sidecar.

## Development

```bash
# Build the extension first (from the repository root)
GEN=ninja make release

# Create the dev environment
cd python
uv sync --extra dev

# Run tests
ANOFOX_EXT_PATH=../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension \
  uv run pytest tests/ -v

# Loader/utils tests run without extension (no env var needed)
uv run pytest tests/test_loader.py tests/test_utils.py -v
```

## License

Business Source License 1.1, the same as the extension. Production use is permitted, except
offering it to third parties on a hosted or embedded basis. See [LICENSE](https://github.com/DataZooDE/anofox-tabular/blob/main/LICENSE) for the full terms.

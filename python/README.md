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

### Python-native API

```python
import anofox
from anofox import validate, quality, pii, diff

conn = anofox.connect()

# ── Email validation ──────────────────────────────────────────────────
validate.email_is_valid(conn, "hi@example.com")                # True
validate.email_is_valid(conn, "hi@example.com", mode="dns")    # True (DNS checked)

import pandas as pd
df = pd.DataFrame({"email": ["a@b.com", "bad-email", "c@d.org"]})
result_df = validate.email_is_valid(conn, df, column="email")
# Returns DataFrame with added 'email_is_valid' column

# ── Phone validation ──────────────────────────────────────────────────
validate.phone_is_valid(conn, "+14155552671", region="US")     # True
validate.phone_format(conn, "+14155552671", "US", "INTERNATIONAL")

# ── Data quality ──────────────────────────────────────────────────────
conn.execute("CREATE TABLE orders AS SELECT * FROM read_parquet('orders.parquet')")

quality.volume(conn, "orders", min_rows=100)
# {"status": "pass", "min_rows": 100, ...}

quality.null_rate(conn, "orders", "amount", max_null_rate=0.05)
quality.distinct_count(conn, "orders", "status", min_distinct=2, max_distinct=10)
quality.schema_check(conn, "orders", ["id", "amount", "created_at"])

# ── High-level profile ────────────────────────────────────────────────
summary = conn.profile(df)   # returns pd.DataFrame with per-column metrics

# ── PII detection ─────────────────────────────────────────────────────
pii.pii_contains(conn, "Call me at +1-415-555-2671")  # True
pii.pii_detect(conn, "Email: test@example.com")        # [{"type": "EMAIL", ...}]
pii.pii_mask(conn, "test@example.com", strategy="redact")

scan_result = pii.pii_scan_table(conn, "orders")  # pd.DataFrame

# ── Diff ──────────────────────────────────────────────────────────────
# Table names or DataFrames both work
changes = diff.joindiff(conn, "orders_v1", "orders_v2", primary_keys="id")
changes = diff.joindiff(conn, df_before, df_after, primary_keys="id")
# Returns pd.DataFrame with diff_type: 'added', 'removed', 'changed', 'unchanged'

# ── Schema validation ─────────────────────────────────────────────────
from anofox.validate import EmailRule, PhoneRule

result = conn.validate(df, schema={
    "email": EmailRule(mode="dns"),
    "phone": PhoneRule(region="DE"),
})
print(result.passed)      # True / False
print(result.failures)    # pd.DataFrame of failed rows
```

## Module overview

| Module | Functions |
|--------|-----------|
| `anofox.validate` | `email_is_valid`, `email_validate`, `phone_is_valid`, `phone_parse`, `phone_format`, `phone_region` |
| `anofox.quality` | `volume`, `null_rate`, `distinct_count`, `freshness`, `zscore`, `iqr`, `schema_check` |
| `anofox.anomaly` | `isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`, `outlier_tree` |
| `anofox.pii` | `pii_detect`, `pii_mask`, `pii_contains`, `pii_scan_table`, `pii_audit_table` |
| `anofox.diff` | `joindiff`, `hashdiff` |
| `anofox.money` | `make_money`, `money_from_cents`, `is_valid_currency`, `currency_symbol`, `money_add`, etc. |
| `anofox.vat` | `make_vat`, `vat_is_valid`, `vat_is_eu_member`, `vat_country_name`, etc. |

## CLI

```bash
# Profile any file (colored table output)
anofox profile data.parquet
anofox profile data.csv --format json

# Quality checks (exit 0 = pass, exit 1 = fail)
anofox quality data.parquet --volume-min 1000
anofox quality data.csv --null-max 0.05 --column email
```

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

1. `ANOFOX_EXT_PATH` environment variable (path to local binary)
2. `extension_path` argument to `connect()`
3. Cached binary in `~/.anofox/extensions/`
4. Download from community registry → S3 mirror (`https://get.erpl.io`)

## Development

```bash
# Build the extension first
make release

# Install package in dev mode
cd python
pip install -e ".[dev]"

# Run tests
ANOFOX_EXT_PATH=../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension \
  pytest tests/ -v

# Loader/utils tests run without extension (no env var needed)
pytest tests/test_loader.py tests/test_utils.py -v
```

## License

MIT

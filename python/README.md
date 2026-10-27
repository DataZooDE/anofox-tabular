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
print(pii.pii_mask(conn, df, column="email", replace=True))   # masks 'email' itself
print(pii.pii_scan_table(conn, "orders"))
print(pii.pii_mask(conn, df, column="email", types=["EMAIL", "PHONE"]))  # no NER model: fast

# ── Diff ──────────────────────────────────────────────────────────────
print(diff.joindiff(conn, "orders", "orders_v2", primary_keys="id"))
# diff_type: 'added', 'removed', 'changed' (and 'unchanged' with include_unchanged=True)
print(diff.diff_schema(conn, "orders", "orders_v2"))
# one row per column: column_name, source_type, target_type, drift
# drift: 'same', 'added', 'removed' or 'retyped'

# ── Schema validation ─────────────────────────────────────────────────
contacts = pd.DataFrame({"email": ["a@b.com", "bad-email"], "phone": ["+49 30 901820", "123"]})
result = conn.validate(contacts, schema={
    "email": EmailRule(mode="regex"),
    "phone": PhoneRule(region="DE"),
})
print(result.passed)      # False
print(result.failures)    # DataFrame: column, row_id, value, rule

# ── Quality rules on a DataFrame ──────────────────────────────────────
checks = conn.quality_check(contacts, {
    "volume": {"min_rows": 1, "max_rows": 10_000},               # min_rows, max_rows
    "null_rate": {"column": "email", "max_null_rate": 0.05},     # column, max_null_rate
    "distinct_count": {"column": "phone", "min_distinct": 2},    # column, min_distinct, max_distinct
})
print(checks.status)                       # 'pass' or 'fail'
print(checks.checks["null_rate"]["null_rate"])
```

## DataFrames

- **pandas and polars.** Every function that takes a DataFrame takes either. Results that are
  DataFrames use the input's library: a polars input gives a polars result, a pandas input
  or a table name gives pandas. A value the extension cannot parse comes back as `None`
  (a missing value in a DataFrame), like SQL `NULL`. A Series is not a DataFrame and raises `TypeError`: pass
  `series.to_frame()` (with `column=series.name` for the row-wise helpers).
- **One type per column.** A pandas `object` column that mixes value types, such as `str`
  and `int` (`["+49 30 1234567", 4930765432]`), cannot be copied into DuckDB and raises
  `TypeError` naming the column and its types. Convert it first:
  `df["phone"] = df["phone"].astype("string")` (missing values stay `NULL`).
- **NaN and NULL.** In a pandas DataFrame `NaN` and `None` both become SQL `NULL`. In a polars
  DataFrame only `null` does; a float `NaN` stays a NaN value, so `null_rate` and `conn.profile`
  do not count it as missing. Use `df.fill_nan(None)` to treat it as missing (see
  [the Python guide](../docs/guides/06-python.md#missing-values-nan-and-null)).
  A single missing value (`None`, `pd.NA` or `NaN`) passed to a scalar helper of
  `anofox.validate`, `anofox.vat`, `anofox.money` or `anofox.pii` gives `None`, like SQL `NULL`.
  A text column that holds only missing values, such as the `float64` NaN column pandas reads
  for an empty CSV column, gives missing results in the row-wise helpers.
- **Column labels.** The `anofox.quality`, `anofox.anomaly`, `anofox.profile`, `anofox.diff`
  and PII table functions also take integer column labels (pandas `read_csv(header=None)` gives
  `0`, `1`, ...), also as `id_column`; the result names the column `"0"`, `"1"`, ... . Where a
  list of columns is accepted (`profile_table(columns=)`, `profile_correlations`,
  `schema_check`, `joindiff(compare_columns=)`), a single name such as `"email"` means that one
  column, not its characters.
- **Row-wise results** (`email_is_valid`, `email_validate`, `phone_is_valid`, `phone_parse`,
  `phone_format`, `phone_region`, the `anofox.vat` functions, `make_money`, `pii_detect`,
  `pii_mask` with `column=`) are a copy of the input plus one column. They keep
  the input's index (pandas) and row order, so `df["ok"] = ...["email_is_valid"]` lines up.
  For `pii_mask` and `pii_mask_value` that copy still holds the raw PII next to the mask;
  `replace=True` puts the masked values in the input column instead, so no raw value is left.
- **Table functions** (`anofox.quality`, `anofox.anomaly`, `anofox.profile`, `anofox.diff`,
  `pii_scan_table`, `pii_audit_table`, `conn.profile`, `conn.quality_check`) take a table name
  or a DataFrame. A DataFrame is copied into a temporary table for the call and dropped
  afterwards. On a connection to a database file (`anofox.connect("my.db")`) that copy is
  written to the file while the call runs. They also take a `pyarrow.Table` (copied by DuckDB
  directly; the result is a pandas DataFrame, as for a table name). Any other object raises
  `TypeError`; the row-wise helpers need a pandas or polars DataFrame (convert a pyarrow Table
  with `table.to_pandas()` or `polars.from_arrow(table)`). The
  functions that read through their own connection (anomaly detection, profiling, PII table
  scans) cannot see a temporary view, such as a DataFrame registered with `conn.register()`:
  its name raises `ValueError` with the hint to pass the DataFrame itself.
  Error messages and result messages name such a copy "the DataFrame" (`joindiff` /
  `hashdiff` / `diff_schema`: "the source DataFrame", "the target DataFrame"), not its
  temporary table name.
- **`conn.profile(df_or_table)`** returns the columns `column`, `type`, `row_count`,
  `null_count`, `null_rate`, `distinct_count`, `distinct_rate`; `anofox.profile.profile_table`
  names the first two `column_name` and `column_type` (the others match).
- **`conn.profile(name)`** resolves a table name like a query does: with tables of the same
  name in several schemas it profiles the one the search path finds. Pass `"schema.table"`
  for another schema; an unknown table raises `duckdb.CatalogException`.
- **`conn.validate(df, schema, include_nulls=False, id_column=None)`** returns `ValidationResult(passed, failures)`. `failures` has
  one row per failing value with the columns `column`, `row_id`, `value` and `rule`. `row_id`
  is the index label of the failing row for a pandas DataFrame and its 0-based row position
  for a polars DataFrame. `id_column="customer_id"` adds that column's value of each failing
  row (the business key) directly after `row_id`; a name that is no column of the DataFrame, or
  that is one of the four failure columns, raises `ValueError`. Each rule is an `EmailRule(mode=...)` or a `PhoneRule` from
  `anofox.validate`; any other rule, such as the string `"email"`, raises `TypeError`.
  `PhoneRule(region="DE")` uses one region for all rows, `PhoneRule(region_column="country")`
  the region code in each row's `country` column (passing both raises `ValueError`), and
  `PhoneRule()` the setting `anofox_tab_phonenumber_default_region`. A missing value (`None`,
  `NaN`) is skipped, as in SQL; `include_nulls=True` reports it as a failure whose rule ends in
  ` missing` (`"email(regex) missing"`). A schema key or `region_column` that is not a column
  of the DataFrame raises `ValueError` listing the columns.
- **`conn.quality_check(df, rules)`** supports the rules `volume` (keys `min_rows`, default 1,
  and `max_rows`), `null_rate` (`column`, `max_null_rate`, default 0.0) and `distinct_count`
  (`column`, `min_distinct`, default 1, and `max_distinct`). Any other rule name, a key the
  rule does not take (`{"volume": {"min": 10}}`) or a missing `column` raises `ValueError`
  listing the valid names. It returns a `QualityResult` with `status` (`"pass"` or `"fail"`)
  and `checks` (one result dict per rule); `result["status"]` works too.

## Results

- The `anofox.quality` checks return a dict with the call's parameters and every result
  column of the SQL function by name (`status`, the metric, the thresholds, `message`).
  `DECIMAL` values are returned as `float`; `_raw` still holds the raw result row.
- Values are passed to DuckDB as bound parameters and column names are quoted, so any text
  (`o'brien@example.ie`) is safe to pass. numpy scalars (`df["qty"].max()`, `numpy.float32`,
  `numpy.datetime64`), numpy arrays and `pandas.Timestamp` are passed as the equivalent Python
  values, also in `conn.execute(...)` parameters and `define_checks` params.
- Every function also takes a plain `duckdb.DuckDBPyConnection` with the extension loaded in
  place of an `AnofoxConnection`.
- The anomaly wrappers use the SQL defaults: every tuning parameter defaults to `None`, is
  passed as `NULL`, and the extension applies its default (`n_trees` 100, `contamination` 0.1,
  `eps` 0.5, `min_pts` 5, ...). DBSCAN takes the SQL names `eps=` and `min_pts=`; the former
  names `epsilon=` and `min_points=` still work but give a `DeprecationWarning` (both names of
  one parameter raise `TypeError`). `isolation_forest` and `isolation_forest_mv` take
  `seed=` for reproducible scores (`isolation_forest_mv` also `ndim=`). The per-row modes
  are `"scores"` (isolation forest), `"clusters"` (DBSCAN; `"scores"` is accepted too) and
  `"outliers"` (outlier tree); an unknown `output_mode` raises `ValueError`. Column lists
  can be a list or a comma-separated string (`"amount, items"`). A column whose name contains
  a comma cannot be selected and raises `ValueError`.
- The per-row output (`"scores"`, `"clusters"`, `"outliers"`) of a pandas DataFrame carries
  the input's index labels, so `df["is_anomaly"] = scores["is_anomaly"]` aligns; rows with a
  `NULL` feature are not in the output and get `NaN`. `row_id` is the 1-based row number in
  the input. A polars output keeps the input's row order and `row_id`. A float `NaN` is left
  out like a `NULL` by every detector, also in a polars DataFrame, so pandas and polars give
  the same result. A per-row output without rows has the same columns and dtypes as one with
  rows (`outlier_tree`'s `conditions` stays an `object` column, a `List` in polars).
- The per-row output of `isolation_forest` and `dbscan` names the scored column `value`, as the
  SQL functions do, whatever the input column is called; `id_column=` adds a key to join on.
- `email_validate` with a DataFrame adds one `email_validate` column of dicts (`valid`,
  `stage`, `reason`, `mx_hosts`, `smtp_debug`), a `Struct` column in polars. Expand it with
  `pd.DataFrame(out["email_validate"].dropna().tolist(), index=...)` or polars
  `out.unnest("email_validate")` (see [the Python guide](../docs/guides/06-python.md)).
- The return annotations name the scalar result (`Optional[bool]`, `Optional[dict]`, ...) and,
  for a DataFrame input, `pandas.DataFrame | polars.DataFrame`.
- `isolation_forest` and `isolation_forest_mv` warn (`UserWarning`) when 1 to 9 rows were
  scored (`anomaly.MIN_ISOLATION_FOREST_ROWS` is 10): so few rows score alike, and usually
  none is flagged. Pass at least 10 rows with values, or check so few values against a fixed
  range or a business rule (`quality.compliance`) instead.
- The PII wrappers `pii_detect`, `pii_mask`, `pii_contains`, `pii_scan_table` and
  `pii_audit_table` take `types=` (a list or a comma-separated string) to detect only those
  types in the call; it sets `anofox_pii_enabled_types` and restores it afterwards
  (`pii.enabled_types(conn, types)` does the same for a `with` block). Without the NER types
  (`NAME`, `ORGANIZATION`, `LOCATION`, `MISC`) the NER model does not run, which makes
  masking many rows much faster.
- `id_column=` adds an input column to the per-row output; with `output_mode="summary"` it
  raises `ValueError` (`id_column needs output_mode='scores'`, `'outliers'` for
  `outlier_tree`). Errors raised by DuckDB keep their type, without the `LINE 1:` excerpt
  of the wrapper's internal query. `anofox quality` names the input file in its errors.
- `outlier_tree(..., output_mode="outliers")` returns `conditions` as a list of dicts per
  outlier (polars: `List(Struct)`), each with the keys `column`, `type` (`"numeric"` or
  `"categorical"`), `operator`, `value` (numeric threshold, else `None`) and `values`
  (categories, else `None`). The SQL function returns the same conditions as JSON text.
- With no scored row (`total_count` 0) the `isolation_forest` summary has `avg_value`
  `None`. A DataFrame column of only `None` raises `ValueError` ("column 'amount' has no
  non-null values") in `isolation_forest`, `isolation_forest_mv`, `dbscan` and `dbscan_mv`.

## Module overview

| Module | Functions |
|--------|-----------|
| `anofox.validate` | `email_is_valid`, `email_validate`, `phone_is_valid`, `phone_parse`, `phone_format(conn, phone, region, format="E164")` (`fmt=` is a deprecated alias), `phone_region` |
| `anofox.quality` | `volume`, `null_rate`, `distinct_count`, `freshness`, `zscore`, `iqr`, `schema_check`, `regex_match`, `values_in_set`, `agg_check`, `duplicate_count`, `occurrence`, `match_rate`, `compliance`, `rel_count_change`, `metric_anomaly_iqr`, `rolling_values_in_set`, `define_checks`, `run_checks` |
| `anofox.anomaly` | `isolation_forest`, `isolation_forest_mv`, `dbscan`, `dbscan_mv`, `outlier_tree` |
| `anofox.pii` | `pii_detect`, `pii_mask`, `pii_mask_value`, `pii_contains`, `pii_scan_table`, `pii_audit_table`, `enabled_types` |
| `anofox.diff` | `joindiff`, `hashdiff`, `diff_schema` |
| `anofox.profile` | `profile_table`, `profile_summary`, `profile_correlations` |
| `anofox.money` | `make_money(conn, amount, currency)` (or a DataFrame with `column=` and `currency=` / `currency_column=`), `money_from_cents`, `is_valid_currency`, `currency_symbol`, `money_add`, etc. |
| `anofox.vat` | `make_vat`, `vat_is_valid`, `vat_is_valid_syntax`, `vat_validate` (valid, country, reason), `vat_exists` (prefix check only), `vat_split`, `vat_normalize`, `vat_format(conn, vat, style="plain"\|"iso")`, `is_valid_vat_country`, `vat_is_eu_member`, `vat_country_name` |

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

The package resolves the extension in this order:

1. `extension_path` argument to `connect()` (path to a local binary)
2. `ANOFOX_EXT_PATH` environment variable
3. DuckDB's own install: `INSTALL anofox_tabular FROM community` and
   `LOAD anofox_tabular`. DuckDB picks the build for its version and platform,
   caches it in `~/.duckdb/extensions/` and checks its signature. With
   `allow_unsigned=True` (the default) DuckDB would skip that check, so the
   install and a signature-checked load run first on a separate in-memory
   database, and the connection then loads the checked file.
4. Cached binary in `~/.anofox/extensions/<duckdb version>/<platform>/`
5. Download from the DuckDB community registry, then the S3 mirror
   (`https://get.erpl.io`), both laid out as
   `<base>/v<duckdb version>/<platform>/anofox_tabular.duckdb_extension.gz`
   (`duckdb` 1.5.6 downloads from `v1.5.6/`, `duckdb` 2.0.0 from `v2.0.0/`)

Step 3 fails when there is no network, no community build for this DuckDB
version and platform, or the build does not load; then steps 4 and 5 run. A
non-default `s3_base_url` is an explicit choice of repository: steps 4 and 5
then run before step 3. If no source works, `connect()` raises a
`RuntimeError` that lists the error of each source.

Downloads exist only for DuckDB releases. With a pre-release, development or
post-release `duckdb` package (`2.0.0rc1`, `1.5.7.dev42`, `1.5.6.post1`)
`connect()` raises `anofox.UnsupportedDuckDBVersionError` (a `RuntimeError`)
before any download is attempted; install a released `duckdb`, or set
`ANOFOX_EXT_PATH` / `extension_path` to an extension built for that DuckDB.
The package declares `duckdb>=1.0.0` without an upper bound: the extension is
published for each supported DuckDB release line (currently 1.5.x and 2.0).

Downloads in step 5 are verified against a `.sha256` sidecar published next
to the archive. The S3 mirror publishes sidecars for every build; the
community registry does not, so by default these downloads come from the
mirror (step 3 needs no sidecar). Set `ANOFOX_ALLOW_INSECURE_DOWNLOAD=1` to
accept archives without a sidecar.

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

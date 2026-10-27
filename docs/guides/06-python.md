# Python

> Use anofox-tabular from Python, with SQL or with the `anofox` package and pandas DataFrames.

**Use this guide to:**
- Install the Python package and open a connection
- Run the SQL functions from Python
- Validate, check, mask, score and diff pandas or polars DataFrames (or pyarrow Tables)
- Load the extension into a plain `duckdb` connection instead

## Install

```bash
pip install anofox-tabular
# or, in a uv project
uv add anofox-tabular
# DataFrame support
pip install "anofox-tabular[pandas]"    # or [polars]
```

The package is named `anofox-tabular`. You import it as `anofox`.

## Connect

`anofox.connect()` opens a DuckDB connection with the extension loaded. On first use it
installs the extension from the DuckDB community repository (`INSTALL anofox_tabular FROM
community`), which DuckDB caches in `~/.duckdb/extensions/`. If that fails, it downloads the
binary for your DuckDB version and platform from `https://get.erpl.io` and caches it in
`~/.anofox/extensions/`. To use a binary you built yourself, pass `extension_path=` or set the
environment variable `ANOFOX_EXT_PATH`.

```python
import anofox

conn = anofox.connect()  # in-memory database; anofox.connect("my.db") opens a file
print(conn.execute("SELECT email_is_valid('user@example.com') AS ok").fetchall())
```

```text
[(True,)]
```

`conn.execute()` runs any SQL, so every function in the [API reference](../api/00-conventions.md)
is available. The modules below wrap the common calls and accept DataFrames.

With `duckdb` 2.0, a result of `conn.execute()` stays open until a fetch returns no more rows
(`fetchone()` on a one-row result does not close it), and DuckDB refuses `register()` and `sql()`
on the connection while it is open. The `anofox` functions and the `register()`, `unregister()`
and `sql()` methods of the connection first run such a result to its end; `conn.complete()`
does it explicitly. On `duckdb` 1.5.x a result does not stay open, and `conn.complete()` does
nothing.

## Validate a DataFrame column

```python
import pandas as pd
from anofox import validate

customers = pd.DataFrame({
    "id": [1, 2, 3, 4],
    "email": ["ana@example.com", "bob(at)example.com", "carla@example.org", None],
    "note": ["call +1 650-253-0000", "no contact data", "SSN 123-45-6789",
             "IBAN DE89370400440532013000"],
})
print(validate.email_is_valid(conn, customers, column="email"))
```

```text
   id               email                         note  email_is_valid
0   1     ana@example.com         call +1 650-253-0000            True
1   2  bob(at)example.com              no contact data           False
2   3   carla@example.org              SSN 123-45-6789            True
3   4                 NaN  IBAN DE89370400440532013000            <NA>
```

The result is a copy of the DataFrame with a new `email_is_valid` column. It keeps the input's
index and row order, so you can also assign the column back:

```python
customers["email_ok"] = validate.email_is_valid(conn, customers, column="email")["email_is_valid"]
print(customers[["id", "email_ok"]].to_string(index=False))
customers = customers.drop(columns="email_ok")
```

```text
 id  email_ok
  1      True
  2     False
  3      True
  4      <NA>
```

`email_validate` says why an address fails. For a DataFrame it adds one `email_validate` column
holding a dict per row (`valid`, `stage`, `reason`, `mx_hosts`, `smtp_debug`; a `Struct` column
in polars). To get the fields as columns, expand the dicts (in polars: `out.unnest("email_validate")`):

```python
out = validate.email_validate(conn, customers, column="email")
checked = out["email_validate"].dropna()
details = pd.DataFrame(checked.tolist(), index=checked.index)
print(customers[["id", "email"]].join(details[["valid", "stage", "reason"]]).to_string(index=False))
```

```text
 id              email valid stage         reason
  1    ana@example.com  True regex            NaN
  2 bob(at)example.com False regex invalid_format
  3  carla@example.org  True regex            NaN
  4                NaN   NaN   NaN            NaN
```

The phone helpers (`phone_is_valid`, `phone_parse`, `phone_format`, `phone_region`) take one
region for all rows (`region="DE"`; `hint=` for `phone_region`), or a column with the region of
each row (`region_column="country"`). Without either, the setting
`anofox_tab_phonenumber_default_region` applies (`US` unless you change it). A number that
cannot be parsed gives `None` (a missing value in a DataFrame):

```python
phones = pd.DataFrame({"phone": ["030 901820", "(650) 253-0000", "12"],
                       "country": ["DE", "US", "DE"]})
out = validate.phone_format(conn, phones, format="INTERNATIONAL", column="phone",
                            region_column="country")
print(out.to_string(index=False))
```

```text
         phone country    phone_format
    030 901820      DE   +49 30 901820
(650) 253-0000      US +1 650 253 0000
            12      DE             NaN
```

`conn.validate` checks several columns at once. Its `PhoneRule` takes the same `region=` or
`region_column=` (not both):

```python
from anofox.validate import PhoneRule

result = conn.validate(phones, {"phone": PhoneRule(region_column="country")})
print(result.passed)
print(result.failures.to_string(index=False))
```

```text
False
column  row_id value                         rule
 phone       2    12 phone(region_column=country)
```

`row_id` is the index label of the failing row (its 0-based position for a polars DataFrame).
To report the business key instead of looking the row up, pass `id_column=`: its value is added
directly after `row_id`:

```python
contacts = pd.DataFrame({
    "customer_id": ["C-1001", "C-1002", "C-1003"],
    "phone": ["030 901820", "(650) 253-0000", "12"],
    "country": ["DE", "US", "DE"],
})
result = conn.validate(contacts, {"phone": PhoneRule(region_column="country")},
                       id_column="customer_id")
print(result.failures.to_string(index=False))
```

```text
column  row_id customer_id value                         rule
 phone       2      C-1003    12 phone(region_column=country)
```

A missing value (`None`, `NaN`) is not a failure, as in SQL and in the row-wise helpers above.
Pass `include_nulls=True` to report it too; its rule ends in `missing`. A schema key or
`region_column` that is not a column of the DataFrame raises `ValueError` and lists the columns.

```python
from anofox.validate import EmailRule

result = conn.validate(customers, {"email": EmailRule()})
print(result.failures.to_string(index=False))
result = conn.validate(customers, {"email": EmailRule()}, include_nulls=True)
print(result.failures.to_string(index=False))
```

```text
column  row_id              value         rule
 email       1 bob(at)example.com email(regex)
column  row_id              value                 rule
 email       1 bob(at)example.com         email(regex)
 email       3                NaN email(regex) missing
```

A pandas `object` column must hold one type of value. A column that mixes, for example, `str`
and `int` raises `TypeError` with the column name and its types. Convert it first with
`df["phone"] = df["phone"].astype("string")`.

## Check VAT numbers and amounts

The `anofox.vat` functions and `money.make_money` take a single value or a DataFrame column
the same way:

```python
from anofox import money, vat

suppliers = pd.DataFrame({
    "supplier": ["Brenner GmbH", "Acme UK Ltd", "Typo AG"],
    "vat_id": ["de 111-111-125", "UK123456789", "DE12"],
    "total": [1249.5, 89.99, None],
    "currency": ["EUR", "gbp", "EUR"],
})
checked = vat.vat_is_valid(conn, suppliers, column="vat_id")
print(checked[["supplier", "vat_is_valid"]].to_string(index=False))
print(vat.vat_format(conn, "de 111-111-125", "iso"), vat.vat_format(conn, "UK123456789"))
print(vat.vat_exists(conn, "DE12"))  # only the prefix is checked
priced = money.make_money(conn, suppliers, column="total", currency_column="currency")
print(priced["make_money"].tolist())
```

```text
    supplier  vat_is_valid
Brenner GmbH          True
 Acme UK Ltd         False
     Typo AG         False
DE111111125 None
True
[{'amount': Decimal('1249.500'), 'currency': 'EUR'}, {'amount': Decimal('89.990'), 'currency': 'GBP'}, <NA>]
```

`vat_exists` checks only that a VAT number starts with a supported prefix (`UK` is not one;
use `GB` or `XI`). None of the functions queries the EU VIES service.

## Run quality checks

The check functions in `anofox.quality` take a table name or a DataFrame and return a dict
with every result column of the SQL function by name:

```python
from anofox import quality

result = quality.null_rate(conn, customers, "email", max_null_rate=0.1)
print(result["status"], result["null_rate"], result["message"])
print(quality.volume(conn, customers, min_rows=1)["status"])
```

```text
fail 0.25 Null rate 0.25 (1/4) exceeds maximum 0.1
pass
```

A DataFrame is copied into a temporary table for the call and dropped afterwards. On a
connection to a database file (`anofox.connect("my.db")`) that copy is written to the file
while the call runs. The functions that read through a second connection (anomaly detection,
profiling, PII table scans) need a regular table: pass a DataFrame, or a table you created,
not a view you registered with `conn.register()`. Such a view passed by name raises
`ValueError` with the hint to pass the DataFrame itself.

All times are UTC, as in the SQL functions. A naive `datetime` (and text without an offset)
passed as `reference_time`, `reference_date`, `start_date` or `end_date` is taken as UTC; a
time zone aware `datetime` (also a `pandas.Timestamp`) and ISO text with an offset are the
instant they name, whatever the session `TimeZone`. `define_checks` stores an aware `datetime`
as naive UTC too:

```python
import datetime

scans = pd.DataFrame({
    "shipment_id": ["SHP-1001", "SHP-1002"],
    "scanned_at": [datetime.datetime(2026, 7, 14, 8, 40), datetime.datetime(2026, 7, 14, 10, 0)],
})
berlin_summer = datetime.timezone(datetime.timedelta(hours=2))
for reference in (
    datetime.datetime(2026, 7, 14, 12, 0),                        # naive: UTC
    datetime.datetime(2026, 7, 14, 14, 0, tzinfo=berlin_summer),  # the same instant
):
    result = quality.freshness(conn, scans, "scanned_at", "3 hours", reference_time=reference)
    print(result["age_seconds"], result["status"])
```

```text
7200 pass
7200 pass
```

## Nightly suite from Python

For a scheduled job, keep the checks in a table and run them all at once:
`quality.define_checks` writes the checks table, and `quality.run_checks` runs every check in
it (the SQL function `run_checks`; see [Data quality checks](02-data-quality-checks.md) for
the check types and their parameters). Both table names may be schema-qualified. The checks
name their target by table name, so the targets must be tables or views in the database, not
DataFrames:

```python
conn.execute("CREATE SCHEMA IF NOT EXISTS sales")
conn.execute("CREATE SCHEMA IF NOT EXISTS dq")
conn.execute("""
    CREATE OR REPLACE TABLE sales.orders AS SELECT * FROM (VALUES
        ('SO-1001', 'C-17', 'shipped', 129.90),
        ('SO-1002', 'C-18', 'pending', 45.00),
        ('SO-1003', NULL, 'shipped', 310.00),
        ('SO-1004', 'C-20', 'returned', -45.00)
    ) t(order_id, customer_id, status, amount)
""")

quality.define_checks(conn, [
    {"check_name": "orders_loaded", "check_type": "volume",
     "table_name": "sales.orders", "lower_threshold": 1},
    {"check_name": "customer_set", "check_type": "null_rate", "table_name": "sales.orders",
     "column_name": "customer_id", "upper_threshold": 0.0},
    {"check_name": "known_status", "check_type": "values_in_set", "table_name": "sales.orders",
     "column_name": "status", "params": {"allowed_values": ["shipped", "pending", "returned"]},
     "lower_threshold": 1.0},
    {"check_name": "amount_positive", "check_type": "compliance", "table_name": "sales.orders",
     "params": {"expression": "amount > 0"}, "lower_threshold": 1.0, "monitor_only": True},
], checks_table="dq.checks")

results = quality.run_checks(conn, "dq.checks", result_table="dq.check_results")
for r in results:
    print(f"{r['check_name']:<16} {r['status']:<5} {r['message']}")
```

```text
amount_positive  warn  compliance value 0.75 violates thresholds (0.75 < lower 1.0)
customer_set     fail  null_rate value 0.25 violates thresholds (0.25 > upper 0.0)
known_status     pass  values_in_set value 1.0 is within thresholds
orders_loaded    pass  volume value 4.0 is within thresholds
```

`run_checks` returns one dict per result row, ordered by `check_name`. With `result_table`
each run is also appended to that table (created on the first run), so the history builds up
for trend checks such as `metric_anomaly_iqr`; the returned rows are those of this run. A
check with `monitor_only` reports `warn` instead of `fail`. To fail the pipeline, exit with a
non-zero code when a check failed (`fail`) or could not run (`error`):

```python
def gate(results):
    """Raise SystemExit (exit code 1) when a check failed or could not run."""
    bad = [r for r in results if r["status"] in ("fail", "error")]
    for r in bad:
        print(f"{r['status'].upper()}: {r['check_name']} on {r['table_name']}: {r['message']}")
    if bad:
        raise SystemExit(1)

try:
    gate(results)
except SystemExit as stop:
    print("exit code", stop.code)
```

```text
FAIL: customer_set on sales.orders: null_rate value 0.25 violates thresholds (0.25 > upper 0.0)
exit code 1
```

In the scheduled script, call `gate(results)` as the last step: the scheduler (cron, Airflow,
a CI job) sees exit code 1 and marks the run as failed, while `warn` rows are only reported.
Open the connection on the database file (`anofox.connect("warehouse.duckdb")`) so the checks
table and the result history persist between runs.

## Mask PII

```python
from anofox import pii

print(pii.pii_mask(conn, "Mail ana@example.com, SSN 123-45-6789", strategy="redact"))
print(pii.pii_contains(conn, "no contact data"))
masked = pii.pii_mask(conn, customers.head(3), column="note", strategy="redact")
print(masked[["id", "pii_mask"]].to_string(index=False))
```

```text
Mail [EMAIL], SSN [US_SSN]
False
 id        pii_mask
  1    call [PHONE]
  2 no contact data
  3    SSN [US_SSN]
```

The masked result is a copy of the DataFrame plus a `pii_mask` column, so it still holds the raw
`note` next to the mask. To share the result, pass `replace=True`: the masked text then replaces
the `note` column and no raw value is left. `pii_mask_value` takes `replace=True` too.

```python
masked = pii.pii_mask(conn, customers.head(3), column="note", replace=True)
print(masked[["id", "note"]].to_string(index=False))
```

```text
 id            note
  1    call [PHONE]
  2 no contact data
  3    SSN [US_SSN]
```

### Restrict the PII types

By default every PII type is detected, including the NER types (`NAME`, `ORGANIZATION`,
`LOCATION`, `MISC`) that run a neural model on each text. The model is loaded on the first
call that needs it, and it runs per row: masking 200 short notes takes several seconds (on
the order of 10 to 15 s on a laptop CPU), against a few milliseconds for the pattern types.
When you only need some types, pass `types=` (a list or a comma-separated string) to
`pii_detect`, `pii_mask`, `pii_contains`, `pii_scan_table` or `pii_audit_table`:

```python
notes = pd.DataFrame({"note": [
    "Anna Schmidt, anna.schmidt@example.de",
    "call +49 30 1234567",
]})
masked = pii.pii_mask(conn, notes, column="note", types=["EMAIL", "PHONE"])
print(masked["pii_mask"].tolist())
```

```text
['Anna Schmidt, [EMAIL]', 'call [PHONE]']
```

`types=` sets `anofox_pii_enabled_types` for the call and restores the previous value
afterwards; the name stays unmasked because `NAME` was not requested. To use the same types
for several calls, set it once with `pii.enabled_types(conn, ["EMAIL", "PHONE"])` as a
`with` block, or with `SET anofox_pii_enabled_types = 'EMAIL,PHONE'` for the connection. The
setting applies to the whole database, so do not detect PII on another thread of the same
database at the same time. An unknown type raises `duckdb.InvalidInputException`
(`Unknown PII type`).

## Score anomalies

```python
from anofox import anomaly

readings = pd.DataFrame({
    "sensor_id": range(1, 21),
    "value": [10.0 + (i % 5) for i in range(19)] + [95.0],
})
scores = anomaly.isolation_forest(
    conn, readings, "value",
    output_mode="scores", n_trees=100, sample_size=16, contamination=0.05,
    id_column="sensor_id", seed=42,
)
print(scores[scores["is_anomaly"]][["sensor_id", "value"]].to_string(index=False))
```

```text
 sensor_id  value
        20   95.0
```

The per-row output of `isolation_forest` and `dbscan` names the scored column `value`, as the
SQL functions do, whatever the input column is called (here it is `value` already).
With `output_mode="summary"` the function returns a dict with the summary row instead. Pass a
`seed` for scores that are the same on every run; without it the trees are random. The
Python defaults are the SQL defaults: a parameter you leave out is passed as `NULL`, so the
extension applies its own default. `dbscan` returns one row per analyzed row with
`output_mode="clusters"`, `outlier_tree` one row per outlier value with `output_mode="outliers"`
(a row with outlying values in two columns gives two rows, one per `column_name`). Rows
with a `NULL` in a selected column are left out; `row_id` is still the 1-based row number in
the input, so the remaining rows keep their numbers. A `NaN` is left out like a `NULL`, also in
a polars DataFrame; see [Missing values](#missing-values-nan-and-null). When no row is
scored (an empty DataFrame, or only missing values), the `isolation_forest` summary has
`total_count` 0 and `avg_value` `None`, and a per-row output has no rows but the same columns
and dtypes as one with rows. With 1 to 9 scored rows `isolation_forest` and
`isolation_forest_mv` give a `UserWarning`: so few rows score alike and usually none is flagged
(`anomaly.MIN_ISOLATION_FOREST_ROWS` is 10). Pass at least 10 rows with values, or check so few
values against a fixed range or a business rule instead (for example with
`quality.compliance(conn, df, "amount BETWEEN 0 AND 5000")`). A DataFrame column that holds
only `None` has no type;
the numeric detectors raise `ValueError: column 'amount' has no non-null values` for it.

### Join scores back to the rows

The per-row output of every detector, also of `isolation_forest_mv` and `dbscan_mv`, has one
row per analyzed row. It holds the scores, not the input columns. For a pandas DataFrame it
carries the input's index labels, so a score column can be assigned straight back to the input.
A row with a `NULL` in a selected column is left out of the output, and it gets `NaN` in the
assigned column:

```python
orders = pd.DataFrame({
    "order_id": [f"SO-{1001 + i}" for i in range(12)],
    "amount": [42.5, 39.9, 45.0, 41.2, 44.8, 40.1, 43.3, 38.7, 46.2, 41.9, 39.5, 980.0],
    "items": [2, 2, 3, 2, 3, 2, 3, 2, 3, 2, 2, 3],
})
open_orders = orders.set_index("order_id")
open_orders.loc["SO-1003", "amount"] = None   # not settled yet
scores = anomaly.isolation_forest(
    conn, open_orders, "amount", output_mode="scores",
    sample_size=11, contamination=0.05, seed=42,
)
open_orders["is_anomaly"] = scores["is_anomaly"]
print(open_orders.loc[["SO-1002", "SO-1003", "SO-1012"]])
```

```text
          amount  items is_anomaly
order_id
SO-1002     39.9      2      False
SO-1003      NaN      3        NaN
SO-1012    980.0      3       True
```

The index labels are those of the scored rows, also after a filter (`df[df["amount"] > 0]`)
or on a custom index. `row_id` is the 1-based row number in the input (rows with a `NULL` are
left out but keep their number). A polars DataFrame has no index: its output keeps the input's
row order and `row_id`. To join on a key instead, pass it as `id_column`; its value is added
to the output directly after `row_id`:

```python
scores = anomaly.isolation_forest_mv(
    conn, orders, ["amount", "items"], output_mode="scores",
    sample_size=12, contamination=0.05, id_column="order_id", seed=42,
)
print(scores.columns.tolist())
flagged = scores.loc[scores["is_anomaly"], ["order_id", "anomaly_score"]]
print(orders.merge(flagged, on="order_id").to_string(index=False))
```

```text
['row_id', 'order_id', 'anomaly_score', 'is_anomaly']
order_id  amount  items  anomaly_score
 SO-1012   980.0      3       0.745399
```

`id_column` works only with a per-row `output_mode`; with the default `"summary"` it raises
`ValueError: isolation_forest_mv: id_column needs output_mode='scores'`.

### Choose `eps` for DBSCAN

`eps` (the SQL name; `epsilon=` is a deprecated alias) is a distance in the units of the data,
also for a single column. For order amounts in euros the default radius of 0.5 euros is far too
small: almost every order is noise. Choose `eps` in euros, or scale the column first, for
example to z-scores, and then choose `eps` on that scale:

```python
for eps in (0.5, 5.0):
    result = anomaly.dbscan(conn, orders, "amount", eps=eps, min_pts=3)
    print(eps, result["cluster_count"], result["noise_count"])

amount = orders["amount"]
scaled = orders.assign(amount=(amount - amount.mean()) / amount.std())
print(anomaly.dbscan(conn, scaled, "amount", eps=0.5, min_pts=3)["noise_count"])
```

```text
0.5 1 9
5.0 1 1
1
```

With several columns, scale them all: with amounts in euros and item counts in one call, a
radius that suits the item counts is far too small for the amounts. Put the columns on one
scale first, and then choose `eps` on that scale:

```python
raw = anomaly.dbscan_mv(conn, orders, ["amount", "items"], eps=0.5, min_pts=3)
print(raw["cluster_count"], raw["noise_count"])

features = ["amount", "items"]
scaled = orders.copy()
scaled[features] = (orders[features] - orders[features].mean()) / orders[features].std()
clusters = anomaly.dbscan_mv(conn, scaled, features, output_mode="clusters",
                             eps=0.5, min_pts=3, id_column="order_id")
print(clusters.loc[clusters["is_anomaly"], ["order_id", "cluster_id"]].to_string(index=False))
```

```text
1 9
order_id  cluster_id
 SO-1012          -1
```

### Read the OutlierTree conditions

`outlier_tree(..., output_mode="outliers")` explains each outlier with the conditions that define
its group. The `conditions` column holds a list of dicts (a `List(Struct)` column in polars) with
the keys `column`, `type`, `operator`, `value` (numeric threshold, else `None`) and `values`
(categories, else `None`). The SQL function returns the same conditions as JSON text.

```python
staff = pd.DataFrame({
    "staff_id": [f"E{i:04d}" for i in range(1, 201)],
    "level": ["junior" if i <= 100 else "senior" for i in range(1, 201)],
    "salary": [92000 if i == 7 else (48000 + i % 10 * 500 if i <= 100 else 88000 + i % 10 * 600)
               for i in range(1, 201)],
})
out = anomaly.outlier_tree(conn, staff, ["level", "salary"], output_mode="outliers",
                           id_column="staff_id")
salary = out[out["column_name"] == "salary"].iloc[0]
print(salary["staff_id"], salary["outlier_value"], salary["conditions"])
```

```text
E0007 92000 [{'column': 'level', 'type': 'categorical', 'operator': 'in', 'value': None, 'values': ['junior']}]
```

## Diff two DataFrames

```python
from anofox import diff

before = pd.DataFrame({"id": [1, 2, 3], "price": [10.0, 20.0, 30.0]})
after = pd.DataFrame({"id": [2, 3, 4], "price": [20.0, 35.0, 40.0]})
changes = diff.joindiff(conn, before, after, primary_keys="id")
print(changes.sort_values("id").to_string(index=False))
```

```text
diff_type  id  price changed_columns
  removed   1    NaN            <NA>
  changed   3   35.0         [price]
    added   4   40.0            <NA>
```

`changed_columns` lists the columns that differ in a `changed` row; it is missing for `added`
and `removed` rows.

`diff.diff_schema` compares the columns instead of the rows: one row per column of either
DataFrame (or table), with its type on each side and the drift (`same`, `added`, `removed` or
`retyped`). A row diff compares only the columns both sides have.

```python
old = pd.DataFrame({"id": [1, 2], "price": [10.0, 20.0], "fax": ["030 1234", None]})
new = pd.DataFrame({"id": ["A-1", "A-2"], "price": [10.0, 20.0], "channel": ["web", "shop"]})
print(diff.diff_schema(conn, old, new).to_string(index=False))
```

```text
column_name source_type target_type   drift
         id      BIGINT     VARCHAR retyped
      price      DOUBLE      DOUBLE    same
        fax     VARCHAR         NaN removed
    channel         NaN     VARCHAR   added
```

## Profile a table

```python
summary = anofox.profile.profile_summary(conn, customers)
print(summary[["row_count", "column_count", "total_nulls"]].to_string(index=False))
```

```text
 row_count  column_count  total_nulls
         4             3            1
```

There are two ways to profile from Python:

- `anofox.profile.profile_table`, `profile_summary` and `profile_correlations` call the SQL
  functions of the same name and return all their columns (29 per column for `profile_table`:
  quantiles, top values, patterns, NaN/Inf counts and more). See
  [Profiling](../api/10-profiling.md) for the columns. `profile_table` profiles a sample of
  `sample_size` rows (default 1,000,000) of a larger table unless `exact=True`; its `row_count`
  is then the rows scanned, as in SQL, not the table's row count (`profile_summary` has that).
  `top_values` is a list of `{"value": ..., "count": ...}` dicts. A DataFrame column holding only
  `None` has no type of its own: DuckDB 2.0 copies it as type `NULL`, so its `column_type` is
  `"NULL"` (DuckDB 1.5: `INTEGER`); give it a type first (`df["fax"].astype("string")`) for a
  stable result.
- `conn.profile(df_or_table)` is a short overview: per column only `type`, `row_count`,
  `null_count`, `null_rate`, `distinct_count` and `distinct_rate`, computed with plain SQL
  counts (exact, no sampling).

```python
products = pd.DataFrame({
    "sku": [f"SKU-{100 + i % 7}" for i in range(300)],
    "price": [9.5 + i % 4 for i in range(300)],
})
sampled = anofox.profile.profile_table(conn, products, sample_size=50)
print(sampled[["column_name", "row_count", "is_sampled"]].to_string(index=False))
print(anofox.profile.profile_summary(conn, products)["row_count"].iloc[0])
print(anofox.profile.profile_table(conn, products, columns="sku")["top_values"].iloc[0][:2])
```

```text
column_name  row_count  is_sampled
        sku         50        True
      price         50        True
300
[{'value': 'SKU-100', 'count': 43}, {'value': 'SKU-101', 'count': 43}]
```

```python
print(conn.profile(customers)[["column", "null_count", "distinct_count"]].to_string(index=False))
```

```text
column  null_count  distinct_count
    id           0               4
 email           1               3
  note           0               4
```

The two name their first columns differently; the other five have the same names:

| `conn.profile` | `profile_table` |
|---|---|
| `column` | `column_name` |
| `type` | `column_type` |
| `row_count`, `null_count`, `null_rate`, `distinct_count`, `distinct_rate` | same names |

To combine them, rename first, e.g.
`conn.profile(df).rename(columns={"column": "column_name", "type": "column_type"})`.

## Missing values: NaN and NULL

pandas and polars mark a missing number differently, and the difference survives the copy into
DuckDB:

- **pandas**: `NaN` and `None` in any column (float, string or object) become SQL `NULL`. A text
  column that holds only missing values (pandas reads an empty CSV column as `float64` NaN) gives
  missing results in the row-wise helpers, not a type error.
- **polars**: `None` (`null`) becomes `NULL`, but a float `NaN` stays a `DOUBLE` NaN, an ordinary
  value. It is not counted by `null_rate` or `conn.profile`; the numeric checks report it (for
  example `zscore`: "1 NaN/Inf value(s) ignored"), and `make_money` gives `None` for it.

```python
import polars as pl

discounts = [0.1, float("nan"), None, 0.0]
for orders in (pd.DataFrame({"discount": discounts}), pl.DataFrame({"discount": discounts})):
    result = quality.null_rate(conn, orders, "discount", max_null_rate=1.0)
    print(type(orders).__module__.split(".")[0], result["null_count"])
```

```text
pandas 2
polars 1
```

To treat a polars NaN as missing, convert it first: `df.fill_nan(None)` (`pl.from_pandas()`
already turns NaN into `null`).

A single missing value (`None`, `pd.NA` or `float("nan")`) passed to a scalar helper of
`anofox.validate`, `anofox.vat`, `anofox.money` or `anofox.pii` gives `None`, as the SQL function
gives `NULL`:

```python
from anofox import money, vat

print(validate.email_is_valid(conn, None), vat.vat_is_valid(conn, pd.NA),
      money.make_money(conn, float("nan"), "EUR"))
```

```text
None None None
```

The anomaly wrappers copy a polars NaN as `NULL`, so for them both libraries behave the same: a
`NaN` row is left out by every detector, like a `NULL` row (the DataFrame you pass is not
changed). An infinite value is kept:

| Detector | `NaN` or `NULL` | `inf` / `-inf` |
|---|---|---|
| `isolation_forest`, `isolation_forest_mv` | Left out (not in `total_count`, no score row) | Scored; usually flagged as anomalies |
| `dbscan`, `dbscan_mv` | Left out | Kept, and `NOISE` |
| `outlier_tree` | Left out | Can be flagged |

```python
amounts = [49.9, 50.1, 50.0, 49.8, 50.2, 50.3, 49.7, 50.4, 50.0, 49.9, 50.1, 50.2,
           float("nan"), None]
for orders in (pd.DataFrame({"amount": amounts}), pl.DataFrame({"amount": amounts})):
    forest = anomaly.isolation_forest(conn, orders, "amount", seed=7)
    clusters = anomaly.dbscan(conn, orders, "amount", eps=0.5, min_pts=3)
    print(forest["total_count"], clusters["total_count"], clusters["noise_count"])
```

```text
12 12 0
12 12 0
```

In SQL a `NaN` in a table is a value: the Isolation Forest skips it, but DBSCAN calls it noise
and OutlierTree counts it in `total_rows`. Filter infinite values yourself if they are errors
rather than extreme values.

## Use a pyarrow Table

The functions that take a table name (the checks in `anofox.quality`, the detectors in
`anofox.anomaly`, `anofox.profile`, `anofox.diff`, `pii_scan_table` and `pii_audit_table`) also
take a `pyarrow.Table`, which DuckDB copies without a conversion. They return a pandas
DataFrame for it, as for a table name. A float `NaN` in it is left out by the detectors like a
`NULL`. The row-wise helpers (`validate.email_is_valid(..., column=)` and the like) need a
pandas or polars DataFrame and raise `TypeError` for a pyarrow Table: convert it with
`table.to_pandas()` or `polars.from_arrow(table)`.

```python
import pyarrow as pa

ledger = pa.table({
    "invoice_no": [f"INV-{20260001 + i}" for i in range(30)],
    "amount": [4800.0 if i == 17 else 120.0 + (i % 6) * 4.5 for i in range(30)],
})
print(quality.volume(conn, ledger, min_rows=30)["status"])
scores = anomaly.isolation_forest(conn, ledger, "amount", output_mode="scores",
                                  id_column="invoice_no", contamination=0.05, seed=7)
print(type(scores).__module__.split(".")[0], scores.loc[scores["is_anomaly"], "invoice_no"].tolist())
```

```text
pass
pandas ['INV-20260018']
```

## Use polars

Every wrapper also takes a polars DataFrame and then returns a polars DataFrame. A table name
gives a pandas DataFrame.

```python
import polars as pl

orders = pl.DataFrame({"order_id": [1, 2, 3], "email": ["a@b.com", "bad", "c@d.org"]})
checked = validate.email_is_valid(conn, orders, column="email")
print(type(checked).__name__, checked["email_is_valid"].to_list())
print(type(anofox.profile.profile_summary(conn, orders)).__name__)
conn.close()
```

```text
DataFrame [True, False, True]
DataFrame
```

## Use a plain duckdb connection

You can also load the extension into a connection from the `duckdb` package. Use a
`duckdb` release the extension is built for (v1.5.6 or 2.0):

```python no-run
import duckdb

con = duckdb.connect()
con.execute("INSTALL anofox_tabular FROM community")
con.execute("LOAD anofox_tabular")
print(con.execute("SELECT vat_is_valid('DE111111125')").fetchall())  # [(True,)]
```

This snippet downloads the extension from the community repository. It was run once by hand
for this guide; the automatic check of this page does not run it.

## Next steps

- [Getting started](01-getting-started.md): the SQL functions without Python
- [python/README.md](../../python/README.md): the full Python API, the `anofox` CLI and the
  pytest plugin
- [API reference](../api/00-conventions.md)

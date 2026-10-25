# Python

> Use anofox-tabular from Python, with SQL or with the `anofox` package and pandas DataFrames.

**Use this guide to:**
- Install the Python package and open a connection
- Run the SQL functions from Python
- Validate, check, mask, score and diff pandas or polars DataFrames
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
downloads the extension binary for your DuckDB version and platform, and caches it in
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

The phone helpers (`phone_is_valid`, `phone_parse`, `phone_format`, `phone_region`) take one
region for all rows (`region="DE"`; `hint=` for `phone_region`), or a column with the region of
each row (`region_column="country"`). Without either, the setting
`anofox_tab_phonenumber_default_region` applies (`US` unless you change it). A number that
cannot be parsed gives `None` (a missing value in a DataFrame):

```python
phones = pd.DataFrame({"phone": ["030 901820", "(650) 253-0000", "12"],
                       "country": ["DE", "US", "DE"]})
out = validate.phone_format(conn, phones, fmt="INTERNATIONAL", column="phone",
                            region_column="country")
print(out.to_string(index=False))
```

```text
         phone country    phone_format
    030 901820      DE   +49 30 901820
(650) 253-0000      US +1 650 253 0000
            12      DE             NaN
```

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
not a view you registered with `conn.register()`.

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

With `output_mode="summary"` the function returns a dict with the summary row instead. Pass a
`seed` for scores that are the same on every run; without it the trees are random. The
Python defaults are the SQL defaults: a parameter you leave out is passed as `NULL`, so the
extension applies its own default. `dbscan` returns one row per input row with
`output_mode="clusters"`, `outlier_tree` with `output_mode="outliers"`.

## Diff two DataFrames

```python
from anofox import diff

before = pd.DataFrame({"id": [1, 2, 3], "price": [10.0, 20.0, 30.0]})
after = pd.DataFrame({"id": [2, 3, 4], "price": [20.0, 35.0, 40.0]})
changes = diff.joindiff(conn, before, after, primary_keys="id")
print(changes.sort_values("id").to_string(index=False))
```

```text
diff_type  id  price
  removed   1    NaN
  changed   3   35.0
    added   4   40.0
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

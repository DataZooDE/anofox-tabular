# Python

> Use anofox-tabular from Python, with SQL or with the `anofox` package and pandas DataFrames.

**Use this guide to:**
- Install the Python package and open a connection
- Run the SQL functions from Python
- Validate, check, mask, score and diff pandas DataFrames
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

The result is a copy of the DataFrame with a new `email_is_valid` column.

## Run quality checks

The check functions in `anofox.quality` take a table name and return a dict. Register the
DataFrame and copy it into a table first:

```python
from anofox import quality

conn.register("customers_df", customers)
conn.execute("CREATE TABLE customers AS SELECT * FROM customers_df")

result = quality.null_rate(conn, "customers", "email", max_null_rate=0.1)
print(result["status"], result["_raw"][-1])
print(quality.volume(conn, "customers", min_rows=1)["status"])
```

```text
fail Null rate 0.25 (1/4) exceeds maximum 0.1
pass
```

Use a real table, not a registered view, for the functions that read through a second
connection (anomaly detection, profiling, PII table scans). The wrappers in `anofox.anomaly`,
`anofox.diff` and `anofox.profile` accept a DataFrame and copy it into a table for you.

## Mask PII

```python
from anofox import pii

print(pii.pii_mask(conn, "Mail ana@example.com, SSN 123-45-6789", strategy="redact"))
print(pii.pii_contains(conn, "no contact data"))
```

```text
Mail [EMAIL], SSN [US_SSN]
False
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
    id_column="sensor_id",
)
print(scores[scores["is_anomaly"]][["sensor_id", "value"]].to_string(index=False))
```

```text
 sensor_id  value
        20   95.0
```

With `output_mode="summary"` the function returns a dict with the summary row instead.

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
summary = anofox.profile.profile_summary(conn, "customers")
print(summary[["row_count", "column_count", "total_nulls"]].to_string(index=False))
conn.close()
```

```text
 row_count  column_count  total_nulls
         4             3            1
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

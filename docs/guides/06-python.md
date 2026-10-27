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
extension applies its own default. `dbscan` returns one row per analyzed row with
`output_mode="clusters"`, `outlier_tree` one row per outlier with `output_mode="outliers"`. Rows
with a `NULL` in a selected column are left out; `row_id` is still the 1-based row number in
the input, so the remaining rows keep their numbers. A polars `NaN` is not a `NULL`; see
[Missing values](#missing-values-nan-and-null) for how each detector treats it.

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

## Missing values: NaN and NULL

pandas and polars mark a missing number differently, and the difference survives the copy into
DuckDB:

- **pandas**: `NaN` and `None` in any column (float, string or object) become SQL `NULL`.
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

The anomaly detectors treat a NaN and an infinite value differently from each other. A `NULL` row
is left out by all of them.

| Detector | `NaN` | `inf` / `-inf` |
|---|---|---|
| `isolation_forest`, `isolation_forest_mv` | Left out like a `NULL` (not in `total_count`, no score row) | Scored; usually flagged as anomalies |
| `dbscan`, `dbscan_mv` | Kept, and always `NOISE` (`is_anomaly = True`) | Kept, and `NOISE` |
| `outlier_tree` | Kept in `total_rows`, but never flagged | Can be flagged |

So the same polars column can give different counts in the two detectors:

```python
amounts = [49.9, 50.1, 50.0, 49.8, 50.2, 50.3, float("nan"), None]
orders = pl.DataFrame({"amount": amounts})
forest = anomaly.isolation_forest(conn, orders, "amount", seed=7)
clusters = anomaly.dbscan(conn, orders, "amount", epsilon=0.5, min_points=3)
print(forest["total_count"], clusters["total_count"], clusters["noise_count"])

clean = orders.fill_nan(None)
clusters = anomaly.dbscan(conn, clean, "amount", epsilon=0.5, min_points=3)
print(clusters["total_count"], clusters["noise_count"])
```

```text
6 7 1
6 0
```

For the same treatment in every detector, call `df.fill_nan(None)` before you run them. Then a
NaN is a `NULL` and every detector leaves it out. Filter infinite values yourself if they are
errors rather than extreme values.

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

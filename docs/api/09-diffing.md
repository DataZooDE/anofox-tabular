# Diffing

> Compare two tables row by row on a primary key and list the added, removed and changed rows.

**Use this page to:**
- Find the rows that differ between two versions of a table
- Use a single or a compound primary key
- Compare only some columns, or also list the unchanged rows
- List the schema drift (added, removed and retyped columns) between two tables
- Pass the diff parameters by position or by name

The row diffs are rewritten into one `FULL OUTER JOIN` query that DuckDB runs natively.
`diff_schema` compares the two column lists. The tables can be tables, views or TEMP tables.

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`diff_joindiff`](#diff_joindiff) | `anofox_tab_diff_joindiff` | table | one row per added, removed or changed row |
| [`diff_hashdiff`](#diff_hashdiff) | `anofox_tab_diff_hashdiff` | table | the same result as `diff_joindiff` without its options |
| [`diff_schema`](#diff_schema) | `anofox_tab_diff_schema` | table | one row per column of either table, with its drift |

Setup for the examples on this page:

```sql
CREATE TABLE customers_v1 (id INTEGER, name VARCHAR, city VARCHAR, score INTEGER);
INSERT INTO customers_v1 VALUES
    (1, 'Ada',   'Berlin',  10),
    (2, 'Bob',   'Hamburg', 20),
    (3, 'Cleo',  'Munich',  30),
    (4, 'Dan',   'Cologne', 40);

CREATE TABLE customers_v2 (id INTEGER, name VARCHAR, city VARCHAR, score INTEGER);
INSERT INTO customers_v2 VALUES
    (1, 'Ada',   'Berlin',  10),   -- unchanged
    (2, 'Bob',   'Bremen',  20),   -- city changed
    (3, 'Cleo',  'Munich',  35),   -- score changed
    (5, 'Eve',   'Leipzig', 50);   -- added; id 4 was removed
```

---

## `diff_joindiff`

Canonical name: `anofox_tab_diff_joindiff`. Table function.

Joins the source and the target table on the primary key and classifies every row.

```text
diff_joindiff(
    source_table    VARCHAR,
    target_table    VARCHAR,
    primary_key     VARCHAR | VARCHAR[]
    [, compare_columns VARCHAR[]
    [, include_all     BOOLEAN ]]
) -> TABLE(diff_type VARCHAR, <primary key columns>, <other target columns>)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `source_table` | `VARCHAR` | required | The old table or view |
| `target_table` | `VARCHAR` | required | The new table or view |
| `primary_key` | `VARCHAR` or `VARCHAR[]` | required | One key column, or a list for a compound key. By name: `primary_key := 'id'` or `primary_keys := ['id', 'day']` |
| `compare_columns` | `VARCHAR[]` | all non-key columns present in both tables | The columns that decide `changed`. `NULL` keeps the default |
| `include_all` | `BOOLEAN` | `false` | Also return the `unchanged` rows |

Output columns:

| Column | Type | Description |
|--------|------|-------------|
| `diff_type` | `VARCHAR` | `added`, `removed`, `changed`, or `unchanged` (only with `include_all`) |
| key columns | the key's type | `COALESCE` of the source and target key value. When the key's type differs between the tables and is not numeric on both sides, `VARCHAR` |
| other target columns | their type | The values of the target row. `NULL` for a `removed` row, which has no target row |

Behavior:

- Keys are matched NULL-safely (`IS NOT DISTINCT FROM`). A row whose key is `NULL` on both sides
  is compared, not reported as removed and added.
- A row is `changed` when any compare column `IS DISTINCT FROM` its counterpart, so a change from
  or to `NULL` counts.
- Columns that exist in only one of the two tables (added, dropped or renamed columns) are not
  compared by default; the trace log names them as a warning. [`diff_schema`](#diff_schema)
  lists them.
- A key or compare column whose type differs between the tables is compared on its text form
  (`CAST(... AS VARCHAR)`), unless both types are numeric: `7` and `'007'` differ, `8001` and
  `'n/a'` differ instead of aborting the diff with a conversion error. Numeric type changes
  (`INTEGER` to `BIGINT`, `DECIMAL` to `DOUBLE`) compare by value, so `5000.00` equals `5000.0`.
  The trace log names each column with both types.
- The key and compare columns are checked against both tables at bind time. A missing column or
  table is a binder error.
- The primary key must be unique in both tables. A repeated key value would join every source row
  of that key with every target row and report false changes, so the diff raises an error naming
  the table and one repeated key instead (see below). Two `NULL` keys count as repeated, matching
  the NULL-safe join. The check costs one `GROUP BY` over the key columns per table and runs
  inside the diff query, so it also covers TEMP tables and views.
- The output has no source values. Join the result back to the source table to see old values.

```sql
SELECT * FROM diff_joindiff('customers_v1', 'customers_v2', 'id') ORDER BY id;
```

```text
┌───────────┬───────┬─────────┬─────────┬───────┐
│ diff_type │  id   │  name   │  city   │ score │
│  varchar  │ int32 │ varchar │ varchar │ int32 │
├───────────┼───────┼─────────┼─────────┼───────┤
│ changed   │     2 │ Bob     │ Bremen  │    20 │
│ changed   │     3 │ Cleo    │ Munich  │    35 │
│ removed   │     4 │ NULL    │ NULL    │  NULL │
│ added     │     5 │ Eve     │ Leipzig │    50 │
└───────────┴───────┴─────────┴─────────┴───────┘
```

The removed row (`id` 4) has `NULL` in every target column. To show old and new values side by
side, join the source table:

```sql
SELECT d.diff_type, d.id, s.city AS old_city, d.city AS new_city,
       s.score AS old_score, d.score AS new_score
FROM diff_joindiff('customers_v1', 'customers_v2', 'id') d
LEFT JOIN customers_v1 s USING (id)
ORDER BY d.id;
```

```text
┌───────────┬───────┬──────────┬──────────┬───────────┬───────────┐
│ diff_type │  id   │ old_city │ new_city │ old_score │ new_score │
│  varchar  │ int32 │ varchar  │ varchar  │   int32   │   int32   │
├───────────┼───────┼──────────┼──────────┼───────────┼───────────┤
│ changed   │     2 │ Hamburg  │ Bremen   │        20 │        20 │
│ changed   │     3 │ Munich   │ Munich   │        30 │        35 │
│ removed   │     4 │ Cologne  │ NULL     │        40 │      NULL │
│ added     │     5 │ NULL     │ Leipzig  │      NULL │        50 │
└───────────┴───────┴──────────┴──────────┴───────────┴───────────┘
```

### Columns whose type changed

A migration that turned the integer `zip` into a `VARCHAR` (keeping leading zeros, `'n/a'` for
unknowns), the `DECIMAL` credit limit into a `DOUBLE` and the key into a `BIGINT`:

```sql
CREATE TABLE crm_v1 (customer_id INTEGER, zip INTEGER, credit_limit DECIMAL(12, 2));
INSERT INTO crm_v1 VALUES (1001, 80331, 5000.00), (1003, 7, 2500.00), (1004, 8001, 1200.00);

CREATE TABLE crm_v2 (customer_id BIGINT, zip VARCHAR, credit_limit DOUBLE);
INSERT INTO crm_v2 VALUES (1001, '80331', 5000.0), (1003, '007', 2500.0), (1004, 'n/a', 1200.0);

SELECT diff_type, customer_id, zip, credit_limit
FROM diff_joindiff('crm_v1', 'crm_v2', 'customer_id', include_all := true)
ORDER BY customer_id;
```

```text
┌───────────┬─────────────┬─────────┬──────────────┐
│ diff_type │ customer_id │   zip   │ credit_limit │
│  varchar  │    int64    │ varchar │    double    │
├───────────┼─────────────┼─────────┼──────────────┤
│ unchanged │        1001 │ 80331   │       5000.0 │
│ changed   │        1003 │ 007     │       2500.0 │
│ changed   │        1004 │ n/a     │       1200.0 │
└───────────┴─────────────┴─────────┴──────────────┘
```

`zip` is compared as text, so `7` -> `'007'` and `8001` -> `'n/a'` are changes. The credit
limits and the keys compare by value. The output shows the target's types.

### Compare a subset of columns

With `compare_columns`, only those columns decide whether a row is `changed`. Here the score
change of `id` 3 is ignored:

```sql
SELECT diff_type, id
FROM diff_joindiff('customers_v1', 'customers_v2', 'id', ['city'])
ORDER BY id;
```

```text
┌───────────┬───────┐
│ diff_type │  id   │
│  varchar  │ int32 │
├───────────┼───────┤
│ changed   │     2 │
│ removed   │     4 │
│ added     │     5 │
└───────────┴───────┘
```

### Include unchanged rows

```sql
SELECT diff_type, count(*) AS n
FROM diff_joindiff('customers_v1', 'customers_v2', 'id', NULL, true)
GROUP BY diff_type
ORDER BY diff_type;
```

```text
┌───────────┬───────┐
│ diff_type │   n   │
│  varchar  │ int64 │
├───────────┼───────┤
│ added     │     1 │
│ changed   │     2 │
│ removed   │     1 │
│ unchanged │     1 │
└───────────┴───────┘
```

### Named parameters

Every parameter can be passed by name, and positional and named arguments can be mixed. A named
value overrides the positional value of the same parameter:

```sql
SELECT diff_type, id
FROM diff_joindiff('customers_v1', 'customers_v2', 'id', include_all := true)
ORDER BY id;
```

```text
┌───────────┬───────┐
│ diff_type │  id   │
│  varchar  │ int32 │
├───────────┼───────┤
│ unchanged │     1 │
│ changed   │     2 │
│ changed   │     3 │
│ removed   │     4 │
│ added     │     5 │
└───────────┴───────┘
```

```sql
SELECT diff_type, id
FROM diff_joindiff(source_table := 'customers_v1', target_table := 'customers_v2',
                   primary_key := 'id', compare_columns := ['score'])
ORDER BY id;
```

```text
┌───────────┬───────┐
│ diff_type │  id   │
│  varchar  │ int32 │
├───────────┼───────┤
│ changed   │     3 │
│ removed   │     4 │
│ added     │     5 │
└───────────┴───────┘
```

Passing both `primary_key` and `primary_keys` is an error:

```sql
SELECT * FROM diff_joindiff('customers_v1', 'customers_v2', primary_key := 'id', primary_keys := ['id']);
```

```text
Binder Error: diff_joindiff: pass either primary_key or primary_keys, not both
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM diff_joindiff('customers_v1', 'customers_v2', primary_key...
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

A primary key that is not unique is an error:

```sql
CREATE TABLE visits_v1 (id INTEGER, page VARCHAR);
INSERT INTO visits_v1 VALUES (1, 'home'), (1, 'cart'), (2, 'home');
SELECT * FROM diff_joindiff('visits_v1', 'visits_v1', 'id');
```

```text
Invalid Input Error: diff_joindiff: primary key is not unique in source table 'visits_v1' (id = 1 appears 2 times)
```

### Compound keys

Pass a list of key columns. Rows with a `NULL` key part are matched to each other:

```sql
CREATE TABLE stock_v1 (store VARCHAR, sku VARCHAR, qty INTEGER);
INSERT INTO stock_v1 VALUES ('A', 'x1', 5), ('A', 'x2', 7), ('B', 'x1', 3), (NULL, 'x9', 1);

CREATE TABLE stock_v2 (store VARCHAR, sku VARCHAR, qty INTEGER);
INSERT INTO stock_v2 VALUES ('A', 'x1', 5), ('A', 'x2', 8), ('B', 'x3', 4), (NULL, 'x9', 1);

SELECT * FROM diff_joindiff('stock_v1', 'stock_v2', ['store', 'sku'])
ORDER BY store, sku;
```

```text
┌───────────┬─────────┬─────────┬───────┐
│ diff_type │  store  │   sku   │  qty  │
│  varchar  │ varchar │ varchar │ int32 │
├───────────┼─────────┼─────────┼───────┤
│ changed   │ A       │ x2      │     8 │
│ removed   │ B       │ x1      │  NULL │
│ added     │ B       │ x3      │     4 │
└───────────┴─────────┴─────────┴───────┘
```

The row `(NULL, 'x9')` exists on both sides with the same `qty`, so it does not appear.

---

## `diff_hashdiff`

Canonical name: `anofox_tab_diff_hashdiff`. Table function.

Computes the same row-level diff as `diff_joindiff` with its defaults: all shared non-key columns
are compared and unchanged rows are left out. A hash and bisection algorithm is planned but not
implemented.

```text
diff_hashdiff(
    source_table VARCHAR,
    target_table VARCHAR,
    primary_key  VARCHAR | VARCHAR[]
) -> TABLE(diff_type VARCHAR, <primary key columns>, <other target columns>)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `source_table` | `VARCHAR` | required | The old table or view |
| `target_table` | `VARCHAR` | required | The new table or view |
| `primary_key` | `VARCHAR` or `VARCHAR[]` | required | One key column, or a list. By name: `primary_key :=` or `primary_keys :=` |

The output columns are the same as for [`diff_joindiff`](#diff_joindiff). The primary key must be
unique in both tables, as for `diff_joindiff`. Columns on one side only are not compared; list
them with [`diff_schema`](#diff_schema).

```sql
SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id') ORDER BY id;
```

```text
┌───────────┬───────┬─────────┬─────────┬───────┐
│ diff_type │  id   │  name   │  city   │ score │
│  varchar  │ int32 │ varchar │ varchar │ int32 │
├───────────┼───────┼─────────┼─────────┼───────┤
│ changed   │     2 │ Bob     │ Bremen  │    20 │
│ changed   │     3 │ Cleo    │ Munich  │    35 │
│ removed   │     4 │ NULL    │ NULL    │  NULL │
│ added     │     5 │ Eve     │ Leipzig │    50 │
└───────────┴───────┴─────────┴─────────┴───────┘
```

The parameters `bisection_threshold` and `bisection_factor` are declared but not implemented.
Passing them, by position or by name, is a binder error:

```sql
SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id', bisection_threshold := 1000);
```

```text
Binder Error: diff_hashdiff: the bisection_threshold/bisection_factor parameters are not implemented yet; remove them (diff_hashdiff currently computes the same full diff as diff_joindiff)
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id', bisecti...
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

`compare_columns` and `include_all` do not exist on `diff_hashdiff`:

```sql
SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id', include_all := true);
```

```text
Binder Error: Invalid named parameter "include_all" for function diff_hashdiff

LINE 1: ...hdiff('customers_v1', 'customers_v2', 'id', include_all := true);
                                                                      ^^^^
```

---

## `diff_schema`

Canonical name: `anofox_tab_diff_schema`. Table function.

Compares the columns of two tables and returns one row per column of either table: the type on
each side and the drift. Run it before a row diff to see which columns the diff compares.

```text
diff_schema(
    source_table VARCHAR,
    target_table VARCHAR
) -> TABLE(column_name VARCHAR, source_type VARCHAR, target_type VARCHAR, drift VARCHAR)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `source_table` | `VARCHAR` | required | The old table or view |
| `target_table` | `VARCHAR` | required | The new table or view |

Output columns:

| Column | Type | Description |
|--------|------|-------------|
| `column_name` | `VARCHAR` | The column name, as spelled in the source table (in the target table for an `added` column) |
| `source_type` | `VARCHAR` | The type in the source table, as `DESCRIBE` shows it. `NULL` for an `added` column |
| `target_type` | `VARCHAR` | The type in the target table. `NULL` for a `removed` column |
| `drift` | `VARCHAR` | `same`, `added` (target only), `removed` (source only) or `retyped` (both, different types) |

Behavior:

- Columns match case-insensitively, like DuckDB identifiers and the row diffs: `Amount` and
  `amount` are the `same` column.
- A renamed column has no match, so it is one `removed` and one `added` row.
- Two types are the same when their `DESCRIBE` text is equal: `DECIMAL(10,4)` to
  `DECIMAL(18,6)` is `retyped`.
- The rows come in source column order, followed by the `added` columns in target column order.
- The tables can be tables, views, TEMP tables or qualified names: `billing.invoices` is a
  table of the schema `billing` or, without such a schema, of the attached database `billing`.
  An unknown table or view is a binder error; a `NULL` argument is an error naming it.
- Both parameters can be passed by name (`source_table :=`, `target_table :=`).

A billing migration renamed `currency` to `ccy`, dropped `fax`, added `payment_terms`, turned
the integer `cost_center` into a `VARCHAR` code and only changed the case of `Amount`:

```sql
CREATE TABLE invoices_v1 (invoice_id INTEGER, customer_name VARCHAR, Amount DECIMAL(12, 2),
                          currency VARCHAR, cost_center INTEGER, fax VARCHAR, issued_on DATE);
CREATE TABLE invoices_v2 (invoice_id INTEGER, customer_name VARCHAR, amount DECIMAL(12, 2),
                          ccy VARCHAR, cost_center VARCHAR, issued_on DATE, payment_terms VARCHAR);

SELECT * FROM diff_schema('invoices_v1', 'invoices_v2');
```

```text
┌───────────────┬───────────────┬───────────────┬─────────┐
│  column_name  │  source_type  │  target_type  │  drift  │
│    varchar    │    varchar    │    varchar    │ varchar │
├───────────────┼───────────────┼───────────────┼─────────┤
│ invoice_id    │ INTEGER       │ INTEGER       │ same    │
│ customer_name │ VARCHAR       │ VARCHAR       │ same    │
│ Amount        │ DECIMAL(12,2) │ DECIMAL(12,2) │ same    │
│ currency      │ VARCHAR       │ NULL          │ removed │
│ cost_center   │ INTEGER       │ VARCHAR       │ retyped │
│ fax           │ VARCHAR       │ NULL          │ removed │
│ issued_on     │ DATE          │ DATE          │ same    │
│ ccy           │ NULL          │ VARCHAR       │ added   │
│ payment_terms │ NULL          │ VARCHAR       │ added   │
└───────────────┴───────────────┴───────────────┴─────────┘
```

Only the drift:

```sql
SELECT column_name, source_type, target_type, drift
FROM diff_schema('invoices_v1', 'invoices_v2')
WHERE drift <> 'same';
```

```text
┌───────────────┬─────────────┬─────────────┬─────────┐
│  column_name  │ source_type │ target_type │  drift  │
│    varchar    │   varchar   │   varchar   │ varchar │
├───────────────┼─────────────┼─────────────┼─────────┤
│ currency      │ VARCHAR     │ NULL        │ removed │
│ cost_center   │ INTEGER     │ VARCHAR     │ retyped │
│ fax           │ VARCHAR     │ NULL        │ removed │
│ ccy           │ NULL        │ VARCHAR     │ added   │
│ payment_terms │ NULL        │ VARCHAR     │ added   │
└───────────────┴─────────────┴─────────────┴─────────┘
```

## See also

- [Guide: diffing and migration checks](../guides/05-diffing-and-migration.md)
- [Data quality checks](07-data-quality.md), for example [`match_rate`](07-data-quality.md#match_rate)
- [Conventions: named parameters](00-conventions.md#named-parameters)

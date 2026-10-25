# Diffing

> Compare two tables row by row on a primary key and list the added, removed and changed rows.

**Use this page to:**
- Find the rows that differ between two versions of a table
- Use a single or a compound primary key
- Compare only some columns, or also list the unchanged rows
- Pass the diff parameters by position or by name

Both functions are rewritten into one `FULL OUTER JOIN` query that DuckDB runs natively. The
tables can be tables, views or TEMP tables.

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`diff_joindiff`](#diff_joindiff) | `anofox_tab_diff_joindiff` | table | one row per added, removed or changed row |
| [`diff_hashdiff`](#diff_hashdiff) | `anofox_tab_diff_hashdiff` | table | the same result as `diff_joindiff` without its options |

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
| key columns | the key's type | `COALESCE` of the source and target key value |
| other target columns | their type | The values of the target row. `NULL` for a `removed` row, which has no target row |

Behavior:

- Keys are matched NULL-safely (`IS NOT DISTINCT FROM`). A row whose key is `NULL` on both sides
  is compared, not reported as removed and added.
- A row is `changed` when any compare column `IS DISTINCT FROM` its counterpart, so a change from
  or to `NULL` counts.
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
unique in both tables, as for `diff_joindiff`.

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

## See also

- [Guide: diffing and migration checks](../guides/05-diffing-and-migration.md)
- [Data quality checks](07-data-quality.md), for example [`match_rate`](07-data-quality.md#match_rate)
- [Conventions: named parameters](00-conventions.md#named-parameters)

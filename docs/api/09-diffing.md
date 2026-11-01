# Diffing

> Compare two tables row by row on a primary key and list the added, removed and changed rows.

**Use this page to:**
- Find the rows that differ between two versions of a table
- Use a single or a compound primary key
- Compare only some columns, or also list the unchanged rows
- Ignore floating-point noise in numeric columns
- List the schema drift (added, removed and retyped columns) between two tables
- Pass the diff parameters by position or by name

The row diffs are rewritten into one `FULL OUTER JOIN` query that DuckDB runs natively.
`diff_schema` compares the two column lists. The tables can be tables, views or TEMP tables.

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`diff_joindiff`](#diff_joindiff) | `anofox_tab_diff_joindiff` | table | one row per added, removed or changed row |
| [`diff_hashdiff`](#diff_hashdiff) | `anofox_tab_diff_hashdiff` | table | the same result as `diff_joindiff` with its defaults; can round floating-point columns first |
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
    [, float_tolerance := DOUBLE ]
) -> TABLE(diff_type VARCHAR, <primary key columns>, <other target columns>, changed_columns VARCHAR[])
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `source_table` | `VARCHAR` | required | The old table or view |
| `target_table` | `VARCHAR` | required | The new table or view |
| `primary_key` | `VARCHAR` or `VARCHAR[]` | required | One key column, or a list for a compound key. By name: `primary_key := 'id'` or `primary_keys := ['id', 'day']` |
| `compare_columns` | `VARCHAR[]` | all non-key columns present in both tables | The columns that decide `changed`. `NULL` keeps the default |
| `include_all` | `BOOLEAN` | `false` | Also return the `unchanged` rows |
| `float_tolerance` | `DOUBLE` (by name only) | `0` (exact) | Numeric compare columns whose values differ by at most this much (absolute, inclusive) count as equal. `NULL` keeps the default; a negative, `NaN` or infinite value is an error. See [Floating-point noise](#floating-point-noise) |

Output columns:

| Column | Type | Description |
|--------|------|-------------|
| `diff_type` | `VARCHAR` | `added`, `removed`, `changed`, or `unchanged` (only with `include_all`) |
| key columns | the key's type | `COALESCE` of the source and target key value. When the key's type differs between the tables and is not numeric on both sides, `VARCHAR` |
| other target columns | their type | The values of the target row. `NULL` for a `removed` row, which has no target row |
| `changed_columns` | `VARCHAR[]` | The compare columns that differ, in compare order, for a `changed` row; `[]` for an `unchanged` row; `NULL` for an `added` or `removed` row, which has no counterpart to compare. Always the last column |

Behavior:

- Keys are matched NULL-safely (`IS NOT DISTINCT FROM`). A row whose key is `NULL` on both sides
  is compared, not reported as removed and added.
- A row is `changed` when any compare column `IS DISTINCT FROM` its counterpart, so a change from
  or to `NULL` counts. With `float_tolerance` a numeric compare column is equal when
  `|source - target| <= float_tolerance`; `NULL` still equals only `NULL`.
- Columns that exist in only one of the two tables (added, dropped or renamed columns) are not
  compared by default; the trace log names them as a warning. [`diff_schema`](#diff_schema)
  lists them.
- A key or compare column whose type differs between the tables is compared on its text form
  (`CAST(... AS VARCHAR)`), unless both types are numeric: `7` and `'007'` differ, `8001` and
  `'n/a'` differ instead of aborting the diff with a conversion error. Numeric type changes
  (`INTEGER` to `BIGINT`, `DECIMAL` to `DOUBLE`) compare by value, so `5000.00` equals `5000.0`.
  The trace log names each column with both types: as a warning, except for a change that only
  widens the type (`DECIMAL(10,2)` to `DECIMAL(12,2)`, `INTEGER` to `BIGINT`), which is logged
  at `debug` level.
- A `TIMESTAMP WITH TIME ZONE` column whose type changed is compared on its UTC instant, whatever
  the session `TimeZone`. Against a `VARCHAR` column it is rendered as UTC text with a `+00`
  suffix (`2026-03-01 08:30:00+00`, the text DuckDB prints under `TimeZone = 'UTC'`); against a
  plain `TIMESTAMP` both sides compare as UTC timestamps, reading the `TIMESTAMP` as UTC, and a
  key column of that pair is returned as a `TIMESTAMP`.
- The key and compare columns are checked against both tables at bind time. A missing column or
  table is a binder error.
- `JSON` columns are compared as text: `{"a":1}` and `{"a": 1}` differ. Compare a normalized
  form through views (`json(col)::VARCHAR`, or the extracted fields) when only the content
  matters.
- The names `diff_type` and `changed_columns` (any case) are reserved for the output. A target
  column with one of these names, key or not, compared or not, is a binder error that names the
  column and suggests a view that renames it (see
  [Columns named like an output column](#columns-named-like-an-output-column)). A source column
  of that name that the target lacks is not in the output and does not matter.
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
┌───────────┬───────┬─────────┬─────────┬───────┬─────────────────┐
│ diff_type │  id   │  name   │  city   │ score │ changed_columns │
│  varchar  │ int32 │ varchar │ varchar │ int32 │    varchar[]    │
├───────────┼───────┼─────────┼─────────┼───────┼─────────────────┤
│ changed   │     2 │ Bob     │ Bremen  │    20 │ [city]          │
│ changed   │     3 │ Cleo    │ Munich  │    35 │ [score]         │
│ removed   │     4 │ NULL    │ NULL    │  NULL │ NULL            │
│ added     │     5 │ Eve     │ Leipzig │    50 │ NULL            │
└───────────┴───────┴─────────┴─────────┴───────┴─────────────────┘
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

### Which columns changed

`changed_columns` lists the compare columns that differ in a `changed` row. Unnest it to count the
changes per column, for example in a migration report:

```sql
SELECT diff_type, id, changed_columns
FROM diff_joindiff('customers_v1', 'customers_v2', 'id', include_all := true)
ORDER BY id;
```

```text
┌───────────┬───────┬─────────────────┐
│ diff_type │  id   │ changed_columns │
│  varchar  │ int32 │    varchar[]    │
├───────────┼───────┼─────────────────┤
│ unchanged │     1 │ []              │
│ changed   │     2 │ [city]          │
│ changed   │     3 │ [score]         │
│ removed   │     4 │ NULL            │
│ added     │     5 │ NULL            │
└───────────┴───────┴─────────────────┘
```

```sql
SELECT col, count(*) AS n
FROM diff_joindiff('customers_v1', 'customers_v2', 'id'), unnest(changed_columns) AS u(col)
GROUP BY col
ORDER BY col;
```

```text
┌─────────┬───────┐
│   col   │   n   │
│ varchar │ int64 │
├─────────┼───────┤
│ city    │     1 │
│ score   │     1 │
└─────────┴───────┘
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

A `TIMESTAMPTZ` column that became UTC text or a plain `TIMESTAMP` compares on its UTC instant, so
the result is the same in every session time zone:

```sql
SET TimeZone = 'Europe/Berlin';
CREATE TABLE tickets_v1 (ticket_id INTEGER, opened_at TIMESTAMPTZ);
INSERT INTO tickets_v1 VALUES (5001, '2026-03-01 08:30:00+00'), (5002, '2026-03-02 00:15:00+00');

CREATE TABLE tickets_v2 (ticket_id INTEGER, opened_at VARCHAR);
INSERT INTO tickets_v2 VALUES (5001, '2026-03-01 08:30:00+00'), (5002, '2026-03-02 00:20:00+00');

SELECT diff_type, ticket_id, opened_at, changed_columns
FROM diff_joindiff('tickets_v1', 'tickets_v2', 'ticket_id');
```

```text
┌───────────┬───────────┬────────────────────────┬─────────────────┐
│ diff_type │ ticket_id │       opened_at        │ changed_columns │
│  varchar  │   int32   │        varchar         │    varchar[]    │
├───────────┼───────────┼────────────────────────┼─────────────────┤
│ changed   │      5002 │ 2026-03-02 00:20:00+00 │ [opened_at]     │
└───────────┴───────────┴────────────────────────┴─────────────────┘
```

### Columns named like an output column

A ticket export with its own `diff_type` column cannot be diffed as it is, because the diff adds
a column of that name:

```sql
CREATE TABLE helpdesk_v1 (ticket_id INTEGER, diff_type VARCHAR, priority VARCHAR);
INSERT INTO helpdesk_v1 VALUES (7001, 'edit', 'high'), (7002, 'create', 'low');
CREATE TABLE helpdesk_v2 (ticket_id INTEGER, diff_type VARCHAR, priority VARCHAR);
INSERT INTO helpdesk_v2 VALUES (7001, 'edit', 'urgent'), (7002, 'create', 'low');

SELECT * FROM diff_joindiff('helpdesk_v1', 'helpdesk_v2', 'ticket_id');
```

```text
Binder Error: diff_joindiff: column 'diff_type' of target table 'helpdesk_v2' has the name of a diff output column (diff_type, changed_columns); rename it in a view and diff the views, e.g. CREATE VIEW helpdesk_v2_renamed AS SELECT * RENAME (diff_type AS src_diff_type) FROM helpdesk_v2
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues

LINE 1: SELECT * FROM diff_joindiff('helpdesk_v1', 'helpdesk_v2', 'ticket_id');
                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```

Diff two views that rename it:

```sql
CREATE VIEW helpdesk_v1_r AS SELECT * RENAME (diff_type AS change_kind) FROM helpdesk_v1;
CREATE VIEW helpdesk_v2_r AS SELECT * RENAME (diff_type AS change_kind) FROM helpdesk_v2;

SELECT * FROM diff_joindiff('helpdesk_v1_r', 'helpdesk_v2_r', 'ticket_id');
```

```text
┌───────────┬───────────┬─────────────┬──────────┬─────────────────┐
│ diff_type │ ticket_id │ change_kind │ priority │ changed_columns │
│  varchar  │   int32   │   varchar   │ varchar  │    varchar[]    │
├───────────┼───────────┼─────────────┼──────────┼─────────────────┤
│ changed   │      7001 │ edit        │ urgent   │ [priority]      │
└───────────┴───────────┴─────────────┴──────────┴─────────────────┘
```

`compare_columns` does not help here: the output shows every target column, compared or not.

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

### Floating-point noise

A migration that recomputes amounts in floating point (`quantity * unit_price`) produces values
such as `19.990000000000002` where the legacy system stored `19.99`. Compared exactly, each of
them is a change. `float_tolerance` treats numeric compare columns within an absolute
difference as equal:

```sql
CREATE TABLE erp_orders (order_id INTEGER, customer VARCHAR, net_amount DOUBLE, qty INTEGER);
INSERT INTO erp_orders VALUES
    (5001, 'Nordwind GmbH', 19.99, 1), (5002, 'Alpenblick AG', 250.00, 5),
    (5003, 'Seeblick KG', 99.95, 1), (5004, 'Elbtal OHG', NULL, 1);

CREATE TABLE platform_orders (order_id INTEGER, customer VARCHAR, net_amount DOUBLE, qty INTEGER);
INSERT INTO platform_orders VALUES
    (5001, 'Nordwind GmbH', 19.990000000000002, 1), (5002, 'Alpenblick AG', 250.00, 5),
    (5003, 'Seeblick KG', 99.50, 1), (5004, 'Elbtal OHG', 12.00, 1);

SELECT diff_type, order_id, net_amount, changed_columns
FROM diff_joindiff('erp_orders', 'platform_orders', 'order_id')
ORDER BY order_id;
```

```text
┌───────────┬──────────┬────────────────────┬─────────────────┐
│ diff_type │ order_id │     net_amount     │ changed_columns │
│  varchar  │  int32   │       double       │    varchar[]    │
├───────────┼──────────┼────────────────────┼─────────────────┤
│ changed   │     5001 │ 19.990000000000002 │ [net_amount]    │
│ changed   │     5003 │               99.5 │ [net_amount]    │
│ changed   │     5004 │               12.0 │ [net_amount]    │
└───────────┴──────────┴────────────────────┴─────────────────┘
```

```sql
SELECT diff_type, order_id, net_amount, changed_columns
FROM diff_joindiff('erp_orders', 'platform_orders', 'order_id', float_tolerance := 1e-9)
ORDER BY order_id;
```

```text
┌───────────┬──────────┬────────────┬─────────────────┐
│ diff_type │ order_id │ net_amount │ changed_columns │
│  varchar  │  int32   │   double   │    varchar[]    │
├───────────┼──────────┼────────────┼─────────────────┤
│ changed   │     5003 │       99.5 │ [net_amount]    │
│ changed   │     5004 │       12.0 │ [net_amount]    │
└───────────┴──────────┴────────────┴─────────────────┘
```

- The tolerance applies to every compare column that is numeric on both sides: `FLOAT`,
  `DOUBLE`, `DECIMAL` and the integer types, also when the type changed (`DECIMAL` to `DOUBLE`).
  The difference is computed in `DOUBLE`. Key columns are always matched exactly.
- The comparison is inclusive: with `float_tolerance := 1` the quantities `3` and `4` are equal.
- `NULL` against a value is still a change (`5004` above); `NULL` against `NULL` is not.
- `changed_columns` follows the tolerance, and the output shows the target values unchanged.

`diff_hashdiff` has `float_precision` instead, which rounds before comparing; see
[`diff_hashdiff`](#diff_hashdiff).

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
value overrides the positional value of the same parameter. Because every overload accepts every
parameter by name, `duckdb_functions()` lists an overload's positional parameters followed by all
named ones, so names such as `source_table` appear twice in its `parameters` column:

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
┌───────────┬─────────┬─────────┬───────┬─────────────────┐
│ diff_type │  store  │   sku   │  qty  │ changed_columns │
│  varchar  │ varchar │ varchar │ int32 │    varchar[]    │
├───────────┼─────────┼─────────┼───────┼─────────────────┤
│ changed   │ A       │ x2      │     8 │ [qty]           │
│ removed   │ B       │ x1      │  NULL │ NULL            │
│ added     │ B       │ x3      │     4 │ NULL            │
└───────────┴─────────┴─────────┴───────┴─────────────────┘
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
    [, float_precision := INTEGER ]
) -> TABLE(diff_type VARCHAR, <primary key columns>, <other target columns>, changed_columns VARCHAR[])
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `source_table` | `VARCHAR` | required | The old table or view |
| `target_table` | `VARCHAR` | required | The new table or view |
| `primary_key` | `VARCHAR` or `VARCHAR[]` | required | One key column, or a list. By name: `primary_key :=` or `primary_keys :=` |
| `float_precision` | `INTEGER` (by name only) | no rounding | Round `FLOAT` and `DOUBLE` compare columns to this many decimal digits before comparing. `DECIMAL` and integer columns are compared as they are. `NULL` keeps the default; a negative value is an error |

The output columns are the same as for [`diff_joindiff`](#diff_joindiff). The primary key must be
unique in both tables, as for `diff_joindiff`. Columns on one side only are not compared; list
them with [`diff_schema`](#diff_schema).

```sql
SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id') ORDER BY id;
```

```text
┌───────────┬───────┬─────────┬─────────┬───────┬─────────────────┐
│ diff_type │  id   │  name   │  city   │ score │ changed_columns │
│  varchar  │ int32 │ varchar │ varchar │ int32 │    varchar[]    │
├───────────┼───────┼─────────┼─────────┼───────┼─────────────────┤
│ changed   │     2 │ Bob     │ Bremen  │    20 │ [city]          │
│ changed   │     3 │ Cleo    │ Munich  │    35 │ [score]         │
│ removed   │     4 │ NULL    │ NULL    │  NULL │ NULL            │
│ added     │     5 │ Eve     │ Leipzig │    50 │ NULL            │
└───────────┴───────┴─────────┴─────────┴───────┴─────────────────┘
```

With `float_precision`, values that agree to that many decimal digits are equal (the
`erp_orders` tables of [Floating-point noise](#floating-point-noise)):

```sql
SELECT diff_type, order_id, net_amount, changed_columns
FROM diff_hashdiff('erp_orders', 'platform_orders', 'order_id', float_precision := 6)
ORDER BY order_id;
```

```text
┌───────────┬──────────┬────────────┬─────────────────┐
│ diff_type │ order_id │ net_amount │ changed_columns │
│  varchar  │  int32   │   double   │    varchar[]    │
├───────────┼──────────┼────────────┼─────────────────┤
│ changed   │     5003 │       99.5 │ [net_amount]    │
│ changed   │     5004 │       12.0 │ [net_amount]    │
└───────────┴──────────┴────────────┴─────────────────┘
```

Rounding has a boundary: two values close to each other on either side of it round apart. At
4 digits `1.08344999` rounds to `1.0834` and `1.08345001` to `1.0835`, so they still differ,
while `diff_joindiff` with `float_tolerance := 0.0001` counts them as equal. Use
`float_tolerance` when a fixed maximum difference is what matters:

```sql
CREATE TABLE fx_rates_legacy (currency_pair VARCHAR, rate DOUBLE);
INSERT INTO fx_rates_legacy VALUES ('EURUSD', 1.08344999), ('EURCHF', 0.94120000);
CREATE TABLE fx_rates_new (currency_pair VARCHAR, rate DOUBLE);
INSERT INTO fx_rates_new VALUES ('EURUSD', 1.08345001), ('EURCHF', 0.94120001);

SELECT diff_type, currency_pair, rate, changed_columns
FROM diff_hashdiff('fx_rates_legacy', 'fx_rates_new', 'currency_pair', float_precision := 4);
```

```text
┌───────────┬───────────────┬────────────┬─────────────────┐
│ diff_type │ currency_pair │    rate    │ changed_columns │
│  varchar  │    varchar    │   double   │    varchar[]    │
├───────────┼───────────────┼────────────┼─────────────────┤
│ changed   │ EURUSD        │ 1.08345001 │ [rate]          │
└───────────┴───────────────┴────────────┴─────────────────┘
```

```sql
SELECT count(*) AS changed
FROM diff_joindiff('fx_rates_legacy', 'fx_rates_new', 'currency_pair', float_tolerance := 0.0001);
```

```text
┌─────────┐
│ changed │
│  int64  │
├─────────┤
│       0 │
└─────────┘
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

`compare_columns`, `include_all` and `float_tolerance` do not exist on `diff_hashdiff`, and
`float_precision` does not exist on `diff_joindiff`:

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

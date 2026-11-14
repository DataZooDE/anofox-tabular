---
name: anofox-tabular-diff
description: >
  Row-level and schema diffing of two tables with the anofox_tabular DuckDB extension:
  `diff_joindiff` (added / removed / changed rows on a primary key, compare subset,
  unchanged rows, float tolerance), `diff_hashdiff` (same diff, optional float rounding) and
  `diff_schema` (added / removed / retyped columns), plus migration sign-off with `volume`,
  `match_rate` and `profile_summary`. Use when the user wants to compare two versions of a
  table, validate a data migration or reconcile old and new copies, or list schema drift.
version: 2026.11.13
user-invocable: false
---

# Anofox Tabular — Diffing & Migration Cheat Sheet

**Extension:** `anofox_tabular` v2026.11.13 | **DuckDB:** v2.0.0 / v1.5.6 | **Naming:** short alias (`diff_joindiff`) = canonical `anofox_tab_diff_joindiff`

Compares a source (old) and a target (new) table on a primary key, or their column lists. The
row diffs are rewritten into one native `FULL OUTER JOIN` query.

Load with `LOAD anofox_tabular;` (install: `INSTALL anofox_tabular FROM community;`).

```sql no-run
INSTALL anofox_tabular FROM community;
LOAD anofox_tabular;
```

## Critical gotchas

- **Argument order is `(source_table, target_table, primary_key, ...)`**: old first, new second.
  `added` = key only in target, `removed` = key only in source.
- **Table names are strings** (`'orders_old'`), not identifiers. Tables, views, TEMP tables and
  qualified names (`'billing.invoices'`) all work: the diff functions read in your own
  transaction, so uncommitted rows and TEMP tables are seen. (`profile_summary`, used in the
  sign-off workflow, does reject TEMP tables and uncommitted changes.)
- **`compare_columns` is a `VARCHAR[]` list** (`['city', 'score']`), never a comma-separated
  string; `'city, score'` fails with a cast error. `primary_key` is a `VARCHAR` (one column) or a
  `VARCHAR[]` (compound key). `compare_columns := []` is the same as the default (all shared
  non-key columns).
- **To pass `include_all` positionally you must also pass `compare_columns`**: use `NULL` for
  "default compare set" (`diff_joindiff('a', 'b', 'id', NULL, true)`), or simply
  `include_all := true` by name.
- **Every parameter accepts `name := value`** on all three functions; positional and named can be
  mixed and a named value overrides the positional one. `primary_key :=` takes a single name,
  `primary_keys :=` a list; passing both is a binder error.
- **`float_tolerance` exists only on `diff_joindiff`, `float_precision` only on
  `diff_hashdiff`, both by name only.** `compare_columns` / `include_all` do not exist on
  `diff_hashdiff` (binder error `Invalid named parameter`).
- **`bisection_threshold` / `bisection_factor` on `diff_hashdiff` are declared but not
  implemented**: passing either (positionally as 4th/5th argument, or by name) is a binder
  error. `diff_hashdiff` is not faster than `diff_joindiff`; it computes the same full diff.
- **The primary key must be unique in both tables** (two `NULL` keys count as a repeat). A
  repeated key raises `Invalid Input Error: ... primary key is not unique in source table ...`
  at run time instead of reporting false changes. Deduplicate in a view first.
- **Target columns named `diff_type` or `changed_columns` (any case) are a binder error** —
  key or not, compared or not. Diff views that `RENAME` them. A source-only column of that name
  is harmless. `compare_columns` does not help (every target column is in the output).
- **Output shows target values only.** `removed` rows have `NULL` in every non-key column. Join
  the source back for old values with a NULL-safe join, as the diff matches keys:
  `LEFT JOIN source s ON d.key IS NOT DISTINCT FROM s.key` (one predicate per key column;
  `USING (key)` finds no old values for a `NULL` key).
- **Columns in only one table are never compared** (renamed column = one removed + one added
  column); the trace log warns. Run `diff_schema` first; diff a view that renames back.
- **NULL semantics:** keys match NULL-safely (`IS NOT DISTINCT FROM`); a change from/to `NULL`
  is a change; `NULL` vs `NULL` is equal (also with `float_tolerance`).
- **Type changes:** a key/compare column whose type differs is compared as text unless both
  types are numeric (`7` vs `'007'` differ; `5000.00 DECIMAL` vs `5000.0 DOUBLE` equal).
  `TIMESTAMPTZ` vs text/`TIMESTAMP` compares on the UTC instant. `JSON` compares as text.
- **Missing table / column is a binder error** naming it; `NULL` for a table or key argument is
  `<function>: <argument> must not be NULL`. `NULL` for `compare_columns`, `include_all`,
  `float_tolerance`, `float_precision` keeps the default.

## `diff_joindiff`

```text
diff_joindiff(
    source_table     VARCHAR,
    target_table     VARCHAR,
    primary_key      VARCHAR | VARCHAR[]
    [, compare_columns VARCHAR[]
    [, include_all     BOOLEAN ]]
    [, float_tolerance := DOUBLE ]
) -> TABLE(diff_type VARCHAR, <key columns>, <other target columns>, changed_columns VARCHAR[])
```

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `source_table` | `VARCHAR` | required | Old table / view |
| `target_table` | `VARCHAR` | required | New table / view |
| `primary_key` | `VARCHAR` or `VARCHAR[]` | required | Key column(s). Named: `primary_key := 'id'` or `primary_keys := ['id', 'day']` |
| `compare_columns` | `VARCHAR[]` | all non-key columns in both tables | Columns that decide `changed`. `NULL` = default |
| `include_all` | `BOOLEAN` | `false` | Also return `unchanged` rows |
| `float_tolerance` | `DOUBLE`, by name only | `0` (exact) | Numeric compare columns with `abs(source - target) <= tol` are equal (inclusive, computed in `DOUBLE`; keys always exact). Negative / `NaN` / `inf` is an error |

Output columns, in this order:

| Column | Type | Meaning |
|---|---|---|
| `diff_type` | `VARCHAR` | `added`, `removed`, `changed`, `unchanged` (only with `include_all`) |
| key columns | key type | `COALESCE(source key, target key)`; `VARCHAR` if the key type differs and is not numeric on both sides |
| other target columns | their type | Target row values (all target columns, compared or not); `NULL` for `removed` |
| `changed_columns` | `VARCHAR[]` | Compare columns that differ (compare order) for `changed`; `[]` for `unchanged`; `NULL` for `added`/`removed`. Always last |

Sample data used by the examples below:

```sql
CREATE TABLE customers_v1 (id INTEGER, name VARCHAR, city VARCHAR, score INTEGER);
INSERT INTO customers_v1 VALUES
    (1, 'Ada', 'Berlin', 10), (2, 'Bob', 'Hamburg', 20),
    (3, 'Cleo', 'Munich', 30), (4, 'Dan', 'Cologne', 40);

CREATE TABLE customers_v2 (id INTEGER, name VARCHAR, city VARCHAR, score INTEGER);
INSERT INTO customers_v2 VALUES
    (1, 'Ada', 'Berlin', 10),   -- unchanged
    (2, 'Bob', 'Bremen', 20),   -- city changed
    (3, 'Cleo', 'Munich', 35),  -- score changed
    (5, 'Eve', 'Leipzig', 50);  -- added; id 4 removed
```

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

Positional vs named forms (all equivalent styles):

```sql
-- compare subset positionally (a LIST), unchanged rows positionally (needs compare_columns first)
SELECT diff_type, id FROM diff_joindiff('customers_v1', 'customers_v2', 'id', ['city']) ORDER BY id;
SELECT diff_type, id FROM diff_joindiff('customers_v1', 'customers_v2', 'id', NULL, true) ORDER BY id;

-- by name
SELECT diff_type, id
FROM diff_joindiff(source_table := 'customers_v1', target_table := 'customers_v2',
                   primary_key := 'id', compare_columns := ['score'], include_all := true)
ORDER BY id;
```

`compare_columns` as a comma-separated string is wrong:

```sql
SELECT * FROM diff_joindiff('customers_v1', 'customers_v2', 'id', 'city, score');
```

```text
Invalid Input Error: Failed to cast value: Type VARCHAR with value 'city, score' can't be cast to the destination type VARCHAR[]
```

Passing both key forms:

```sql
SELECT * FROM diff_joindiff('customers_v1', 'customers_v2', primary_key := 'id', primary_keys := ['id']);
```

```text
Binder Error: diff_joindiff: pass either primary_key or primary_keys, not both
```

### Old and new values side by side

```sql
SELECT d.diff_type, d.id, s.city AS old_city, d.city AS new_city,
       s.score AS old_score, d.score AS new_score, d.changed_columns
FROM diff_joindiff('customers_v1', 'customers_v2', 'id') d
LEFT JOIN customers_v1 s ON d.id IS NOT DISTINCT FROM s.id
ORDER BY d.id;
```

### Count changes per column

```sql
SELECT col, count(*) AS n
FROM diff_joindiff('customers_v1', 'customers_v2', 'id'), unnest(changed_columns) AS u(col)
GROUP BY col ORDER BY col;
```

### Compound keys

```sql
CREATE TABLE stock_v1 (store VARCHAR, sku VARCHAR, qty INTEGER);
INSERT INTO stock_v1 VALUES ('A', 'x1', 5), ('A', 'x2', 7), ('B', 'x1', 3), (NULL, 'x9', 1);
CREATE TABLE stock_v2 (store VARCHAR, sku VARCHAR, qty INTEGER);
INSERT INTO stock_v2 VALUES ('A', 'x1', 5), ('A', 'x2', 8), ('B', 'x3', 4), (NULL, 'x9', 1);

SELECT * FROM diff_joindiff('stock_v1', 'stock_v2', ['store', 'sku']) ORDER BY store, sku;
-- (NULL, 'x9') matches itself NULL-safely and is not reported
```

### Non-unique key

```sql
CREATE TABLE visits_v1 (id INTEGER, page VARCHAR);
INSERT INTO visits_v1 VALUES (1, 'home'), (1, 'cart'), (2, 'home');
SELECT * FROM diff_joindiff('visits_v1', 'visits_v1', 'id');
```

```text
Invalid Input Error: diff_joindiff: primary key is not unique in source table 'visits_v1' (id = 1 appears 2 times)
```

Fix: pick a truly unique (compound) key, or diff deduplicated views
(`CREATE VIEW v AS SELECT * FROM t QUALIFY row_number() OVER (PARTITION BY id ORDER BY ...) = 1`).

### Output-column name collision and the RENAME workaround

```sql
CREATE TABLE helpdesk_v1 (ticket_id INTEGER, diff_type VARCHAR, priority VARCHAR);
INSERT INTO helpdesk_v1 VALUES (7001, 'edit', 'high'), (7002, 'create', 'low');
CREATE TABLE helpdesk_v2 (ticket_id INTEGER, diff_type VARCHAR, priority VARCHAR);
INSERT INTO helpdesk_v2 VALUES (7001, 'edit', 'urgent'), (7002, 'create', 'low');

SELECT * FROM diff_joindiff('helpdesk_v1', 'helpdesk_v2', 'ticket_id');
```

```text
Binder Error: diff_joindiff: column 'diff_type' of target table 'helpdesk_v2' has the name of a diff output column (diff_type, changed_columns); rename it in a view and diff the views, e.g. CREATE VIEW helpdesk_v2_renamed AS SELECT * RENAME (diff_type AS src_diff_type) FROM helpdesk_v2
```

```sql
CREATE VIEW helpdesk_v1_r AS SELECT * RENAME (diff_type AS change_kind) FROM helpdesk_v1;
CREATE VIEW helpdesk_v2_r AS SELECT * RENAME (diff_type AS change_kind) FROM helpdesk_v2;
SELECT * FROM diff_joindiff('helpdesk_v1_r', 'helpdesk_v2_r', 'ticket_id');
-- changed | 7001 | edit | urgent | [priority]
```

Same view trick for a renamed column (`SELECT * RENAME (town AS city) FROM crm_new`) or for
normalising values before comparing (`lower(trim(email)) AS email`, `json(col)::VARCHAR`).

### Floating-point noise

```sql
CREATE TABLE erp_orders (order_id INTEGER, customer VARCHAR, net_amount DOUBLE, qty INTEGER);
INSERT INTO erp_orders VALUES
    (5001, 'Nordwind GmbH', 19.99, 1), (5002, 'Alpenblick AG', 250.00, 5),
    (5003, 'Seeblick KG', 99.95, 1), (5004, 'Elbtal OHG', NULL, 1);
CREATE TABLE platform_orders (order_id INTEGER, customer VARCHAR, net_amount DOUBLE, qty INTEGER);
INSERT INTO platform_orders VALUES
    (5001, 'Nordwind GmbH', 19.990000000000002, 1), (5002, 'Alpenblick AG', 250.00, 5),
    (5003, 'Seeblick KG', 99.50, 1), (5004, 'Elbtal OHG', 12.00, 1);

-- exact: 5001, 5003, 5004 changed; with tolerance: only 5003 and 5004 (NULL -> 12.0 still a change)
SELECT diff_type, order_id, net_amount, changed_columns
FROM diff_joindiff('erp_orders', 'platform_orders', 'order_id', float_tolerance := 1e-9)
ORDER BY order_id;
```

```sql
SELECT * FROM diff_joindiff('erp_orders', 'platform_orders', 'order_id', float_tolerance := -0.01);
```

```text
Binder Error: diff_joindiff: float_tolerance must be a finite number >= 0 (got -0.01)
```

The tolerance applies to every compare column numeric on both sides (`FLOAT`, `DOUBLE`,
`DECIMAL`, integers, also after a type change). With `float_tolerance := 1`, qty `3` vs `4` is
equal. `changed_columns` follows the tolerance; output values are untouched target values.

## `diff_hashdiff`

```text
diff_hashdiff(
    source_table VARCHAR,
    target_table VARCHAR,
    primary_key  VARCHAR | VARCHAR[]
    [, float_precision := INTEGER ]
) -> TABLE(diff_type VARCHAR, <key columns>, <other target columns>, changed_columns VARCHAR[])
```

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `source_table`, `target_table`, `primary_key` / `primary_keys` | as `diff_joindiff` | required | Same meaning and named forms |
| `float_precision` | `INTEGER`, by name only | no rounding | Round `FLOAT`/`DOUBLE` compare columns to N decimals before comparing. `DECIMAL` and integers untouched. Negative is an error |
| `bisection_threshold`, `bisection_factor` | `BIGINT` | — | Declared (positions 4 and 5, or by name), **not implemented**: always a binder error |

Same output as `diff_joindiff` with its defaults (all shared non-key columns compared, no
`unchanged` rows). Same unique-key, name-collision and type-change rules.

```sql
SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id') ORDER BY id;

SELECT diff_type, order_id, net_amount, changed_columns
FROM diff_hashdiff('erp_orders', 'platform_orders', 'order_id', float_precision := 6)
ORDER BY order_id;
```

Rounding has a boundary: at 4 digits `1.08344999` -> `1.0834` and `1.08345001` -> `1.0835` still
differ, while `diff_joindiff(..., float_tolerance := 0.0001)` treats them as equal. Prefer
`float_tolerance` when a maximum absolute difference is the requirement.

```sql
SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id', bisection_threshold := 1000);
```

```text
Binder Error: diff_hashdiff: the bisection_threshold/bisection_factor parameters are not implemented yet; remove them (diff_hashdiff currently computes the same full diff as diff_joindiff)
```

```sql
SELECT * FROM diff_hashdiff('customers_v1', 'customers_v2', 'id', include_all := true);
```

```text
Binder Error: Invalid named parameter "include_all" for function diff_hashdiff
```

## `diff_schema`

```text
diff_schema(source_table VARCHAR, target_table VARCHAR)
  -> TABLE(column_name VARCHAR, source_type VARCHAR, target_type VARCHAR, drift VARCHAR)
```

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `source_table` | `VARCHAR` | required | Old table / view (also `source_table :=`) |
| `target_table` | `VARCHAR` | required | New table / view (also `target_table :=`) |

| Column | Type | Meaning |
|---|---|---|
| `column_name` | `VARCHAR` | Spelling from source (from target for `added`) |
| `source_type` | `VARCHAR` | `DESCRIBE` type text in source; `NULL` for `added` |
| `target_type` | `VARCHAR` | Type in target; `NULL` for `removed` |
| `drift` | `VARCHAR` | `same`, `added` (target only), `removed` (source only), `retyped` |

- Names match case-insensitively (`Amount` vs `amount` is `same`). A rename is one `removed` + one
  `added` row. Types compare on `DESCRIBE` text: `DECIMAL(10,4)` -> `DECIMAL(18,6)` is `retyped`.
- Row order: source column order, then `added` columns in target order.
- Unknown table: `Binder Error: diff_schema: table or view '<name>' does not exist`.

```sql
CREATE TABLE invoices_v1 (invoice_id INTEGER, customer_name VARCHAR, Amount DECIMAL(12, 2),
                          currency VARCHAR, cost_center INTEGER, fax VARCHAR, issued_on DATE);
CREATE TABLE invoices_v2 (invoice_id INTEGER, customer_name VARCHAR, amount DECIMAL(12, 2),
                          ccy VARCHAR, cost_center VARCHAR, issued_on DATE, payment_terms VARCHAR);

SELECT * FROM diff_schema('invoices_v1', 'invoices_v2') WHERE drift <> 'same';
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

## Settings

No diff-specific settings. Skipped one-sided columns and type changes are logged through the
shared trace settings: `SET anofox_tab_trace_level = 'warn';` keeps the warnings and hides info
lines (widening type changes such as `INTEGER` -> `BIGINT` log at `debug`).

## Canonical workflow: migration sign-off

1. `diff_schema` — which columns are compared at all; fix renames with a view.
2. `diff_joindiff` — counts per `diff_type`, per-column change counts, side-by-side review.
3. Restrict to columns that must not change (`compare_columns`), absorb float noise
   (`float_tolerance`).
4. Back the diff with whole-table checks (`profile_summary`, `volume`, `match_rate`): a diff can
   balance out (one removed + one added keeps the row count) and does not check referential
   integrity.
5. One boolean verdict.

```sql
CREATE TABLE orders_old AS
SELECT i AS order_id, 100 + (i % 4) AS customer_id, round(10 + i * 1.25, 2) AS amount,
       CASE WHEN i % 3 = 0 THEN 'shipped' ELSE 'open' END AS status
FROM range(1, 11) t(i);

CREATE TABLE orders_new AS
SELECT order_id, customer_id,
       CASE WHEN order_id = 3 THEN round(amount) ELSE amount END AS amount,
       CASE WHEN order_id = 7 THEN 'cancelled' ELSE status END AS status
FROM orders_old WHERE order_id <> 5
UNION ALL SELECT 11, 999, 99.99, 'open';

CREATE TABLE customers AS SELECT 100 + i AS id FROM range(0, 4) t(i);
```

```sql
-- 1. schema drift: amount DECIMAL(23,2) -> DECIMAL(25,2) is 'retyped' (the CTAS widened it);
--    numeric on both sides, so the row diff still compares it by value
SELECT * FROM diff_schema('orders_old', 'orders_new') WHERE drift <> 'same';

-- 2. change counts: added 1, changed 2, removed 1 (+ unchanged 7 with include_all)
SELECT diff_type, count(*) AS n
FROM diff_joindiff('orders_old', 'orders_new', 'order_id', include_all := true)
GROUP BY diff_type ORDER BY diff_type;

-- per column
SELECT col, count(*) AS changed_rows
FROM diff_joindiff('orders_old', 'orders_new', 'order_id'), unnest(changed_columns) AS u(col)
GROUP BY col ORDER BY col;
```

```sql
-- 4. whole-table shape of both copies (regular tables only, committed data)
SELECT 'old' AS copy, row_count, column_count, total_nulls, duplicate_row_count
FROM profile_summary('orders_old')
UNION ALL
SELECT 'new', row_count, column_count, total_nulls, duplicate_row_count
FROM profile_summary('orders_new');

-- volume(table, min_rows, max_rows); match_rate(left, right, left_keys CSV, right_keys CSV, min_rate)
SELECT 'volume' AS check_name, status, message FROM volume('orders_new', 10, 10)
UNION ALL
SELECT 'customer_fk', status, message
FROM match_rate('orders_new', 'customers', 'customer_id', 'id', 1.0);
```

```sql
-- 5. verdict
WITH d AS (
    SELECT count(*) AS differences
    FROM diff_joindiff('orders_old', 'orders_new', 'order_id',
                       compare_columns := ['amount', 'customer_id'])
),
v AS (SELECT status FROM volume('orders_new', 10, 10)),
m AS (SELECT status FROM match_rate('orders_new', 'customers', 'customer_id', 'id', 1.0))
SELECT d.differences, v.status AS volume, m.status AS customer_fk,
       d.differences = 0 AND v.status = 'pass' AND m.status = 'pass' AS sign_off
FROM d, v, m;
```

```text
┌─────────────┬─────────┬─────────────┬──────────┐
│ differences │ volume  │ customer_fk │ sign_off │
│    int64    │ varchar │   varchar   │ boolean  │
├─────────────┼─────────┼─────────────┼──────────┤
│           3 │ pass    │ fail        │ false    │
└─────────────┴─────────┴─────────────┴──────────┘
```

Here `volume` passes (one order dropped, one invented, still 10 rows); only the diff and the
foreign-key check (`match_rate` finds customer `999`) catch the problem.

## Common patterns

```sql
-- Diff summary as one row
SELECT count(*) FILTER (WHERE diff_type = 'added')   AS added,
       count(*) FILTER (WHERE diff_type = 'removed') AS removed,
       count(*) FILTER (WHERE diff_type = 'changed') AS changed
FROM diff_joindiff('customers_v1', 'customers_v2', 'id');

-- Persist a diff for review
CREATE TABLE customers_diff AS
SELECT * FROM diff_joindiff('customers_v1', 'customers_v2', 'id');

-- Rows where a specific column changed
SELECT id, city FROM diff_joindiff('customers_v1', 'customers_v2', 'id')
WHERE list_contains(changed_columns, 'city');

-- Compare a filtered slice / normalised values through views
CREATE VIEW c1_norm AS SELECT id, lower(trim(name)) AS name, city FROM customers_v1 WHERE id < 4;
CREATE VIEW c2_norm AS SELECT id, lower(trim(name)) AS name, city FROM customers_v2 WHERE id < 4;
SELECT * FROM diff_joindiff('c1_norm', 'c2_norm', 'id');
```

Canonical names work the same: `anofox_tab_diff_joindiff`, `anofox_tab_diff_hashdiff`,
`anofox_tab_diff_schema`.

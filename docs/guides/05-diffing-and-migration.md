# Diffing and Migration Checks

> Compare the old and the new copy of a table after a migration, and decide whether to sign it off.

**Use this guide to:**
- List the rows that a migration added, removed or changed
- Count the changes by type and inspect old and new values side by side
- Diff tables with a compound key, or on a subset of columns
- Back the diff with a profile and a few checks before you switch over
- See which columns a migration added, dropped, renamed or retyped

Every example creates its own data. Run them in order in one DuckDB session, after
`LOAD anofox_tabular;`.

## 1. The two copies

`orders_old` is the table in the old system, `orders_new` the migrated copy. The migration
dropped one order, created one that should not exist, rounded one amount and changed one status:

```sql
CREATE TABLE orders_old AS
SELECT i AS order_id,
       100 + (i % 4) AS customer_id,
       round(10 + i * 1.25, 2) AS amount,
       CASE WHEN i % 3 = 0 THEN 'shipped' ELSE 'open' END AS status
FROM range(1, 11) t(i);

CREATE TABLE orders_new AS
SELECT order_id, customer_id,
       CASE WHEN order_id = 3 THEN round(amount) ELSE amount END AS amount,
       CASE WHEN order_id = 7 THEN 'cancelled' ELSE status END AS status
FROM orders_old
WHERE order_id <> 5
UNION ALL
SELECT 11, 999, 99.99, 'open';
```

## 2. Count the changes

[`diff_joindiff`](../api/09-diffing.md#diff_joindiff) joins both tables on the primary key and
returns one row per difference. Start with a count per `diff_type`:

```sql
SELECT diff_type, count(*) AS rows
FROM diff_joindiff('orders_old', 'orders_new', 'order_id')
GROUP BY diff_type
ORDER BY diff_type;
```

```text
┌───────────┬───────┐
│ diff_type │ rows  │
│  varchar  │ int64 │
├───────────┼───────┤
│ added     │     1 │
│ changed   │     2 │
│ removed   │     1 │
└───────────┴───────┘
```

With `include_all := true` the unchanged rows are listed too, so the counts add up to all keys
of both tables:

```sql
SELECT diff_type, count(*) AS rows
FROM diff_joindiff('orders_old', 'orders_new', 'order_id', include_all := true)
GROUP BY diff_type
ORDER BY diff_type;
```

```text
┌───────────┬───────┐
│ diff_type │ rows  │
│  varchar  │ int64 │
├───────────┼───────┤
│ added     │     1 │
│ changed   │     2 │
│ removed   │     1 │
│ unchanged │     7 │
└───────────┴───────┘
```

## 3. Look at the changed rows

The diff returns the target (new) values. Join the source table to put old and new values next
to each other:

```sql
SELECT d.diff_type, d.order_id,
       o.amount AS old_amount, d.amount AS new_amount,
       o.status AS old_status, d.status AS new_status
FROM diff_joindiff('orders_old', 'orders_new', 'order_id') d
LEFT JOIN orders_old o USING (order_id)
ORDER BY d.order_id;
```

```text
┌───────────┬──────────┬───────────────┬───────────────┬────────────┬────────────┐
│ diff_type │ order_id │  old_amount   │  new_amount   │ old_status │ new_status │
│  varchar  │  int64   │ decimal(23,2) │ decimal(25,2) │  varchar   │  varchar   │
├───────────┼──────────┼───────────────┼───────────────┼────────────┼────────────┤
│ changed   │        3 │         13.75 │         14.00 │ shipped    │ shipped    │
│ removed   │        5 │         16.25 │          NULL │ open       │ NULL       │
│ changed   │        7 │         18.75 │         18.75 │ open       │ cancelled  │
│ added     │       11 │          NULL │         99.99 │ NULL       │ open       │
└───────────┴──────────┴───────────────┴───────────────┴────────────┴────────────┘
```

The last output column, `changed_columns`, names the columns that differ in each changed row
(`NULL` for added and removed rows), so a per-column change count needs no join:

```sql
SELECT col, count(*) AS changed_rows
FROM diff_joindiff('orders_old', 'orders_new', 'order_id'), unnest(changed_columns) AS u(col)
GROUP BY col
ORDER BY col;
```

```text
┌─────────┬──────────────┐
│   col   │ changed_rows │
│ varchar │    int64     │
├─────────┼──────────────┤
│ amount  │            1 │
│ status  │            1 │
└─────────┴──────────────┘
```

## 4. Compare only some columns

Some differences are expected, for example a status that moved on in the new system. Pass the
columns that must not change as `compare_columns`. Here only `amount` and `customer_id` count:

```sql
SELECT diff_type, order_id
FROM diff_joindiff('orders_old', 'orders_new', 'order_id',
                   compare_columns := ['amount', 'customer_id'])
ORDER BY order_id;
```

```text
┌───────────┬──────────┐
│ diff_type │ order_id │
│  varchar  │  int64   │
├───────────┼──────────┤
│ changed   │        3 │
│ removed   │        5 │
│ added     │       11 │
└───────────┴──────────┘
```

## 5. Compound keys

When a row is identified by several columns, pass them as a list. Key parts that are `NULL` on
both sides still match:

```sql
CREATE TABLE prices_old (region VARCHAR, sku VARCHAR, price DECIMAL(10,2));
INSERT INTO prices_old VALUES ('EU', 'A1', 9.99), ('EU', 'B2', 4.50), ('US', 'A1', 10.99), (NULL, 'Z9', 1.00);

CREATE TABLE prices_new (region VARCHAR, sku VARCHAR, price DECIMAL(10,2));
INSERT INTO prices_new VALUES ('EU', 'A1', 9.99), ('EU', 'B2', 4.75), ('US', 'A1', 10.99), (NULL, 'Z9', 1.00);

SELECT * FROM diff_joindiff('prices_old', 'prices_new', ['region', 'sku']);
```

```text
┌───────────┬─────────┬─────────┬───────────────┬─────────────────┐
│ diff_type │ region  │   sku   │     price     │ changed_columns │
│  varchar  │ varchar │ varchar │ decimal(10,2) │    varchar[]    │
├───────────┼─────────┼─────────┼───────────────┼─────────────────┤
│ changed   │ EU      │ B2      │          4.75 │ [price]         │
└───────────┴─────────┴─────────┴───────────────┴─────────────────┘
```

## 6. Back the diff with a profile and checks

A diff tells you which rows differ. Before you switch over, also check the new table as a whole.

Compare the shape of both tables with
[`profile_summary`](../api/10-profiling.md#profile_summary):

```sql
SELECT 'old' AS copy, row_count, column_count, total_nulls, duplicate_row_count
FROM profile_summary('orders_old')
UNION ALL
SELECT 'new', row_count, column_count, total_nulls, duplicate_row_count
FROM profile_summary('orders_new')
ORDER BY copy DESC;
```

```text
┌─────────┬───────────┬──────────────┬─────────────┬─────────────────────┐
│  copy   │ row_count │ column_count │ total_nulls │ duplicate_row_count │
│ varchar │   int64   │    int64     │    int64    │        int64        │
├─────────┼───────────┼──────────────┼─────────────┼─────────────────────┤
│ old     │        10 │            4 │           0 │                   0 │
│ new     │        10 │            4 │           0 │                   0 │
└─────────┴───────────┴──────────────┴─────────────┴─────────────────────┘
```

Check that the new table has the expected number of rows with
[`volume`](../api/07-data-quality.md#volume), and that every order points to a known customer
with [`match_rate`](../api/07-data-quality.md#match_rate):

```sql
CREATE TABLE customers AS SELECT 100 + i AS id FROM range(0, 4) t(i);

SELECT 'volume' AS check_name, status, message FROM volume('orders_new', 10, 10)
UNION ALL
SELECT 'customer_fk', status, message
FROM match_rate('orders_new', 'customers', 'customer_id', 'id', 1.0);
```

```text
┌─────────────┬─────────┬────────────────────────────────────────────┐
│ check_name  │ status  │                  message                   │
│   varchar   │ varchar │                  varchar                   │
├─────────────┼─────────┼────────────────────────────────────────────┤
│ volume      │ pass    │ Row count 10 is within acceptable range    │
│ customer_fk │ fail    │ Match rate 0.9 (9/10) is below minimum 1.0 │
└─────────────┴─────────┴────────────────────────────────────────────┘
```

The row count passes: the migration dropped one order and created one, so the total is still
10. The diff and `profile_summary` agree on that. Only the foreign-key check catches order 11 and
its unknown customer 999. A volume check alone would have missed the problem.

## 7. Schema drift and type changes

The diff compares rows, not schemas. A column that exists in only one of the two tables (added,
dropped or renamed) is not compared, and the trace log names it as a warning. List the schema
drift explicitly with [`diff_schema`](../api/09-diffing.md#diff_schema). Here a CRM migration
dropped `fax`, added `segment`, renamed `city` to `town` and changed the types of `zip`,
`credit_limit` and the key:

```sql
CREATE TABLE crm_old (customer_id INTEGER, company VARCHAR, city VARCHAR, zip INTEGER,
                      credit_limit DECIMAL(12, 2), fax VARCHAR);
INSERT INTO crm_old VALUES
    (1001, 'Brauhaus Mueller GmbH', 'Munich',   80331, 5000.00, '+49 89 123456'),
    (1003, 'Nordsee Fisch KG',      'Kiel',         7, 2500.00, NULL),
    (1004, 'Alpine Outdoor AG',     'Zurich',    8001, 1200.00, '+41 44 765432');

CREATE TABLE crm_new (customer_id BIGINT, company VARCHAR, town VARCHAR, zip VARCHAR,
                      credit_limit DOUBLE, segment VARCHAR);
INSERT INTO crm_new VALUES
    (1001, 'Brauhaus Mueller GmbH', 'Munich', '80331', 5000.0, 'enterprise'),
    (1003, 'Nordsee Fisch KG',      'Kiel',   '007',   2500.0, 'smb'),
    (1004, 'Alpine Outdoor AG',     'Zurich', 'n/a',   1200.0, 'enterprise');

SELECT * FROM diff_schema('crm_old', 'crm_new') WHERE drift <> 'same';
```

```text
┌──────────────┬───────────────┬─────────────┬─────────┐
│ column_name  │  source_type  │ target_type │  drift  │
│   varchar    │    varchar    │   varchar   │ varchar │
├──────────────┼───────────────┼─────────────┼─────────┤
│ customer_id  │ INTEGER       │ BIGINT      │ retyped │
│ city         │ VARCHAR       │ NULL        │ removed │
│ zip          │ INTEGER       │ VARCHAR     │ retyped │
│ credit_limit │ DECIMAL(12,2) │ DOUBLE      │ retyped │
│ fax          │ VARCHAR       │ NULL        │ removed │
│ town         │ NULL          │ VARCHAR     │ added   │
│ segment      │ NULL          │ VARCHAR     │ added   │
└──────────────┴───────────────┴─────────────┴─────────┘
```

A renamed column shows up as one removed and one added column (`city` and `town`). To compare
it anyway, diff a view that renames it back.

Columns whose type changed are still compared. When the old and the new type are not both
numeric, the diff compares the values as text, so `7` and `'007'` differ and `'n/a'` does not
abort the diff with a conversion error. Numeric type changes compare by value: `5000.00` and
`5000.0` are equal. A `TIMESTAMPTZ` column that became text or a plain `TIMESTAMP` compares on
its UTC instant, so the result does not depend on the session `TimeZone`; a plain `TIMESTAMP` is
read as UTC.

```sql
SELECT diff_type, customer_id, zip, credit_limit
FROM diff_joindiff('crm_old', 'crm_new', 'customer_id')
ORDER BY customer_id;
```

```text
┌───────────┬─────────────┬─────────┬──────────────┐
│ diff_type │ customer_id │   zip   │ credit_limit │
│  varchar  │    int64    │ varchar │    double    │
├───────────┼─────────────┼─────────┼──────────────┤
│ changed   │        1003 │ 007     │       2500.0 │
│ changed   │        1004 │ n/a     │       1200.0 │
└───────────┴─────────────┴─────────┴──────────────┘
```

## 8. A sign-off query

Put the pieces together in one query that returns a single verdict:

```sql
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

## Next steps

- [Diffing API reference](../api/09-diffing.md)
- [Profiling API reference](../api/10-profiling.md)
- [Guide: data quality checks](02-data-quality-checks.md), to keep checking the new table after the migration
- [Conventions: named parameters](../api/00-conventions.md#named-parameters)

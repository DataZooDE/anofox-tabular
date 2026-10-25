# Profiling

> Per-column statistics, a one-row table overview and pairwise correlations of numeric columns.

**Use this page to:**
- Get null rates, distinct counts, numeric statistics and frequent values per column
- Detect columns that hold emails, phone numbers, UUIDs, URLs, IP addresses or ISO dates
- Get a quick overview of a table: rows, column types, nulls, duplicate rows
- Find correlated numeric columns

The profiling functions read the table through a second connection. Pass a regular table or a
view; a TEMP table is not found. See [Conventions](00-conventions.md#table-and-column-arguments).

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`profile_table`](#profile_table) | `anofox_tab_profile_table` | table | one row per column, 27 columns |
| [`profile_summary`](#profile_summary) | `anofox_tab_profile_summary` | table | one row, 11 columns |
| [`profile_correlations`](#profile_correlations) | `anofox_tab_profile_correlations` | table | one row per pair of numeric columns |

Setup for the examples on this page:

```sql
CREATE TABLE orders AS
SELECT i AS order_id,
       'user' || (i % 7) || '@example.com' AS email,
       CASE WHEN i % 10 = 0 THEN NULL ELSE round(10 + (i % 13) * 2.5, 2) END AS amount,
       (i % 5) + 1 AS quantity,
       ['red', 'red', 'red', 'red', 'red', 'green', 'green', 'green', 'blue', 'blue'][(i % 10) + 1] AS color,
       DATE '2026-01-01' + (i % 30)::INTEGER AS order_date
FROM range(1, 201) t(i);
```

---

## `profile_table`

Canonical name: `anofox_tab_profile_table`. Table function.

Profiles each column of a table and returns one row per column.

```text
profile_table(
    table_name VARCHAR
    [, columns     VARCHAR[]
    [, sample_size BIGINT
    [, exact       BOOLEAN ]]]
) -> TABLE(column_name VARCHAR, column_type VARCHAR, ... 27 columns)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view to profile |
| `columns` | `VARCHAR[]` | all columns | Columns to profile. `NULL` keeps the default |
| `sample_size` | `BIGINT` | `1000000` | Profile a sample of this many rows when the table is larger. Must be positive. `NULL` keeps the default |
| `exact` | `BOOLEAN` | `false` | `true` scans every row even when the table is larger than `sample_size` |

Output columns:

| Column | Type | Description |
|--------|------|-------------|
| `column_name` | `VARCHAR` | Column name |
| `column_type` | `VARCHAR` | DuckDB type of the column |
| `row_count` | `BIGINT` | Rows scanned |
| `null_count` | `BIGINT` | `NULL` values |
| `null_rate` | `DOUBLE` | `null_count / row_count` |
| `distinct_count` | `BIGINT` | Distinct non-`NULL` values |
| `distinct_rate` | `DOUBLE` | `distinct_count / row_count` |
| `min_val` | `VARCHAR` | Minimum as text; `NULL` for `STRUCT`, `MAP` and `UNION` |
| `max_val` | `VARCHAR` | Maximum as text; `NULL` for `STRUCT`, `MAP` and `UNION` |
| `mean` | `DOUBLE` | Numeric columns only |
| `median` | `DOUBLE` | Numeric columns only |
| `stddev` | `DOUBLE` | Numeric columns only |
| `p25` | `DOUBLE` | 25th percentile, numeric columns only |
| `p75` | `DOUBLE` | 75th percentile, numeric columns only |
| `skewness` | `DOUBLE` | Numeric columns only |
| `kurtosis` | `DOUBLE` | Numeric columns only |
| `top_values` | `STRUCT(value VARCHAR, count BIGINT)[]` | The 5 most frequent values, most frequent first. The order of values with equal counts is not fixed |
| `avg_length` | `DOUBLE` | String length; element count for lists; cardinality for maps |
| `min_length` | `BIGINT` | As `avg_length` |
| `max_length` | `BIGINT` | As `avg_length` |
| `pattern_summary` | `VARCHAR` | Detected pattern, see below; `NULL` for numeric, boolean and temporal columns |
| `is_unique` | `BOOLEAN` | All non-`NULL` values are distinct |
| `is_constant` | `BOOLEAN` | All non-`NULL` values are equal |
| `zero_count` | `BIGINT` | Zero values, numeric columns only |
| `negative_count` | `BIGINT` | Negative values, numeric columns only |
| `is_sampled` | `BOOLEAN` | `true` when a sample was profiled |
| `actual_sample_size` | `BIGINT` | Rows actually scanned |

`pattern_summary` for string columns is the first of `email`, `phone`, `uuid`, `url`,
`ip_address`, `iso_date` and `numeric_string` that at least 80% of the non-`NULL` values match,
else `mixed`. Phone detection uses `phonenumber_is_valid` with region `US`. Complex columns get
`list`, `map`, `struct` or `union`.

```sql
SELECT column_name, column_type, null_count, distinct_count,
       round(mean, 2) AS mean, pattern_summary
FROM profile_table('orders')
ORDER BY column_name;
```

```text
┌─────────────┬───────────────┬────────────┬────────────────┬────────┬─────────────────┐
│ column_name │  column_type  │ null_count │ distinct_count │  mean  │ pattern_summary │
│   varchar   │    varchar    │   int64    │     int64      │ double │     varchar     │
├─────────────┼───────────────┼────────────┼────────────────┼────────┼─────────────────┤
│ amount      │ DECIMAL(22,1) │         20 │             13 │  24.74 │ NULL            │
│ color       │ VARCHAR       │          0 │              3 │   NULL │ mixed           │
│ email       │ VARCHAR       │          0 │              7 │   NULL │ email           │
│ order_date  │ DATE          │          0 │             30 │   NULL │ NULL            │
│ order_id    │ BIGINT        │          0 │            200 │  100.5 │ NULL            │
│ quantity    │ BIGINT        │          0 │              5 │    3.0 │ NULL            │
└─────────────┴───────────────┴────────────┴────────────────┴────────┴─────────────────┘
```

Profile selected columns. `top_values` lists the most frequent values with their counts:

```sql
SELECT column_name, top_values, is_unique, is_constant
FROM profile_table('orders', ['color']);
```

```text
┌─────────────┬─────────────────────────────────────────────────────────────────────────────────────────────┬───────────┬─────────────┐
│ column_name │                                         top_values                                          │ is_unique │ is_constant │
│   varchar   │                           struct("value" varchar, count bigint)[]                           │  boolean  │   boolean   │
├─────────────┼─────────────────────────────────────────────────────────────────────────────────────────────┼───────────┼─────────────┤
│ color       │ [{'value': red, 'count': 100}, {'value': green, 'count': 60}, {'value': blue, 'count': 40}] │ false     │ false       │
└─────────────┴─────────────────────────────────────────────────────────────────────────────────────────────┴───────────┴─────────────┘
```

With `exact = true` every row is scanned, whatever `sample_size` says:

```sql
SELECT column_name, row_count, is_sampled, actual_sample_size
FROM profile_table('orders', ['amount'], 50, true);
```

```text
┌─────────────┬───────────┬────────────┬────────────────────┐
│ column_name │ row_count │ is_sampled │ actual_sample_size │
│   varchar   │   int64   │  boolean   │       int64        │
├─────────────┼───────────┼────────────┼────────────────────┤
│ amount      │       200 │ false      │                200 │
└─────────────┴───────────┴────────────┴────────────────────┘
```

> **Known issue:** when the table has more rows than `sample_size` and `exact` is `false`, the
> generated sampling query fails with a parser error (`syntax error at or near "REPEATABLE"`).
> Until this is fixed, pass `exact = true` or a `sample_size` above the row count. The default
> `sample_size` of 1,000,000 hits this only on larger tables.

```sql
SELECT column_name, is_sampled FROM profile_table('orders', ['amount'], 50);
```

```text
Invalid Input Error: profile_table query failed: Parser Error: syntax error at or near "REPEATABLE"

LINE 1: ... AS (SELECT * FROM query_table('orders') USING SAMPLE 50 ROWS REPEATABLE (42))SELECT 'amount' AS column_name, 'DECIMAL...
                                                                         ^
-> Unexpected? Please report it: https://github.com/DataZooDE/anofox-tabular/issues
```

---

## `profile_summary`

Canonical name: `anofox_tab_profile_summary`. Table function.

Returns one row that describes the whole table.

```text
profile_summary(table_name VARCHAR) -> TABLE(... 11 columns)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |

Output columns:

| Column | Type | Description |
|--------|------|-------------|
| `row_count` | `BIGINT` | Rows in the table |
| `column_count` | `BIGINT` | Columns in the table |
| `numeric_columns` | `BIGINT` | Numeric columns |
| `string_columns` | `BIGINT` | String columns |
| `temporal_columns` | `BIGINT` | Date and time columns |
| `boolean_columns` | `BIGINT` | Boolean columns |
| `complex_columns` | `BIGINT` | `LIST`, `MAP`, `STRUCT` and `UNION` columns |
| `total_nulls` | `BIGINT` | `NULL` cells over all columns |
| `total_null_rate` | `DOUBLE` | `total_nulls / (row_count * column_count)` |
| `duplicate_row_count` | `BIGINT` | Rows that duplicate another row |
| `estimated_memory_bytes` | `BIGINT` | A rough estimate: `row_count * column_count * 8` |

```sql
SELECT row_count, column_count, numeric_columns, string_columns, temporal_columns,
       total_nulls, round(total_null_rate, 4) AS total_null_rate, duplicate_row_count
FROM profile_summary('orders');
```

```text
┌───────────┬──────────────┬─────────────────┬────────────────┬──────────────────┬─────────────┬─────────────────┬─────────────────────┐
│ row_count │ column_count │ numeric_columns │ string_columns │ temporal_columns │ total_nulls │ total_null_rate │ duplicate_row_count │
│   int64   │    int64     │      int64      │     int64      │      int64       │    int64    │     double      │        int64        │
├───────────┼──────────────┼─────────────────┼────────────────┼──────────────────┼─────────────┼─────────────────┼─────────────────────┤
│       200 │            6 │               3 │              2 │                1 │          20 │          0.0167 │                   0 │
└───────────┴──────────────┴─────────────────┴────────────────┴──────────────────┴─────────────┴─────────────────┴─────────────────────┘
```

---

## `profile_correlations`

Canonical name: `anofox_tab_profile_correlations`. Table function.

Computes the Pearson and Spearman correlation of every pair of numeric columns. Each pair appears
once.

```text
profile_correlations(table_name VARCHAR [, columns VARCHAR[]])
    -> TABLE(column_a VARCHAR, column_b VARCHAR, pearson DOUBLE, spearman DOUBLE, n BIGINT)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `table_name` | `VARCHAR` | required | Table or view |
| `columns` | `VARCHAR[]` | all numeric columns | Columns to correlate |

| Column | Type | Description |
|--------|------|-------------|
| `column_a` | `VARCHAR` | First column of the pair |
| `column_b` | `VARCHAR` | Second column of the pair |
| `pearson` | `DOUBLE` | Pearson correlation, -1 to 1 |
| `spearman` | `DOUBLE` | Spearman rank correlation, -1 to 1 |
| `n` | `BIGINT` | Rows where both values are non-`NULL` |

The function is tagged as a `matrix` producer of the
[integration contract](11-integration-contract.md).

```sql
SELECT column_a, column_b, round(pearson, 3) AS pearson, round(spearman, 3) AS spearman, n
FROM profile_correlations('orders')
ORDER BY column_a, column_b;
```

```text
┌──────────┬──────────┬─────────┬──────────┬───────┐
│ column_a │ column_b │ pearson │ spearman │   n   │
│ varchar  │ varchar  │ double  │  double  │ int64 │
├──────────┼──────────┼─────────┼──────────┼───────┤
│ amount   │ quantity │   0.014 │    0.014 │   180 │
│ order_id │ amount   │    0.01 │     0.01 │   180 │
│ order_id │ quantity │     0.0 │      0.0 │   200 │
└──────────┴──────────┴─────────┴──────────┴───────┘
```

```sql
SELECT column_a, column_b, round(pearson, 3) AS pearson
FROM profile_correlations('orders', ['amount', 'quantity']);
```

```text
┌──────────┬──────────┬─────────┐
│ column_a │ column_b │ pearson │
│ varchar  │ varchar  │ double  │
├──────────┼──────────┼─────────┤
│ amount   │ quantity │   0.014 │
└──────────┴──────────┴─────────┘
```

## See also

- [Data quality checks](07-data-quality.md)
- [Integration contract](11-integration-contract.md)
- [Conventions](00-conventions.md)

## Summary
The diff module is small and mostly implemented as a `bind_replace` SQL generator rather than a native vectorized table function. That keeps execution inside DuckDB’s optimizer, but the generated SQL has correctness holes around nullable keys and identifier handling. The module also contains a substantial unused native table-function path, which makes the implementation look more complete than it is. `src/include/anofox_diff.hpp` is minimal and has no notable issues.

## Findings

### [HIGH] Nullable primary keys are misclassified and can duplicate rows
Affected: `src/anofox_diff.cpp:228`, `src/anofox_diff.cpp:254`, `src/anofox_diff.cpp:255`, `src/anofox_diff.cpp:237`

The generated join uses `s.pk = t.pk`, and row presence is inferred from `s.<first_pk> IS NULL` / `t.<first_pk> IS NULL`. If a primary key column is nullable, matching NULL keys do not join, and source-only rows with NULL keys are classified as `added` because the first `CASE` branch checks `s.pk IS NULL`. This also affects compound keys where the first key is NULL even if the row exists on both sides.

Suggested improvement: either reject nullable/NULL primary key values explicitly, or join with NULL-safe equality and track side presence separately.

```sql
FROM (SELECT true AS _s_present, * FROM source) s
FULL OUTER JOIN (SELECT true AS _t_present, * FROM target) t
ON s.pk IS NOT DISTINCT FROM t.pk

CASE
  WHEN s._s_present IS NULL THEN 'added'
  WHEN t._t_present IS NULL THEN 'removed'
  WHEN ... THEN 'changed'
  ELSE 'unchanged'
END
```

### [MEDIUM] Column identifiers are concatenated without quoting
Affected: `src/anofox_diff.cpp:228`, `src/anofox_diff.cpp:237`, `src/anofox_diff.cpp:254`, `src/anofox_diff.cpp:255`, `src/anofox_diff.cpp:269`, `src/anofox_diff.cpp:278`

Primary key and compare column names are inserted directly into SQL. Columns named `select`, containing spaces, quotes, dots, or other special characters fail to parse or bind, and maliciously crafted strings can alter the generated expression. The table names get parsed through `QualifiedName`, but columns do not receive equivalent identifier serialization.

Suggested improvement: quote every identifier component with DuckDB’s `KeywordHelper::WriteOptionallyQuoted`.

```cpp
#include "duckdb/parser/keyword_helper.hpp"

static string Q(const string &identifier) {
    return KeywordHelper::WriteOptionallyQuoted(identifier);
}

pk_join_condition += "s." + Q(primary_keys[i]) + " IS NOT DISTINCT FROM t." + Q(primary_keys[i]);
pk_select += "COALESCE(s." + Q(pk) + ", t." + Q(pk) + ") AS " + Q(pk);
```

### [MEDIUM] No schema validation before generating the diff query
Affected: `src/anofox_diff.cpp:82`, `src/anofox_diff.cpp:97`, `src/anofox_diff.cpp:259`, `src/anofox_diff.cpp:261`, `src/anofox_diff.cpp:277`

`ValidatePrimaryKeys` and `ValidateCompareColumns` exist but are never called. As a result, missing compare columns surface as binder errors from generated SQL, while mismatched source/target schemas can silently produce surprising results because `s IS DISTINCT FROM t` compares whole row structs and the output always projects `t.* EXCLUDE (...)`. This is especially risky when the source has columns the target lacks, or the same column names have incompatible types.

Suggested improvement: during bind, inspect both table schemas and validate that primary keys exist on both sides, compare columns exist on both sides, and the default “compare all” set is explicitly computed as the shared non-PK columns. Generate comparisons column-by-column instead of relying on whole-row struct comparison.

```cpp
// Pseudocode
auto columns = ResolveComparableColumns(context, source_table, target_table, primary_keys, compare_columns);
for (auto &col : columns) {
    diff_predicates.push_back("s." + Q(col) + " IS DISTINCT FROM t." + Q(col));
}
```

### [MEDIUM] `diff_hashdiff` accepts tuning parameters but ignores them
Affected: `src/anofox_diff.cpp:382`, `src/anofox_diff.cpp:388`, `src/anofox_diff.cpp:521`, `src/anofox_diff.cpp:533`

`diff_hashdiff` registers overloads with `bisection_threshold` and `bisection_factor`, but `HashDiffBindReplace` discards both values and runs the same full outer join as `diff_joindiff`. This is a correctness and performance contract issue: callers can reasonably expect those parameters to change the algorithm, memory profile, or result shape. For large tables, the name implies a hash/bisection strategy but the implementation always materializes a join plan.

Suggested improvement: either implement the hash/bisection behavior, or remove/rename the overloads until supported. If retained as a compatibility shim, emit a clear binder error or warning rather than silently ignoring the parameters.

### [LOW] Dead native table-function implementation is misleading
Affected: `src/anofox_diff.cpp:22`, `src/anofox_diff.cpp:30`, `src/anofox_diff.cpp:55`, `src/anofox_diff.cpp:115`, `src/anofox_diff.cpp:162`, `src/anofox_diff.cpp:189`

The native bind/local state/execution/finalize path is not registered, and the execution function would return an empty result set if it ever were used. The unused diff-type constants and validation helpers add to the impression that there is a second implementation path. This increases maintenance risk because future changes may accidentally wire up a stub.

Suggested improvement: delete the unused native table-function scaffolding, or complete it and register it intentionally. Keeping only the `bind_replace` implementation would make the module’s actual behavior much easier to review.

### [LOW] Registration code duplicates alias plumbing instead of using the project helper
Affected: `src/anofox_diff.cpp:400`, `src/anofox_diff.cpp:484`, `src/anofox_diff.cpp:507`, `src/anofox_diff.cpp:561`

The module manually copies `TableFunctionSet` entries into alias sets even though `anofox_function_alias.hpp` already provides `RegisterTableFunctionSetWithAlias`. The manual copy path is easy to forget when new fields are added to `TableFunction`, and it duplicates a project-level abstraction already intended for this purpose.

Suggested improvement: build the function sets, then register them with `RegisterTableFunctionSetWithAlias(loader, joindiff_set, "diff_joindiff")` and the equivalent hashdiff call.

## Quick wins
- Quote all primary key and compare column identifiers with `KeywordHelper::WriteOptionallyQuoted`.
- Replace PK-null row presence checks with side-presence marker columns.
- Decide whether NULL primary keys are supported; if not, fail with a clear binder/runtime error.
- Remove the unused native table-function scaffolding or add a comment stating that the module is SQL-rewrite-only.
- Call or remove the unused validation helpers.
- Make `diff_hashdiff` parameters effective, or reject them until the algorithm exists.
## Summary
The metric module has useful coverage for simple SQL-rewritten checks and a C++ isolation forest path, but it is currently fragile around SQL construction, edge cases, and API consistency. The highest-risk issue is that user-provided table and column names are interpolated directly into SQL without identifier/literal escaping, which breaks valid names and can change the generated query. DBSCAN metric functions are especially concerning because they do not call the existing DBSCAN implementation and instead return placeholder results. The isolation forest path is functionally more complete, but it materializes all input rows and scores in memory and leaves several validation and API-default paths inconsistent with the advertised interface.

## Findings

### [HIGH] User input is interpolated into generated SQL without escaping
Affected: `src/anofox_metric.cpp:56`, `src/anofox_metric.cpp:71`, `src/anofox_metric.cpp:98`, `src/anofox_metric.cpp:111`, `src/anofox_metric.cpp:153`, `src/anofox_metric.cpp:195`, `src/anofox_metric.cpp:215`, `src/anofox_metric.cpp:221`, `src/anofox_metric.cpp:255`, `src/anofox_metric.cpp:653`, `src/anofox_metric.cpp:699`, `src/anofox_metric.cpp:1265`

Table names, column names, schema-check required columns, interval/timestamp strings, and `query_table('...')` arguments are assembled by string concatenation. A table name containing `'`, a column name containing `"`, or a required column containing `'` will either fail to parse or change the generated SQL. The same issue applies to schema checks, where `table_name` is also embedded as a string literal and not schema-qualified.

Suggested improvement: centralize SQL quoting helpers and use them everywhere generated SQL is unavoidable.

```cpp
static string QuoteLiteral(const string &s) {
	return "'" + StringUtil::Replace(s, "'", "''") + "'";
}

static string QuoteIdentifier(const string &s) {
	return "\"" + StringUtil::Replace(s, "\"", "\"\"") + "\"";
}

static string QueryTableRef(const string &table_name) {
	return "query_table(" + QuoteLiteral(table_name) + ")";
}
```

Then build column references as `QuoteIdentifier(column_name)` and list literals as `QuoteLiteral(required_col)` rather than manually adding quotes.

### [HIGH] DBSCAN functions return placeholder SQL instead of DBSCAN results
Affected: `src/anofox_metric.cpp:1242`, `src/anofox_metric.cpp:1267`, `src/anofox_metric.cpp:1273`, `src/anofox_metric.cpp:1276`, `src/anofox_metric.cpp:1290`, `src/anofox_metric.cpp:1337`, `src/anofox_metric.cpp:1343`, `src/anofox_metric.cpp:1345`

`anofox_tab_dbscan` and `anofox_tab_dbscan_mv` include `anofox_dbscan.hpp`, but the registered functions are `bind_replace` SQL stubs. Univariate clusters always return `cluster_id = 0`, `point_type = 'CORE'`, fixed neighbor/anomaly values, and summary mode derives `cluster_count` from `COUNT(DISTINCT value)` plus a hard-coded noise predicate. Multivariate summary is even more detached from input data, returning constants like `cluster_count = 3`, `noise_count = 1`, and `total_count = 10`.

Suggested improvement: implement DBSCAN as a real table function, analogous to `IsolationForestExecute`, using `DBSCAN::Fit`, `GetResults`, `GetClusterCount`, `GetNoiseCount`, `GetLargestClusterSize`, and `ComputeAnomalyScores`. Until then, do not expose these functions as production metrics or rename them clearly as test/demo placeholders.

### [MEDIUM] Optional defaults are coded but not actually registered
Affected: `src/anofox_metric.cpp:414`, `src/anofox_metric.cpp:508`, `src/anofox_metric.cpp:1155`, `src/anofox_metric.cpp:1217`, `src/anofox_metric.cpp:1434`, `src/anofox_metric.cpp:1439`, `src/anofox_metric.cpp:1467`, `src/anofox_metric.cpp:1472`, `src/anofox_metric.cpp:1569`

The bind functions contain defaults and error messages say isolation forest requires “at least 2 arguments”, while comments advertise calls like `n_trees=100`, `sample_size=256`, and so on. Registration only adds fixed 6- and 7-argument overloads for univariate isolation forest, fixed 6- through 13-argument overloads for multivariate isolation forest, and a fixed 5-argument overload for DBSCAN. Calls such as `isolation_forest('t', 'x')` or `dbscan('t', 'x')` will not reach the bind functions despite defaults being present.

Suggested improvement: either register all intended arities or remove the unreachable defaulting code/comments. Prefer one consistent mechanism, for example table function sets covering 2..7 arities for isolation forest and 2..5 arities for DBSCAN.

### [MEDIUM] Empty and degenerate aggregate inputs produce inconsistent or missing metric rows
Affected: `src/anofox_metric.cpp:70`, `src/anofox_metric.cpp:73`, `src/anofox_metric.cpp:109`, `src/anofox_metric.cpp:114`, `src/anofox_metric.cpp:121`, `src/anofox_metric.cpp:130`, `src/anofox_metric.cpp:151`, `src/anofox_metric.cpp:229`

Several SQL metrics divide by `COUNT(*)` or rely on cross joins with derived data without guarding zero-row or zero-variance cases. `null_rate` on an empty table computes `0 / 0`, yielding a NULL rate and likely a misleading fail/message. `zscore` can return no row at all for an empty/non-null-free input because `stats` is cross joined with an empty `with_zscore`; for a single value or constant column, `STDDEV` is NULL/zero and z-score expressions become NULL/undefined. `freshness` on an empty table produces NULL timestamps and a NULL message.

Suggested improvement: make every metric return exactly one row with explicit empty-input semantics. Use `NULLIF(total_count, 0)`, `COALESCE`, and special-case `stddev IS NULL OR stddev = 0`.

```sql
CASE
  WHEN total_count = 0 THEN 'fail'
  WHEN stddev IS NULL OR stddev = 0 THEN 'pass'
  WHEN outlier_count = 0 THEN 'pass'
  ELSE 'fail'
END
```

### [MEDIUM] Numeric parameter validation misses NaN and signed conversion edge cases
Affected: `src/anofox_metric.cpp:415`, `src/anofox_metric.cpp:416`, `src/anofox_metric.cpp:417`, `src/anofox_metric.cpp:424`, `src/anofox_metric.cpp:509`, `src/anofox_metric.cpp:510`, `src/anofox_metric.cpp:511`, `src/anofox_metric.cpp:518`, `src/anofox_metric.cpp:581`, `src/anofox_metric.cpp:1253`, `src/anofox_metric.cpp:1254`, `src/anofox_metric.cpp:1324`, `src/anofox_metric.cpp:1325`

Several signed `BIGINT` inputs are read directly into `size_t`, so negative values wrap before validation. Some cases are later caught by upper-bound checks, but `min_pts = -1` becomes a huge `size_t` and passes `min_pts < 1`. Floating-point checks also do not reject `NaN`: comparisons like `eps <= 0.0` and `contamination > 0.5` are false for NaN, allowing invalid parameters into SQL generation or algorithm code.

Suggested improvement: read integer parameters into `int64_t`, validate signed ranges first, then cast. Reject non-finite doubles explicitly.

```cpp
auto min_pts_i = input.inputs[3].GetValue<int64_t>();
if (min_pts_i < 1) {
	throw BinderException("min_pts must be >= 1");
}
auto min_pts = static_cast<size_t>(min_pts_i);

if (!std::isfinite(eps) || eps <= 0.0) {
	throw BinderException("eps must be finite and > 0.0");
}
```

### [MEDIUM] Isolation forest materializes all data and results before producing output
Affected: `src/anofox_metric.cpp:701`, `src/anofox_metric.cpp:743`, `src/anofox_metric.cpp:801`, `src/anofox_metric.cpp:808`, `src/anofox_metric.cpp:828`, `src/anofox_metric.cpp:866`, `src/anofox_metric.cpp:872`

The isolation forest table function fetches the entire source query, copies each cell through `DataChunk::GetValue`, stores the full feature matrix, computes a full scores vector, and then stores a second full `state.results` vector. This is costly for large tables and especially expensive for categorical data due to repeated `Value` materialization and `ToString()` conversions. The algorithm may require a batch fit, but output does not need to duplicate every score in a separate result struct.

Suggested improvement: reserve approximate storage where possible, avoid per-cell `GetValue` in favor of vector access APIs, and for scores mode stream directly from the scores/data arrays after fitting instead of copying into `state.results`. If full materialization is unavoidable, document it and consider an explicit input-size guard or memory error with a clear message.

### [LOW] Invalid `output_mode` silently falls back to summary
Affected: `src/anofox_metric.cpp:440`, `src/anofox_metric.cpp:599`, `src/anofox_metric.cpp:891`, `src/anofox_metric.cpp:1173`, `src/anofox_metric.cpp:1235`, `src/anofox_metric.cpp:1268`, `src/anofox_metric.cpp:1338`

Any `output_mode` other than exactly `"scores"` for isolation forest or `"clusters"` for DBSCAN is treated as summary. Typos like `'score'`, `'cluster'`, or casing differences are accepted and change behavior silently.

Suggested improvement: normalize the mode once and reject unknown values with a binder error. For isolation forest: allow only `"summary"` and `"scores"`. For DBSCAN: allow only `"summary"` and `"clusters"`.

### [LOW] Dead legacy isolation-forest SQL code remains in the module
Affected: `src/anofox_metric.cpp:944`, `src/anofox_metric.cpp:948`, `src/anofox_metric.cpp:986`, `src/anofox_metric.cpp:1022`, `src/anofox_metric.cpp:1085`, `src/anofox_metric.cpp:1147`, `src/anofox_metric.cpp:1181`

The file keeps deprecated SQL-generation functions and bind-replace functions for isolation forest, but registration uses the C++ table functions at `src/anofox_metric.cpp:1437` and `src/anofox_metric.cpp:1470`. The stale code includes placeholder anomaly logic and duplicates parsing/validation paths, which makes the module harder to review and increases the chance a future registration change accidentally exposes incorrect behavior.

Suggested improvement: remove the unused legacy functions or move them to tests/docs if they are intentionally retained as historical reference.

## Quick wins
- Add `QuoteLiteral`, `QuoteIdentifier`, and `QueryTableRef` helpers and replace all hand-built quote fragments.
- Validate `output_mode` explicitly in every bind path.
- Read signed integer parameters into `int64_t`, validate, then cast to `size_t`.
- Reject `NaN` and infinite values for `eps`, `contamination`, thresholds, and probabilities.
- Register the advertised shorter overloads or remove the unreachable defaulting code and comments.
- Add sqllogictests for quoted identifiers, apostrophes in names, empty tables, all-NULL columns, constant z-score input, `NaN` parameters, negative `min_pts`, and invalid output modes.
- Replace DBSCAN placeholder SQL with a real table-function implementation or temporarily mark the exposed functions as unsupported.
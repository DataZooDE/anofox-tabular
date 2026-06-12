## Summary
The outlier module has useful algorithm coverage, but several correctness issues can produce wrong clusters, wrong row ids, duplicate outlier rows, or stale isolation-forest models. The biggest risks are in DBSCAN’s cluster expansion semantics and the table-function SQL/data-loading path for `outlier_tree`. The implementations also rely heavily on full materialization, `Value` extraction, repeated vector allocations, and repeated statistics recomputation, which will not scale well on larger DuckDB tables. There are also a few places where unchecked casts or narrow integer storage can turn valid SQL inputs or wide data into undefined or misleading behavior.

## Findings

### [HIGH] DBSCAN misclassifies core and border points
Affected file:line references: `src/anofox_dbscan.cpp:50`, `src/anofox_dbscan.cpp:62`, `src/anofox_dbscan.cpp:97`, `src/anofox_dbscan.cpp:105`, `src/anofox_dbscan.cpp:161`

`RegionQuery` excludes the point itself, but `Fit` and `ExpandCluster` compare `neighbors.size()` directly to `min_pts_`. Standard DBSCAN defines `minPts` as including the query point, so this implementation is effectively one point stricter. More importantly, `ExpandCluster` only assigns points when `!visited[current_idx]`; a point previously marked `NOISE` can later be reached from a core point but will be skipped and remain noise instead of becoming a border point.

Suggested improvement: either include `point_idx` in `RegionQuery`, or compare `neighbors.size() + 1 >= min_pts_`, and always attach reachable noise/unassigned points to the cluster before the visited check.

```cpp
if (results_[current_idx].cluster_id == -1) {
    results_[current_idx].cluster_id = cluster_id;
}
if (visited[current_idx]) {
    if (point_types[current_idx] == PointType::NOISE) {
        point_types[current_idx] = PointType::BORDER;
        results_[current_idx].point_type = PointType::BORDER;
    }
    continue;
}
```

### [HIGH] `outlier_tree` builds SQL by concatenating unescaped user input
Affected file:line references: `src/anofox_outlier_tree.cpp:795`, `src/anofox_outlier_tree.cpp:807`, `src/anofox_outlier_tree.cpp:909`, `src/anofox_outlier_tree.cpp:912`

The table name and column names are interpolated directly into a SQL string. Double quotes in column names and single quotes in table names are not escaped, so valid DuckDB identifiers can fail, and malicious input can alter the generated query. This is especially risky because the function opens a new `Connection` and runs the generated query at execution time.

Suggested improvement: avoid query construction where possible by using `bind_replace` and DuckDB’s parser/binder structures. If this function must build SQL, centralize identifier/literal quoting and escape embedded quotes.

```cpp
static string QuoteIdentifier(const string &s) {
    return "\"" + StringUtil::Replace(s, "\"", "\"\"") + "\"";
}
static string QuoteLiteral(const string &s) {
    return "'" + StringUtil::Replace(s, "'", "''") + "'";
}
```

### [HIGH] Negative integer parameters wrap to huge `size_t` values before validation
Affected file:line references: `src/anofox_outlier_tree.cpp:823`, `src/anofox_outlier_tree.cpp:825`, `src/anofox_outlier_tree.cpp:826`, `src/include/anofox_outlier_tree.hpp:145`

`max_depth`, `min_size_numeric`, and `min_size_categ` are read as `int64_t` but immediately assigned to `size_t`. A negative SQL value such as `-1` becomes a very large unsigned value before the `max_depth < 1` validation runs, so it passes validation and can cause excessive recursion limits or make minimum-size checks impossible to satisfy.

Suggested improvement: validate signed temporaries first, then cast.

```cpp
auto depth_i = input.inputs[3].GetValue<int64_t>();
if (depth_i < 1 || depth_i > 1024) {
    throw BinderException("max_depth must be between 1 and 1024");
}
auto max_depth = static_cast<size_t>(depth_i);
```

### [MEDIUM] Reported `row_id` values do not match source table rows after NULL filtering
Affected file:line references: `src/anofox_outlier_tree.cpp:941`, `src/anofox_outlier_tree.cpp:953`, `src/anofox_outlier_tree.cpp:970`, `src/anofox_outlier_tree.cpp:990`

Rows containing any NULL are skipped during materialization, and `OutlierExplanation::row_idx` is based on the compacted, filtered dataset. The output then reports `row_idx + 1`, which is not the original source row number whenever earlier rows were skipped. `total_rows` also reports the filtered row count, not the scanned table count, which may surprise users.

Suggested improvement: preserve source row ids while scanning and store/report those ids in `OutlierExplanation`, or make the output column explicitly named as a filtered-row ordinal. Prefer adding `row_number() over ()` to the source query and carrying that through the model input.

### [MEDIUM] Isolation trees append to old nodes when models are refit
Affected file:line references: `src/anofox_isolation_forest.cpp:385`, `src/anofox_isolation_forest.cpp:517`, `src/anofox_isolation_forest.cpp:955`, `src/anofox_isolation_forest.cpp:1176`, `src/include/anofox_isolation_forest.hpp:171`

`IsolationTree::BuildTree` and `BuildTreeMixed` append nodes without clearing `nodes_`, and `root_idx_` remains `0`. `IsolationForest::Fit`/`FitMixed` use `trees_.resize(n_trees_)`, which reuses existing `IsolationTree` instances when fitting the same forest object again. A second fit can therefore score through the stale first tree rooted at node 0.

Suggested improvement: clear tree state at the start of each build, or make `Fit` replace `trees_` with fresh instances.

```cpp
void IsolationTree::BuildTree(...) {
    nodes_.clear();
    root_idx_ = 0;
    BuildTreeRecursive(...);
}
```

### [MEDIUM] Categorical right-branch explanations are logically wrong
Affected file:line references: `src/anofox_outlier_tree.cpp:154`, `src/anofox_outlier_tree.cpp:157`, `src/include/anofox_outlier_tree.hpp:40`, `src/include/anofox_outlier_tree.hpp:56`

For numeric splits, the right branch flips `is_less_than`; for categorical splits, the right branch keeps the same `left_categories` and has no negation flag. As a result, outliers found in the right branch can be explained as `column IN (...)` even though that branch actually means `column NOT IN (...)`.

Suggested improvement: add an operator/negation field to `SplitCondition` for categorical conditions and use it in `ToString`, `ToJSON`, and right-branch construction.

### [MEDIUM] Duplicate outlier suppression does not replace worse existing results
Affected file:line references: `src/anofox_outlier_tree.cpp:239`, `src/anofox_outlier_tree.cpp:314`, `src/anofox_outlier_tree.cpp:736`

`ShouldSkipDuplicateOutlier` returns `false` when the new score is more extreme than an existing row/column result, but it does not remove or update the existing result. The caller then appends the new outlier, leaving duplicate row/column findings and inflating summary counts.

Suggested improvement: return the existing index and replace it when the new score is better, or keep a map keyed by `(row_idx, target_col)`.

```cpp
std::unordered_map<RowColKey, size_t> best;
```

### [MEDIUM] JSON output is assembled without escaping strings
Affected file:line references: `src/include/anofox_outlier_tree.hpp:56`, `src/include/anofox_outlier_tree.hpp:58`, `src/include/anofox_outlier_tree.hpp:66`, `src/include/anofox_outlier_tree.hpp:120`

`SplitCondition::ToJSON` writes column names and category names directly into JSON string literals. Quotes, backslashes, control characters, and newlines will produce malformed JSON and can change the apparent structure of the `conditions` field.

Suggested improvement: use DuckDB’s JSON utilities if available in this extension, or add a small JSON string escaper and apply it to every string value.

### [MEDIUM] Isolation forest accepts inconsistent shapes and can read out of bounds
Affected file:line references: `src/anofox_isolation_forest.cpp:393`, `src/anofox_isolation_forest.cpp:415`, `src/anofox_isolation_forest.cpp:499`, `src/anofox_isolation_forest.cpp:1126`, `src/anofox_isolation_forest.cpp:1149`, `src/anofox_isolation_forest.cpp:1278`

`Fit` assumes `data[0]` has at least one feature and all rows have the same width; `Score` assumes the point has all trained features. `FitMixed` assumes `column_info.size() == data.size()` and all `ColumnData` columns have the same row count. Violating these assumptions can underflow `uniform_int_distribution(0, n_features - 1)` or read past vector bounds.

Suggested improvement: validate inputs at public API boundaries and throw `InvalidInputException`/`std::invalid_argument` before training or scoring.

### [MEDIUM] `outlier_tree` materializes and processes data row-by-row through `Value`
Affected file:line references: `src/anofox_outlier_tree.cpp:915`, `src/anofox_outlier_tree.cpp:938`, `src/anofox_outlier_tree.cpp:946`, `src/anofox_outlier_tree.cpp:957`, `src/anofox_outlier_tree.cpp:990`

The execution path fetches the full source table through a nested `Connection`, calls `DataChunk::GetValue` for every cell, builds full in-memory `std::vector` columns, and writes output with `SetValue` per cell. This loses most of DuckDB’s vectorized execution benefits and creates many temporary `Value` and `std::string` objects. Large tables will be memory-heavy and slow before the algorithm itself runs.

Suggested improvement: use `UnifiedVectorFormat`/flat vector access while reading chunks, reserve column storage when row counts are known, and write outputs through flat vector buffers. If the function remains whole-table by design, expose or enforce row limits and document the full materialization behavior.

### [LOW] DBSCAN and isolation forest use narrow integer storage for feature/depth metadata
Affected file:line references: `src/include/anofox_isolation_forest.hpp:108`, `src/include/anofox_isolation_forest.hpp:113`, `src/include/anofox_isolation_forest.hpp:117`, `src/anofox_isolation_forest.cpp:354`, `src/anofox_isolation_forest.cpp:358`, `src/anofox_isolation_forest.cpp:641`

Feature indices and depths are stored as `uint16_t`, and hyperplane dimensionality is stored as `uint8_t`. Wide tables or high `ndim` values can silently truncate metadata and send scoring down the wrong feature path. Even if current SQL overloads make this unlikely, the C++ API itself does not enforce those limits.

Suggested improvement: use `size_t`/`idx_t` for feature indices and dimensions, or validate before narrowing and throw a clear error.

### [LOW] Repeated allocations and recomputation make tree building unnecessarily expensive
Affected file:line references: `src/anofox_outlier_tree.cpp:120`, `src/anofox_outlier_tree.cpp:125`, `src/anofox_outlier_tree.cpp:382`, `src/anofox_outlier_tree.cpp:398`, `src/anofox_isolation_forest.cpp:112`, `src/anofox_isolation_forest.cpp:139`, `src/anofox_dbscan.cpp:50`

Several hot paths repeatedly allocate temporary vectors and recompute the same statistics: OutlierTree recomputes current variance/entropy for each candidate split, partitioning is performed once to score a split and again to recurse, isolation split candidates rebuild value vectors for parent/children, and DBSCAN allocates a new neighbor vector for every region query. This is not a correctness bug, but it raises CPU and allocator pressure substantially.

Suggested improvement: cache current cluster statistics, return partition results with the chosen split, reuse scratch buffers, and consider a spatial index or distance-cache strategy for DBSCAN when inputs are large.

## Quick wins
- Validate all `Value` inputs for NULL before calling `ToString()`/`GetValue<T>()` in bind functions.
- Validate `output_mode` explicitly instead of treating unknown modes as summary.
- Preserve original row numbers through NULL filtering for all table outputs.
- Add a categorical negation flag to `SplitCondition`.
- Escape strings in `ToJSON`.
- Clear `IsolationTree::nodes_` before every tree build.
- Add shape checks for row width, column count, and mixed-column lengths.
- Replace signed-to-unsigned parameter parsing with signed validation followed by casting.
- Reserve `OutlierTree` column vectors once a row estimate is known.
- Add focused regression tests for DBSCAN noise-to-border promotion and `min_pts` semantics.
# Updating DuckDB

anofox-tabular builds against DuckDB's C++ extension API, which is not stable between
releases. One source tree serves two DuckDB lines at the same time:

| Line | DuckDB | extension-ci-tools | Where |
|------|--------|--------------------|-------|
| v1.5 (variegata) | `v1.5.6`, the `./duckdb` submodule pin | `v1.5-variegata` | local builds, `duckdb-v1_5_6-*` CI jobs, releases |
| 2.0 (cyanoptera) | `v2.0.0` built from commit `d54cbbe3df17e8e5ce68312f483b4155836112aa` (no tag yet) | `v2.0-cyanoptera` | `duckdb-next-build` CI job, `Next20Tests.yml`, community-extensions `repo.ref` |

duckdb/community-extensions builds `repo.ref` against the 2.0 commit above (`duckdb_commit` in
its `build.yml`) and `repo.variegata` against v1.5.6, so both have to build from the same commit.
The v1.4 (andium) line is no longer built.

## The compat-shim approach

Call sites never test the DuckDB version. Every API difference goes behind a helper that detects
the API by its *shape*: two overloads, one that only compiles against the new API (SFINAE on the
trailing return type), one for the old API; the caller passes `0` so the new one wins wherever it
exists. A version number cannot express the truth - v1.5.6 already carries some 2.0 APIs as
backports.

The helpers live in [DataZooDE/duckdb-compat](https://github.com/DataZooDE/duckdb-compat), a
header-only submodule (`./duckdb-compat`, namespace `duckdb_compat`). It is wired as an include
path only (`include_directories(duckdb-compat/include)`): never `target_link_libraries` it,
DuckDB exports the extension targets and the generate step would fail. A missing helper goes
there, with a case in its `test/probe.cpp` compiled against every DuckDB line.

What the 2.0 port changed, and the rule for new code:

- **Function names** are `Identifier`s on 2.0, constructed explicitly: a name held in a
  `std::string` goes through `duckdb_compat::FunctionName(name)` (literals convert by
  themselves). Table-function binds declare `duckdb_compat::BindNames &names` and fill it with
  `emplace_back`.
- **Registration** goes through `src/include/anofox_function_alias.hpp`. It marks every scalar
  function fallible (2.0 turns an exception from a non-fallible scalar function into an INTERNAL
  error) and names the parameters from the `FunctionDescription`s (2.0 otherwise shows `col0`,
  `col1`, ... in `duckdb_functions()`). Register new functions through it, not with
  `loader.RegisterFunction` directly.
- **Stability and named parameters** are setters on 2.0: `duckdb_compat::SetStability(fn, ...)`,
  `duckdb_compat::AddNamedParameter(fn, name, type)`.
- **Vectors carry their own size on 2.0**: end every table-function chunk with
  `duckdb_compat::SetChildCardinality(output, n)` (on 1.5.6 the same as `SetCardinality`). A chunk
  sized with the deprecated `SetCardinality` leaves its vectors at size 0, and `IS NULL` over the
  output reads garbage.
- **Scalar-function binds** take a `BindScalarFunctionInput` on 2.0. Every bind here only needs
  the client context, so write it as `unique_ptr<FunctionData> F(ClientContext &)` and register it
  with `func.SetBindCallback(&ScalarContextBind<F>::Bind)` (`anofox_function_alias.hpp`).
- **Vector data is const on 2.0**: write output through `duckdb_compat::FlatData<T>(vector)` (not
  `FlatVector::GetData`), read inputs through `UnifiedVectorFormat::GetData<T>(format)` (not a
  `reinterpret_cast` of `format.data`). Include `anofox_duckdb_vectors.hpp` for `ListVector` /
  `StructVector`, and take struct children with `VectorRef(StructVector::GetEntries(v)[i])`
  (`unique_ptr<Vector>` on 1.5.6, `Vector` on 2.0).
- **SQL built at bind time**: parse with `ParseWithContextSettings` / `ParseSubquery(sql, context,
  ...)` (`anofox_sql_utils.hpp`; `ClientContext::GetParserOptions` is private on 2.0), stream
  internal queries with `StreamQuery(con, sql)` (2.0 replaced `Connection::SendQuery` with
  `Submit`).
- **Query results**: `duckdb_compat::ResultNames(*result)` / `ResultTypes(*result)` /
  `FetchRows(*result)`, not `result->names`, `result->types` or `result->GetValue`.
- **Catalog names** are `Identifier`s on 2.0: `duckdb_compat::Ident(string)` into catalog lookups,
  `duckdb_compat::Str(...)` out (column names, `QualifiedName` parts via
  `duckdb_compat::QualifiedCatalog/Schema/LocalName`), `duckdb_compat::EntryName(name)` for
  `EntryLookupInfo` (keep `name` alive: 1.5.6 stores a reference).
- **C++ tests** (`test/cpp/`, linked into `unittest`) call extension internals; 2.0 no longer
  links out-of-tree extensions into `unittest`, so `CMakeLists.txt` links the static extension
  explicitly.
- **Tests**: `lambda x: expr`, not `x -> expr` (2.0 rejects the arrow).

## Bumping a line

1. **v1.5 patch release**: bump `./duckdb` to the tag and `./extension-ci-tools` to the matching
   branch, and `duckdb_version` in the `duckdb-v1_5_6-*` jobs of
   `.github/workflows/MainDistributionPipeline.yml`.
2. **2.0 commit** (when community-extensions moves `duckdb_commit` in its `build.yml`): update
   `duckdb_commit` in the `duckdb-next-build` job, `DUCKDB_COMMIT` in `Next20Tests.yml` and this
   file. The submodule pin stays on v1.5; build 2.0 in a scratch worktree whose `duckdb` is at
   that commit and `extension-ci-tools` at the matching branch.
3. Survey every compile error against the exact commit (`ninja -k 0`) and map each one to an
   existing helper or a new duckdb-compat helper.
4. Run the suite through `build/release/test/unittest "test/*"` on both lines, deleting the
   `unittest` binary before each build so a failed build cannot leave a stale green binary. On
   2.0 the extension is not linked into the shell: use `unittest`, or the shell with `-unsigned`
   and an explicit `LOAD '<path>'`. A 2.0 **debug** build checks every chunk's vector sizes
   (`DataChunk::Verify`) and finds a missed `SetChildCardinality`.
5. Run the full CI matrix on the branch before merging (`gh workflow run
   MainDistributionPipeline.yml --ref <branch>`): pull-request runs of the 2.0 line build only
   linux_amd64 and windows_amd64.

Open deprecations on 2.0 (warnings only): `DataChunk::SetValue` (use `Vector::Append`),
`Vector::ToUnifiedFormat(count, ...)` (`duckdb_compat::ToUnifiedFormat`), `ListVector::GetEntry`.

## Finding out what changed

- DuckDB's [release notes](https://github.com/duckdb/duckdb/releases) and the history of
  [core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions)
- The git history of the header whose API changed
- v1.5.6 marks its 2.0 backports with `NOTE(backport)` comments in the headers

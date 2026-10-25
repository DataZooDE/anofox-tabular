# Updating DuckDB

anofox-tabular builds against DuckDB's C++ extension API, which is not stable between
releases. One source tree serves two DuckDB lines at the same time:

| Line | DuckDB | extension-ci-tools | Where |
|------|--------|--------------------|-------|
| 2.0 (cyanoptera) | `v2.0.0` (commit `d54cbbe3df17e8e5ce68312f483b4155836112aa`), the `./duckdb` submodule pin | `v2.0-cyanoptera` | local builds, `duckdb-next-build` CI job, `Next20Tests.yml`, `NERTests.yml`, community-extensions `repo.ref` |
| v1.5 (variegata) | `v1.5.6` | `v1.5-variegata` | `duckdb-v1_5_6-*` CI jobs, releases, community-extensions `repo.variegata` |

Local development happens on 2.0, the line community-extensions publishes by default. v1.5.6 is
checked by CI on every pull request; to run it locally, see [Checking v1.5.6
locally](#checking-v156-locally).

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

## Local builds on 2.0

- **Version stamp.** DuckDB 2.0 takes its version from `scripts/ci/release_version.txt`, not from
  git, so a plain build of the `v2.0.0` tag is stamped `v2.0.0-devNNNNN`, and released clients
  (the Python package) refuse to load the extension. The `Makefile` sets `OVERRIDE_GIT_DESCRIBE`
  to the tag when `./duckdb` sits exactly on a release tag; check the configure line
  `-- git hash ..., version v2.0.0`.
- **Switching an existing checkout.** After `git submodule update` moves `./duckdb` to another
  line, delete `build/release/CMakeCache.txt` (and `build/release/CMakeFiles`) before building:
  the v1.5 cache sets `CMAKE_CXX_STANDARD=11`, which the 2.0 configure step rejects.
- **Shell.** The 2.0 shell does not link the extension statically. `scripts/duckdb-dev.sh` starts
  `build/release/duckdb` with the extension loaded (`-unsigned`), on both lines:
  `scripts/duckdb-dev.sh -c "SELECT email_is_valid('a@b.com')"`. With `AI_AGENT` set and stdout
  not a terminal, the 2.0 shell switches to a terse agent mode; the script passes `-no-agent`.
- **Compiler flags.** DuckDB 2.0 compiles with `-march=x86-64-v3`. Fused multiply-add can change
  the last bit of a `DOUBLE` result compared with a v1.5.6 build; tests compare such values
  rounded.
- **Python.** The dev dependency group pins `duckdb==2.0.0` on Python 3.11 and newer (DuckDB 2.0
  needs 3.11) and `duckdb==1.5.6` on Python 3.10.

## Checking v1.5.6 locally

Create a worktree whose submodules sit on the v1.5 line, build it there, and run the suites:

```bash no-run
git worktree add ../anofox-tabular-v15 HEAD
cd ../anofox-tabular-v15
git submodule update --init --recursive
git -C duckdb checkout v1.5.6
git -C extension-ci-tools fetch origin v1.5-variegata && git -C extension-ci-tools checkout FETCH_HEAD
GEN=ninja make release
DATAZOO_DISABLE_TELEMETRY=1 ./build/release/test/unittest "test/*"
cd python && ANOFOX_EXT_PATH=../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension \
  uv run --python 3.10 pytest tests/
```

## Bumping a line

1. **v1.5 patch release**: bump `./duckdb` to the tag and `./extension-ci-tools` to the matching
   branch, and `duckdb_version` in the `duckdb-v1_5_6-*` jobs of
   `.github/workflows/MainDistributionPipeline.yml`.
2. **2.0 commit or tag** (when community-extensions moves `duckdb_commit` in its `build.yml`):
   bump `./duckdb` to it and `./extension-ci-tools` to the matching branch, update
   `duckdb_commit` in the `duckdb-next-build` job, `DUCKDB_COMMIT` in `Next20Tests.yml`, the vcpkg
   commit in `NERTests.yml` and this file.
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

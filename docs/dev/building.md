# Building from Source

> Build the extension, the DuckDB CLI and the test runner from a checkout.

**Use this page to:**
- Set up the toolchain and vcpkg
- Build a release or debug binary
- Find the build artifacts

Most users do not need to build: `INSTALL anofox_tabular FROM community;` installs a release.
See [Getting started](../guides/01-getting-started.md).

## Requirements

| Tool | Notes |
|------|-------|
| C++17 compiler | GCC, Clang or MSVC |
| CMake | 3.10 or newer |
| Ninja | Optional, faster builds (`GEN=ninja`) |
| vcpkg | Provides openssl, c-ares, spdlog, curl and, on x64 Linux and macOS, OpenVINO (see `vcpkg.json`) |
| libpostal | Optional. Found in `/usr/local` or `/usr`, or through pkg-config. Without it the postal module is left out. |
| Python 3 and uv | Only for the Python package and its tests |

## Clone

The repository has git submodules (DuckDB, extension-ci-tools, duckdb-compat,
posthog-telemetry, datazoo-banner). Clone them with it:

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-tabular.git
cd anofox-tabular
```

In an existing clone, run `git submodule update --init --recursive`.

## vcpkg

The Makefile uses the vcpkg toolchain from `VCPKG_ROOT`:

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

The first build compiles the vcpkg dependencies, which takes a while; later builds reuse them.

## Build

```bash
GEN=ninja make release      # or: make debug, make reldebug
```

Set `ANOFOX_TABULAR_NO_LIBPOSTAL=1` to build without the postal module. Without it, a build
that does not find libpostal runs `scripts/install-deps-ci.sh`, which builds libpostal from
source.

## Artifacts

| Path | What it is |
|------|------------|
| `build/release/duckdb` | DuckDB CLI with the extension linked in |
| `build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension` | Loadable extension |
| `build/release/test/unittest` | SQL test runner |

Load the loadable binary into another DuckDB of the same version:

```sql no-run
LOAD './build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension';
```

A locally built extension is unsigned. Start the DuckDB CLI with `-unsigned`, or set
`allow_unsigned_extensions` in the client config.

## DuckDB versions

The source tree builds against DuckDB v1.5.6 (the `duckdb` submodule) and DuckDB 2.0. API
differences sit behind the duckdb-compat helpers. See [Updating DuckDB](updating-duckdb.md).

## Next steps

- [Testing](testing.md)
- [Architecture](architecture.md)

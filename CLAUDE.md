# DuckDB Extension Development Context 

This file provides guidance to AI coding Agents when working with code in this repository.

## Quick Start

## Python Development

Always use **uv** for Python environment management. Never use `pip`, `venv`, or `conda` directly.

```bash
# Install uv if not present
curl -LsSf https://astral.sh/uv/install.sh | sh

# Create/sync the Python environment
cd python
uv sync --extra dev

# Run tests
cd python
ANOFOX_EXT_PATH=../build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension \
  uv run pytest tests/ -v

# Run tests that don't need the extension
uv run pytest tests/test_loader.py tests/test_utils.py -v

# Add a dependency
uv add <package>
uv add --dev <package>
```

## Project Overview

**Anofox Tabular** is a DuckDB extension providing data quality validation and improvement primitives. 
It's built on the DuckDB C++ extension template and consists of 10 modules:

- **Email**: Regex, DNS, and SMTP validation with configurable timeouts
- **Postal**: Address parsing and expansion via libpostal
- **Phone**: Phone number parsing/formatting (built-in implementation)
- **Money**: Currency-aware DECIMAL money values
- **VAT**: EU VAT number validation (28 countries)
- **PII**: Pattern- and NER-based PII detection and masking
- **Data quality**: Check functions, check suites (`run_checks`) and `_events` outputs
- **Anomaly detection**: Isolation Forest, DBSCAN, OutlierTree
- **Diffing**: `diff_joindiff` / `diff_hashdiff`
- **Profiling**: `profile_table`, `profile_summary`, `profile_correlations`

Every SQL function has a canonical `anofox_tab_*` name and a short alias (`email_is_valid`);
only `anofox_ner_status` and `anofox_contract_version` have no alias.

The extension is implemented entirely in C++17 with dependencies managed through vcpkg.

## Build System

- **Makefile**: Entry point that wraps `extension-ci-tools/makefiles/duckdb_extension.Makefile`
- **CMakeLists.txt**: Defines targets `anofox_tabular_extension` (static) and `anofox_tabular_loadable_extension` (dynamic)
- **vcpkg**: Dependencies (openssl, c-ares, spdlog, curl, OpenVINO) are declared in `vcpkg.json`; libpostal is built separately

```bash
GEN=ninja make release                   # Build the extension (requires vcpkg)
```

Always use ninja for faster builds. Seldomly use `make clean` to clean the build directory.

### Build Artifacts

- `build/release/duckdb` – DuckDB 2.0 CLI. It does not link the extension statically: start it
  with `scripts/duckdb-dev.sh`, which loads the extension (works on v1.5.6 builds too)
- `build/release/extension/anofox_tabular/anofox_tabular.duckdb_extension` – Loadable binary
- `build/release/test/unittest` – Test runner

If you get linker error with duplicate symbol, eg. `duckdb::LogicalType::VARCHAR, you probably have to use LogicalTypeId::VARCHAR instead.

## Architecture

### Extension Entry Point
`src/anofox_tabular_extension.cpp` / `src/include/anofox_tabular_extension.hpp`

The `LoadInternal()` function registers all modules on extension load. Each module (email, postal, phone) follows a two-part pattern:

1. **Options Registration** – SQL-level configuration options (e.g., `SET anofox_tab_email_dns_timeout_ms`)
2. **Functions Registration** – SQL scalar and table functions

### Module Organization

If not sure how implemenation and API of duckdb functionlity works, have a look at public duckdb extension on github. Or
look at the c++ code of `./duckdb` or the cpp tests in `./duckdb/test`.

Each feature module has three layers:

1. **Header** (`src/include/anofox_*.hpp`) – Public interface
2. **Implementation** (`src/anofox_*.cpp`) – Core logic
3. **Specialized Components** – E.g., `anofox_email_dns.cpp`, `anofox_email_smtp.cpp`

**Email Module** (`anofox_email.*`)
- Validates email addresses through three stages: regex → DNS → SMTP
- DNS lookups handled by `anofox_email_dns.cpp` (uses c-ares)
- SMTP verification in `anofox_email_smtp.cpp` (socket-based, respects per-host and overall timeouts)
- Returns structured results including MX hosts and SMTP transcript

**Postal Module** (`anofox_postal.*`)
- Wraps libpostal for address parsing and normalization
- Manages data bundle lifecycle (`postal_load_data()` downloads ~500 MB assets; the first postal call does it otherwise)
- Path controlled via the `anofox_tab_postal_data_path` config option
- Not built on Windows; the postal SQL tests run only with `POSTAL_AVAILABLE`

**Phone Module** (`anofox_phonenumber.*`)
- Built-in international phone parsing/formatting (`anofox_phonenumber_metadata.cpp` holds the region metadata)
- Supports four output formats: E164, INTERNATIONAL, NATIONAL, RFC3966
- Region hints allow contextual parsing (defaults to `anofox_tab_phonenumber_default_region`)

**Data Quality Modules** (`anofox_metric.*`, `anofox_check.*`, `anofox_check_suite.*`, `anofox_contract.*`)
- Check functions (`volume`, `null_rate`, …, `zscore`, `iqr`), the time-aware checks with
  `_events` companions, the `run_checks` suite runner and the integration contract

**Other modules**: `anofox_money*`, `anofox_vat*`, `anofox_pii.*` / `anofox_ner.*`,
`anofox_isolation_forest.*`, `anofox_dbscan.*`, `anofox_outlier_tree.*`, `anofox_diff.*`,
`anofox_profile.*`

### Tracing
Add tracing messages to your implementation. The goal of the tracing messages should be, that we easier understand errors and logical problems. 

`src/anofox_trace.*` provides centralized logging via spdlog:
- Global on/off: `SET anofox_tab_trace_enabled` (default `true`)
- Level control: `SET anofox_tab_trace_level = 'trace'|'debug'|'info'|'warn'|'error'|'critical'|'off'` (default `info`)
- All logs prefixed with subsystem: `[anofox] email: …`, etc.
- Compile-time configuration: spdlog is header-only with custom fmt bundling

## Testing

### Running individual SQL commands to try something out.
It is possible to run individual SQL commands from the shell and directly observe their result via. Example:

```bash
$ scripts/duckdb-dev.sh -c "LOAD anofox_tabular; SET anofox_tab_trace_level = 'warn'; SELECT DISTINCT function_name, function_type FROM duckdb_functions() WHERE function_name LIKE 'anofox_tab_email%' OR function_name LIKE 'anofox_tab_phonenumber_p%' ORDER BY function_name"
┌──────────────────────────────┬───────────────┐
│        function_name         │ function_type │
│           varchar            │    varchar    │
├──────────────────────────────┼───────────────┤
│ anofox_tab_email_config      │ table         │
│ anofox_tab_email_is_valid    │ scalar        │
│ anofox_tab_email_validate    │ scalar        │
│ anofox_tab_phonenumber_parse │ scalar        │
└──────────────────────────────┴───────────────┘
```

`SET anofox_tab_trace_level = 'warn'` hides the `[anofox] [info]` trace lines of later
statements; the lines printed during `LOAD` still appear (on stderr). Prefix the command with `DATAZOO_DISABLE_TELEMETRY=1` to
send no telemetry while testing.

### SQL Test Format
All tests are SQLLogicTests (`.test` files) located in `test/sql/`.
To understand the format on can look at https://duckdb.org/docs/stable/dev/sqllogictest/intro, for details on result verification look at https://duckdb.org/docs/stable/dev/sqllogictest/result_verification. If state between tests is necessary, then look at https://duckdb.org/docs/stable/dev/sqllogictest/persistent_testing. 
If you need examples, look at the extensive test suite of duckdb itself in `./duckdb/test/sql`.


Test files are named after their module (`anofox_email_basic.test`, `anofox_phonenumber.test`,
`anofox_check*.test`, `anofox_pii*.test`, `pragma.test` for options, …); list `test/sql/` for
the current set. The postal tests `anofox_postal.test.disabled` and `anofox_postal.test_disabled`
are disabled; `anofox_postal_null.test` runs only with `POSTAL_AVAILABLE`.

Each test file starts with `require anofox_tabular` and includes `LOAD anofox_tabular`.

### Running SQL Tests
```bash
make test               # Full suite, release build
make test_debug         # Full suite, debug build
```

To run individual tests, you can use the following command:
```bash
./build/release/test/unittest test/sql/anofox_email_basic.test
```

### CPP Tests
It is also possible to define C++ unit tests in `test/cpp`. See: https://duckdb.org/docs/stable/dev/sqllogictest/catch

## Dependencies & Vcpkg

Vcpkg is used to fetch and manage C++ dependencies. We use an external vcpkg. The root of the vcpkg is available via the environment variable `VCPKG_ROOT`.

The following snippet should not be needed in most cases, as it should be set by the Makefile.
```bash
export VCPKG_TOOLCHAIN_PATH="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
```

Declared in `vcpkg.json`:
- **c-ares** – DNS resolution (async, used by email DNS module)
- **spdlog** – Logging (header-only)
- **openssl** – Hashing (PII hash masking)
- **curl** – Downloads (postal data, NER model)
- **openvino** – NER inference; only on x64 Linux and x64 macOS

libpostal is not a vcpkg port: `scripts/install-deps-ci.sh` builds it, and CMake finds it via pkg-config (skipped with `ANOFOX_TABULAR_NO_LIBPOSTAL=1`).

## Updating DuckDB Version

Local development happens on **DuckDB v2.0.0**: the `./duckdb` submodule is pinned to the
`v2.0.0` tag and `./extension-ci-tools` to `v2.0-cyanoptera`, the line community-extensions
publishes by default. The same source tree also builds on v1.5.6, which CI checks on every pull
request; API differences sit behind duckdb-compat helpers. Python tests use `duckdb==2.0.0`
(Python 3.11+). See `docs/dev/updating-duckdb.md` for the line table, the rules for new code
(function names, registration through `anofox_function_alias.hpp`, `SetChildCardinality`,
`lambda x:` instead of `x ->` in tests), local 2.0 specifics (version stamp, shell, `-march`)
and how to check v1.5.6 locally.

## Key Files to Know

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Build configuration, dependency linking |
| `extension_config.cmake` | Extension registration (loads anofox_tabular, json) |
| `vcpkg.json` | Vcpkg dependency pins |
| `src/anofox_tabular_extension.cpp` | Extension entry and module registration |
| `src/anofox_trace.cpp` | Centralized spdlog initialization and management |
| `test/sql/*.test` | SQLLogicTest format test suite |

## Common Development Patterns

### Adding a New SQL Function

1. Declare in the module header (e.g., `src/include/anofox_email.hpp`)
2. Implement in the module source (e.g., `src/anofox_email.cpp`)
3. Register with DuckDB in the module's registration function, under the canonical
   `anofox_tab_*` name plus its short alias, through the helpers in
   `src/include/anofox_function_alias.hpp`:
   ```cpp
   RegisterScalarFunctionWithAlias(loader, func, "short_name", {std::move(desc)});
   // or
   RegisterTableFunctionSetWithAlias(loader, func_set, "short_name", {std::move(desc)});
   ```
4. Add test cases to `test/sql/` following SQLLogicTest format
5. Update README.md and docs/API_REFERENCE.md if user-facing (see the Documentation Update Workflow below)

### Adding a Configuration Option

1. Define in module registration (e.g., `RegisterEmailOptions()`)
2. Add it with `config.AddExtensionOption(...)`, using the module's prefix (`anofox_tab_*`, or `anofox_pii_*` / `anofox_ner_*` for PII)
3. Test with `SET option_name = value;` in `.test` files
4. Document in README.md Configuration Options table

### Reuse of existing code and concepts
If you are adding a new feature, try to reuse existing code and concepts from the project. If you are not sure how to do it, ask.
Also have a look at duckdb source in `./duckdb` and the cpp tests in `./duckdb/test` wether you can find a similar concept or code to reuse.

### Documentation Update Workflow (Mandatory)
For every change that affects user-visible behavior, SQL function signatures, defaults, return schemas, aliases, or examples:

1. Check `README.md` for impacted sections and update them.
2. Check `docs/API_REFERENCE.md` for impacted sections and update them.
3. Add/update tests first (red/green) when behavior changes, then align docs to the final behavior.
4. Before finishing, explicitly verify docs match implementation (names, types, defaults, output columns, examples).

Do not consider work complete if code changed but these documentation checks were skipped.

## Git Commit Guidelines

When creating git commits:
- **NEVER** include any attribution to AI assistants or code generators (e.g., "Generated with Claude Code", "Co-Authored-By: Claude")
- Write clear, concise commit messages that explain the "why" rather than the "what"
- Use conventional commit format when appropriate (feat:, fix:, refactor:, etc.)
- Keep commit messages professional and focused on the technical changes
- Squash trial-and-error commits into clean, logical commits before pushing

Example of a good commit message:
```
Improve build system: libcurl S3 downloads and cleaner configuration

- Replace DuckDB HTTPUtil with libcurl for postal data downloads
- Add CMake fallback to handle multi-container CI workflows
- Simplify libpostal detection logic with better error messages
- Unify build directory structure for all generators
```

See `docs/UPDATING.md` for version bump procedures.

## General C++ Development Rules
You are a senior C++ developer with expertise in modern C++ (C++17/20), STL, and system-level programming. You try to follow the best practices and guidelines for C++ development. Your code is optimized for simplicity, robustness and maintainability.

### Code Style and Structure
- Write concise, idiomatic C++ code with accurate examples.
- Follow modern C++ conventions and best practices.
- Use object-oriented, procedural, or functional programming patterns as appropriate.
- Leverage STL and standard algorithms for collection operations.
- Use descriptive variable and method names (e.g., 'isUserSignedIn', 'calculateTotal').
- Structure files into headers (*.hpp) and implementation files (*.cpp) with logical separation of concerns.

### Naming Conventions
- Use PascalCase for class names.
- Use camelCase for variable names and methods.
- Use SCREAMING_SNAKE_CASE for constants and macros.
- Prefix member variables with an underscore or m_ (e.g., `_userId`, `m_userId`).
- Use namespaces to organize code logically.

### C++ Features Usage
- Prefer modern C++ features (e.g., auto, range-based loops, smart pointers).
- Use `std::unique_ptr` and `std::shared_ptr` for memory management.
- Prefer `std::optional`, `std::variant`, and `std::any` for type-safe alternatives.
- Use `constexpr` and `const` to optimize compile-time computations.
- Use `std::string_view` for read-only string operations to avoid unnecessary copies.
- Try to always reuse existing code and concepts from the project.
- Before introducing new concepts, think hard, and make a detailed plan about changes and integration into the project.

### Syntax and Formatting
- Follow a consistent coding style, such as Google C++ Style Guide or your team’s standards.
- Place braces on the same line for control structures for classes and methods place them on the next line.
- Use clear and consistent commenting practices.

### Error Handling and Validation
- Use exceptions for error handling (e.g., `std::runtime_error`, `std::invalid_argument`).
- Use RAII for resource management to avoid memory leaks.
- Validate inputs at function boundaries.
- Log errors using a logging library (e.g., spdlog, Boost.Log).

### Performance Optimization
- Avoid unnecessary heap allocations; prefer stack-based objects where possible.
- Use `std::move` to enable move semantics and avoid copies.
- Optimize loops with algorithms from `<algorithm>` (e.g., `std::sort`, `std::for_each`).
- Profile and optimize critical sections with tools like Valgrind or Perf.

### Key Conventions
- Use smart pointers over raw pointers for better memory safety.
- Avoid global variables; use singletons sparingly.
- Use `enum class` for strongly typed enumerations.
- Separate interface from implementation in classes.
- Use templates and metaprogramming judiciously for generic solutions.

### Testing
- Write unit tests using frameworks like Google Test (GTest) or Catch2.
- Mock dependencies with libraries like Google Mock.
- Implement integration tests for system components.

### Security
- Use secure coding practices to avoid vulnerabilities (e.g., buffer overflows, dangling pointers).
- Prefer `std::array` or `std::vector` over raw arrays.
- Avoid C-style casts; use `static_cast`, `dynamic_cast`, or `reinterpret_cast` when necessary.
- Enforce const-correctness in functions and member variables.

### Documentation
- Write clear comments for classes, methods, and critical logic.
- Use Doxygen for generating API documentation.
- Document assumptions, constraints, and expected behavior of code.

Follow the official ISO C++ standards and guidelines for best practices in modern C++ development.

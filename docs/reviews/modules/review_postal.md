## Summary
The postal module is compact and mostly follows DuckDB’s manual vector loop style, but it has several correctness and operational hazards around NULL semantics, global libpostal/curl state, and filesystem work. The biggest user-visible bug is that `postal_parse_address(NULL)` returns a non-NULL struct whose children are NULL, which differs from expected SQL NULL propagation. The data download path also mixes user-controlled paths with `std::system`, and the load/initialization lifecycle is not robust under concurrent calls or partial failures. Performance is acceptable for small batches, but parse/expand currently do avoidable per-row materialization and copies.

## Findings

### [HIGH] `postal_parse_address(NULL)` returns a valid struct instead of SQL NULL
Affected: `src/anofox_postal.cpp:323`, `src/anofox_postal.cpp:324`, `src/anofox_postal.cpp:325`

For NULL input rows, the function only marks each struct child as NULL. It never marks the parent `STRUCT` vector row as NULL, so SQL sees a non-NULL struct value with all fields NULL rather than `NULL`. This breaks predicates like `postal_parse_address(NULL) IS NULL` and differs from the explicit parent-null handling used elsewhere in the project for struct results.

Suggested improvement: set parent validity first, and clear it for non-NULL rows.

```cpp
if (!input_data.validity.RowIsValid(idx)) {
	FlatVector::SetNull(result, i, true);
	continue;
}

FlatVector::SetNull(result, i, false);
for (auto &child : children) {
	FlatVector::SetNull(*child, i, true);
}
```

### [HIGH] User-controlled data path is interpolated into a shell command
Affected: `src/anofox_postal.cpp:219`, `src/anofox_postal.cpp:220`, `src/anofox_postal.cpp:492`

`anofox_tab_postal_data_path` is user configurable, and `LoadData` builds a shell command by concatenating `destination` and `data_dir` into `tar -xzf ... -C ...`. Double quotes are not sufficient escaping: a path containing `"` or shell metacharacters can break the command, fail extraction, or execute unintended shell syntax. This is especially risky because DuckDB settings can be supplied from SQL and extension load paths often run with the hosting process’s privileges.

Suggested improvement: avoid the shell entirely. Use a tar/gzip extraction library or a process API that passes argv without shell parsing. If that is not immediately feasible, reject unsafe path characters before command construction as a stopgap, but that should not be the long-term fix.

```cpp
if (data_dir.find('"') != string::npos || destination.find('"') != string::npos) {
	throw InvalidInputException("Postal data path contains unsupported quote character");
}
// Prefer: ExtractTarGz(destination, data_dir) without std::system.
```

### [MEDIUM] `LoadData` is not synchronized and can corrupt shared downloads
Affected: `src/anofox_postal.cpp:127`, `src/anofox_postal.cpp:138`, `src/anofox_postal.cpp:157`, `src/anofox_postal.cpp:219`, `src/anofox_postal.cpp:227`, `src/anofox_postal.cpp:472`

`EnsureInitialized` calls `Initialize` under `init_lock`, but `postal_load_data()` calls `LoadData` directly without taking that lock. Two concurrent connections can download the same tarball to the same destination, remove each other’s partial files, or extract over the same directory. The function also calls process-wide `curl_global_init`/`curl_global_cleanup`; cleanup at the end of one concurrent call can affect another curl user in the same process, including other extension code.

Suggested improvement: serialize data loading through the same manager mutex or a dedicated load mutex, and manage curl global initialization with process-lifetime RAII rather than per-call cleanup.

```cpp
void PostalManager::LoadData(ClientContext &context) {
	std::lock_guard<std::mutex> lock(init_lock);
	LoadDataLocked(context);
}
```

### [MEDIUM] Partial libpostal initialization is not rolled back on failure
Affected: `src/anofox_postal.cpp:263`, `src/anofox_postal.cpp:267`, `src/anofox_postal.cpp:271`, `src/anofox_postal.cpp:276`

`Initialize` performs multiple global libpostal setup calls and throws immediately on the first failed stage. If core setup succeeds but parser or language classifier setup fails, `initialized` remains false while libpostal may be partially initialized. A later retry can then call setup again against already-initialized global state, and the destructor will still call all teardown functions regardless of which setup stages completed.

Suggested improvement: track setup stages and tear down completed stages before throwing, or wrap libpostal setup in an RAII guard that only commits by setting `initialized = true` after every stage succeeds.

```cpp
bool core_ready = false;
bool parser_ready = false;

try {
	if (!libpostal_setup_datadir(data_dir_c) || !libpostal_setup()) {
		throw IOException(...);
	}
	core_ready = true;

	if (!libpostal_setup_parser_datadir(data_dir_c) || !libpostal_setup_parser()) {
		throw IOException(...);
	}
	parser_ready = true;
	...
} catch (...) {
	if (parser_ready) libpostal_teardown_parser();
	if (core_ready) libpostal_teardown();
	throw;
}
```

### [MEDIUM] `data_directory` is read without synchronization
Affected: `src/include/anofox_postal.hpp:52`, `src/include/anofox_postal.hpp:53`, `src/anofox_postal.cpp:74`, `src/anofox_postal.cpp:84`, `src/anofox_postal.cpp:129`, `src/anofox_postal.cpp:235`

`SetDataDirectory` writes `data_directory` under `init_lock`, but `GetDataDirectory`, `LoadData`, and `GetStatus` read it without taking the mutex. A concurrent `SET anofox_tab_postal_data_path = ...` and `postal_status()`/initialization call before libpostal is initialized can race on the `std::string`, which is undefined behavior in C++. The `initialized` atomic does not protect the string itself.

Suggested improvement: copy `data_directory` under the mutex before using it, or make all manager public methods that read/write configuration synchronize consistently.

```cpp
std::string PostalManager::GetDataDirectory() const {
	std::lock_guard<std::mutex> lock(init_lock);
	return data_directory;
}
```

This requires making the mutex `mutable` for `const` access.

### [LOW] Public option name and error message are inconsistent with postal aliases
Affected: `src/anofox_postal.cpp:293`, `src/anofox_postal.cpp:295`, `src/anofox_postal.cpp:492`

The option registered is `anofox_tab_postal_data_path`, but the NULL error message says `anofox_postal_data_path`, and the disabled postal tests also use `anofox_postal_data_path`. The functions expose non-`tab` aliases like `postal_parse_address`, so this mismatch is easy for users to hit and hard to diagnose.

Suggested improvement: either register `anofox_postal_data_path` as an alias option if DuckDB supports that pattern, or standardize all messages/tests/docs on `anofox_tab_postal_data_path`.

### [LOW] Parse and expand materialize avoidable per-row intermediates
Affected: `src/anofox_postal.cpp:88`, `src/anofox_postal.cpp:98`, `src/anofox_postal.cpp:107`, `src/anofox_postal.cpp:118`, `src/anofox_postal.cpp:330`, `src/anofox_postal.cpp:381`, `src/anofox_postal.cpp:386`

Each parse row copies the input to `std::string`, copies all libpostal components into a `std::vector<PostalComponent>`, then copies selected values into DuckDB vectors. Expansion similarly builds a `std::vector<std::string>` and then wraps each item in a `Value` for `ListVector::PushBack`. This is simple but allocation-heavy for large columns and especially expensive for expansion-heavy addresses.

Suggested improvement: keep the libpostal response lifetime local to the vector loop and write directly into DuckDB vectors, using small RAII wrappers for libpostal response destruction. For list output, append directly from `expansions[i]` into the list child vector where possible instead of building a second vector of strings.

## Quick wins
- Set the parent struct validity mask in `PostalParseAddressFunction` for both NULL and non-NULL rows.
- Remove the duplicate `RegisterPostalOptions(loader)` call from either extension load or `RegisterPostalFunctions`.
- Fix the option-name mismatch in `SetPostalDataPathOption`’s error text.
- Add `result.Verify(args.size())` to postal scalar functions during development/tests.
- Guard `LoadData` with a mutex and use RAII for `FILE *`, `CURL *`, and curl global initialization.
- Replace `std::system("tar ...")` with non-shell extraction, or reject unsafe path characters until that replacement lands.
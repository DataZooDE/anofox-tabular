## Summary
The core loader is compact and mostly delegates correctly to module-level registration, but it also wires process-wide telemetry and alias helpers that affect the whole extension surface. The highest-risk issue is thread safety in telemetry: the extension exposes settings that mutate singleton state while function bind/execution paths enqueue telemetry from DuckDB worker/client threads. The alias helper code is another structural risk because it manually reconstructs DuckDB function objects and silently loses metadata as DuckDB’s API evolves. Trace support is simple and uses atomics for configuration, but its output path could be made more robust under parallel execution.

## Findings

### [HIGH] Telemetry singleton access is not consistently synchronized
Affected: `src/anofox_tabular_extension.cpp:22`, `src/anofox_tabular_extension.cpp:30`, `src/anofox_tabular_extension.cpp:60`; context: `posthog-telemetry/src/telemetry.cpp:132`, `posthog-telemetry/src/telemetry.cpp:160`, `posthog-telemetry/src/telemetry.cpp:185`

`OnTelemetryEnabled` and `OnTelemetryKey` mutate a process-wide `PostHogTelemetry` singleton, while function bind paths across the extension call `CaptureFunctionExecution`. In the telemetry implementation, `_queue` is lazily initialized without holding `_thread_lock`, and `_api_key` is copied directly in `CaptureExtensionLoad`/`CaptureFunctionExecution` while `SetAPIKey` writes it under a mutex. Under parallel DuckDB execution or concurrent sessions changing settings, this is a data race on `_queue` and `_api_key`, with possible double queue construction, lost tasks, or undefined behavior from concurrent `std::string` access.

Suggested improvement: make capture methods take one lock to snapshot enabled/key/platform/version and initialize the queue, then enqueue outside the lock if desired.

```cpp
void PostHogTelemetry::CaptureFunctionExecution(...) {
    std::string api_key;
    TelemetryTaskQueue<PostHogEvent> *queue;
    {
        std::lock_guard<std::mutex> lock(_thread_lock);
        if (!_telemetry_enabled.load()) {
            return;
        }
        EnsureQueueInitializedLocked();
        api_key = _api_key;
        queue = _queue.get();
    }
    queue->EnqueueTask([api_key](auto event) { PostHogProcess(api_key, event); }, event);
}
```

### [MEDIUM] Function alias helpers drop DuckDB callback and optimizer metadata
Affected: `src/include/anofox_function_alias.hpp:18`, `src/include/anofox_function_alias.hpp:36`, `src/include/anofox_function_alias.hpp:56`, `src/include/anofox_function_alias.hpp:74`

The alias helpers reconstruct `ScalarFunction` and `TableFunction` objects by manually copying a small subset of fields. Current DuckDB scalar functions also carry fields such as `bind_extended`, `init_local_state`, `statistics`, `bind_expression`, `get_modified_databases`, serialization callbacks, `function_info`, error mode, and collation handling. Table functions carry many more callbacks and flags, including `bind_operator`, statistics/cardinality/progress callbacks, serialization, pushdown flags, pruning flags, ordering, function info, and initialization timing. Any function using those fields will behave differently through its alias, which is especially dangerous because aliases look semantically equivalent in the catalog.

Suggested improvement: copy the function object first, then only change the exposed name, or centralize a complete metadata-copy helper with tests.

```cpp
ScalarFunction alias_func = func;
alias_func.name = alias_name;

TableFunction alias_func = func;
alias_func.name = alias_name;
```

If direct assignment is not supported for all DuckDB versions, explicitly copy every public field from the current DuckDB structs and add regression tests that compare primary and alias behavior for bind data, null handling, errors, and pushdown flags.

### [MEDIUM] SQL telemetry setting cannot suppress the extension-load event
Affected: `src/anofox_tabular_extension.cpp:54`, `src/anofox_tabular_extension.cpp:57`, `src/anofox_tabular_extension.cpp:60`, `src/anofox_tabular_extension.cpp:69`

`LoadInternal` registers telemetry options first, but then immediately enables the default API key and captures `extension_load`. A user cannot normally execute `SET anofox_telemetry_enabled=false` until after the extension has loaded and registered the option, so the SQL setting does not prevent the first telemetry event. This contradicts the comment that registering options first allows users to disable telemetry via SQL settings.

Suggested improvement: check a pre-load source before emitting the load event, or avoid sending `extension_load` until after the current database setting has been read. At minimum, remove the misleading comment and document that only `DATAZOO_DISABLE_TELEMETRY` can suppress load-time telemetry.

```cpp
if (telemetry.IsEnabled()) {
    telemetry.CaptureExtensionLoad("anofox_tabular", version);
}
```

Also avoid resetting the API key unconditionally after registering the option if settings are meant to override it.

### [LOW] Trace writes can interleave across parallel tasks
Affected: `src/anofox_trace.cpp:73`, `src/anofox_trace.cpp:81`

`AnofoxTrace` writes to `std::cerr` using multiple stream operations. Standard streams avoid memory corruption, but messages from parallel DuckDB tasks can interleave at token boundaries, making trace output hard to read and potentially misleading during debugging. The function also drops the level string, so severity filtering is invisible in the emitted log line.

Suggested improvement: compose the full line first and guard the write with a small mutex, or route through the project’s existing logging dependency.

```cpp
static std::mutex trace_mutex;
std::lock_guard<std::mutex> lock(trace_mutex);
std::cerr << "[anofox][" << LevelToString(level) << "] " << message << '\n';
```

## Quick wins
- Replace manual alias reconstruction with copy-then-rename helpers for both scalar and table functions.
- Add unit tests that execute a primary function and its alias through NULL inputs, bind callbacks, and table-function bind replacement.
- Lock telemetry queue initialization and all `_api_key` reads.
- Make the load-time telemetry behavior explicit and test disabling via environment variable and SQL settings separately.
- Include the log level in `AnofoxTrace` output and serialize trace writes with a mutex.
## Summary
The PII module has a useful recognizer layout and generally avoids repeated regex compilation by storing compiled recognizers in the `PIIEngine` singleton. The highest-risk issues are around global mutable state and thread safety under DuckDB parallel execution, plus table functions that build SQL from user-controlled identifiers. Several registered configuration options are only partially honored, so users can set options that do not affect the main scalar APIs. Performance is acceptable for small strings, but the table functions and type-specific detection paths materialize many `Value` objects and full result sets instead of streaming/vectorizing.

## Findings

### [HIGH] Global PII configuration is unsynchronized and process-wide
Affected: `src/anofox_pii.cpp:106`, `src/anofox_pii.cpp:117`, `src/anofox_pii.cpp:147`, `src/anofox_pii.cpp:3153`, `src/anofox_pii.cpp:3182`, `src/include/anofox_pii.hpp:507`, `src/include/anofox_pii.hpp:537`

`PIIConfig` is a process-wide singleton with mutable `double`, `MaskStrategy`, `std::vector<PIIType>`, and `bool` fields. DuckDB can execute scalar/table functions in parallel while another connection changes extension options, so reads like `GetDefaultMaskStrategy()`, `IsTypeEnabled()`, and `IsDeepValidationEnabled()` can race with setters mutating the same fields. It also ignores `ClientContext` and `SetScope`, so settings from one database/connection can leak into another.

Suggested improvement: avoid mutable process-wide config for query behavior. Bind a per-query snapshot from DuckDB settings into `FunctionData`, or store state per database/client. If a singleton remains, guard all fields with a mutex and return copies rather than references:

```cpp
class PIIConfig {
public:
    PIIConfigSnapshot Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {min_confidence_, default_mask_strategy_, enabled_types_, deep_validation_};
    }

private:
    mutable std::mutex mutex_;
    std::vector<PIIType> enabled_types_;
};
```

### [HIGH] Lazy Base58 lookup initialization has a data race
Affected: `src/anofox_pii.cpp:992`, `src/anofox_pii.cpp:997`, `src/anofox_pii.cpp:998`

`CryptoAddressRecognizer::DecodeBase58()` uses a function-static `std::unordered_map` and populates it when `base58_map.empty()`. Two DuckDB worker threads validating crypto addresses can both observe the map as empty and mutate it concurrently, which is undefined behavior.

Suggested improvement: initialize the lookup in a thread-safe function-local static lambda, or use a fixed array:

```cpp
static const auto base58_map = [] {
    std::array<int, 256> map;
    map.fill(-1);
    for (int i = 0; i < 58; ++i) {
        map[static_cast<unsigned char>(BASE58_ALPHABET[i])] = i;
    }
    return map;
}();
```

### [HIGH] Table scan/audit functions build SQL with unescaped identifiers
Affected: `src/anofox_pii.cpp:2638`, `src/anofox_pii.cpp:2695`, `src/anofox_pii.cpp:2853`, `src/anofox_pii.cpp:2929`

`pii_audit_table()` and `pii_scan_table()` parse the table name for catalog lookup, but then execute a new SQL string using the original `bind_data.table_name`. Column names are wrapped in double quotes but embedded quotes are not escaped, and the table name is appended raw. This can break on valid identifiers and creates SQL-injection risk if these functions are exposed to untrusted SQL inputs.

Suggested improvement: do not reconstruct SQL from raw strings. Prefer DuckDB relation/table scan APIs, or construct fully quoted identifiers from catalog-resolved names and escape embedded quotes. Also validate column filters against catalog columns before query construction.

### [MEDIUM] Configured enabled types and confidence are not applied consistently
Affected: `src/anofox_pii.cpp:147`, `src/anofox_pii.cpp:169`, `src/anofox_pii.cpp:1627`, `src/anofox_pii.cpp:1778`, `src/anofox_pii.cpp:1875`, `src/anofox_pii.cpp:2727`, `src/anofox_pii.cpp:2730`, `src/anofox_pii.cpp:3231`

`anofox_pii_enabled_types` is described as controlling which PII types to detect, but `PIIEngine::Detect()` only honors an explicit `types` argument and the main scalar functions call `engine.Detect(text)` without consulting `PIIConfig`. `anofox_pii_min_confidence` is only applied in `pii_audit_table()`, while NER recognizers use hard-coded `0.7` thresholds and most scalar/table detection functions return all matches. This makes the configuration misleading and query results inconsistent across APIs.

Suggested improvement: pass a config snapshot into `Detect()` and apply enabled-type/confidence filtering in one central place:

```cpp
auto config = PIIConfig::Get().Snapshot();
auto matches = engine.Detect(text, config.EnabledTypes());
EraseIf(matches, [&](const PIIMatch &m) {
    return m.confidence < config.min_confidence;
});
```

### [MEDIUM] Overlapping matches can corrupt masking semantics
Affected: `src/anofox_pii.cpp:1627`, `src/anofox_pii.cpp:1650`, `src/anofox_pii.cpp:1726`, `src/anofox_pii.cpp:1734`

`Detect()` aggregates matches from every recognizer and only sorts by `start_pos`; it does not resolve overlaps or duplicates. Since patterns can overlap across recognizers, `Mask()` may apply multiple replacements over the same source span using stale offsets. This can produce surprising output, double-mask text, or let a lower-priority recognizer overwrite a better match.

Suggested improvement: define recognizer priority and normalize matches before returning or masking. For example, sort by start position, then longer length or higher priority/confidence, and discard overlaps:

```cpp
std::sort(matches.begin(), matches.end(), ByStartThenPriority);
std::vector<PIIMatch> non_overlapping;
size_t covered_until = 0;
for (const auto &m : matches) {
    if (m.start_pos >= covered_until) {
        non_overlapping.push_back(m);
        covered_until = m.end_pos;
    }
}
```

### [MEDIUM] Table functions materialize entire scans before producing output
Affected: `src/anofox_pii.cpp:2654`, `src/anofox_pii.cpp:2742`, `src/anofox_pii.cpp:2891`, `src/anofox_pii.cpp:2962`, `src/anofox_pii.cpp:2977`

`pii_audit_table()` and `pii_scan_table()` scan all selected columns on the first call and store all results in `state.results` before returning any rows. Large tables or high-match columns can consume substantial memory, delay first output, and bypass DuckDB’s normal streaming execution model. `pii_audit_table()` is especially expensive because each match stores the full original and masked value.

Suggested improvement: stream work through table-function state: keep the active column, query result, chunk, row offset, and pending output rows, and emit up to `STANDARD_VECTOR_SIZE` rows per invocation. For scan summaries, aggregate incrementally but avoid retaining per-match original values.

### [LOW] Several `std::isdigit`/`std::isalpha` calls have undefined behavior for negative `char`
Affected: `src/anofox_pii.cpp:311`, `src/anofox_pii.cpp:341`, `src/anofox_pii.cpp:374`, `src/anofox_pii.cpp:408`, `src/anofox_pii.cpp:457`, `src/anofox_pii.cpp:514`, `src/anofox_pii.cpp:560`

Multiple recognizers call C character-classification functions with plain `char`. For non-ASCII bytes where `char` is signed, passing a negative value other than `EOF` is undefined behavior. Some functions already use the correct `static_cast<unsigned char>` pattern, so this is mostly consistency debt.

Suggested improvement:

```cpp
if (std::isdigit(static_cast<unsigned char>(c))) {
    clean += c;
}
```

## Quick wins
- Apply `static_cast<unsigned char>` consistently for every `std::isdigit`, `std::isalpha`, `std::tolower`, and `std::toupper` call.
- Replace the mutable Base58 `unordered_map` with a thread-safe static lookup array.
- Centralize match filtering for enabled types, confidence threshold, and overlap resolution in `PIIEngine::Detect()`.
- Escape or avoid SQL identifier string construction in `pii_scan_table()` and `pii_audit_table()`.
- Refactor repeated `pii_detect_*` functions through one helper that writes LIST/STRUCT vectors directly instead of constructing `Value::LIST` per row.
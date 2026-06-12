## Summary
The phonenumber module is small and memory-safe in the narrow sense, with no obvious raw-resource leaks and reasonable use of DuckDB vectors for manual row loops. The main correctness risk is semantic: parsing treats many national numbers as international before considering the region hint, and shared country codes such as NANPA are collapsed to the first configured region. The module also keeps default-region state in a process-wide singleton even though DuckDB can execute queries concurrently and has scoped settings. Performance will degrade on large inputs because regexes are compiled repeatedly per row and often more than once per function call.

## Findings

### [HIGH] Process-wide default region can produce inconsistent results across sessions and parallel queries
Affected: `src/anofox_phonenumber.cpp:504`, `src/anofox_phonenumber.cpp:511`, `src/anofox_phonenumber.cpp:560`, `src/anofox_phonenumber.cpp:971`

`PhoneNumberManager` stores `default_region` in a global singleton and the SET callback writes directly to it. The mutex prevents a C++ data race, but it does not give DuckDB query-level consistency: a concurrent `SET anofox_tab_phonenumber_default_region = ...` can change results while another query is processing chunks with `NULL` region hints. The scalar functions are also marked `FunctionStability::CONSISTENT`, which is misleading because results depend on mutable global state outside the function arguments.

Suggested improvement: avoid storing the setting in the singleton. Read/snapshot the DuckDB setting through bind/local function data and pass that region into the parser for the whole query, or make the default region an explicit argument-only concern. At minimum, validate and snapshot the current value once per function invocation/chunk and reconsider `FunctionStability`.

```cpp
struct PhoneBindData : FunctionData {
    string default_region;
    unique_ptr<FunctionData> Copy() const override { return make_uniq<PhoneBindData>(*this); }
    bool Equals(const FunctionData &other) const override { return true; }
};
```

### [HIGH] Region hints are ignored for shared country codes
Affected: `src/anofox_phonenumber.cpp:214`, `src/include/anofox_phonenumber_metadata.hpp:63`, `src/anofox_phonenumber_metadata.cpp:8`

After extracting country code `1`, parsing always calls `GetMainRegionForCountryCode`, which returns the first region in the metadata list. For NANPA numbers this means Canadian numbers parse as `US` even when the caller passes `CA`; `phonenumber_is_valid_for_region('+1 5062345678', 'CA')` will fail because `parts.region_code` becomes `US`. The metadata explicitly models `{1, {"US", "CA"}}`, but the parser never uses the region hint to disambiguate.

Suggested improvement: when a country code maps to multiple regions, prefer a region hint whose metadata has the same country code, then fall back by trying each region's length and type patterns.

```cpp
auto regions = COUNTRY_CODE_TO_REGIONS.at(country_code);
if (!region_hint.empty()) {
    auto hint = StringUtil::Upper(region_hint);
    if (std::find(regions.begin(), regions.end(), hint) != regions.end()) {
        region_code = hint;
    }
}
```

### [MEDIUM] National numbers are parsed as international numbers before using the region hint
Affected: `src/anofox_phonenumber.cpp:52`, `src/anofox_phonenumber.cpp:177`, `src/anofox_phonenumber.cpp:180`

`ExtractCountryCode` attempts to consume a 1-3 digit country code even when the normalized input does not start with `+`. That makes valid national numbers fail or be misclassified if their leading digits match a known country code. For example, a US national number in area code `442` can be treated as country code `44`, fail GB length validation, and never fall back to the explicit `US` region.

Suggested improvement: only extract a country code unconditionally for explicit international formats, e.g. `+...` or a recognized international dialing prefix. For plain national input, select metadata from `region_hint` or the default region first and strip only that region's national prefix.

```cpp
if (!normalized.empty() && normalized[0] == '+') {
    std::tie(country_code, national_number) = ExtractCountryCode(normalized);
} else {
    // interpret as national using region_hint/default_region
}
```

### [MEDIUM] Regexes are compiled per row and often multiple times per row
Affected: `src/anofox_phonenumber.cpp:96`, `src/anofox_phonenumber.cpp:108`, `src/anofox_phonenumber.cpp:113`, `src/anofox_phonenumber.cpp:374`, `src/anofox_phonenumber.cpp:379`

`DeterminePhoneNumberType` constructs up to three `std::regex` objects for every parsed number, and `IsValid` constructs fixed/mobile regexes again after `Parse` already did type detection. On large DuckDB vectors this creates substantial CPU and heap churn. `std::regex` compilation is far more expensive than matching and should not be in the per-row hot path.

Suggested improvement: precompile patterns once per region, either by storing `std::regex` members alongside metadata or by building an immutable region-pattern cache with `std::call_once`. Also let `Parse` expose enough classification detail for `IsValid` to avoid matching the same patterns twice.

### [LOW] Invalid format options silently become NATIONAL and formatting failures return the input
Affected: `src/anofox_phonenumber.cpp:529`, `src/anofox_phonenumber.cpp:669`, `src/anofox_phonenumber.cpp:674`

`ParseFormatOption` maps any unknown format string to `NATIONAL`, and `PhoneFormatFunction` catches all `std::exception` values and returns the original number. This hides typos like `'INTERNATIONL'`, invalid inputs, and unexpected internal errors behind a plausible-looking non-NULL result. It makes bad data harder to detect in SQL pipelines.

Suggested improvement: throw `InvalidInputException` for unknown format strings and decide explicitly whether invalid phone numbers should return `NULL` or raise. Avoid catching broad `std::exception` unless the function contract is documented as best-effort normalization.

### [LOW] Default region values are not validated
Affected: `src/anofox_phonenumber.cpp:504`, `src/anofox_phonenumber.cpp:560`

`SetDefaultRegion` uppercases and stores any string, including unsupported or malformed regions. After `SET anofox_tab_phonenumber_default_region='ZZ'`, null-region parsing silently starts failing for otherwise valid local numbers. The status function will report the invalid setting as if it were usable.

Suggested improvement: validate the region against `REGION_METADATA` in the SET callback and reject unsupported values.

```cpp
auto region = StringUtil::Upper(parameter.ToString());
if (!phonenumber::GetMetadataForRegion(region)) {
    throw InvalidInputException("Unsupported phone region: %s", region);
}
```

## Quick wins
- Add regression tests for `CA`/NANPA parsing, `phonenumber_is_valid_for_region('+1 ...', 'CA')`, and national numbers whose leading digits match known country codes.
- Precompile fixed/mobile/toll-free regexes once per region and reuse them from both `Parse` and `IsValid`.
- Validate `anofox_tab_phonenumber_default_region` on `SET`.
- Make `ParseFormatOption` reject unknown format strings instead of defaulting to `NATIONAL`.
- Update README/API wording that still describes this as libphonenumber-backed if the custom implementation is now intentional.
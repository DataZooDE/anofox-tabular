## Summary
The VAT module is compact and mostly read-only after initialization, so it is unlikely to have resource leaks or obvious thread-safety issues under parallel DuckDB execution. The largest correctness risk is that exposed “valid” and “format” functions do not implement the behavior their names imply: `vat_format` is a public stub, and `vat_is_valid` performs syntax-only validation while the model already tracks checksum capability. The implementation also leaves several edge cases around VAT/ISO country aliases and case normalization. Performance is acceptable for small inputs, but repeated regex construction and per-row string materialization will become expensive on DuckDB-sized vectors.

## Findings

### [HIGH] `vat_format` always returns `NULL`
Affected file:line references: `src/anofox_vat.cpp:128`, `src/anofox_vat.cpp:140`, `src/anofox_vat.cpp:150`, `src/anofox_vat.cpp:151`, `src/anofox_vat.cpp:286`

`anofox_tab_vat_format` is registered as a public scalar function, but the implementation ignores both non-null arguments and sets every non-null row to `NULL`. This makes the function unusable and can silently corrupt downstream transformations that expect formatted VAT strings.

Suggested improvement: either remove/avoid registering the function until implemented, or implement the advertised styles by reusing `SplitVAT`, `ConvertISOToVAT`, and `SetStringResult`.

```cpp
IterateTwoStringInputs(args, result, [&](const std::string &vat, const std::string &style, size_t idx) {
  auto parsed = registry.SplitVAT(vat);
  if (!parsed) {
    FlatVector::SetNull(result, idx, true);
    return;
  }
  auto [country, digits] = *parsed;
  auto normalized_style = StringUtil::Lower(style);
  if (normalized_style == "plain") {
    SetStringResult(result, idx, digits);
  } else if (normalized_style == "iso") {
    SetStringResult(result, idx, registry.ConvertISOToVAT(country) + digits);
  } else {
    throw InvalidInputException("Unsupported VAT format style: %s", style);
  }
});
```

### [HIGH] `vat_is_valid` does not perform checksum validation
Affected file:line references: `src/anofox_vat_country.cpp:25`, `src/anofox_vat_country.cpp:57`, `src/include/anofox_vat.hpp:141`, `src/include/anofox_vat.hpp:143`, `src/anofox_vat.cpp:161`, `src/anofox_vat.cpp:171`, `src/anofox_vat.cpp:172`

The country table models `has_checksum`, and most countries are marked as having checksum support, but `anofox_tab_vat_is_valid` only calls `IsValidSyntax`. That means values with a valid shape but invalid check digits will be reported as valid. For a function named `vat_is_valid`, this is a significant false-positive risk.

Suggested improvement: split the API explicitly into syntax validation and full validation. Add checksum implementations for countries where `has_checksum` is true, and make `VATIsValidFunc` call both syntax and checksum checks.

```cpp
if (!registry.IsValidSyntax(country, digits)) {
  SetBoolResult(result, idx, false);
  return;
}
SetBoolResult(result, idx, registry.IsValidChecksum(country, digits));
```

### [MEDIUM] Country utility functions reject VAT aliases and lowercase country codes
Affected file:line references: `src/anofox_vat_country.cpp:15`, `src/anofox_vat_country.cpp:18`, `src/anofox_vat_country.cpp:60`, `src/anofox_vat_country.cpp:64`, `src/anofox_vat_country.cpp:72`, `src/anofox_vat.cpp:41`, `src/anofox_vat.cpp:106`, `src/anofox_vat.cpp:115`

`SplitVAT` accepts lowercase VAT strings and converts VAT prefixes such as `EL` to `GR` and `XI` to `GB`, but the country-code utilities perform direct map lookups. As a result, `anofox_tab_is_valid_vat_country('de')`, `anofox_tab_is_valid_vat_country('EL')`, and `anofox_tab_vat_is_eu_member('EL')` return false even though those are natural inputs for VAT users.

Suggested improvement: centralize country-code normalization and use it in all country-code entry points.

```cpp
std::string VATRegistry::NormalizeCountryCode(const std::string &code) const {
  auto normalized = NormalizeVAT(code);
  return ConvertVATToISO(normalized);
}

bool VATRegistry::IsValidCountry(const std::string &code) const {
  return countries_.find(NormalizeCountryCode(code)) != countries_.end();
}
```

### [MEDIUM] `std::isalnum` and `std::toupper` have undefined behavior for negative `char`
Affected file:line references: `src/anofox_vat_country.cpp:95`, `src/anofox_vat_country.cpp:98`, `src/anofox_vat_country.cpp:99`, `src/anofox_vat_country.cpp:100`

`NormalizeVAT` passes a plain `char` to `<cctype>` functions. If `char` is signed and the input contains bytes with the high bit set, this is undefined behavior unless the value is converted to `unsigned char` or is `EOF`.

Suggested improvement: cast before calling `<cctype>`, and cast the result back to `char`.

```cpp
for (unsigned char c : input) {
  if (std::isalnum(c)) {
    result += static_cast<char>(std::toupper(c));
  }
}
```

### [MEDIUM] Regex patterns are compiled once per row
Affected file:line references: `src/anofox_vat_country.cpp:80`, `src/anofox_vat_country.cpp:87`, `src/anofox_vat_country.cpp:88`, `src/anofox_vat_country.cpp:89`

`IsValidSyntax` constructs a new `std::regex` for every row. In DuckDB scalar execution this is expensive, especially for large vectors or repeated calls over a table, and the patterns are static after registry initialization.

Suggested improvement: precompile anchored regexes during `InitializeCountries` or the `VATRegistry` constructor and store them in `CountryInfo`.

```cpp
struct CountryInfo {
  std::string country_code;
  std::string country_name;
  std::string pattern;
  std::regex compiled_pattern;
  bool is_eu_member;
  bool has_checksum;
};
```

### [LOW] Manual vector loops copy every input string
Affected file:line references: `src/include/anofox_vat.hpp:30`, `src/include/anofox_vat.hpp:47`, `src/include/anofox_vat.hpp:57`, `src/include/anofox_vat.hpp:80`, `src/include/anofox_vat.hpp:81`

The generic iterators convert every `string_t` to `std::string` before invoking the operation. Several operations only need to inspect the string or normalize it once, so this forces avoidable per-row allocation and copying.

Suggested improvement: pass `string_t` or `std::string_view` into the lambda and only allocate when normalization or result construction requires it. For simple boolean functions, consider `UnaryExecutor::Execute<string_t, bool>` or `BinaryExecutor` patterns so DuckDB can preserve constant/dictionary behavior more efficiently.

## Quick wins

- Implement or unregister `anofox_tab_vat_format`; avoid shipping a public function that always returns `NULL`.
- Add a `NormalizeCountryCode` helper and use it in `IsValidCountry`, `IsEUMember`, and `GetCountryName`.
- Fix `<cctype>` calls by casting to `unsigned char`.
- Precompile VAT regexes once in the registry instead of per row.
- Reserve `NormalizeVAT` output capacity with `result.reserve(input.size())`.
- Add non-skipped tests for lowercase country utility inputs, `EL`/`XI` aliases, invalid checksum examples, and real `vat_format` outputs.
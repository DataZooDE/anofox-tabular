#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace duckdb {

struct VAT {
  std::string country;  // ISO country code (AT, BE, BG, etc.)
  std::string digits;   // Normalized VAT digits without country prefix
};

// ============================================================================
// Generic Single-Input String Iterator
// ============================================================================
// Handles all boilerplate: vector format conversion, validity checking, loop,
// and result verification. The operation lambda/function receives each valid
// input as a std::string_view into the input vector (no per-row std::string
// copy) and the output index.
//
// Usage:
//   IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
//       auto value = registry.DoSomething(input);
//       SetBoolResult(result, idx, value);
//   });
template <typename Operation>
inline void IterateSingleStringInput(DataChunk& args, Vector& result,
                                     Operation op) {
  auto& input_vec = args.data[0];
  size_t count = args.size();

  UnifiedVectorFormat idata;
  input_vec.ToUnifiedFormat(count, idata);
  auto input_strings = UnifiedVectorFormat::GetData<string_t>(idata);

  for (size_t i = 0; i < count; i++) {
    size_t idx = idata.sel->get_index(i);
    if (!idata.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    auto& input = input_strings[idx];
    op(std::string_view(input.GetData(), input.GetSize()), i);
  }

  result.Verify(count);
}

// ============================================================================
// Generic Two-Input String Iterator (for functions with 2 string inputs)
// ============================================================================
template <typename Operation>
inline void IterateTwoStringInputs(DataChunk& args, Vector& result,
                                   Operation op) {
  auto& input1_vec = args.data[0];
  auto& input2_vec = args.data[1];
  size_t count = args.size();

  UnifiedVectorFormat idata1, idata2;
  input1_vec.ToUnifiedFormat(count, idata1);
  input2_vec.ToUnifiedFormat(count, idata2);
  auto input1_strings = UnifiedVectorFormat::GetData<string_t>(idata1);
  auto input2_strings = UnifiedVectorFormat::GetData<string_t>(idata2);

  for (size_t i = 0; i < count; i++) {
    size_t idx1 = idata1.sel->get_index(i);
    size_t idx2 = idata2.sel->get_index(i);

    if (!idata1.validity.RowIsValid(idx1) ||
        !idata2.validity.RowIsValid(idx2)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    auto& input1 = input1_strings[idx1];
    auto& input2 = input2_strings[idx2];
    op(std::string_view(input1.GetData(), input1.GetSize()),
       std::string_view(input2.GetData(), input2.GetSize()), i);
  }

  result.Verify(count);
}

// ============================================================================
// Output Result Setters
// ============================================================================

// Set a boolean result value
inline void SetBoolResult(Vector& result, size_t idx, bool value) {
  auto result_data = FlatVector::GetData<bool>(result);
  result_data[idx] = value;
}

// Set a string result value
inline void SetStringResult(Vector& result, size_t idx,
                           const std::string& value) {
  auto result_strings = FlatVector::GetData<string_t>(result);
  result_strings[idx] = StringVector::AddString(result, value);
}

// Set struct result (country, digits)
inline void SetVATStructResult(Vector& result, size_t idx,
                               const std::string& country,
                               const std::string& digits) {
  auto& struct_entries = StructVector::GetEntries(result);
  auto& country_vec = *struct_entries[0];
  auto& digits_vec = *struct_entries[1];
  auto country_strings = FlatVector::GetData<string_t>(country_vec);
  auto digits_strings = FlatVector::GetData<string_t>(digits_vec);

  country_strings[idx] = StringVector::AddString(country_vec, country);
  digits_strings[idx] = StringVector::AddString(digits_vec, digits);
}

class VATRegistry {
 public:
  static VATRegistry& Instance();

  bool IsValidCountry(std::string_view code) const;
  bool IsEUMember(std::string_view code) const;
  std::string GetCountryName(std::string_view code) const;
  bool IsValidSyntax(const std::string& country, const std::string& digits) const;
  // Validates the country-specific check digits. Returns true for countries
  // without an implemented checksum (syntax-only validation).
  bool IsValidChecksum(const std::string& country, const std::string& digits) const;
  std::string NormalizeVAT(std::string_view input) const;
  // Uppercases the code and resolves VAT prefixes (EL -> GR, XI -> GB) to ISO.
  std::string NormalizeCountryCode(std::string_view code) const;
  std::optional<std::pair<std::string, std::string>> SplitVAT(
      std::string_view input) const;
  std::string ConvertVATToISO(const std::string& vat_country) const;
  std::string ConvertISOToVAT(const std::string& iso_country) const;

 private:
  VATRegistry();
  VATRegistry(const VATRegistry&) = delete;
  VATRegistry& operator=(const VATRegistry&) = delete;

  struct CountryInfo {
    std::string country_code;
    std::string country_name;
    std::string pattern;  // Regex pattern without country prefix
    bool is_eu_member;
    bool has_checksum;  // Whether a check-digit algorithm is implemented
    std::regex compiled_pattern;  // Anchored pattern, compiled at registry init
  };

  std::unordered_map<std::string, CountryInfo> countries_;
  std::unordered_map<std::string, std::string> vat_to_iso_;
  std::unordered_map<std::string, std::string> iso_to_vat_;

  void InitializeCountries();
};

// Extension registration functions
void RegisterVATOptions(ExtensionLoader& loader);
void RegisterVATFunctions(ExtensionLoader& loader);

}  // namespace duckdb

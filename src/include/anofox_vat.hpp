#pragma once

#include "duckdb.hpp"
#include <string>
#include <memory>
#include <optional>

namespace duckdb {

struct VAT {
  std::string country;  // ISO country code (AT, BE, BG, etc.)
  std::string digits;   // Normalized VAT digits without country prefix
};

class VATRegistry {
 public:
  static VATRegistry& Instance();

  bool IsValidCountry(const std::string& code) const;
  bool IsEUMember(const std::string& code) const;
  std::string GetCountryName(const std::string& code) const;
  bool IsValidSyntax(const std::string& country, const std::string& digits) const;
  std::string NormalizeVAT(const std::string& input) const;
  std::optional<std::pair<std::string, std::string>> SplitVAT(
      const std::string& input) const;
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
    bool has_checksum;  // Whether checksum validation is supported
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

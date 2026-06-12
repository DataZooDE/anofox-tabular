#include "anofox_vat.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace duckdb {

VATRegistry& VATRegistry::Instance() {
  static VATRegistry instance;
  return instance;
}

VATRegistry::VATRegistry() {
  InitializeCountries();
  vat_to_iso_["EL"] = "GR";
  vat_to_iso_["XI"] = "GB";
  iso_to_vat_["GR"] = "EL";
  iso_to_vat_["GB"] = "XI";
  // Precompile anchored syntax patterns once; IsValidSyntax runs per row.
  for (auto& entry : countries_) {
    entry.second.compiled_pattern =
        std::regex("^" + entry.second.pattern + "$",
                   std::regex::ECMAScript | std::regex::optimize);
  }
}

void VATRegistry::InitializeCountries() {
  // All 29 countries: 28 EU + UK
  // Format: country_code, country_name, pattern (without country prefix), is_eu_member,
  // has_checksum
  //
  // has_checksum is only set for countries whose check-digit algorithm is
  // implemented (and verified with known examples) in anofox_vat_checksum.cpp.
  // All other countries are validated by syntax only.
  countries_["AT"] = {"AT", "Austria", "U[0-9]{8}", true, true};
  countries_["BE"] = {"BE", "Belgium", "[0-1][0-9]{9}", true, true};
  countries_["BG"] = {"BG", "Bulgaria", "[0-9]{9,10}", true, false};
  countries_["CY"] = {"CY", "Cyprus", "(?!12)[0-69][0-9]{7}[A-Z]", true, false};
  countries_["CZ"] = {"CZ", "Czech Republic", "[0-9]{8,10}", true, false};
  countries_["DE"] = {"DE", "Germany", "[0-9]{9}", true, true};
  countries_["DK"] = {"DK", "Denmark", "[0-9]{8}", true, true};
  countries_["EE"] = {"EE", "Estonia", "10[0-9]{7}", true, false};
  countries_["ES"] = {"ES", "Spain",
                      "([A-Z][0-9]{8}|[0-9]{8}[A-Z]|[A-Z][0-9]{7}[A-Z])",
                      true, true};
  countries_["FI"] = {"FI", "Finland", "[0-9]{8}", true, true};
  countries_["FR"] = {"FR", "France", "[A-HJ-NP-Z0-9]{2}[0-9]{9}", true, true};
  countries_["GB"] = {"GB", "United Kingdom",
                      "([0-9]{9}|[0-9]{12}|(HA|GD)[0-9]{3})", false, false};
  countries_["GR"] = {"GR", "Greece", "[0-9]{9}", true, true};
  countries_["HR"] = {"HR", "Croatia", "[0-9]{11}", true, false};
  countries_["HU"] = {"HU", "Hungary", "[0-9]{8}", true, false};
  countries_["IE"] = {"IE", "Ireland", "([0-9][A-Z][0-9]{5}|[0-9]{7}[A-Z]?)[A-Z]",
                      true, true};
  countries_["IT"] = {"IT", "Italy", "[0-9]{11}", true, true};
  countries_["LT"] = {"LT", "Lithuania", "([0-9]{7}1[0-9]|[0-9]{10}1[0-9])",
                      true, false};
  countries_["LU"] = {"LU", "Luxembourg", "[0-9]{8}", true, true};
  countries_["LV"] = {"LV", "Latvia", "[0-9]{11}", true, false};
  countries_["MT"] = {"MT", "Malta", "[0-9]{8}", true, false};
  countries_["NL"] = {"NL", "Netherlands", "[0-9]{9}B[0-9]{2}", true, true};
  countries_["PL"] = {"PL", "Poland", "[0-9]{10}", true, true};
  countries_["PT"] = {"PT", "Portugal", "[0-9]{9}", true, true};
  countries_["RO"] = {"RO", "Romania", "[1-9][0-9]{1,9}", true, false};
  countries_["SE"] = {"SE", "Sweden", "[0-9]{10}01", true, true};
  countries_["SI"] = {"SI", "Slovenia", "[0-9]{8}", true, true};
  countries_["SK"] = {"SK", "Slovakia", "[0-9]{10}", true, false};
}

bool VATRegistry::IsValidCountry(std::string_view code) const {
  return countries_.find(NormalizeCountryCode(code)) != countries_.end();
}

bool VATRegistry::IsEUMember(std::string_view code) const {
  auto it = countries_.find(NormalizeCountryCode(code));
  if (it == countries_.end()) {
    return false;
  }
  return it->second.is_eu_member;
}

std::string VATRegistry::GetCountryName(std::string_view code) const {
  auto it = countries_.find(NormalizeCountryCode(code));
  if (it == countries_.end()) {
    return "";
  }
  return it->second.country_name;
}

bool VATRegistry::IsValidSyntax(const std::string& country,
                                const std::string& digits) const {
  auto it = countries_.find(country);
  if (it == countries_.end()) {
    return false;
  }
  return std::regex_match(digits, it->second.compiled_pattern);
}

std::string VATRegistry::NormalizeVAT(std::string_view input) const {
  std::string result;
  result.reserve(input.size());
  // Convert to uppercase and remove spaces, punctuation
  for (unsigned char c : input) {
    if (std::isalnum(c)) {
      result += static_cast<char>(std::toupper(c));
    }
  }
  return result;
}

std::string VATRegistry::NormalizeCountryCode(std::string_view code) const {
  return ConvertVATToISO(NormalizeVAT(code));
}

std::optional<std::pair<std::string, std::string>> VATRegistry::SplitVAT(
    std::string_view input) const {
  if (input.length() < 2) {
    return std::nullopt;
  }

  std::string normalized = NormalizeVAT(input);
  if (normalized.length() < 2) {
    return std::nullopt;
  }

  std::string country_code = normalized.substr(0, 2);
  std::string iso_country = ConvertVATToISO(country_code);

  if (!IsValidCountry(iso_country)) {
    return std::nullopt;
  }

  std::string digits = normalized.substr(2);
  return std::make_pair(iso_country, digits);
}

std::string VATRegistry::ConvertVATToISO(
    const std::string& vat_country) const {
  auto it = vat_to_iso_.find(vat_country);
  if (it != vat_to_iso_.end()) {
    return it->second;
  }
  return vat_country;
}

std::string VATRegistry::ConvertISOToVAT(
    const std::string& iso_country) const {
  auto it = iso_to_vat_.find(iso_country);
  if (it != iso_to_vat_.end()) {
    return it->second;
  }
  return iso_country;
}

}  // namespace duckdb

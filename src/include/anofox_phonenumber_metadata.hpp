#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <regex>

namespace duckdb {
namespace anofox {
namespace phonenumber {

// Phone number type constants
struct PhoneNumberType {
	static constexpr const char* FIXED_LINE = "fixed_line";
	static constexpr const char* MOBILE = "mobile";
	static constexpr const char* FIXED_LINE_OR_MOBILE = "fixed_line_or_mobile";
	static constexpr const char* TOLL_FREE = "toll_free";
	static constexpr const char* PREMIUM_RATE = "premium_rate";
	static constexpr const char* SHARED_COST = "shared_cost";
	static constexpr const char* VOIP = "voip";
	static constexpr const char* PERSONAL_NUMBER = "personal_number";
	static constexpr const char* PAGER = "pager";
	static constexpr const char* UAN = "uan";
	static constexpr const char* VOICEMAIL = "voicemail";
	static constexpr const char* UNKNOWN = "unknown";
};

// Metadata for a single country/region
struct CountryMetadata {
	int country_code;
	std::string region_code;
	std::vector<int> valid_lengths;          // Valid national number lengths
	std::string national_prefix;             // E.g., "0" for GB, empty for US
	std::string international_prefix;        // E.g., "011" for US, "00" for GB
	std::string example_number;              // Example national number
	std::string national_format_pattern;     // Pattern for national formatting

	// Regex patterns for different phone number types (stored as strings, compiled on first use)
	std::string fixed_line_pattern;
	std::string mobile_pattern;
	std::string toll_free_pattern;

	// Check if a length is valid for this country
	bool IsValidLength(size_t length) const {
		for (int valid_len : valid_lengths) {
			if (length == static_cast<size_t>(valid_len)) {
				return true;
			}
		}
		return false;
	}
};

// Global metadata maps (defined in anofox_phonenumber_metadata.cpp)
extern const std::unordered_map<int, std::vector<std::string>> COUNTRY_CODE_TO_REGIONS;
extern const std::unordered_map<std::string, CountryMetadata> REGION_METADATA;
extern const std::unordered_set<int> NANPA_CODES;

// Helper functions
inline const CountryMetadata* GetMetadataForRegion(const std::string& region_code) {
	auto it = REGION_METADATA.find(region_code);
	if (it != REGION_METADATA.end()) {
		return &it->second;
	}
	return nullptr;
}

inline std::string GetMainRegionForCountryCode(int country_code) {
	auto it = COUNTRY_CODE_TO_REGIONS.find(country_code);
	if (it != COUNTRY_CODE_TO_REGIONS.end() && !it->second.empty()) {
		return it->second[0];  // Return first region (main region)
	}
	return "";
}

inline bool IsNANPACountry(const std::string& region_code) {
	auto metadata = GetMetadataForRegion(region_code);
	if (metadata) {
		return NANPA_CODES.find(metadata->country_code) != NANPA_CODES.end();
	}
	return false;
}

} // namespace phonenumber
} // namespace anofox
} // namespace duckdb

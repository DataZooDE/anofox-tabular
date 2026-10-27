#pragma once

#include <optional>
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

// One layout rule for a region, modelled on libphonenumber's <numberFormat>.
// The first rule whose leading_digits match the start of the national
// significant number (NSN) and whose pattern matches the whole NSN wins.
struct NumberFormat {
	std::string leading_digits;       // Regex matched at the start of the NSN; empty matches any NSN
	std::string pattern;              // Regex over the whole NSN with one capture group per block
	std::string national_format;      // NATIONAL layout incl. the national prefix, e.g. "0$1 $2"
	std::string international_format; // INTERNATIONAL layout after "+<cc> ", e.g. "$1 $2"
};

// Metadata for a single country/region
struct CountryMetadata {
	int country_code;
	std::string region_code;
	std::vector<int> valid_lengths;          // Possible national significant number lengths
	std::string national_prefix;             // E.g., "0" for GB, "1" for US, empty for IT
	std::string international_prefix;        // E.g., "011" for US, "00" for GB
	std::string example_number;              // Example national number

	// Regex patterns for different phone number types (stored as strings, compiled on first use)
	std::string fixed_line_pattern;
	std::string mobile_pattern;
	std::string toll_free_pattern;

	// Layout rules for NATIONAL, INTERNATIONAL and RFC3966 output, tried in order
	std::vector<NumberFormat> number_formats;

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

// Precompiled regex patterns for a region. Compiled once on first use and
// shared across all rows/threads (read-only after construction).
struct CompiledNumberFormat {
	std::optional<std::regex> leading_digits; // nullopt matches any NSN
	std::regex pattern;
	const NumberFormat *format;
};

struct CompiledRegionPatterns {
	std::optional<std::regex> fixed_line;
	std::optional<std::regex> mobile;
	std::optional<std::regex> toll_free;
	std::vector<CompiledNumberFormat> number_formats;
};

// Global metadata maps (defined in anofox_phonenumber_metadata.cpp)
extern const std::unordered_map<int, std::vector<std::string>> COUNTRY_CODE_TO_REGIONS;
extern const std::unordered_map<std::string, CountryMetadata> REGION_METADATA;
extern const std::unordered_set<int> NANPA_CODES;

// Returns the precompiled patterns for a region, or nullptr if the region is
// unknown. The underlying map is built once (thread-safe magic static).
const CompiledRegionPatterns *GetCompiledPatternsForRegion(const std::string &region_code);

// Returns the first layout rule of the region that applies to the national
// significant number, or nullptr when none does (callers then emit the NSN as is).
const NumberFormat *FindNumberFormat(const std::string &region_code, const std::string &national_number,
                                     std::smatch &match);

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

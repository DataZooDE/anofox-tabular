#include "anofox_phonenumber_metadata.hpp"

namespace duckdb {
namespace anofox {
namespace phonenumber {

// Country code to regions mapping
// Each country code can map to multiple regions (e.g., 1 -> US, CA, etc.)
const std::unordered_map<int, std::vector<std::string>> COUNTRY_CODE_TO_REGIONS = {
	{1, {"US", "CA"}},    // North American Numbering Plan
	{44, {"GB"}},         // United Kingdom
	{49, {"DE"}},         // Germany
	{33, {"FR"}},         // France
	{39, {"IT"}},         // Italy
	{34, {"ES"}},         // Spain
	{61, {"AU"}},         // Australia
	{81, {"JP"}},         // Japan
	{86, {"CN"}},         // China
	{91, {"IN"}},         // India
	{55, {"BR"}},         // Brazil
	{52, {"MX"}},         // Mexico
	{54, {"AR"}},         // Argentina
	{7, {"RU"}},          // Russia
	{31, {"NL"}},         // Netherlands
	{46, {"SE"}},         // Sweden
	{47, {"NO"}},         // Norway
	{45, {"DK"}},         // Denmark
	{358, {"FI"}},        // Finland
	{41, {"CH"}},         // Switzerland
};

// North American Numbering Plan (NANPA) country codes
const std::unordered_set<int> NANPA_CODES = {1};

// Region metadata
const std::unordered_map<std::string, CountryMetadata> REGION_METADATA = {
	// United States (US)
	{"US", {
		1,                                    // country_code
		"US",                                 // region_code
		{10},                                 // valid_lengths (10-digit national numbers)
		"",                                   // national_prefix (empty for US)
		"011",                                // international_prefix
		"2015550123",                         // example_number
		"($1) $2-$3",                         // national_format_pattern (for formatting)
		// Pattern for US phone numbers: Area code 2-9, next digit 0-8, then 7 more digits
		"[2-9]\\d{2}[2-9]\\d{6}",            // fixed_line_pattern
		"[2-9]\\d{2}[2-9]\\d{6}",            // mobile_pattern (same as fixed_line in US)
		"8[0-8]\\d{8}",                      // toll_free_pattern (800, 888, etc.)
	}},

	// Canada (CA) - Same as US for NANPA
	{"CA", {
		1,                                    // country_code
		"CA",                                 // region_code
		{10},                                 // valid_lengths
		"",                                   // national_prefix
		"011",                                // international_prefix
		"5062345678",                         // example_number
		"($1) $2-$3",                         // national_format_pattern
		"[2-9]\\d{2}[2-9]\\d{6}",            // fixed_line_pattern
		"[2-9]\\d{2}[2-9]\\d{6}",            // mobile_pattern
		"8[0-8]\\d{8}",                      // toll_free_pattern
	}},

	// United Kingdom (GB)
	{"GB", {
		44,                                   // country_code
		"GB",                                 // region_code
		{10},                                 // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1212345678",                         // example_number
		"$1 $2 $3",                           // national_format_pattern
		"2\\d{9}|3\\d{9}|[58]\\d{9}",        // fixed_line_pattern (020, 030, 08, 05)
		"7\\d{9}",                            // mobile_pattern (07xxx)
		"80\\d{8}",                           // toll_free_pattern (0800)
	}},

	// Germany (DE)
	{"DE", {
		49,                                   // country_code
		"DE",                                 // region_code
		{10, 11},                             // valid_lengths (10 or 11 digits)
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"30123456",                           // example_number
		"$1 $2",                              // national_format_pattern
		"[2-9]\\d{9,10}",                    // fixed_line_pattern
		"1[567]\\d{9}",                      // mobile_pattern (015x, 016x, 017x)
		"800\\d{7}",                         // toll_free_pattern (0800)
	}},

	// France (FR)
	{"FR", {
		33,                                   // country_code
		"FR",                                 // region_code
		{9},                                  // valid_lengths (9-digit national numbers)
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"123456789",                          // example_number
		"$1 $2 $3 $4 $5",                     // national_format_pattern (01 23 45 67 89)
		"[1-5]\\d{8}",                        // fixed_line_pattern (01-05)
		"[67]\\d{8}",                         // mobile_pattern (06, 07)
		"80\\d{7}",                           // toll_free_pattern (0800)
	}},

	// Italy (IT)
	{"IT", {
		39,                                   // country_code
		"IT",                                 // region_code
		{9, 10, 11},                          // valid_lengths
		"",                                   // national_prefix (Italy doesn't use a national prefix)
		"00",                                 // international_prefix
		"0212345678",                         // example_number
		"$1 $2 $3",                           // national_format_pattern
		"0\\d{9,10}",                         // fixed_line_pattern
		"3\\d{8,9}",                          // mobile_pattern
		"80\\d{7,9}",                         // toll_free_pattern
	}},

	// Spain (ES)
	{"ES", {
		34,                                   // country_code
		"ES",                                 // region_code
		{9},                                  // valid_lengths
		"",                                   // national_prefix
		"00",                                 // international_prefix
		"912345678",                          // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[89]\\d{8}",                         // fixed_line_pattern
		"[67]\\d{8}",                         // mobile_pattern
		"[89]00\\d{6}",                       // toll_free_pattern
	}},

	// Australia (AU)
	{"AU", {
		61,                                   // country_code
		"AU",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"0011",                               // international_prefix
		"212345678",                          // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[2378]\\d{8}",                       // fixed_line_pattern
		"4\\d{8}",                            // mobile_pattern
		"180[02]\\d{6}",                      // toll_free_pattern
	}},

	// Japan (JP)
	{"JP", {
		81,                                   // country_code
		"JP",                                 // region_code
		{9, 10},                              // valid_lengths
		"0",                                  // national_prefix
		"010",                                // international_prefix
		"312345678",                          // example_number
		"$1-$2-$3",                           // national_format_pattern
		"[3-5]\\d{8,9}",                      // fixed_line_pattern
		"[789]0\\d{8}",                       // mobile_pattern
		"120\\d{6}",                          // toll_free_pattern
	}},

	// China (CN)
	{"CN", {
		86,                                   // country_code
		"CN",                                 // region_code
		{10, 11, 12},                         // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1012345678",                         // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[2-8]\\d{9,11}",                     // fixed_line_pattern
		"1[3-9]\\d{9}",                       // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
	}},

	// India (IN)
	{"IN", {
		91,                                   // country_code
		"IN",                                 // region_code
		{10},                                 // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"2212345678",                         // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[2-7]\\d{9}",                        // fixed_line_pattern
		"[89]\\d{9}",                         // mobile_pattern
		"1800\\d{6,7}",                       // toll_free_pattern
	}},

	// Brazil (BR)
	{"BR", {
		55,                                   // country_code
		"BR",                                 // region_code
		{10, 11},                             // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1123456789",                         // example_number
		"($1) $2-$3",                         // national_format_pattern
		"[1-9][1-9]\\d{8}",                   // fixed_line_pattern
		"[1-9][6-9]\\d{8}",                   // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
	}},

	// Mexico (MX)
	{"MX", {
		52,                                   // country_code
		"MX",                                 // region_code
		{10, 11},                             // valid_lengths
		"01",                                 // national_prefix
		"00",                                 // international_prefix
		"5512345678",                         // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[2-9]\\d{9,10}",                     // fixed_line_pattern
		"1\\d{10}",                           // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
	}},

	// Argentina (AR)
	{"AR", {
		54,                                   // country_code
		"AR",                                 // region_code
		{10},                                 // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1123456789",                         // example_number
		"$1 $2-$3",                           // national_format_pattern
		"[2-9]\\d{9}",                        // fixed_line_pattern
		"9\\d{10}",                           // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
	}},

	// Russia (RU)
	{"RU", {
		7,                                    // country_code
		"RU",                                 // region_code
		{10},                                 // valid_lengths
		"8",                                  // national_prefix
		"810",                                // international_prefix
		"4951234567",                         // example_number
		"($1) $2-$3-$4",                      // national_format_pattern
		"[3489]\\d{9}",                       // fixed_line_pattern
		"9\\d{9}",                            // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
	}},

	// Netherlands (NL)
	{"NL", {
		31,                                   // country_code
		"NL",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"201234567",                          // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[1-58]\\d{8}",                       // fixed_line_pattern
		"6\\d{8}",                            // mobile_pattern
		"800\\d{6}",                          // toll_free_pattern
	}},

	// Sweden (SE)
	{"SE", {
		46,                                   // country_code
		"SE",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"812345678",                          // example_number
		"$1-$2 $3",                           // national_format_pattern
		"[1-9]\\d{8}",                        // fixed_line_pattern
		"7[0236]\\d{7}",                      // mobile_pattern
		"20\\d{7}",                           // toll_free_pattern
	}},

	// Norway (NO)
	{"NO", {
		47,                                   // country_code
		"NO",                                 // region_code
		{8},                                  // valid_lengths
		"",                                   // national_prefix
		"00",                                 // international_prefix
		"21234567",                           // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[2-79]\\d{7}",                       // fixed_line_pattern
		"[49]\\d{7}",                         // mobile_pattern
		"80[01]\\d{5}",                       // toll_free_pattern
	}},

	// Denmark (DK)
	{"DK", {
		45,                                   // country_code
		"DK",                                 // region_code
		{8},                                  // valid_lengths
		"",                                   // national_prefix
		"00",                                 // international_prefix
		"32123456",                           // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[2-9]\\d{7}",                        // fixed_line_pattern
		"[2-9]\\d{7}",                        // mobile_pattern (same as fixed_line in Denmark)
		"80\\d{6}",                           // toll_free_pattern
	}},

	// Finland (FI)
	{"FI", {
		358,                                  // country_code
		"FI",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"912345678",                          // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[1-35]\\d{8}",                       // fixed_line_pattern
		"4\\d{8}",                            // mobile_pattern
		"800\\d{6}",                          // toll_free_pattern
	}},

	// Switzerland (CH)
	{"CH", {
		41,                                   // country_code
		"CH",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"212345678",                          // example_number
		"$1 $2 $3",                           // national_format_pattern
		"[2-9]\\d{8}",                        // fixed_line_pattern
		"7[5-9]\\d{7}",                       // mobile_pattern
		"800\\d{6}",                          // toll_free_pattern
	}},
};

namespace {

std::optional<std::regex> CompilePattern(const std::string &pattern) {
	if (pattern.empty()) {
		return std::nullopt;
	}
	try {
		return std::regex(pattern, std::regex::optimize);
	} catch (const std::regex_error &) {
		// Invalid metadata pattern: treat as absent rather than failing per row.
		return std::nullopt;
	}
}

} // namespace

const CompiledRegionPatterns *GetCompiledPatternsForRegion(const std::string &region_code) {
	// Built exactly once on first use; magic statics make this thread-safe.
	static const auto compiled_patterns = [] {
		std::unordered_map<std::string, CompiledRegionPatterns> result;
		result.reserve(REGION_METADATA.size());
		for (const auto &entry : REGION_METADATA) {
			CompiledRegionPatterns patterns;
			patterns.fixed_line = CompilePattern(entry.second.fixed_line_pattern);
			patterns.mobile = CompilePattern(entry.second.mobile_pattern);
			patterns.toll_free = CompilePattern(entry.second.toll_free_pattern);
			result.emplace(entry.first, std::move(patterns));
		}
		return result;
	}();

	auto it = compiled_patterns.find(region_code);
	if (it != compiled_patterns.end()) {
		return &it->second;
	}
	return nullptr;
}

} // namespace phonenumber
} // namespace anofox
} // namespace duckdb

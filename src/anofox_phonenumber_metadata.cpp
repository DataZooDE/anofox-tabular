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
		"1",                                  // national_prefix (NANP trunk prefix: 1-212-555-0123)
		"011",                                // international_prefix
		"2015550123",                         // example_number
		// Pattern for US phone numbers: Area code 2-9, next digit 0-8, then 7 more digits
		"[2-9]\\d{2}[2-9]\\d{6}",            // fixed_line_pattern
		"[2-9]\\d{2}[2-9]\\d{6}",            // mobile_pattern (same as fixed_line in US)
		"8[0-8]\\d{8}",                      // toll_free_pattern (800, 888, etc.)
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"", "(\\d{3})(\\d{3})(\\d{4})", "($1) $2-$3", "$1 $2 $3"},
		},
	}},

	// Canada (CA) - Same as US for NANPA
	{"CA", {
		1,                                    // country_code
		"CA",                                 // region_code
		{10},                                 // valid_lengths
		"1",                                  // national_prefix (NANP trunk prefix: 1-416-555-0199)
		"011",                                // international_prefix
		"5062345678",                         // example_number
		"[2-9]\\d{2}[2-9]\\d{6}",            // fixed_line_pattern
		"[2-9]\\d{2}[2-9]\\d{6}",            // mobile_pattern
		"8[0-8]\\d{8}",                      // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"", "(\\d{3})(\\d{3})(\\d{4})", "($1) $2-$3", "$1 $2 $3"},
		},
	}},

	// United Kingdom (GB)
	{"GB", {
		44,                                   // country_code
		"GB",                                 // region_code
		{10},                                 // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1212345678",                         // example_number
		"[1-3]\\d{9}|[58]\\d{9}",             // fixed_line_pattern (01, 02, 03, 05, 08)
		"7\\d{9}",                            // mobile_pattern (07xxx)
		"80\\d{8}",                           // toll_free_pattern (0800)
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"2|5[56]|7(?:0|6[013-9])", "(\\d{2})(\\d{4})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
			{"1(?:[02-69]1|1)|3|8|9", "(\\d{3})(\\d{3})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
			{"[157]", "(\\d{4})(\\d{6})", "0$1 $2", "$1 $2"},
		},
	}},

	// Germany (DE)
	{"DE", {
		49,                                   // country_code
		"DE",                                 // region_code
		{5, 6, 7, 8, 9, 10, 11},              // valid_lengths (area code + subscriber number vary in length)
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"3012345678",                         // example_number (030 1234 5678, Berlin)
		// fixed_line_pattern: Berlin/Hamburg/Frankfurt/Munich (2-digit area code) take a
		// 5-8 digit subscriber number, other areas (3-5 digit area code) 6-11 digits in total
		"(?:30|40|69|89)\\d{5,8}|(?!30|40|69|89)[2-9]\\d{5,10}",
		"1(?:5\\d{9}|[67]\\d{8,9})",            // mobile_pattern (015x: 11 digits, 016x/017x: 10-11)
		"800\\d{7}",                         // toll_free_pattern (0800)
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"1[67]", "(\\d{3})(\\d{7,8})", "0$1 $2", "$1 $2"},
			{"15", "(\\d{4})(\\d{7})", "0$1 $2", "$1 $2"},
			{"800", "(\\d{3})(\\d{7})", "0$1 $2", "$1 $2"},
			{"30|40|69|89", "(\\d{2})(\\d{5,8})", "0$1 $2", "$1 $2"},
			{"[2-9][1-9]1|20[1-389]|228|234|345", "(\\d{3})(\\d{3,8})", "0$1 $2", "$1 $2"},
			{"[2-9]", "(\\d{4})(\\d{2,7})", "0$1 $2", "$1 $2"},
		},
	}},

	// France (FR)
	{"FR", {
		33,                                   // country_code
		"FR",                                 // region_code
		{9},                                  // valid_lengths (9-digit national numbers)
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"123456789",                          // example_number
		"[1-5]\\d{8}",                        // fixed_line_pattern (01-05)
		"[67]\\d{8}",                         // mobile_pattern (06, 07)
		"80\\d{7}",                           // toll_free_pattern (0800)
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"8", "(\\d{3})(\\d{2})(\\d{2})(\\d{2})", "0 $1 $2 $3 $4", "$1 $2 $3 $4"},
			{"[1-79]", "(\\d)(\\d{2})(\\d{2})(\\d{2})(\\d{2})", "0$1 $2 $3 $4 $5", "$1 $2 $3 $4 $5"},
		},
	}},

	// Italy (IT)
	{"IT", {
		39,                                   // country_code
		"IT",                                 // region_code
		{9, 10, 11},                          // valid_lengths
		"",                                   // national_prefix (Italy doesn't use a national prefix)
		"00",                                 // international_prefix
		"0212345678",                         // example_number
		"0\\d{9,10}",                         // fixed_line_pattern
		"3\\d{8,9}",                          // mobile_pattern
		"80\\d{7,9}",                         // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"0[26]", "(\\d{2})(\\d{3,4})(\\d{4})", "$1 $2 $3", "$1 $2 $3"},
			{"0[13-57-9][0159]", "(\\d{3})(\\d{3,4})(\\d{4})", "$1 $2 $3", "$1 $2 $3"},
			{"0", "(\\d{4})(\\d{5,7})", "$1 $2", "$1 $2"},
			{"3", "(\\d{3})(\\d{3})(\\d{3,4})", "$1 $2 $3", "$1 $2 $3"},
			{"8", "(\\d{3})(\\d{3,6})", "$1 $2", "$1 $2"},
		},
	}},

	// Spain (ES)
	{"ES", {
		34,                                   // country_code
		"ES",                                 // region_code
		{9},                                  // valid_lengths
		"",                                   // national_prefix
		"00",                                 // international_prefix
		"912345678",                          // example_number
		"[89]\\d{8}",                         // fixed_line_pattern
		"[67]\\d{8}",                         // mobile_pattern
		"[89]00\\d{6}",                       // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"[89]00", "(\\d{3})(\\d{3})(\\d{3})", "$1 $2 $3", "$1 $2 $3"},
			{"[5-9]", "(\\d{3})(\\d{2})(\\d{2})(\\d{2})", "$1 $2 $3 $4", "$1 $2 $3 $4"},
		},
	}},

	// Australia (AU)
	{"AU", {
		61,                                   // country_code
		"AU",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"0011",                               // international_prefix
		"212345678",                          // example_number
		"[2378]\\d{8}",                       // fixed_line_pattern
		"4\\d{8}",                            // mobile_pattern
		"180[02]\\d{6}",                      // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"[2378]", "(\\d)(\\d{4})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
			{"4", "(\\d{3})(\\d{3})(\\d{3})", "0$1 $2 $3", "$1 $2 $3"},
		},
	}},

	// Japan (JP)
	{"JP", {
		81,                                   // country_code
		"JP",                                 // region_code
		{9, 10},                              // valid_lengths
		"0",                                  // national_prefix
		"010",                                // international_prefix
		"312345678",                          // example_number
		"[3-5]\\d{8,9}",                      // fixed_line_pattern
		"[789]0\\d{8}",                       // mobile_pattern
		"120\\d{6}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"120", "(\\d{3})(\\d{3})(\\d{3})", "0$1-$2-$3", "$1-$2-$3"},
			{"[36]", "(\\d)(\\d{4})(\\d{4})", "0$1-$2-$3", "$1-$2-$3"},
			{"[45]", "(\\d{2})(\\d{3})(\\d{4})", "0$1-$2-$3", "$1-$2-$3"},
			{"[3-9]", "(\\d{2})(\\d{4})(\\d{4})", "0$1-$2-$3", "$1-$2-$3"},
		},
	}},

	// China (CN)
	{"CN", {
		86,                                   // country_code
		"CN",                                 // region_code
		{10, 11, 12},                         // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1012345678",                         // example_number
		"10\\d{8}|[2-8]\\d{9,11}",             // fixed_line_pattern (010 Beijing, 02x-08xx)
		"1[3-9]\\d{9}",                       // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"1[3-9]", "(\\d{3})(\\d{4})(\\d{4})", "$1 $2 $3", "$1 $2 $3"},
			{"[48]00", "(\\d{3})(\\d{3})(\\d{4})", "$1 $2 $3", "$1 $2 $3"},
			{"10|2", "(\\d{2})(\\d{4})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
			{"[3-9]", "(\\d{3})(\\d{3,4})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
		},
	}},

	// India (IN)
	{"IN", {
		91,                                   // country_code
		"IN",                                 // region_code
		{10},                                 // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"2212345678",                         // example_number
		"[2-7]\\d{9}",                        // fixed_line_pattern
		"[89]\\d{9}",                         // mobile_pattern
		"1800\\d{6,7}",                       // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"1", "(\\d{4})(\\d{3})(\\d{3,4})", "$1 $2 $3", "$1 $2 $3"},
			{"[89]", "(\\d{5})(\\d{5})", "0$1 $2", "$1 $2"},
			{"11|2[02]|33|4[04]|79", "(\\d{2})(\\d{4})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
			{"[2-7]", "(\\d{3})(\\d{3})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
		},
	}},

	// Brazil (BR)
	{"BR", {
		55,                                   // country_code
		"BR",                                 // region_code
		{10, 11},                             // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1123456789",                         // example_number
		"[1-9][1-9]\\d{8}",                   // fixed_line_pattern
		"[1-9][1-9]9\\d{8}|[1-9][6-9]\\d{8}",  // mobile_pattern (area code + 9 + 8 digits; legacy 8-digit)
		"800\\d{7}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"800", "(\\d{3})(\\d{3})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
			{"[1-9][1-9]", "(\\d{2})(\\d{4,5})(\\d{4})", "($1) $2-$3", "$1 $2-$3"},
		},
	}},

	// Mexico (MX)
	{"MX", {
		52,                                   // country_code
		"MX",                                 // region_code
		{10, 11},                             // valid_lengths
		"01",                                 // national_prefix
		"00",                                 // international_prefix
		"5512345678",                         // example_number
		"[2-9]\\d{9,10}",                     // fixed_line_pattern
		"1\\d{10}",                           // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"33|55|81", "(\\d{2})(\\d{4})(\\d{4})", "$1 $2 $3", "$1 $2 $3"},
			{"[2-9]", "(\\d{3})(\\d{3})(\\d{4})", "$1 $2 $3", "$1 $2 $3"},
			{"1(?:33|55|81)", "(\\d)(\\d{2})(\\d{4})(\\d{4})", "$1 $2 $3 $4", "$1 $2 $3 $4"},
			{"1", "(\\d)(\\d{3})(\\d{3})(\\d{4})", "$1 $2 $3 $4", "$1 $2 $3 $4"},
		},
	}},

	// Argentina (AR)
	{"AR", {
		54,                                   // country_code
		"AR",                                 // region_code
		{10},                                 // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"1123456789",                         // example_number
		"[1-9]\\d{9}",                        // fixed_line_pattern (011 Buenos Aires, 02xx, 03xx)
		"9\\d{10}",                           // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"11", "(\\d{2})(\\d{4})(\\d{4})", "0$1 $2-$3", "$1 $2-$3"},
			{"[2-9]", "(\\d{3})(\\d{3})(\\d{4})", "0$1 $2-$3", "$1 $2-$3"},
		},
	}},

	// Russia (RU)
	{"RU", {
		7,                                    // country_code
		"RU",                                 // region_code
		{10},                                 // valid_lengths
		"8",                                  // national_prefix
		"810",                                // international_prefix
		"4951234567",                         // example_number
		"[3489]\\d{9}",                       // fixed_line_pattern
		"9\\d{9}",                            // mobile_pattern
		"800\\d{7}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"", "(\\d{3})(\\d{3})(\\d{2})(\\d{2})", "8 ($1) $2-$3-$4", "$1 $2-$3-$4"},
		},
	}},

	// Netherlands (NL)
	{"NL", {
		31,                                   // country_code
		"NL",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"201234567",                          // example_number
		"[1-58]\\d{8}",                       // fixed_line_pattern
		"6\\d{8}",                            // mobile_pattern
		"800\\d{6}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"6", "(\\d)(\\d{8})", "0$1 $2", "$1 $2"},
			{"80|9", "(\\d{3})(\\d{4,7})", "0$1 $2", "$1 $2"},
			{"1[035]|2[0346]|3[03568]|4[0356]|5[0358]|7|8[458]", "(\\d{2})(\\d{3})(\\d{4})", "0$1 $2 $3", "$1 $2 $3"},
			{"[1-58]", "(\\d{3})(\\d{3})(\\d{3})", "0$1 $2 $3", "$1 $2 $3"},
		},
	}},

	// Sweden (SE)
	{"SE", {
		46,                                   // country_code
		"SE",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"812345678",                          // example_number
		"[1-9]\\d{8}",                        // fixed_line_pattern
		"7[0236]\\d{7}",                      // mobile_pattern
		"20\\d{7}",                           // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"8", "(\\d)(\\d{3})(\\d{3})(\\d{2})", "0$1-$2 $3 $4", "$1 $2 $3 $4"},
			{"1[013689]|2[0136]|3[1356]|4[0246]|54|6[03]|7|90", "(\\d{2})(\\d{3})(\\d{2})(\\d{2})", "0$1-$2 $3 $4", "$1 $2 $3 $4"},
			{"[1-9]", "(\\d{3})(\\d{2})(\\d{2})(\\d{2})", "0$1-$2 $3 $4", "$1 $2 $3 $4"},
		},
	}},

	// Norway (NO)
	{"NO", {
		47,                                   // country_code
		"NO",                                 // region_code
		{8},                                  // valid_lengths
		"",                                   // national_prefix
		"00",                                 // international_prefix
		"21234567",                           // example_number
		"[2-79]\\d{7}",                       // fixed_line_pattern
		"[49]\\d{7}",                         // mobile_pattern
		"80[01]\\d{5}",                       // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"[489]|59", "(\\d{3})(\\d{2})(\\d{3})", "$1 $2 $3", "$1 $2 $3"},
			{"[2-79]", "(\\d{2})(\\d{2})(\\d{2})(\\d{2})", "$1 $2 $3 $4", "$1 $2 $3 $4"},
		},
	}},

	// Denmark (DK)
	{"DK", {
		45,                                   // country_code
		"DK",                                 // region_code
		{8},                                  // valid_lengths
		"",                                   // national_prefix
		"00",                                 // international_prefix
		"32123456",                           // example_number
		"[2-9]\\d{7}",                        // fixed_line_pattern
		"[2-9]\\d{7}",                        // mobile_pattern (same as fixed_line in Denmark)
		"80\\d{6}",                           // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"", "(\\d{2})(\\d{2})(\\d{2})(\\d{2})", "$1 $2 $3 $4", "$1 $2 $3 $4"},
		},
	}},

	// Finland (FI)
	{"FI", {
		358,                                  // country_code
		"FI",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"912345678",                          // example_number
		"[1-359]\\d{8}",                      // fixed_line_pattern (09 Helsinki)
		"4\\d{8}",                            // mobile_pattern
		"800\\d{6}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"4|50", "(\\d{2})(\\d{4,8})", "0$1 $2", "$1 $2"},
			{"[2568][1-8]|3[1-9]|9", "(\\d)(\\d{4,9})", "0$1 $2", "$1 $2"},
			{"", "(\\d{2})(\\d{4,8})", "0$1 $2", "$1 $2"},
		},
	}},

	// Switzerland (CH)
	{"CH", {
		41,                                   // country_code
		"CH",                                 // region_code
		{9},                                  // valid_lengths
		"0",                                  // national_prefix
		"00",                                 // international_prefix
		"212345678",                          // example_number
		"[2-9]\\d{8}",                        // fixed_line_pattern
		"7[5-9]\\d{7}",                       // mobile_pattern
		"800\\d{6}",                          // toll_free_pattern
		{  // number_formats: {leading_digits, pattern, national_format, international_format}
			{"8[047]|90", "(\\d{3})(\\d{3})(\\d{3})", "0$1 $2 $3", "$1 $2 $3"},
			{"[2-79]|81", "(\\d{2})(\\d{3})(\\d{2})(\\d{2})", "0$1 $2 $3 $4", "$1 $2 $3 $4"},
		},
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
			for (const auto &format : entry.second.number_formats) {
				auto pattern = CompilePattern(format.pattern);
				if (!pattern) {
					continue;
				}
				CompiledNumberFormat compiled {CompilePattern(format.leading_digits), std::move(*pattern), &format};
				patterns.number_formats.push_back(std::move(compiled));
			}
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

const NumberFormat *FindNumberFormat(const std::string &region_code, const std::string &national_number,
                                     std::smatch &match) {
	const CompiledRegionPatterns *patterns = GetCompiledPatternsForRegion(region_code);
	if (!patterns) {
		return nullptr;
	}
	for (const auto &candidate : patterns->number_formats) {
		// leading_digits only has to match a prefix of the national number
		if (candidate.leading_digits &&
		    !std::regex_search(national_number, *candidate.leading_digits, std::regex_constants::match_continuous)) {
			continue;
		}
		if (std::regex_match(national_number, match, candidate.pattern)) {
			return candidate.format;
		}
	}
	return nullptr;
}

} // namespace phonenumber
} // namespace anofox
} // namespace duckdb

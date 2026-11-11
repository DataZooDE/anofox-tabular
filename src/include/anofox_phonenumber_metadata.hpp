#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "re2/stringpiece.h"

namespace duckdb_re2 {
class RE2;
}

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

//! Bit n of a length mask is set when a national significant number (NSN) of n digits is possible
using LengthMask = uint32_t;

//! A libphonenumber <PhoneNumberDesc>: the numbers of one type in a region
struct NumberDesc {
	//! Regex over the whole NSN; empty when the region has no numbers of this type
	std::string_view pattern;
	//! Possible NSN lengths; 0 when upstream lists none (no length check)
	LengthMask possible_lengths;
	//! Lengths only diallable within the region (e.g. without area code)
	LengthMask local_only_lengths;
	std::string_view example;

	bool HasLength(size_t length) const {
		return length < 32 && (possible_lengths & (LengthMask(1) << length)) != 0;
	}
	bool HasLocalOnlyLength(size_t length) const {
		return length < 32 && (local_only_lengths & (LengthMask(1) << length)) != 0;
	}
};

//! Index of each NumberDesc in CountryMetadata::descs (libphonenumber's order)
enum NumberDescIndex : size_t {
	GENERAL_DESC = 0,
	FIXED_LINE_DESC,
	MOBILE_DESC,
	TOLL_FREE_DESC,
	PREMIUM_RATE_DESC,
	SHARED_COST_DESC,
	PERSONAL_NUMBER_DESC,
	VOIP_DESC,
	PAGER_DESC,
	UAN_DESC,
	VOICEMAIL_DESC,
	NUMBER_DESC_COUNT
};

//! One layout rule of a region, libphonenumber's <numberFormat>. The first rule
//! whose leading_digits match the start of the NSN and whose pattern matches the
//! whole NSN wins.
struct NumberFormat {
	std::string_view leading_digits; // Regex matched at the start of the NSN; empty matches any NSN
	std::string_view pattern;        // Regex over the whole NSN with one capture group per block
	std::string_view format;         // Layout with $1..$9, e.g. "0$1 $2" (NATIONAL) or "$1 $2"
};

//! Metadata of one region, generated from libphonenumber (see
//! scripts/generate_phone_metadata.py). Plain constant data; the regular
//! expressions are compiled per region on first use (GetCompiledPatterns).
struct CountryMetadata {
	std::string_view region_code;
	int country_code;
	//! Regex for the international (IDD) prefix, e.g. "00", "011", "0(?:0|1[2-9])"
	std::string_view international_prefix;
	//! The national (trunk) prefix, e.g. "0", "1" (NANP), empty for IT or ES
	std::string_view national_prefix;
	//! Regex matched at the start of a national number for prefixes dropped when
	//! parsing (usually the national prefix); empty: nothing is dropped
	std::string_view national_prefix_for_parsing;
	//! Replaces the national_prefix_for_parsing match when its last group matched,
	//! e.g. AR "9$1" (011 15 ... -> 9 11 ...)
	std::string_view national_prefix_transform_rule;
	//! For regions sharing a country code: regex matched at the start of the NSN
	//! that identifies this region (e.g. KZ "33|7")
	std::string_view leading_digits;
	bool same_mobile_and_fixed_line_pattern;
	std::array<NumberDesc, NUMBER_DESC_COUNT> descs;
	//! NATIONAL layout rules (national prefix included)
	const NumberFormat *national_formats;
	size_t national_format_count;
	//! INTERNATIONAL / RFC 3966 layout rules
	const NumberFormat *intl_formats;
	size_t intl_format_count;

	const NumberDesc &GeneralDesc() const {
		return descs[GENERAL_DESC];
	}
};

//! The regions of one country calling code, main region first
struct CountryCodeRegions {
	int country_code;
	const std::string_view *regions;
	size_t region_count;
};

//! Compiled regular expressions of one region (read-only once built)
struct CompiledNumberFormat {
	std::unique_ptr<duckdb_re2::RE2> leading_digits; // nullptr matches any NSN
	std::unique_ptr<duckdb_re2::RE2> pattern;
	const NumberFormat *format;
};

struct CompiledRegionPatterns {
	CompiledRegionPatterns();
	~CompiledRegionPatterns();
	CompiledRegionPatterns(CompiledRegionPatterns &&) noexcept;

	std::unique_ptr<duckdb_re2::RE2> international_prefix;
	std::unique_ptr<duckdb_re2::RE2> national_prefix_for_parsing;
	std::unique_ptr<duckdb_re2::RE2> leading_digits;
	std::array<std::unique_ptr<duckdb_re2::RE2>, NUMBER_DESC_COUNT> descs;
	std::vector<CompiledNumberFormat> national_formats;
	std::vector<CompiledNumberFormat> intl_formats;
};

//! Capture groups (incl. group 0) read from a metadata pattern; the generator
//! rejects patterns with more
constexpr int MAX_PATTERN_GROUPS = 10;

//! Expands $0..$9 in a libphonenumber rule ("$1 $2", "9$1") with the groups
//! of a match; an unmatched group expands to nothing
std::string ExpandGroups(std::string_view rule, const duckdb_re2::StringPiece *groups, int group_count);

//! Version of the phonenumbers package the metadata was generated from
const char *MetadataVersion();

//! Metadata of a region (upper-case ISO 3166 code), nullptr if unknown
const CountryMetadata *GetMetadataForRegion(std::string_view region_code);

//! Regions of a country calling code, main region first; nullptr if unknown
const CountryCodeRegions *GetRegionsForCountryCode(int country_code);

//! The main region of a country calling code ("US" for 1), empty if unknown
std::string GetMainRegionForCountryCode(int country_code);

//! The compiled patterns of a region, built on first use (thread-safe)
const CompiledRegionPatterns &GetCompiledPatterns(const CountryMetadata &metadata);

//! True when the NSN fully matches the region's pattern for that type, with a
//! length the type allows (libphonenumber's isNumberMatchingDesc)
bool IsNumberMatchingDesc(const std::string &national_number, const CountryMetadata &metadata, NumberDescIndex desc);

//! Length of the region's international (IDD) prefix at the start of `digits`,
//! 0 when there is none
size_t InternationalPrefixLength(const std::string &digits, const CountryMetadata &metadata);

//! Returns the first layout rule in `formats` that applies to the NSN, or
//! nullptr when none does (the NSN is then emitted as is), and the layout with
//! its $N groups expanded in `formatted`.
const NumberFormat *ApplyNumberFormat(const std::vector<CompiledNumberFormat> &formats,
                                      const std::string &national_number, std::string &formatted);

} // namespace phonenumber
} // namespace anofox
} // namespace duckdb

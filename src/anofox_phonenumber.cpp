#include "anofox_phonenumber.hpp"
#include "anofox_phonenumber_metadata.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_text_fold.hpp"
#include "telemetry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "anofox_duckdb_vectors.hpp"
#include "duckdb_compat.hpp"

#include "anofox_trace.hpp"

#include <mutex>
#include <string>
#include <regex>
#include <cctype>
#include <algorithm>
#include <optional>
#include <tuple>

#include "re2/re2.h"

namespace duckdb {
namespace anofox {
namespace phonenumber {

namespace {

bool IsAsciiLetter(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiDigit(char c) {
	return c >= '0' && c <= '9';
}

bool IsBlank(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

//! A raw phone field split into the number itself and the parts around it.
struct PhoneInput {
	//! The number without a leading label and without a trailing extension
	std::string number;
	//! Digits of a trailing extension ("ext. 123" -> "123"); empty when there is none
	std::string extension;
	//! The number contains a letter that is not part of a label or an extension marker
	bool has_letters = false;
};

//! Leading labels people type into a phone field ("Tel: 030 ...", "Mobil 0171 ...").
//! Longer labels come first so that "telefon" is not read as "tel" + "efon".
constexpr const char* PHONE_LABELS[] = {"telephone", "telefon", "phone", "mobile", "mobil", "cell", "tel", "fax"};
//! Markers of a trailing extension ("ext. 12", "x12", "Durchwahl 12", "DW 12")
constexpr const char* EXTENSION_MARKERS[] = {"extension", "durchwahl", "ext", "dw", "x"};

//! Case-insensitive test whether `text` has `word` (lower-case ASCII) at position `pos`
bool HasWordAt(const std::string& text, size_t pos, const char* word) {
	const size_t len = std::char_traits<char>::length(word);
	if (pos + len > text.size()) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (std::tolower(static_cast<unsigned char>(text[pos + i])) != word[i]) {
			return false;
		}
	}
	return true;
}

//! Splits a raw phone field into number, extension and a letter flag.
//! - A leading label (PHONE_LABELS, case-insensitive) followed by '.', ':' or a blank is dropped;
//!   this also accepts the RFC 3966 form "tel:+1-650-253-0000".
//! - A trailing extension marker (EXTENSION_MARKERS, case-insensitive, not glued to a preceding
//!   letter) followed by optional '.', ':', '=', blanks and then only digits is split off; this
//!   also reads the RFC 3966 parameter "tel:+49-30-1234567;ext=123". Separators such as '-'
//!   never start an extension: "030 12345-67" stays one number.
//! - Any other ASCII letter in the remaining number sets has_letters.
PhoneInput SplitPhoneInput(const std::string& raw) {
	PhoneInput input;
	std::string text = raw;
	StringUtil::Trim(text);

	for (const char* label : PHONE_LABELS) {
		const size_t len = std::char_traits<char>::length(label);
		if (HasWordAt(text, 0, label) && text.size() > len &&
		    (text[len] == '.' || text[len] == ':' || IsBlank(text[len]))) {
			size_t pos = len;
			while (pos < text.size() && (text[pos] == '.' || text[pos] == ':' || IsBlank(text[pos]))) {
				pos++;
			}
			text = text.substr(pos);
			break;
		}
	}

	// Trailing extension: <number> <marker>[.][:] <digits>
	size_t digits_start = text.size();
	while (digits_start > 0 && IsAsciiDigit(text[digits_start - 1])) {
		digits_start--;
	}
	if (digits_start < text.size()) {
		size_t marker_end = digits_start;
		while (marker_end > 0 && (IsBlank(text[marker_end - 1]) || text[marker_end - 1] == '.' ||
		                          text[marker_end - 1] == ':' || text[marker_end - 1] == '=')) {
			marker_end--;
		}
		for (const char* marker : EXTENSION_MARKERS) {
			const size_t len = std::char_traits<char>::length(marker);
			if (marker_end < len || !HasWordAt(text, marker_end - len, marker)) {
				continue;
			}
			const size_t marker_start = marker_end - len;
			// "FAX 5678" must not be read as "FA" + "x 5678"
			if (marker_start > 0 && IsAsciiLetter(text[marker_start - 1])) {
				continue;
			}
			std::string number = text.substr(0, marker_start);
			// An extension needs a number in front of it ("x123" alone is not a number)
			if (std::none_of(number.begin(), number.end(), IsAsciiDigit)) {
				continue;
			}
			input.extension = text.substr(digits_start);
			text = std::move(number);
			break;
		}
	}

	input.has_letters = std::any_of(text.begin(), text.end(), IsAsciiLetter);
	input.number = std::move(text);
	return input;
}

// Helper to normalize phone number string (remove spaces, hyphens, parentheses, etc.)
std::string NormalizePhoneNumber(const std::string& input) {
	std::string normalized;
	normalized.reserve(input.size());

	for (char c : input) {
		if (c >= '0' && c <= '9') {
			normalized += c;
		} else if (c == '+') {
			// Keep + sign only at the beginning
			if (normalized.empty()) {
				normalized += c;
			}
		}
	}

	return normalized;
}

// Smallest and largest national significant number (libphonenumber's
// MIN_LENGTH_FOR_NSN / MAX_LENGTH_FOR_NSN)
constexpr size_t MIN_NSN_LENGTH = 2;
constexpr size_t MAX_NSN_LENGTH = 17;

// Splits the digits after '+' into a known country calling code and the rest.
// Country codes are prefix-free, so the first 1-3 digit prefix that is a code
// is the code (libphonenumber's extractCountryCode). {0, ""} if there is none.
std::pair<int, std::string> ExtractCountryCode(const std::string& digits) {
	if (digits.empty() || digits[0] == '0') {
		return {0, ""};
	}
	for (size_t len = 1; len <= 3 && len <= digits.size(); len++) {
		const int code = std::stoi(digits.substr(0, len));
		if (GetRegionsForCountryCode(code)) {
			return {code, digits.substr(len)};
		}
	}
	return {0, ""};
}

// libphonenumber's ValidationResult of testNumberLength
enum class LengthResult { IS_POSSIBLE, IS_POSSIBLE_LOCAL_ONLY, TOO_SHORT, INVALID_LENGTH, TOO_LONG };

LengthResult TestNumberLength(const std::string& national_number, const CountryMetadata& metadata) {
	const auto& general = metadata.GeneralDesc();
	if (general.possible_lengths == 0) {
		return LengthResult::INVALID_LENGTH;
	}
	const size_t length = national_number.size();
	if (general.HasLocalOnlyLength(length)) {
		return LengthResult::IS_POSSIBLE_LOCAL_ONLY;
	}
	size_t min_length = 0;
	while (!general.HasLength(min_length)) {
		min_length++;
	}
	size_t max_length = 31;
	while (!general.HasLength(max_length)) {
		max_length--;
	}
	if (length < min_length) {
		return LengthResult::TOO_SHORT;
	}
	if (length > max_length) {
		return LengthResult::TOO_LONG;
	}
	return general.HasLength(length) ? LengthResult::IS_POSSIBLE : LengthResult::INVALID_LENGTH;
}

bool MatchesGeneralPattern(const std::string& national_number, const CountryMetadata& metadata) {
	const auto& regex = GetCompiledPatterns(metadata).descs[GENERAL_DESC];
	return regex && duckdb_re2::RE2::FullMatch(duckdb_re2::StringPiece(national_number.data(), national_number.size()),
	                                            *regex);
}

// libphonenumber's maybeStripNationalPrefixAndCarrierCode: drops the region's
// national prefix for parsing (e.g. the trunk 0 of "030 ...", the NANP 1 of
// "1-212-...", MX 044) or replaces it by the transform rule (AR "011 15 ..." ->
// "9 11 ..."), unless the number fits the region's general pattern only with it.
// Returns true when the number changed.
bool MaybeStripNationalPrefix(std::string& national_number, const CountryMetadata& metadata) {
	const auto& regex = GetCompiledPatterns(metadata).national_prefix_for_parsing;
	if (national_number.empty() || !regex) {
		return false;
	}
	constexpr int MAX_GROUPS = 10;
	const int group_count = std::min(regex->NumberOfCapturingGroups() + 1, MAX_GROUPS);
	duckdb_re2::StringPiece groups[MAX_GROUPS];
	if (!regex->Match(duckdb_re2::StringPiece(national_number.data(), national_number.size()), 0,
	                  national_number.size(), duckdb_re2::RE2::ANCHOR_START, groups, group_count)) {
		return false;
	}
	const size_t prefix_end = groups[0].size();
	const bool viable_original = MatchesGeneralPattern(national_number, metadata);
	const auto& transform_rule = metadata.national_prefix_transform_rule;
	const bool last_group_matched = group_count > 1 && groups[group_count - 1].data() != nullptr;

	std::string candidate;
	if (transform_rule.empty() || !last_group_matched) {
		candidate = national_number.substr(prefix_end);
	} else {
		for (size_t i = 0; i < transform_rule.size(); i++) {
			if (transform_rule[i] == '$' && i + 1 < transform_rule.size() && transform_rule[i + 1] >= '0' &&
			    transform_rule[i + 1] <= '9') {
				const int group = transform_rule[i + 1] - '0';
				if (group < group_count && groups[group].data()) {
					candidate.append(groups[group].data(), groups[group].size());
				}
				i++;
			} else {
				candidate += transform_rule[i];
			}
		}
		candidate += national_number.substr(prefix_end);
	}
	if (viable_original && !MatchesGeneralPattern(candidate, metadata)) {
		return false;
	}
	AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse: national prefix of " + national_number + " -> " + candidate +
	                                       ", region=" + std::string(metadata.region_code));
	national_number = std::move(candidate);
	return true;
}

// libphonenumber's getNumberTypeHelper
const char* NumberTypeOf(const std::string& national_number, const CountryMetadata& metadata) {
	if (!IsNumberMatchingDesc(national_number, metadata, GENERAL_DESC)) {
		return PhoneNumberType::UNKNOWN;
	}
	static constexpr std::pair<NumberDescIndex, const char*> SPECIAL_TYPES[] = {
	    {PREMIUM_RATE_DESC, PhoneNumberType::PREMIUM_RATE},
	    {TOLL_FREE_DESC, PhoneNumberType::TOLL_FREE},
	    {SHARED_COST_DESC, PhoneNumberType::SHARED_COST},
	    {VOIP_DESC, PhoneNumberType::VOIP},
	    {PERSONAL_NUMBER_DESC, PhoneNumberType::PERSONAL_NUMBER},
	    {PAGER_DESC, PhoneNumberType::PAGER},
	    {UAN_DESC, PhoneNumberType::UAN},
	    {VOICEMAIL_DESC, PhoneNumberType::VOICEMAIL},
	};
	for (const auto& entry : SPECIAL_TYPES) {
		if (IsNumberMatchingDesc(national_number, metadata, entry.first)) {
			return entry.second;
		}
	}
	if (IsNumberMatchingDesc(national_number, metadata, FIXED_LINE_DESC)) {
		if (metadata.same_mobile_and_fixed_line_pattern ||
		    IsNumberMatchingDesc(national_number, metadata, MOBILE_DESC)) {
			return PhoneNumberType::FIXED_LINE_OR_MOBILE;
		}
		return PhoneNumberType::FIXED_LINE;
	}
	if (!metadata.same_mobile_and_fixed_line_pattern && IsNumberMatchingDesc(national_number, metadata, MOBILE_DESC)) {
		return PhoneNumberType::MOBILE;
	}
	return PhoneNumberType::UNKNOWN;
}

// The region of a number of a country calling code, libphonenumber's
// getRegionCodeForNumberFromRegionList: in the code's region order (main region
// first), the first region whose leading digits match or, for a region without
// leading digits, in which the number has a known type. The region argument
// plays no part: a GB-wide 0800 number is GB also for a customer in Guernsey.
// Empty when the number is valid in none of the regions.
std::string ResolveRegionForCountryCode(int country_code, const std::string& national_number) {
	const auto* entry = GetRegionsForCountryCode(country_code);
	if (!entry) {
		return "";
	}
	if (entry->region_count == 1) {
		return std::string(entry->regions[0]);
	}
	for (size_t i = 0; i < entry->region_count; i++) {
		const auto& candidate = entry->regions[i];
		const CountryMetadata* metadata = GetMetadataForRegion(candidate);
		if (!metadata) {
			continue;
		}
		const auto& leading_digits = GetCompiledPatterns(*metadata).leading_digits;
		const bool matches =
		    leading_digits ? leading_digits->Match(duckdb_re2::StringPiece(national_number.data(), national_number.size()),
		                                           0, national_number.size(), duckdb_re2::RE2::ANCHOR_START, nullptr, 0)
		                   : NumberTypeOf(national_number, *metadata) != PhoneNumberType::UNKNOWN;
		if (matches) {
			AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse: +" + std::to_string(country_code) + " " +
			                                       national_number + " is in region " + std::string(candidate));
			return std::string(candidate);
		}
	}
	return "";
}

// Whether the reason means that the module has no metadata to judge the number
bool IsUnsupportedReason(const std::string& reason) {
	return reason == PhoneNumberInvalidReason::UNSUPPORTED_REGION ||
	       reason == PhoneNumberInvalidReason::INVALID_COUNTRY_CODE;
}

std::mutex& RegionMutex() {
	static std::mutex region_mutex;
	return region_mutex;
}

} // namespace

bool PhoneNumberParts::CannotJudge() const {
	return IsUnsupportedReason(reason);
}

PhoneNumberManager& PhoneNumberManager::Instance() {
	static PhoneNumberManager instance;
	return instance;
}

PhoneNumberManager::PhoneNumberManager() : default_region("US") {
}

PhoneNumberManager::~PhoneNumberManager() {
}

void PhoneNumberManager::EnsureInitialized() {
	if (!initialized.load()) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber EnsureInitialized triggered");
		Initialize();
	}
}

PhoneNumberParts PhoneNumberManager::Parse(const std::string& raw_number, const std::string& region_hint,
                                           const std::string& default_region) {
	EnsureInitialized();
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber parse start number=" + raw_number + " region_hint=" + region_hint);

	PhoneNumberParts parts;

	// Fold full-width digits/signs to ASCII and no-break spaces (U+00A0, U+2007,
	// U+202F) to blanks
	const std::string folded = FoldFullWidthForms(raw_number);

	// A blank field (empty or whitespace only) is a missing value, not a broken number
	if (std::all_of(folded.begin(), folded.end(),
	                [](char c) { return IsBlank(c) || c == '\v' || c == '\f'; })) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: blank input");
		parts.reason = PhoneNumberInvalidReason::EMPTY;
		return parts;
	}

	// Drop a leading label, split off a trailing extension, then normalize
	auto input = SplitPhoneInput(folded);
	std::string normalized = NormalizePhoneNumber(input.number);
	if (normalized.empty() || normalized == "+") {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: empty number");
		parts.reason = PhoneNumberInvalidReason::NOT_A_NUMBER;
		return parts;
	}
	if (input.has_letters) {
		// No vanity numbers: a letter inside the number is a typo or a note, not a digit
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: letters in number=" + input.number);
		parts.reason = PhoneNumberInvalidReason::INVALID_CHARACTERS;
		return parts;
	}
	if (!input.extension.empty()) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		            "PhoneNumber parse: extension " + input.extension + " split off number=" + input.number);
	}
	parts.extension = input.extension;

	const std::string hint_upper = NormalizeRegionCode(region_hint);

	int country_code = 0;
	std::string national_number;
	// Metadata the national prefix is stripped with: the region the number was
	// read in for a national number, the country code's main region otherwise
	const CountryMetadata* parse_metadata = nullptr;
	std::string read_region;

	if (normalized[0] != '+') {
		// National format, or international format dialled with the region's
		// international prefix (e.g. "0049 ..." from DE, "011 49 ..." from US).
		// The region comes from the hint, falling back to the (session-snapshotted)
		// default region when there is no hint. An unknown hint (e.g. 'XX') makes
		// the number unjudgeable rather than silently using the default.
		read_region = !hint_upper.empty() ? hint_upper : EffectiveDefaultRegion(default_region);
		const CountryMetadata* hint_metadata = GetMetadataForRegion(read_region);
		if (!hint_metadata) {
			AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: unsupported region=" + read_region);
			parts.reason = PhoneNumberInvalidReason::UNSUPPORTED_REGION;
			return parts;
		}
		// libphonenumber's parsePrefixAsIdd: a country code never starts with 0
		const size_t idd_length = InternationalPrefixLength(normalized, *hint_metadata);
		if (idd_length > 0 && idd_length < normalized.size() && normalized[idd_length] != '0') {
			AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse: international prefix " +
			                                       normalized.substr(0, idd_length) + " of region " + read_region +
			                                       " in " + normalized);
			normalized = "+" + normalized.substr(idd_length);
		} else {
			parts.read_in_default_region = hint_upper.empty();
			parse_metadata = hint_metadata;
			country_code = hint_metadata->country_code;
			national_number = normalized;
			// A national number written with the region's own country code but
			// without '+' ("49 30 1234567" in DE), libphonenumber's maybeExtractCountryCode
			const std::string code = std::to_string(country_code);
			if (national_number.compare(0, code.size(), code) == 0) {
				std::string without_code = national_number.substr(code.size());
				MaybeStripNationalPrefix(without_code, *hint_metadata);
				if ((!MatchesGeneralPattern(national_number, *hint_metadata) &&
				     MatchesGeneralPattern(without_code, *hint_metadata)) ||
				    TestNumberLength(national_number, *hint_metadata) == LengthResult::TOO_LONG) {
					AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse: country code " + code + " without + in " +
					                                       national_number);
					national_number = std::move(without_code);
					parts.read_in_default_region = false;
				}
			}
		}
	}

	if (normalized[0] == '+') {
		// Explicit international format: the country code decides, the region
		// argument is not used
		std::tie(country_code, national_number) = ExtractCountryCode(normalized.substr(1));
		if (country_code == 0) {
			AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: unknown country code in " + normalized);
			parts.reason = PhoneNumberInvalidReason::INVALID_COUNTRY_CODE;
			return parts;
		}
		parse_metadata = GetMetadataForRegion(GetMainRegionForCountryCode(country_code));
	}
	D_ASSERT(parse_metadata);

	// The national (trunk) prefix, "+49 (0)30 ...", "1-212-...", MX "044 55 ...":
	// keep the stripped number unless that makes its length impossible
	std::string stripped = national_number;
	if (MaybeStripNationalPrefix(stripped, *parse_metadata)) {
		const auto length_result = TestNumberLength(stripped, *parse_metadata);
		if (length_result != LengthResult::TOO_SHORT && length_result != LengthResult::IS_POSSIBLE_LOCAL_ONLY &&
		    length_result != LengthResult::INVALID_LENGTH) {
			national_number = std::move(stripped);
		}
	}

	const std::string main_region = GetMainRegionForCountryCode(country_code);
	std::string region_code = ResolveRegionForCountryCode(country_code, national_number);
	if (region_code.empty()) {
		// Valid in none of the code's regions: report the region it was read in
		region_code = !read_region.empty() && GetMetadataForRegion(read_region)->country_code == country_code
		                  ? read_region
		                  : main_region;
	}
	const CountryMetadata* metadata = GetMetadataForRegion(region_code);
	if (!metadata) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: no metadata for region=" + region_code);
		parts.reason = PhoneNumberInvalidReason::UNSUPPORTED_REGION;
		return parts;
	}
	parts.region_code = region_code;
	parts.country_code = country_code;

	// Possible: libphonenumber's isPossibleNumber, judged with the code's main region
	const auto length_result = national_number.size() < MIN_NSN_LENGTH || national_number.size() > MAX_NSN_LENGTH
	                               ? LengthResult::INVALID_LENGTH
	                               : TestNumberLength(national_number, *GetMetadataForRegion(main_region));
	if (length_result != LengthResult::IS_POSSIBLE && length_result != LengthResult::IS_POSSIBLE_LOCAL_ONLY) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber parse failed: invalid length=" + std::to_string(national_number.size()) +
		           " for region=" + region_code);
		parts.reason = PhoneNumberInvalidReason::INVALID_LENGTH;
		parts.national_number = national_number;
		return parts;
	}

	parts.valid = true;
	parts.national_number = national_number;
	parts.type = NumberTypeOf(national_number, *metadata);
	parts.matches_valid_pattern = parts.type != PhoneNumberType::UNKNOWN;
	if (!parts.matches_valid_pattern) {
		// Possible (length fits) but in no number range of the region
		parts.reason = PhoneNumberInvalidReason::INVALID_PATTERN;
	}
	if (parts.read_in_default_region && !parts.reason.empty()) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse: national number " + normalized +
		                                       " invalid in default region " + region_code + " (no region given)");
	}

	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber parse success region=" + parts.region_code + " type=" + parts.type);
	return parts;
}

std::string PhoneNumberManager::Format(const std::string& raw_number, const std::string& region_hint,
                                       PhoneNumberFormatOption format_option, const std::string& default_region) {
	EnsureInitialized();

	// Only a valid number (the phonenumber_is_valid check: a possible length
	// and a number in an assigned range) is formatted, so a stored E164 value
	// is always a valid number; phonenumber_parse().reason says why not.
	auto parts = Parse(raw_number, region_hint, default_region);
	if (!parts.valid || !parts.matches_valid_pattern) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber format skipped, number is not valid number=" + raw_number +
		               " reason=" + parts.reason);
		throw InvalidInputException("Invalid phone number: %s", raw_number);
	}

	const std::string country_prefix = "+" + std::to_string(parts.country_code);
	if (format_option == PhoneNumberFormatOption::E164) {
		return country_prefix + parts.national_number;
	}

	// As libphonenumber, the layout rules are those of the country code's main
	// region (a GG number is laid out like a GB one)
	const CountryMetadata* metadata = GetMetadataForRegion(GetMainRegionForCountryCode(parts.country_code));
	if (!metadata) {
		throw InvalidInputException("No metadata for country code: %d", parts.country_code);
	}
	const auto& patterns = GetCompiledPatterns(*metadata);
	const bool national = format_option == PhoneNumberFormatOption::NATIONAL;
	std::string body;
	const NumberFormat* number_format =
	    ApplyNumberFormat(national ? patterns.national_formats : patterns.intl_formats, parts.national_number, body);
	if (!number_format) {
		body = parts.national_number;
	}
	AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber format rule for region=" + std::string(metadata->region_code) +
	                                       ": " + (number_format ? std::string(number_format->pattern) : "none"));

	std::string formatted;
	switch (format_option) {
	case PhoneNumberFormatOption::INTERNATIONAL:
		formatted = country_prefix + " " + body;
		break;
	case PhoneNumberFormatOption::NATIONAL:
		formatted = body;
		break;
	case PhoneNumberFormatOption::RFC3966: {
		// RFC 3966: the international layout with every separator run replaced by '-'
		formatted = "tel:" + country_prefix + "-";
		bool pending_separator = false;
		for (char c : body) {
			if (c >= '0' && c <= '9') {
				if (pending_separator) {
					formatted += '-';
					pending_separator = false;
				}
				formatted += c;
			} else if (formatted.back() != '-') {
				pending_separator = true;
			}
		}
		// The extension is the RFC 3966 'ext' parameter; it reads back via SplitPhoneInput
		if (!parts.extension.empty()) {
			formatted += ";ext=" + parts.extension;
		}
		break;
	}
	case PhoneNumberFormatOption::E164:
		break;
	}

	AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber format success number=" + raw_number + " output=" + formatted);
	return formatted;
}

std::string PhoneNumberManager::GetRegion(const std::string& raw_number, const std::string& region_hint,
                                          const std::string& default_region) {
	EnsureInitialized();
	// Same rule as Format: only a valid number has a region
	auto parts = Parse(raw_number, region_hint, default_region);
	if (!parts.valid || !parts.matches_valid_pattern) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber region skipped, number is not valid number=" + raw_number +
		               " reason=" + parts.reason);
		throw InvalidInputException("Invalid phone number: %s", raw_number);
	}
	return parts.region_code;
}

std::optional<bool> PhoneNumberManager::IsValid(const std::string& raw_number, const std::string& region_hint,
                                                const std::string& default_region) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint, default_region);
	if (parts.CannotJudge()) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber IsValid cannot judge number=" + raw_number +
		                                       " reason=" + parts.reason);
		return std::nullopt;
	}
	const bool is_valid = parts.valid && parts.matches_valid_pattern;
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber IsValid result=" + std::string(is_valid ? "true" : "false") +
	           " number=" + raw_number);
	return is_valid;
}

std::optional<bool> PhoneNumberManager::IsPossible(const std::string& raw_number, const std::string& region_hint,
                                                   const std::string& default_region) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint, default_region);
	if (parts.CannotJudge()) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber IsPossible cannot judge number=" + raw_number +
		                                       " reason=" + parts.reason);
		return std::nullopt;
	}
	// Parse succeeds exactly for a possible length (isPossibleNumber)
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber IsPossible result=" + std::string(parts.valid ? "true" : "false") +
	           " number=" + raw_number);
	return parts.valid;
}

std::optional<bool> PhoneNumberManager::IsValidForRegion(const std::string& raw_number, const std::string& region_hint,
                                                         const std::string& default_region) {
	EnsureInitialized();

	auto hint = NormalizeRegionCode(region_hint);
	const std::string requested_region = hint.empty() ? EffectiveDefaultRegion(default_region) : hint;
	const CountryMetadata* requested = GetMetadataForRegion(requested_region);
	auto parts = Parse(raw_number, region_hint, default_region);
	if (!requested || parts.CannotJudge()) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber IsValidForRegion cannot judge number=" + raw_number +
		                                       " region=" + requested_region);
		return std::nullopt;
	}
	if (!parts.valid) {
		return false;
	}
	// libphonenumber's isValidNumberForRegion: the region's country code, and a
	// number range of that region
	if (parts.country_code != requested->country_code) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber IsValidForRegion failed: country code " +
		                                       std::to_string(parts.country_code) + " is not the one of " +
		                                       requested_region);
		return false;
	}
	return NumberTypeOf(parts.national_number, *requested) != PhoneNumberType::UNKNOWN;
}

std::string PhoneNumberManager::Match(const std::string& number1, const std::string& number2,
                                      const std::string& region_hint, const std::string& default_region) {
	EnsureInitialized();

	auto parts1 = Parse(number1, region_hint, default_region);
	auto parts2 = Parse(number2, region_hint, default_region);

	// As in libphonenumber, a side that cannot be read as a phone number (blank,
	// letters, a placeholder such as "-" or "n/a", fewer than two digits, an
	// unknown country code or region) makes the comparison NOT_A_NUMBER, so
	// two empty or placeholder fields never count as the same number.
	auto is_number = [](const PhoneNumberParts& parts) {
		return parts.national_number.size() >= 2;
	};
	if (!is_number(parts1) || !is_number(parts2)) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber Match result=NOT_A_NUMBER reason1=" + parts1.reason +
		                                       " reason2=" + parts2.reason);
		return "NOT_A_NUMBER";
	}

	// A number of invalid length (a short code such as 411) is compared as written
	if (!parts1.valid || !parts2.valid) {
		if (number1 == number2) {
			AnofoxTrace(AnofoxLogLevel::Debug,
			           "PhoneNumber Match result=EXACT_MATCH (raw string match, invalid length) number1=" + number1 + " number2=" + number2);
			return "EXACT_MATCH";
		}
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber Match result=NO_MATCH (invalid length) number1=" + number1 + " number2=" + number2);
		return "NO_MATCH";
	}

	std::string result;

	// EXACT_MATCH: country code and national number both match
	if (parts1.country_code == parts2.country_code &&
	    parts1.national_number == parts2.national_number) {
		result = "EXACT_MATCH";
	}
	// NSN_MATCH: national numbers match (country codes may differ or be missing)
	else if (parts1.national_number == parts2.national_number) {
		result = "NSN_MATCH";
	}
	// SHORT_NSN_MATCH: one national number is a suffix of the other
	else {
		const std::string& longer = parts1.national_number.size() > parts2.national_number.size() ?
		                            parts1.national_number : parts2.national_number;
		const std::string& shorter = parts1.national_number.size() <= parts2.national_number.size() ?
		                             parts1.national_number : parts2.national_number;

		if (longer.size() >= shorter.size() &&
		    longer.substr(longer.size() - shorter.size()) == shorter) {
			result = "SHORT_NSN_MATCH";
		} else {
			result = "NO_MATCH";
		}
	}

	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber Match result=" + result + " number1=" + number1 + " number2=" + number2);
	return result;
}

std::string PhoneNumberManager::GetExampleNumber(const std::string& region_hint, const std::string& default_region) {
	EnsureInitialized();

	auto hint = NormalizeRegionCode(region_hint);
	std::string region = hint.empty() ? EffectiveDefaultRegion(default_region) : hint;
	const CountryMetadata* metadata = GetMetadataForRegion(region);

	if (!metadata) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber GetExampleNumber failed for region=" + region);
		return "";
	}

	// libphonenumber's getExampleNumber is the fixed-line example; regions
	// without fixed lines fall back to the first type that has one
	std::string_view example;
	for (const auto& desc : {FIXED_LINE_DESC, MOBILE_DESC, TOLL_FREE_DESC, PREMIUM_RATE_DESC, SHARED_COST_DESC,
	                         PERSONAL_NUMBER_DESC, VOIP_DESC, PAGER_DESC, UAN_DESC, VOICEMAIL_DESC}) {
		if (!metadata->descs[desc].example.empty()) {
			example = metadata->descs[desc].example;
			break;
		}
	}
	if (example.empty()) {
		return "";
	}
	std::string formatted = "+" + std::to_string(metadata->country_code) + std::string(example);
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber GetExampleNumber result=" + formatted + " region=" + region);
	return formatted;
}

void PhoneNumberManager::SetDefaultRegion(const std::string& region) {
	EnsureInitialized();
	std::lock_guard<std::mutex> lock(RegionMutex());
	default_region = NormalizeRegionCode(region);
	AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber default region set to " + default_region);
}

std::string PhoneNumberManager::GetDefaultRegion() const {
	std::lock_guard<std::mutex> lock(RegionMutex());
	return default_region;
}

std::string PhoneNumberManager::EffectiveDefaultRegion(const std::string& default_region_p) const {
	auto normalized = NormalizeRegionCode(default_region_p);
	if (!normalized.empty()) {
		return normalized;
	}
	return GetDefaultRegion();
}

PhoneNumberStatus PhoneNumberManager::GetStatus() const {
	PhoneNumberStatus status;
	status.initialized = initialized.load();
	status.default_region = GetDefaultRegion();
	return status;
}

void PhoneNumberManager::Initialize() {
	// No external library initialization needed anymore
	initialized = true;
	AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber initialization complete (custom implementation)");
}

std::string NormalizeRegionCode(const std::string& region) {
	// Also trims the no-break spaces of spreadsheet exports
	auto normalized = std::string(TrimUnicodeSpaces(region));
	normalized = StringUtil::Upper(normalized);
	// 'UK' is not an ISO 3166 code but is what most people type for the
	// United Kingdom; libphonenumber metadata only knows 'GB'.
	if (normalized == "UK") {
		normalized = "GB";
	}
	return normalized;
}

std::string PhoneNumberParts::ReportedReason() const {
	if (read_in_default_region && (reason == PhoneNumberInvalidReason::INVALID_LENGTH ||
	                               reason == PhoneNumberInvalidReason::INVALID_PATTERN)) {
		return PhoneNumberInvalidReason::REGION_MISSING;
	}
	return reason;
}

PhoneNumberFormatOption ParseFormatOption(const std::string& format_str) {
	auto trimmed = std::string(TrimUnicodeSpaces(format_str));
	auto upper = StringUtil::Upper(trimmed);
	if (upper == "E164") {
		return PhoneNumberFormatOption::E164;
	}
	if (upper == "INTERNATIONAL") {
		return PhoneNumberFormatOption::INTERNATIONAL;
	}
	if (upper == "NATIONAL") {
		return PhoneNumberFormatOption::NATIONAL;
	}
	if (upper == "RFC3966") {
		return PhoneNumberFormatOption::RFC3966;
	}
	throw InvalidInputException(
	    "Unknown phone number format '%s'. Expected one of E164, INTERNATIONAL, NATIONAL, RFC3966", trimmed);
}

} // namespace phonenumber
} // namespace anofox
} // namespace duckdb

// SQL Function registrations (keeping existing structure)
#include "duckdb/common/types/value.hpp"
#include "anofox_tabular_banner.hpp"

namespace duckdb {
namespace anofox {

using phonenumber::ParseFormatOption;
using phonenumber::PhoneNumberFormatOption;
using phonenumber::PhoneNumberManager;
using phonenumber::PhoneNumberStatus;

namespace {

static void SetPhonenumberDefaultRegionOption(ClientContext&, SetScope, Value& parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_phonenumber_default_region cannot be NULL");
	}
	auto region = phonenumber::NormalizeRegionCode(parameter.ToString());
	if (!phonenumber::GetMetadataForRegion(region)) {
		throw InvalidInputException(
		    "Unknown region code '%s' for anofox_tab_phonenumber_default_region; expected a supported "
		    "2-letter ISO region code (e.g. 'US', 'GB', 'DE')",
		    parameter.ToString());
	}
	// Store the normalized value (trimmed, uppercase, 'UK' -> 'GB') in the setting
	parameter = Value(region);
	PhoneNumberManager::Instance().SetDefaultRegion(region);
}

//! Bind data snapshotting the session's default region at bind time so that a
//! concurrent SET cannot change results mid-query.
struct PhoneBindData : public FunctionData {
	explicit PhoneBindData(std::string default_region_p) : default_region(std::move(default_region_p)) {
	}

	std::string default_region;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<PhoneBindData>(default_region);
	}
	bool Equals(const FunctionData& other) const override {
		return default_region == other.Cast<PhoneBindData>().default_region;
	}
};

std::string SnapshotDefaultRegion(ClientContext& context) {
	Value value;
	if (context.TryGetCurrentSetting("anofox_tab_phonenumber_default_region", value) && !value.IsNull()) {
		return phonenumber::NormalizeRegionCode(value.ToString());
	}
	return PhoneNumberManager::Instance().GetDefaultRegion();
}

// The expression's bind data: BindInfo() on DuckDB 2.0, the public bind_info
// field before
template <class EXPR>
auto BindDataOf(const EXPR& expr, int) -> decltype(expr.BindInfo().get()) {
	return expr.BindInfo().get();
}
template <class EXPR>
auto BindDataOf(const EXPR& expr, long) -> decltype(expr.bind_info.get()) {
	return expr.bind_info.get();
}

const std::string& GetBoundDefaultRegion(ExpressionState& state) {
	auto& func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto bind_data = BindDataOf(func_expr, 0);
	D_ASSERT(bind_data);
	return bind_data->Cast<PhoneBindData>().default_region;
}

void PhoneParseFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& numbers = args.data[0];
	auto& regions = args.data[1];

	UnifiedVectorFormat number_data;
	UnifiedVectorFormat region_data;
	numbers.ToUnifiedFormat(args.size(), number_data);
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto& children = StructVector::GetEntries(result);

	auto& valid_vec = anofox::VectorRef(children[0]);
	auto& country_code_vec = anofox::VectorRef(children[1]);
	auto& national_number_vec = anofox::VectorRef(children[2]);
	auto& region_vec = anofox::VectorRef(children[3]);
	auto& type_vec = anofox::VectorRef(children[4]);
	auto& reason_vec = anofox::VectorRef(children[5]);
	auto& extension_vec = anofox::VectorRef(children[6]);

	auto valid_data = duckdb_compat::FlatData<bool>(valid_vec);
	auto country_data = duckdb_compat::FlatData<int32_t>(country_code_vec);
	auto national_data = duckdb_compat::FlatData<string_t>(national_number_vec);
	auto region_data_out = duckdb_compat::FlatData<string_t>(region_vec);
	auto type_data = duckdb_compat::FlatData<string_t>(type_vec);
	auto reason_data = duckdb_compat::FlatData<string_t>(reason_vec);
	auto extension_data = duckdb_compat::FlatData<string_t>(extension_vec);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			for (auto& child : children) {
				FlatVector::SetNull(anofox::VectorRef(child), i, true);
			}
			continue;
		}

		auto raw_number = UnifiedVectorFormat::GetData<string_t>(number_data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString()
		                     : std::string();

		auto parts = PhoneNumberManager::Instance().Parse(raw_number, region_hint, default_region);

		// `valid` matches phonenumber_is_valid: a possible length and a valid range,
		// NULL when there is no metadata to judge the number
		const bool is_valid = parts.valid && parts.matches_valid_pattern;
		FlatVector::SetNull(result, i, false);
		FlatVector::SetNull(valid_vec, i, parts.CannotJudge());
		valid_data[i] = is_valid;

		if (is_valid) {
			FlatVector::SetNull(country_code_vec, i, false);
			FlatVector::SetNull(national_number_vec, i, false);
			FlatVector::SetNull(region_vec, i, false);
			FlatVector::SetNull(type_vec, i, false);

			country_data[i] = parts.country_code;
			national_data[i] = StringVector::AddString(national_number_vec, parts.national_number);
			region_data_out[i] = StringVector::AddString(region_vec, parts.region_code);
			type_data[i] = StringVector::AddString(type_vec, parts.type);
			FlatVector::SetNull(reason_vec, i, true);
			if (parts.extension.empty()) {
				FlatVector::SetNull(extension_vec, i, true);
			} else {
				FlatVector::SetNull(extension_vec, i, false);
				extension_data[i] = StringVector::AddString(extension_vec, parts.extension);
			}
		} else {
			FlatVector::SetNull(country_code_vec, i, true);
			FlatVector::SetNull(national_number_vec, i, true);
			// The region the number was read in, so a failure under the
			// default region shows which region that was
			FlatVector::SetNull(region_vec, i, parts.region_code.empty());
			if (!parts.region_code.empty()) {
				region_data_out[i] = StringVector::AddString(region_vec, parts.region_code);
			}
			FlatVector::SetNull(type_vec, i, true);
			FlatVector::SetNull(extension_vec, i, true);
			FlatVector::SetNull(reason_vec, i, false);
			reason_data[i] = StringVector::AddString(reason_vec, parts.ReportedReason());
		}
	}
}

void PhoneFormatFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& numbers = args.data[0];
	auto& regions = args.data[1];
	auto& formats = args.data[2];

	UnifiedVectorFormat number_data;
	UnifiedVectorFormat region_data;
	UnifiedVectorFormat format_data;
	numbers.ToUnifiedFormat(args.size(), number_data);
	regions.ToUnifiedFormat(args.size(), region_data);
	formats.ToUnifiedFormat(args.size(), format_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);
		auto fmt_idx = format_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx) ||
		    !format_data.validity.RowIsValid(fmt_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = UnifiedVectorFormat::GetData<string_t>(number_data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString()
		                     : std::string();
		auto format_str = UnifiedVectorFormat::GetData<string_t>(format_data)[fmt_idx].GetString();

		// Unknown format strings raise an error (outside the try block below)
		auto format_option = ParseFormatOption(format_str);
		try {
			auto formatted =
			    PhoneNumberManager::Instance().Format(raw_number, region_hint, format_option, default_region);
			result_data[i] = StringVector::AddString(result, formatted);
			FlatVector::SetNull(result, i, false);
		} catch (const InvalidInputException&) {
			// Invalid/unparseable numbers yield NULL
			FlatVector::SetNull(result, i, true);
		}
	}
}

void PhoneRegionFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& numbers = args.data[0];
	auto& regions = args.data[1];

	UnifiedVectorFormat number_data;
	UnifiedVectorFormat region_data;
	numbers.ToUnifiedFormat(args.size(), number_data);
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = UnifiedVectorFormat::GetData<string_t>(number_data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString()
		                     : std::string();

		try {
			auto region = PhoneNumberManager::Instance().GetRegion(raw_number, region_hint, default_region);
			result_data[i] = StringVector::AddString(result, region);
			FlatVector::SetNull(result, i, false);
		} catch (const std::exception& e) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void PhoneIsValidFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& numbers = args.data[0];
	auto& regions = args.data[1];

	UnifiedVectorFormat number_data;
	UnifiedVectorFormat region_data;
	numbers.ToUnifiedFormat(args.size(), number_data);
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<bool>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = UnifiedVectorFormat::GetData<string_t>(number_data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString()
		                     : std::string();

		// NULL: no metadata to judge the number (unknown region or country code)
		const auto judgement = PhoneNumberManager::Instance().IsValid(raw_number, region_hint, default_region);
		FlatVector::SetNull(result, i, !judgement.has_value());
		result_data[i] = judgement.value_or(false);
	}
}

void PhoneIsPossibleFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& numbers = args.data[0];
	auto& regions = args.data[1];

	UnifiedVectorFormat number_data;
	UnifiedVectorFormat region_data;
	numbers.ToUnifiedFormat(args.size(), number_data);
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<bool>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = UnifiedVectorFormat::GetData<string_t>(number_data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString()
		                     : std::string();

		// NULL: no metadata to judge the number (unknown region or country code)
		const auto judgement = PhoneNumberManager::Instance().IsPossible(raw_number, region_hint, default_region);
		FlatVector::SetNull(result, i, !judgement.has_value());
		result_data[i] = judgement.value_or(false);
	}
}

void PhoneIsValidForRegionFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& numbers = args.data[0];
	auto& regions = args.data[1];

	UnifiedVectorFormat number_data;
	UnifiedVectorFormat region_data;
	numbers.ToUnifiedFormat(args.size(), number_data);
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<bool>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = UnifiedVectorFormat::GetData<string_t>(number_data)[nr_idx].GetString();
		// When region is NULL, pass empty string to use default region (US)
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString()
		                     : std::string();

		// NULL: no metadata to judge the number (unknown region or country code)
		const auto judgement = PhoneNumberManager::Instance().IsValidForRegion(raw_number, region_hint, default_region);
		FlatVector::SetNull(result, i, !judgement.has_value());
		result_data[i] = judgement.value_or(false);
	}
}

void PhoneMatchFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& numbers1 = args.data[0];
	auto& numbers2 = args.data[1];
	auto& regions = args.data[2];

	UnifiedVectorFormat number1_data;
	UnifiedVectorFormat number2_data;
	UnifiedVectorFormat region_data;
	numbers1.ToUnifiedFormat(args.size(), number1_data);
	numbers2.ToUnifiedFormat(args.size(), number2_data);
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr1_idx = number1_data.sel->get_index(i);
		auto nr2_idx = number2_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number1_data.validity.RowIsValid(nr1_idx) ||
		    !number2_data.validity.RowIsValid(nr2_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number1 = UnifiedVectorFormat::GetData<string_t>(number1_data)[nr1_idx].GetString();
		auto raw_number2 = UnifiedVectorFormat::GetData<string_t>(number2_data)[nr2_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString()
		                     : std::string();

		auto match_result = PhoneNumberManager::Instance().Match(raw_number1, raw_number2, region_hint, default_region);
		result_data[i] = StringVector::AddString(result, match_result);
		FlatVector::SetNull(result, i, false);
	}
}

void PhoneExampleFunction(DataChunk& args, ExpressionState& state, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();
	const auto& default_region = GetBoundDefaultRegion(state);

	auto& regions = args.data[0];

	UnifiedVectorFormat region_data;
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = duckdb_compat::FlatData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto reg_idx = region_data.sel->get_index(i);

		if (!region_data.validity.RowIsValid(reg_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto region_hint = UnifiedVectorFormat::GetData<string_t>(region_data)[reg_idx].GetString();

		auto example = PhoneNumberManager::Instance().GetExampleNumber(region_hint, default_region);
		if (example.empty()) {
			FlatVector::SetNull(result, i, true);
		} else {
			result_data[i] = StringVector::AddString(result, example);
			FlatVector::SetNull(result, i, false);
		}
	}
}

struct PhoneStatusState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<GlobalTableFunctionState> PhoneStatusInit(ClientContext&, TableFunctionInitInput&) {
	return make_uniq<PhoneStatusState>();
}

unique_ptr<FunctionData> PhoneStatusBind(ClientContext& context, TableFunctionBindInput&,
                                         vector<LogicalType>& return_types, duckdb_compat::BindNames &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("phonenumber_status");
	names.emplace_back("initialized");
	return_types.emplace_back(LogicalTypeId::BOOLEAN);
	names.emplace_back("default_region");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	return make_uniq<PhoneBindData>(SnapshotDefaultRegion(context));
}

// Shared bind: capture telemetry and snapshot the session default region
unique_ptr<FunctionData> PhoneScalarBind(ClientContext& context, const char* telemetry_name) {
	PostHogTelemetry::Instance().RecordFunctionCall(telemetry_name);
	return make_uniq<PhoneBindData>(SnapshotDefaultRegion(context));
}

unique_ptr<FunctionData> PhoneParseBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_parse");
}

unique_ptr<FunctionData> PhoneFormatBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_format");
}

unique_ptr<FunctionData> PhoneRegionBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_region");
}

unique_ptr<FunctionData> PhoneIsValidBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_is_valid");
}

unique_ptr<FunctionData> PhoneIsPossibleBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_is_possible");
}

unique_ptr<FunctionData> PhoneIsValidForRegionBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_is_valid_for_region");
}

unique_ptr<FunctionData> PhoneMatchBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_match");
}

unique_ptr<FunctionData> PhoneExampleBind(ClientContext &context) {
	return PhoneScalarBind(context, "phonenumber_example");
}

void PhoneStatusFunction(ClientContext&, TableFunctionInput& input, DataChunk& output) {
	auto& state = input.global_state->Cast<PhoneStatusState>();
	if (state.done) {
		return;
	}

	auto status = PhoneNumberManager::Instance().GetStatus();
	// Report the session's default region (snapshotted at bind time), not the
	// process-wide fallback.
	auto& bind_data = input.bind_data->Cast<PhoneBindData>();
	duckdb_compat::SetChildCardinality(output, 1);
	output.SetValue(0, 0, Value::BOOLEAN(status.initialized));
	output.SetValue(1, 0, Value(bind_data.default_region));
	state.done = true;
}

// All phonenumber scalar functions are CONSISTENT_WITHIN_QUERY: their results
// depend on the session default-region setting, which is snapshotted into the
// bind data at bind time, so they are stable within a query but not across
// queries.
ScalarFunction CreateParseScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalType::STRUCT({{"valid", LogicalTypeId::BOOLEAN},
	                                             {"country_code", LogicalTypeId::INTEGER},
	                                             {"national_number", LogicalTypeId::VARCHAR},
	                                             {"region", LogicalTypeId::VARCHAR},
	                                             {"type", LogicalTypeId::VARCHAR},
	                                             {"reason", LogicalTypeId::VARCHAR},
	                                             {"extension", LogicalTypeId::VARCHAR}}),
	                        PhoneParseFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunction CreateFormatScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalTypeId::VARCHAR, PhoneFormatFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunction CreateRegionScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR,
	                        PhoneRegionFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunction CreateIsValidScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsValidFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunction CreateIsPossibleScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsPossibleFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunction CreateIsValidForRegionScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsValidForRegionFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunction CreateMatchScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalTypeId::VARCHAR, PhoneMatchFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunction CreateExampleScalar(const string& name) {
	ScalarFunction function(duckdb_compat::FunctionName(name), {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, PhoneExampleFunction);
	duckdb_compat::SetStability(function, FunctionStability::CONSISTENT_WITHIN_QUERY);
	duckdb_compat::SetNullHandling(function, FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

TableFunction CreateStatusTable(const string& name) {
	return TableFunction(duckdb_compat::FunctionName(name), {}, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PhoneStatusFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PhoneStatusBind), PhoneStatusInit);
}

} // namespace


void RegisterPhonenumberOptions(ExtensionLoader& loader) {
	auto& config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("anofox_tab_phonenumber_default_region",
	                          "Default region code used when the region hint is NULL",
	                          LogicalTypeId::VARCHAR, Value("US"), SetPhonenumberDefaultRegionOption);
}

void RegisterPhonenumberFunctions(ExtensionLoader& loader) {
	RegisterPhonenumberOptions(loader);

	// Register phonenumber_parse
	{
		FunctionDescription desc;
		desc.description = "Parses a phone number string and returns a struct with validity, country code, national number, region, number type, the extension split off the input and, for an invalid number, the reason.";
		desc.parameter_names = {"phone", "region"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_parse('+14155552671', NULL);"};
		desc.categories = {"phone", "parsing"};
		ScalarFunction parse_func = CreateParseScalar("anofox_tab_phonenumber_parse");
		parse_func.SetBindCallback(&ScalarContextBind<PhoneParseBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, parse_func, "phonenumber_parse", {std::move(desc)});
	}

	// Register phonenumber_format
	{
		FunctionDescription desc;
		desc.description = "Formats a valid phone number in the specified format: 'E164', 'INTERNATIONAL', 'NATIONAL', or 'RFC3966'. Returns NULL for a number that is not valid (see phonenumber_parse().reason).";
		desc.parameter_names = {"phone", "region", "format"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_format('+14155552671', NULL, 'NATIONAL');"};
		desc.categories = {"phone", "formatting"};
		ScalarFunction format_func = CreateFormatScalar("anofox_tab_phonenumber_format");
		format_func.SetBindCallback(&ScalarContextBind<PhoneFormatBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, format_func, "phonenumber_format", {std::move(desc)});
	}

	// Register phonenumber_region
	{
		FunctionDescription desc;
		desc.description = "Returns the 2-letter ISO region code (e.g., 'US', 'DE') of a valid phone number, or NULL for a number that is not valid.";
		desc.parameter_names = {"phone", "region"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_region('+14155552671', NULL);"};
		desc.categories = {"phone", "parsing"};
		ScalarFunction region_func = CreateRegionScalar("anofox_tab_phonenumber_region");
		region_func.SetBindCallback(&ScalarContextBind<PhoneRegionBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, region_func, "phonenumber_region", {std::move(desc)});
	}

	// Register phonenumber_is_valid
	{
		FunctionDescription desc;
		desc.description = "Returns TRUE if the phone number is a valid, dialable number.";
		desc.parameter_names = {"phone", "region"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_is_valid('+14155552671', NULL);"};
		desc.categories = {"phone", "validation"};
		ScalarFunction is_valid_func = CreateIsValidScalar("anofox_tab_phonenumber_is_valid");
		is_valid_func.SetBindCallback(&ScalarContextBind<PhoneIsValidBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, is_valid_func, "phonenumber_is_valid", {std::move(desc)});
	}

	// Register phonenumber_is_possible
	{
		FunctionDescription desc;
		desc.description = "Returns TRUE if the phone number length is plausible for its region (lighter check than is_valid).";
		desc.parameter_names = {"phone", "region"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_is_possible('+14155552671', NULL);"};
		desc.categories = {"phone", "validation"};
		ScalarFunction is_possible_func = CreateIsPossibleScalar("anofox_tab_phonenumber_is_possible");
		is_possible_func.SetBindCallback(&ScalarContextBind<PhoneIsPossibleBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, is_possible_func, "phonenumber_is_possible", {std::move(desc)});
	}

	// Register phonenumber_is_valid_for_region
	{
		FunctionDescription desc;
		desc.description = "Returns TRUE if the phone number is valid for the specified 2-letter ISO region code (e.g., 'US').";
		desc.parameter_names = {"phone", "region"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_is_valid_for_region('+14155552671', 'US');"};
		desc.categories = {"phone", "validation"};
		ScalarFunction is_valid_for_region_func = CreateIsValidForRegionScalar("anofox_tab_phonenumber_is_valid_for_region");
		is_valid_for_region_func.SetBindCallback(&ScalarContextBind<PhoneIsValidForRegionBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, is_valid_for_region_func, "phonenumber_is_valid_for_region", {std::move(desc)});
	}

	// Register phonenumber_match
	{
		FunctionDescription desc;
		desc.description = "Compares two phone number strings and returns the match type: 'EXACT_MATCH', 'NSN_MATCH', 'SHORT_NSN_MATCH', 'NO_MATCH', or 'NOT_A_NUMBER' when either string cannot be read as a phone number (blank, placeholder, letters).";
		desc.parameter_names = {"phone1", "phone2", "region"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_match('+14155552671', '(415) 555-2671', 'US');"};
		desc.categories = {"phone", "comparison"};
		ScalarFunction match_func = CreateMatchScalar("anofox_tab_phonenumber_match");
		match_func.SetBindCallback(&ScalarContextBind<PhoneMatchBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, match_func, "phonenumber_match", {std::move(desc)});
	}

	// Register phonenumber_example
	{
		FunctionDescription desc;
		desc.description = "Returns an example valid phone number for the given 2-letter ISO region code (e.g., 'US').";
		desc.parameter_names = {"region"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_example('US');"};
		desc.categories = {"phone", "utility"};
		ScalarFunction example_func = CreateExampleScalar("anofox_tab_phonenumber_example");
		example_func.SetBindCallback(&ScalarContextBind<PhoneExampleBind>::Bind);
		RegisterScalarFunctionWithAlias(loader, example_func, "phonenumber_example", {std::move(desc)});
	}

	// Register phonenumber_status (telemetry in DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PhoneStatusBind))
	{
		FunctionDescription desc;
		desc.description = "Returns the current configuration and status of the phone number module.";
		desc.examples = {"SELECT * FROM phonenumber_status();"};
		desc.categories = {"phone", "status"};
		TableFunction status_func = CreateStatusTable("anofox_tab_phonenumber_status");
		RegisterTableFunctionWithAlias(loader, status_func, "phonenumber_status", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

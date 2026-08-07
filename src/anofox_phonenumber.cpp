#include "anofox_phonenumber.hpp"
#include "anofox_phonenumber_metadata.hpp"
#include "anofox_function_alias.hpp"
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

#include "anofox_trace.hpp"

#include <mutex>
#include <string>
#include <regex>
#include <cctype>
#include <algorithm>
#include <tuple>

namespace duckdb {
namespace anofox {
namespace phonenumber {

namespace {

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

// Extract country code from explicit international (E.164) format.
// Only numbers starting with '+' carry a country code; plain national input is
// interpreted via the region hint / default region instead.
// Returns {country_code, remaining_national_number}; {0, ""} if no known code.
std::pair<int, std::string> ExtractCountryCode(const std::string& normalized) {
	if (normalized.empty() || normalized[0] != '+') {
		return {0, ""};
	}

	const size_t start_pos = 1;

	// Try to match country codes (1-3 digits)
	// Try 3 digits first, then 2, then 1
	for (int len = 3; len >= 1; len--) {
		if (normalized.size() < start_pos + len) {
			continue;
		}

		std::string code_str = normalized.substr(start_pos, len);
		try {
			int code = std::stoi(code_str);

			// Check if this country code exists in our metadata
			if (COUNTRY_CODE_TO_REGIONS.find(code) != COUNTRY_CODE_TO_REGIONS.end()) {
				std::string national_number = normalized.substr(start_pos + len);
				return {code, national_number};
			}
		} catch (...) {
			continue;
		}
	}

	// No known country code found
	return {0, ""};
}

// Result of matching a national number against a region's precompiled patterns.
struct PatternClassification {
	bool matches_fixed_line = false;
	bool matches_mobile = false;
	bool matches_toll_free = false;

	bool MatchesValidPattern() const {
		return matches_fixed_line || matches_mobile;
	}
	bool MatchesAnyPattern() const {
		return matches_fixed_line || matches_mobile || matches_toll_free;
	}
};

PatternClassification ClassifyNationalNumber(const std::string& national_number, const std::string& region_code) {
	PatternClassification classification;
	const CompiledRegionPatterns* patterns = GetCompiledPatternsForRegion(region_code);
	if (!patterns) {
		return classification;
	}
	classification.matches_toll_free = patterns->toll_free && std::regex_match(national_number, *patterns->toll_free);
	classification.matches_fixed_line =
	    patterns->fixed_line && std::regex_match(national_number, *patterns->fixed_line);
	classification.matches_mobile = patterns->mobile && std::regex_match(national_number, *patterns->mobile);
	return classification;
}

// Determine phone number type from a pattern classification
std::string DeterminePhoneNumberType(const PatternClassification& classification) {
	// Toll-free takes precedence
	if (classification.matches_toll_free) {
		return PhoneNumberType::TOLL_FREE;
	}

	// If matches both, return FIXED_LINE_OR_MOBILE (e.g., US/NANPA numbers)
	if (classification.matches_mobile && classification.matches_fixed_line) {
		return PhoneNumberType::FIXED_LINE_OR_MOBILE;
	}

	if (classification.matches_mobile) {
		return PhoneNumberType::MOBILE;
	}

	if (classification.matches_fixed_line) {
		return PhoneNumberType::FIXED_LINE;
	}

	return PhoneNumberType::UNKNOWN;
}

// Resolve the region for a country code extracted from international input.
// For shared country codes (e.g. +1 NANPA) a region hint belonging to the
// code's region list wins; otherwise the candidates' validation patterns are
// tried, falling back to the main (first) region.
std::string ResolveRegionForCountryCode(int country_code, const std::string& region_hint_upper,
                                        const std::string& national_number) {
	auto it = COUNTRY_CODE_TO_REGIONS.find(country_code);
	if (it == COUNTRY_CODE_TO_REGIONS.end() || it->second.empty()) {
		return "";
	}
	const auto& candidates = it->second;
	if (candidates.size() == 1) {
		return candidates[0];
	}

	if (!region_hint_upper.empty() &&
	    std::find(candidates.begin(), candidates.end(), region_hint_upper) != candidates.end()) {
		return region_hint_upper;
	}

	for (const auto& candidate : candidates) {
		const CountryMetadata* metadata = GetMetadataForRegion(candidate);
		if (!metadata || !metadata->IsValidLength(national_number.size())) {
			continue;
		}
		if (ClassifyNationalNumber(national_number, candidate).MatchesAnyPattern()) {
			return candidate;
		}
	}

	// Fall back to the main region
	return candidates[0];
}

std::mutex& RegionMutex() {
	static std::mutex region_mutex;
	return region_mutex;
}

} // namespace

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

	// Normalize the input
	std::string normalized = NormalizePhoneNumber(raw_number);
	if (normalized.empty()) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: empty number");
		return parts;
	}

	std::string hint_upper = region_hint.empty() ? std::string() : StringUtil::Upper(region_hint);

	int country_code = 0;
	std::string national_number;
	std::string region_code;

	if (normalized[0] == '+') {
		// Explicit international format: derive the region from the country code,
		// using the hint only to disambiguate shared country codes (e.g. +1).
		std::tie(country_code, national_number) = ExtractCountryCode(normalized);
		if (country_code == 0) {
			AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: unknown country code in " + normalized);
			return parts;
		}
		region_code = ResolveRegionForCountryCode(country_code, hint_upper, national_number);
		if (region_code.empty()) {
			AnofoxTrace(AnofoxLogLevel::Debug,
			           "PhoneNumber parse failed: no region found for country_code=" + std::to_string(country_code));
			return parts;
		}
	} else {
		// National format: interpret via the region hint, falling back to the
		// (session-snapshotted) default region.
		region_code = !hint_upper.empty() && GetMetadataForRegion(hint_upper) ? hint_upper
		                                                                      : EffectiveDefaultRegion(default_region);
		const CountryMetadata* hint_metadata = GetMetadataForRegion(region_code);
		if (!hint_metadata) {
			AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: could not determine region");
			return parts;
		}
		country_code = hint_metadata->country_code;
		national_number = normalized;
		// Remove national prefix if present
		if (!hint_metadata->national_prefix.empty() &&
		    national_number.find(hint_metadata->national_prefix) == 0) {
			national_number = national_number.substr(hint_metadata->national_prefix.size());
		}
	}

	const CountryMetadata* metadata = GetMetadataForRegion(region_code);
	if (!metadata) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: no metadata for region=" + region_code);
		return parts;
	}

	// Validate length
	if (!metadata->IsValidLength(national_number.size())) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber parse failed: invalid length=" + std::to_string(national_number.size()) +
		           " for region=" + region_code);
		return parts;
	}

	// Successful parse
	auto classification = ClassifyNationalNumber(national_number, region_code);
	parts.valid = true;
	parts.country_code = country_code;
	parts.national_number = national_number;
	parts.region_code = region_code;
	parts.type = DeterminePhoneNumberType(classification);
	parts.matches_valid_pattern = classification.MatchesValidPattern();

	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber parse success region=" + parts.region_code + " type=" + parts.type);
	return parts;
}

std::string PhoneNumberManager::Format(const std::string& raw_number, const std::string& region_hint,
                                       PhoneNumberFormatOption format_option, const std::string& default_region) {
	EnsureInitialized();

	// Parse the number first
	auto parts = Parse(raw_number, region_hint, default_region);
	if (!parts.valid) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber format parse failed number=" + raw_number);
		throw InvalidInputException("Invalid phone number: %s", raw_number);
	}

	const CountryMetadata* metadata = GetMetadataForRegion(parts.region_code);
	if (!metadata) {
		throw InvalidInputException("No metadata for region: %s", parts.region_code);
	}

	std::string formatted;
	std::string format_label;

	switch (format_option) {
	case PhoneNumberFormatOption::E164:
		// E164: +<country_code><national_number>
		formatted = "+" + std::to_string(parts.country_code) + parts.national_number;
		format_label = "E164";
		break;

	case PhoneNumberFormatOption::INTERNATIONAL:
		// INTERNATIONAL: +<country_code> <formatted_national>
		formatted = "+" + std::to_string(parts.country_code) + " ";
		// Simple formatting: insert space every 3-4 digits
		if (parts.national_number.size() == 10) {
			// US/CA format: XXX XXX XXXX
			formatted += parts.national_number.substr(0, 3) + " " +
			            parts.national_number.substr(3, 3) + " " +
			            parts.national_number.substr(6);
		} else {
			// Generic: just add spaces every 3 digits
			for (size_t i = 0; i < parts.national_number.size(); i++) {
				if (i > 0 && i % 3 == 0) {
					formatted += " ";
				}
				formatted += parts.national_number[i];
			}
		}
		format_label = "INTERNATIONAL";
		break;

	case PhoneNumberFormatOption::NATIONAL:
		// NATIONAL: Regional formatting
		if (parts.national_number.size() == 10) {
			// US/CA format: (XXX) XXX-XXXX
			formatted = "(" + parts.national_number.substr(0, 3) + ") " +
			           parts.national_number.substr(3, 3) + "-" +
			           parts.national_number.substr(6);
		} else {
			// Generic: prepend national prefix if exists
			if (!metadata->national_prefix.empty()) {
				formatted = metadata->national_prefix;
			}
			// Add spaces every 3 digits
			for (size_t i = 0; i < parts.national_number.size(); i++) {
				if (i > 0 && i % 3 == 0) {
					formatted += " ";
				}
				formatted += parts.national_number[i];
			}
		}
		format_label = "NATIONAL";
		break;

	case PhoneNumberFormatOption::RFC3966:
		// RFC3966: tel:+<country_code>-<formatted>
		formatted = "tel:+" + std::to_string(parts.country_code) + "-";
		// Simple formatting with hyphens every 3-4 digits
		if (parts.national_number.size() == 10) {
			formatted += parts.national_number.substr(0, 3) + "-" +
			            parts.national_number.substr(3, 3) + "-" +
			            parts.national_number.substr(6);
		} else {
			for (size_t i = 0; i < parts.national_number.size(); i++) {
				if (i > 0 && i % 3 == 0) {
					formatted += "-";
				}
				formatted += parts.national_number[i];
			}
		}
		format_label = "RFC3966";
		break;
	}

	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber format success number=" + raw_number + " format=" + format_label + " output=" + formatted);
	return formatted;
}

std::string PhoneNumberManager::GetRegion(const std::string& raw_number, const std::string& region_hint,
                                          const std::string& default_region) {
	EnsureInitialized();
	auto parts = Parse(raw_number, region_hint, default_region);
	if (!parts.valid) {
		throw InvalidInputException("Invalid phone number: %s", raw_number);
	}
	return parts.region_code;
}

bool PhoneNumberManager::IsValid(const std::string& raw_number, const std::string& region_hint,
                                 const std::string& default_region) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint, default_region);
	if (!parts.valid) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber IsValid parse failed number=" + raw_number);
		return false;
	}

	// Pattern classification already happened during Parse
	bool is_valid = parts.matches_valid_pattern;
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber IsValid result=" + std::string(is_valid ? "true" : "false") +
	           " number=" + raw_number);
	return is_valid;
}

bool PhoneNumberManager::IsPossible(const std::string& raw_number, const std::string& region_hint,
                                    const std::string& default_region) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint, default_region);
	if (!parts.valid) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber IsPossible parse failed number=" + raw_number);
		return false;
	}

	// IsPossible only checks length, not patterns
	bool is_possible = parts.valid;  // If parse succeeded, it's already length-checked
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber IsPossible result=" + std::string(is_possible ? "true" : "false") +
	           " number=" + raw_number);
	return is_possible;
}

bool PhoneNumberManager::IsValidForRegion(const std::string& raw_number, const std::string& region_hint,
                                          const std::string& default_region) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint, default_region);
	if (!parts.valid) {
		return false;
	}

	// Check if the parsed region matches the requested region
	std::string requested_region =
	    region_hint.empty() ? EffectiveDefaultRegion(default_region) : StringUtil::Upper(region_hint);
	if (parts.region_code != requested_region) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber IsValidForRegion failed: parsed_region=" + parts.region_code +
		           " != requested_region=" + requested_region);
		return false;
	}

	// Also validate against patterns (classified during Parse)
	return parts.matches_valid_pattern;
}

std::string PhoneNumberManager::Match(const std::string& number1, const std::string& number2,
                                      const std::string& region_hint, const std::string& default_region) {
	EnsureInitialized();

	auto parts1 = Parse(number1, region_hint, default_region);
	auto parts2 = Parse(number2, region_hint, default_region);

	// If either parse failed, check if raw strings match exactly
	if (!parts1.valid || !parts2.valid) {
		// For short numbers or invalid formats, if both strings are identical, it's an exact match
		if (number1 == number2) {
			AnofoxTrace(AnofoxLogLevel::Debug,
			           "PhoneNumber Match result=EXACT_MATCH (raw string match, parse failed) number1=" + number1 + " number2=" + number2);
			return "EXACT_MATCH";
		}
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber Match result=NO_MATCH (parse failed) number1=" + number1 + " number2=" + number2);
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

	std::string region = region_hint.empty() ? EffectiveDefaultRegion(default_region) : StringUtil::Upper(region_hint);
	const CountryMetadata* metadata = GetMetadataForRegion(region);

	if (!metadata) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber GetExampleNumber failed for region=" + region);
		return "";
	}

	// Return E164 format of example number
	std::string formatted = "+" + std::to_string(metadata->country_code) + metadata->example_number;
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber GetExampleNumber result=" + formatted + " region=" + region);
	return formatted;
}

void PhoneNumberManager::SetDefaultRegion(const std::string& region) {
	EnsureInitialized();
	std::lock_guard<std::mutex> lock(RegionMutex());
	default_region = StringUtil::Upper(region);
	AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber default region set to " + default_region);
}

std::string PhoneNumberManager::GetDefaultRegion() const {
	std::lock_guard<std::mutex> lock(RegionMutex());
	return default_region;
}

std::string PhoneNumberManager::EffectiveDefaultRegion(const std::string& default_region_p) const {
	if (!default_region_p.empty()) {
		return StringUtil::Upper(default_region_p);
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

PhoneNumberFormatOption ParseFormatOption(const std::string& format_str) {
	auto upper = StringUtil::Upper(format_str);
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
	    "Unknown phone number format '%s'. Expected one of E164, INTERNATIONAL, NATIONAL, RFC3966", format_str);
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
	auto region = StringUtil::Upper(parameter.ToString());
	if (!phonenumber::GetMetadataForRegion(region)) {
		throw InvalidInputException(
		    "Unknown region code '%s' for anofox_tab_phonenumber_default_region; expected a supported "
		    "2-letter ISO region code (e.g. 'US', 'GB', 'DE')",
		    parameter.ToString());
	}
	// Store the normalized (uppercase) value in the setting
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
		return StringUtil::Upper(value.ToString());
	}
	return PhoneNumberManager::Instance().GetDefaultRegion();
}

const std::string& GetBoundDefaultRegion(ExpressionState& state) {
	auto& func_expr = state.expr.Cast<BoundFunctionExpression>();
	D_ASSERT(func_expr.bind_info);
	return func_expr.bind_info->Cast<PhoneBindData>().default_region;
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

	auto& valid_vec = *children[0];
	auto& country_code_vec = *children[1];
	auto& national_number_vec = *children[2];
	auto& region_vec = *children[3];
	auto& type_vec = *children[4];

	auto valid_data = FlatVector::GetData<bool>(valid_vec);
	auto country_data = FlatVector::GetData<int32_t>(country_code_vec);
	auto national_data = FlatVector::GetData<string_t>(national_number_vec);
	auto region_data_out = FlatVector::GetData<string_t>(region_vec);
	auto type_data = FlatVector::GetData<string_t>(type_vec);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			for (auto& child : children) {
				FlatVector::SetNull(*child, i, true);
			}
			continue;
		}

		auto raw_number = reinterpret_cast<string_t*>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString()
		                     : std::string();

		auto parts = PhoneNumberManager::Instance().Parse(raw_number, region_hint, default_region);

		FlatVector::SetNull(result, i, false);
		FlatVector::SetNull(valid_vec, i, false);
		valid_data[i] = parts.valid;

		if (parts.valid) {
			FlatVector::SetNull(country_code_vec, i, false);
			FlatVector::SetNull(national_number_vec, i, false);
			FlatVector::SetNull(region_vec, i, false);
			FlatVector::SetNull(type_vec, i, false);

			country_data[i] = parts.country_code;
			national_data[i] = StringVector::AddString(national_number_vec, parts.national_number);
			region_data_out[i] = StringVector::AddString(region_vec, parts.region_code);
			type_data[i] = StringVector::AddString(type_vec, parts.type);
		} else {
			FlatVector::SetNull(country_code_vec, i, true);
			FlatVector::SetNull(national_number_vec, i, true);
			FlatVector::SetNull(region_vec, i, true);
			FlatVector::SetNull(type_vec, i, true);
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
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);
		auto fmt_idx = format_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx) ||
		    !format_data.validity.RowIsValid(fmt_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = reinterpret_cast<string_t*>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString()
		                     : std::string();
		auto format_str = reinterpret_cast<string_t*>(format_data.data)[fmt_idx].GetString();

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
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = reinterpret_cast<string_t*>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString()
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
	auto result_data = FlatVector::GetData<bool>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = reinterpret_cast<string_t*>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString()
		                     : std::string();

		result_data[i] = PhoneNumberManager::Instance().IsValid(raw_number, region_hint, default_region);
		FlatVector::SetNull(result, i, false);
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
	auto result_data = FlatVector::GetData<bool>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = reinterpret_cast<string_t*>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString()
		                     : std::string();

		result_data[i] = PhoneNumberManager::Instance().IsPossible(raw_number, region_hint, default_region);
		FlatVector::SetNull(result, i, false);
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
	auto result_data = FlatVector::GetData<bool>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr_idx = number_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = reinterpret_cast<string_t*>(number_data.data)[nr_idx].GetString();
		// When region is NULL, pass empty string to use default region (US)
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString()
		                     : std::string();

		result_data[i] = PhoneNumberManager::Instance().IsValidForRegion(raw_number, region_hint, default_region);
		FlatVector::SetNull(result, i, false);
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
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto nr1_idx = number1_data.sel->get_index(i);
		auto nr2_idx = number2_data.sel->get_index(i);
		auto reg_idx = region_data.sel->get_index(i);

		if (!number1_data.validity.RowIsValid(nr1_idx) ||
		    !number2_data.validity.RowIsValid(nr2_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number1 = reinterpret_cast<string_t*>(number1_data.data)[nr1_idx].GetString();
		auto raw_number2 = reinterpret_cast<string_t*>(number2_data.data)[nr2_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString()
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
	auto result_data = FlatVector::GetData<string_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto reg_idx = region_data.sel->get_index(i);

		if (!region_data.validity.RowIsValid(reg_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto region_hint = reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString();

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
                                         vector<LogicalType>& return_types, vector<string>& names) {
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

unique_ptr<FunctionData> PhoneParseBind(ClientContext& context, ScalarFunction&, vector<unique_ptr<Expression>>&) {
	return PhoneScalarBind(context, "phonenumber_parse");
}

unique_ptr<FunctionData> PhoneFormatBind(ClientContext& context, ScalarFunction&, vector<unique_ptr<Expression>>&) {
	return PhoneScalarBind(context, "phonenumber_format");
}

unique_ptr<FunctionData> PhoneRegionBind(ClientContext& context, ScalarFunction&, vector<unique_ptr<Expression>>&) {
	return PhoneScalarBind(context, "phonenumber_region");
}

unique_ptr<FunctionData> PhoneIsValidBind(ClientContext& context, ScalarFunction&, vector<unique_ptr<Expression>>&) {
	return PhoneScalarBind(context, "phonenumber_is_valid");
}

unique_ptr<FunctionData> PhoneIsPossibleBind(ClientContext& context, ScalarFunction&,
                                             vector<unique_ptr<Expression>>&) {
	return PhoneScalarBind(context, "phonenumber_is_possible");
}

unique_ptr<FunctionData> PhoneIsValidForRegionBind(ClientContext& context, ScalarFunction&,
                                                   vector<unique_ptr<Expression>>&) {
	return PhoneScalarBind(context, "phonenumber_is_valid_for_region");
}

unique_ptr<FunctionData> PhoneMatchBind(ClientContext& context, ScalarFunction&, vector<unique_ptr<Expression>>&) {
	return PhoneScalarBind(context, "phonenumber_match");
}

unique_ptr<FunctionData> PhoneExampleBind(ClientContext& context, ScalarFunction&, vector<unique_ptr<Expression>>&) {
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
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(status.initialized));
	output.SetValue(1, 0, Value(bind_data.default_region));
	state.done = true;
}

// All phonenumber scalar functions are CONSISTENT_WITHIN_QUERY: their results
// depend on the session default-region setting, which is snapshotted into the
// bind data at bind time, so they are stable within a query but not across
// queries.
ScalarFunction CreateParseScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalType::STRUCT({{"valid", LogicalTypeId::BOOLEAN},
	                                             {"country_code", LogicalTypeId::INTEGER},
	                                             {"national_number", LogicalTypeId::VARCHAR},
	                                             {"region", LogicalTypeId::VARCHAR},
	                                             {"type", LogicalTypeId::VARCHAR}}),
	                        PhoneParseFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateFormatScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalTypeId::VARCHAR, PhoneFormatFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateRegionScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR,
	                        PhoneRegionFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateIsValidScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsValidFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateIsPossibleScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsPossibleFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateIsValidForRegionScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsValidForRegionFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateMatchScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalTypeId::VARCHAR, PhoneMatchFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateExampleScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, PhoneExampleFunction);
	function.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

TableFunction CreateStatusTable(const string& name) {
	return TableFunction(name, {}, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PhoneStatusFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PhoneStatusBind), PhoneStatusInit);
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
		desc.description = "Parses a phone number string and returns a struct with E164, national, international, and RFC3966 formats, plus the region code.";
		desc.parameter_names = {"phone"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_parse('+14155552671');"};
		desc.categories = {"phone", "parsing"};
		ScalarFunction parse_func = CreateParseScalar("anofox_tab_phonenumber_parse");
		parse_func.bind = PhoneParseBind;
		RegisterScalarFunctionWithAlias(loader, parse_func, "phonenumber_parse", {std::move(desc)});
	}

	// Register phonenumber_format
	{
		FunctionDescription desc;
		desc.description = "Formats a phone number string in the specified format: 'E164', 'INTERNATIONAL', 'NATIONAL', or 'RFC3966'.";
		desc.parameter_names = {"phone", "format"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_format('+14155552671', 'NATIONAL');"};
		desc.categories = {"phone", "formatting"};
		ScalarFunction format_func = CreateFormatScalar("anofox_tab_phonenumber_format");
		format_func.bind = PhoneFormatBind;
		RegisterScalarFunctionWithAlias(loader, format_func, "phonenumber_format", {std::move(desc)});
	}

	// Register phonenumber_region
	{
		FunctionDescription desc;
		desc.description = "Returns the 2-letter ISO region code (e.g., 'US', 'DE') for a given phone number.";
		desc.parameter_names = {"phone"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_region('+14155552671');"};
		desc.categories = {"phone", "parsing"};
		ScalarFunction region_func = CreateRegionScalar("anofox_tab_phonenumber_region");
		region_func.bind = PhoneRegionBind;
		RegisterScalarFunctionWithAlias(loader, region_func, "phonenumber_region", {std::move(desc)});
	}

	// Register phonenumber_is_valid
	{
		FunctionDescription desc;
		desc.description = "Returns TRUE if the phone number is a valid, dialable number.";
		desc.parameter_names = {"phone"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_is_valid('+14155552671');"};
		desc.categories = {"phone", "validation"};
		ScalarFunction is_valid_func = CreateIsValidScalar("anofox_tab_phonenumber_is_valid");
		is_valid_func.bind = PhoneIsValidBind;
		RegisterScalarFunctionWithAlias(loader, is_valid_func, "phonenumber_is_valid", {std::move(desc)});
	}

	// Register phonenumber_is_possible
	{
		FunctionDescription desc;
		desc.description = "Returns TRUE if the phone number length is plausible for its region (lighter check than is_valid).";
		desc.parameter_names = {"phone"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_is_possible('+14155552671');"};
		desc.categories = {"phone", "validation"};
		ScalarFunction is_possible_func = CreateIsPossibleScalar("anofox_tab_phonenumber_is_possible");
		is_possible_func.bind = PhoneIsPossibleBind;
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
		is_valid_for_region_func.bind = PhoneIsValidForRegionBind;
		RegisterScalarFunctionWithAlias(loader, is_valid_for_region_func, "phonenumber_is_valid_for_region", {std::move(desc)});
	}

	// Register phonenumber_match
	{
		FunctionDescription desc;
		desc.description = "Returns TRUE if two phone number strings refer to the same number.";
		desc.parameter_names = {"phone1", "phone2"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
		desc.examples = {"SELECT phonenumber_match('+14155552671', '(415) 555-2671');"};
		desc.categories = {"phone", "comparison"};
		ScalarFunction match_func = CreateMatchScalar("anofox_tab_phonenumber_match");
		match_func.bind = PhoneMatchBind;
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
		example_func.bind = PhoneExampleBind;
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

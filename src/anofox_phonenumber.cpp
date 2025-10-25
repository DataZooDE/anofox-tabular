#include "anofox_phonenumber.hpp"
#include "anofox_phonenumber_metadata.hpp"

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

#include "anofox_trace.hpp"

#include <mutex>
#include <string>
#include <regex>
#include <cctype>
#include <algorithm>

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

// Extract country code from E.164/international format
// Returns {country_code, remaining_national_number}
std::pair<int, std::string> ExtractCountryCode(const std::string& normalized) {
	if (normalized.empty()) {
		return {0, ""};
	}

	// Check if starts with +
	size_t start_pos = 0;
	if (normalized[0] == '+') {
		start_pos = 1;
	}

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

	// No country code found
	return {0, normalized.substr(start_pos)};
}

// Determine phone number type based on regex patterns
std::string DeterminePhoneNumberType(const std::string& national_number, const CountryMetadata* metadata) {
	if (!metadata) {
		return PhoneNumberType::UNKNOWN;
	}

	try {
		// Try toll_free pattern first
		if (!metadata->toll_free_pattern.empty()) {
			std::regex toll_free_regex(metadata->toll_free_pattern);
			if (std::regex_match(national_number, toll_free_regex)) {
				return PhoneNumberType::TOLL_FREE;
			}
		}

		// Try mobile pattern
		if (!metadata->mobile_pattern.empty()) {
			std::regex mobile_regex(metadata->mobile_pattern);
			if (std::regex_match(national_number, mobile_regex)) {
				return PhoneNumberType::MOBILE;
			}
		}

		// Try fixed_line pattern
		if (!metadata->fixed_line_pattern.empty()) {
			std::regex fixed_line_regex(metadata->fixed_line_pattern);
			if (std::regex_match(national_number, fixed_line_regex)) {
				return PhoneNumberType::FIXED_LINE;
			}
		}
	} catch (const std::regex_error& e) {
		AnofoxTrace(AnofoxLogLevel::Warn, "Regex error in DeterminePhoneNumberType: " + std::string(e.what()));
	}

	return PhoneNumberType::UNKNOWN;
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

PhoneNumberParts PhoneNumberManager::Parse(const std::string& raw_number, const std::string& region_hint) {
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

	// Extract country code
	auto [country_code, national_number] = ExtractCountryCode(normalized);

	// If no country code found and we have a region hint, use that region's country code
	if (country_code == 0 && !region_hint.empty()) {
		std::string region = StringUtil::Upper(region_hint);
		const CountryMetadata* metadata = GetMetadataForRegion(region);
		if (metadata) {
			country_code = metadata->country_code;
			national_number = normalized;
			// Remove national prefix if present
			if (!metadata->national_prefix.empty() &&
			    national_number.find(metadata->national_prefix) == 0) {
				national_number = national_number.substr(metadata->national_prefix.size());
			}
		}
	}

	// If still no country code, try default region
	if (country_code == 0) {
		std::string default_reg = GetDefaultRegion();
		const CountryMetadata* metadata = GetMetadataForRegion(default_reg);
		if (metadata) {
			country_code = metadata->country_code;
			national_number = normalized;
			if (!metadata->national_prefix.empty() &&
			    national_number.find(metadata->national_prefix) == 0) {
				national_number = national_number.substr(metadata->national_prefix.size());
			}
		}
	}

	if (country_code == 0) {
		AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber parse failed: could not determine country code");
		return parts;
	}

	// Get region code for this country code
	std::string region_code = GetMainRegionForCountryCode(country_code);
	if (region_code.empty()) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber parse failed: no region found for country_code=" + std::to_string(country_code));
		return parts;
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
	parts.valid = true;
	parts.country_code = country_code;
	parts.national_number = national_number;
	parts.region_code = region_code;
	parts.type = DeterminePhoneNumberType(national_number, metadata);

	AnofoxTrace(AnofoxLogLevel::Debug,
	           "PhoneNumber parse success region=" + parts.region_code + " type=" + parts.type);
	return parts;
}

std::string PhoneNumberManager::Format(const std::string& raw_number, const std::string& region_hint,
                                       PhoneNumberFormatOption format_option) {
	EnsureInitialized();

	// Parse the number first
	auto parts = Parse(raw_number, region_hint);
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

std::string PhoneNumberManager::GetRegion(const std::string& raw_number, const std::string& region_hint) {
	EnsureInitialized();
	auto parts = Parse(raw_number, region_hint);
	if (!parts.valid) {
		throw InvalidInputException("Invalid phone number: %s", raw_number);
	}
	return parts.region_code;
}

bool PhoneNumberManager::IsValid(const std::string& raw_number, const std::string& region_hint) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint);
	if (!parts.valid) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber IsValid parse failed number=" + raw_number);
		return false;
	}

	// For valid, we need to match the pattern
	const CountryMetadata* metadata = GetMetadataForRegion(parts.region_code);
	if (!metadata) {
		return false;
	}

	try {
		// Check against fixed_line or mobile pattern
		bool matches_fixed = false;
		bool matches_mobile = false;

		if (!metadata->fixed_line_pattern.empty()) {
			std::regex fixed_regex(metadata->fixed_line_pattern);
			matches_fixed = std::regex_match(parts.national_number, fixed_regex);
		}

		if (!metadata->mobile_pattern.empty()) {
			std::regex mobile_regex(metadata->mobile_pattern);
			matches_mobile = std::regex_match(parts.national_number, mobile_regex);
		}

		bool is_valid = matches_fixed || matches_mobile;
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber IsValid result=" + std::string(is_valid ? "true" : "false") +
		           " number=" + raw_number);
		return is_valid;
	} catch (const std::regex_error& e) {
		AnofoxTrace(AnofoxLogLevel::Warn, "Regex error in IsValid: " + std::string(e.what()));
		return false;
	}
}

bool PhoneNumberManager::IsPossible(const std::string& raw_number, const std::string& region_hint) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint);
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

bool PhoneNumberManager::IsValidForRegion(const std::string& raw_number, const std::string& region_hint) {
	EnsureInitialized();

	auto parts = Parse(raw_number, region_hint);
	if (!parts.valid) {
		return false;
	}

	// Check if the parsed region matches the requested region
	std::string requested_region = StringUtil::Upper(region_hint.empty() ? GetDefaultRegion() : region_hint);
	if (parts.region_code != requested_region) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		           "PhoneNumber IsValidForRegion failed: parsed_region=" + parts.region_code +
		           " != requested_region=" + requested_region);
		return false;
	}

	// Also validate against patterns
	return IsValid(raw_number, region_hint);
}

std::string PhoneNumberManager::Match(const std::string& number1, const std::string& number2,
                                      const std::string& region_hint) {
	EnsureInitialized();

	auto parts1 = Parse(number1, region_hint);
	auto parts2 = Parse(number2, region_hint);

	// If either parse failed, it's NO_MATCH
	if (!parts1.valid || !parts2.valid) {
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

std::string PhoneNumberManager::GetExampleNumber(const std::string& region_hint) {
	EnsureInitialized();

	std::string region = StringUtil::Upper(region_hint.empty() ? GetDefaultRegion() : region_hint);
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
	if (upper == "RFC3966") {
		return PhoneNumberFormatOption::RFC3966;
	}
	return PhoneNumberFormatOption::NATIONAL;
}

} // namespace phonenumber
} // namespace anofox
} // namespace duckdb

// SQL Function registrations (keeping existing structure)
#include "duckdb/common/types/value.hpp"

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
	PhoneNumberManager::Instance().SetDefaultRegion(parameter.ToString());
}

void PhoneParseFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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

		auto parts = PhoneNumberManager::Instance().Parse(raw_number, region_hint);

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

void PhoneFormatFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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

		try {
			auto format_option = ParseFormatOption(format_str);
			auto formatted = PhoneNumberManager::Instance().Format(raw_number, region_hint, format_option);
			result_data[i] = StringVector::AddString(result, formatted);
			FlatVector::SetNull(result, i, false);
		} catch (const std::exception& e) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void PhoneRegionFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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
			auto region = PhoneNumberManager::Instance().GetRegion(raw_number, region_hint);
			result_data[i] = StringVector::AddString(result, region);
			FlatVector::SetNull(result, i, false);
		} catch (const std::exception& e) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void PhoneIsValidFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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

		result_data[i] = PhoneNumberManager::Instance().IsValid(raw_number, region_hint);
		FlatVector::SetNull(result, i, false);
	}
}

void PhoneIsPossibleFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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

		result_data[i] = PhoneNumberManager::Instance().IsPossible(raw_number, region_hint);
		FlatVector::SetNull(result, i, false);
	}
}

void PhoneIsValidForRegionFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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

		if (!number_data.validity.RowIsValid(nr_idx) ||
		    !region_data.validity.RowIsValid(reg_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = reinterpret_cast<string_t*>(number_data.data)[nr_idx].GetString();
		auto region_hint = reinterpret_cast<string_t*>(region_data.data)[reg_idx].GetString();

		result_data[i] = PhoneNumberManager::Instance().IsValidForRegion(raw_number, region_hint);
		FlatVector::SetNull(result, i, false);
	}
}

void PhoneMatchFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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

		auto match_result = PhoneNumberManager::Instance().Match(raw_number1, raw_number2, region_hint);
		result_data[i] = StringVector::AddString(result, match_result);
		FlatVector::SetNull(result, i, false);
	}
}

void PhoneExampleFunction(DataChunk& args, ExpressionState&, Vector& result) {
	PhoneNumberManager::Instance().EnsureInitialized();

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

		auto example = PhoneNumberManager::Instance().GetExampleNumber(region_hint);
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

unique_ptr<FunctionData> PhoneStatusBind(ClientContext&, TableFunctionBindInput&, vector<LogicalType>& return_types,
                                         vector<string>& names) {
	names.emplace_back("initialized");
	return_types.emplace_back(LogicalTypeId::BOOLEAN);
	names.emplace_back("default_region");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	return nullptr;
}

void PhoneStatusFunction(ClientContext&, TableFunctionInput& input, DataChunk& output) {
	auto& state = input.global_state->Cast<PhoneStatusState>();
	if (state.done) {
		return;
	}

	auto status = PhoneNumberManager::Instance().GetStatus();
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(status.initialized));
	output.SetValue(1, 0, Value(status.default_region));
	state.done = true;
}

ScalarFunction CreateParseScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalType::STRUCT({{"valid", LogicalTypeId::BOOLEAN},
	                                             {"country_code", LogicalTypeId::INTEGER},
	                                             {"national_number", LogicalTypeId::VARCHAR},
	                                             {"region", LogicalTypeId::VARCHAR},
	                                             {"type", LogicalTypeId::VARCHAR}}),
	                        PhoneParseFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateFormatScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalTypeId::VARCHAR, PhoneFormatFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateRegionScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR,
	                        PhoneRegionFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateIsValidScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsValidFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateIsPossibleScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsPossibleFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateIsValidForRegionScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN,
	                        PhoneIsValidForRegionFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateMatchScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
	                        LogicalTypeId::VARCHAR, PhoneMatchFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateExampleScalar(const string& name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, PhoneExampleFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

TableFunction CreateStatusTable(const string& name) {
	return TableFunction(name, {}, PhoneStatusFunction, PhoneStatusBind, PhoneStatusInit);
}

} // namespace


void RegisterPhonenumberOptions(ExtensionLoader& loader) {
	auto& config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("anofox_phonenumber_default_region",
	                          "Default region code used when the region hint is NULL",
	                          LogicalTypeId::VARCHAR, Value("US"), SetPhonenumberDefaultRegionOption);
}

void RegisterPhonenumberFunctions(ExtensionLoader& loader) {
	RegisterPhonenumberOptions(loader);
	loader.RegisterFunction(CreateParseScalar("anofox_phonenumber_parse"));
	loader.RegisterFunction(CreateFormatScalar("anofox_phonenumber_format"));
	loader.RegisterFunction(CreateRegionScalar("anofox_phonenumber_region"));
	loader.RegisterFunction(CreateIsValidScalar("anofox_phonenumber_is_valid"));
	loader.RegisterFunction(CreateIsPossibleScalar("anofox_phonenumber_is_possible"));
	loader.RegisterFunction(CreateIsValidForRegionScalar("anofox_phonenumber_is_valid_for_region"));
	loader.RegisterFunction(CreateMatchScalar("anofox_phonenumber_match"));
	loader.RegisterFunction(CreateExampleScalar("anofox_phonenumber_example"));
	loader.RegisterFunction(CreateStatusTable("anofox_phonenumber_status"));
}

} // namespace anofox
} // namespace duckdb

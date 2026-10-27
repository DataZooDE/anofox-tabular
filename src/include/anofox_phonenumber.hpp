#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/common.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

void RegisterPhonenumberOptions(ExtensionLoader &loader);
void RegisterPhonenumberFunctions(ExtensionLoader &loader);

namespace phonenumber {

struct PhoneNumberParts {
	bool valid = false;
	int country_code = 0;
	//! The national significant number; set for a valid number and also for one
	//! that failed only the length check (phonenumber_parse still reports NULL
	//! then). Empty when the input could not be read as a number at all.
	std::string national_number;
	//! The region the number was read in: the region argument or the default
	//! region for a national number, the country code's region otherwise. Set
	//! also for an invalid number once the region is known; empty before that.
	std::string region_code;
	std::string type;
	//! True if the national number matches the region's fixed-line or mobile
	//! validation pattern (computed once during Parse, reused by IsValid).
	bool matches_valid_pattern = false;
	//! Why the number is not valid (a PhoneNumberInvalidReason value); empty
	//! when it is valid.
	std::string reason;
	//! Digits of a trailing extension ("ext. 123", "x123", "Durchwahl 123") that
	//! was split off before validation; empty when the input has none.
	std::string extension;
	//! A national number (no '+', no international prefix) read in the default
	//! region because the region argument was NULL or empty
	bool read_in_default_region = false;

	//! The reason phonenumber_parse reports: region_missing instead of
	//! invalid_length / invalid_pattern when the number was read in the default
	//! region only because no region was given. `reason` keeps the precise
	//! cause for internal callers (PII deep validation judges the length).
	std::string ReportedReason() const;
};

//! Values of the `reason` field of phonenumber_parse
struct PhoneNumberInvalidReason {
	//! The value is blank: empty or whitespace only (a quoted '' CSV cell)
	static constexpr const char *EMPTY = "empty";
	static constexpr const char *NOT_A_NUMBER = "not_a_number";
	//! The number contains letters (other than a leading label or an extension marker)
	static constexpr const char *INVALID_CHARACTERS = "invalid_characters";
	static constexpr const char *UNSUPPORTED_REGION = "unsupported_region";
	static constexpr const char *INVALID_COUNTRY_CODE = "unsupported_country_code";
	static constexpr const char *INVALID_LENGTH = "invalid_length";
	static constexpr const char *INVALID_PATTERN = "invalid_pattern";
	//! A national number (no '+', no international prefix) given without a region
	//! argument that is invalid under the default region: the region, not the
	//! number, is the likely problem
	static constexpr const char *REGION_MISSING = "region_missing";
};

enum class PhoneNumberFormatOption {
	E164,
	INTERNATIONAL,
	NATIONAL,
	RFC3966
};

struct PhoneNumberStatus {
	bool initialized = false;
	std::string default_region;
};

class PhoneNumberManager {
public:
	static PhoneNumberManager &Instance();

	void EnsureInitialized();
	//! All methods take an optional default_region that callers should snapshot
	//! from the session setting at bind time. When empty, the process-wide
	//! default (kept for callers without a session, e.g. PII detection) is used.
	PhoneNumberParts Parse(const std::string &raw_number, const std::string &region_hint,
	                       const std::string &default_region = "");
	std::string Format(const std::string &raw_number, const std::string &region_hint, PhoneNumberFormatOption format,
	                   const std::string &default_region = "");
	std::string GetRegion(const std::string &raw_number, const std::string &region_hint,
	                      const std::string &default_region = "");
	bool IsValid(const std::string &raw_number, const std::string &region_hint,
	             const std::string &default_region = "");
	bool IsPossible(const std::string &raw_number, const std::string &region_hint,
	                const std::string &default_region = "");
	bool IsValidForRegion(const std::string &raw_number, const std::string &region_hint,
	                      const std::string &default_region = "");
	std::string Match(const std::string &number1, const std::string &number2, const std::string &region_hint,
	                  const std::string &default_region = "");
	std::string GetExampleNumber(const std::string &region_hint, const std::string &default_region = "");

	void SetDefaultRegion(const std::string &region);
	std::string GetDefaultRegion() const;

	//! Returns default_region (uppercased) if non-empty, otherwise the
	//! process-wide default region.
	std::string EffectiveDefaultRegion(const std::string &default_region) const;

	PhoneNumberStatus GetStatus() const;

private:
	PhoneNumberManager();
	~PhoneNumberManager();

	void Initialize();

	std::atomic<bool> initialized {false};
	std::string default_region;
};

PhoneNumberFormatOption ParseFormatOption(const std::string &format_str);

//! Canonical form of a user-supplied region code: surrounding whitespace is
//! removed, letters are upper-cased and the common alias 'UK' maps to the ISO
//! code 'GB'. Applied to region hints and to the default-region setting.
std::string NormalizeRegionCode(const std::string &region);

} // namespace phonenumber

} // namespace anofox
} // namespace duckdb

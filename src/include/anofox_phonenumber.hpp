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
	std::string national_number;
	std::string region_code;
	std::string type;
	//! True if the national number matches the region's fixed-line or mobile
	//! validation pattern (computed once during Parse, reused by IsValid).
	bool matches_valid_pattern = false;
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

} // namespace phonenumber

} // namespace anofox
} // namespace duckdb

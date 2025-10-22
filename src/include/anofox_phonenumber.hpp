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
	PhoneNumberParts Parse(const std::string &raw_number, const std::string &region_hint);
	std::string Format(const std::string &raw_number, const std::string &region_hint, PhoneNumberFormatOption format);
	std::string GetRegion(const std::string &raw_number, const std::string &region_hint);
	bool IsValid(const std::string &raw_number, const std::string &region_hint);
	bool IsPossible(const std::string &raw_number, const std::string &region_hint);
	bool IsValidForRegion(const std::string &raw_number, const std::string &region_hint);
	std::string Match(const std::string &number1, const std::string &number2, const std::string &region_hint);
	std::string GetExampleNumber(const std::string &region_hint);

	void SetDefaultRegion(const std::string &region);
	std::string GetDefaultRegion() const;

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

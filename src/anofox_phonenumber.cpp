#include "anofox_phonenumber.hpp"

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

#include <mutex>
#include <string>
#include <phonenumbers/phonenumber.pb.h>
#include <phonenumbers/phonenumberutil.h>

namespace duckdb {
namespace anofox {
namespace phonenumber {

using i18n::phonenumbers::PhoneNumber;
using i18n::phonenumbers::PhoneNumberUtil;

namespace {

PhoneNumberUtil &GetUtil() {
	auto *util = PhoneNumberUtil::GetInstance();
	if (!util) {
		throw IOException("Failed to initialize libphonenumber");
	}
	return *util;
}

std::string ToTypeString(PhoneNumberUtil::PhoneNumberType type) {
	switch (type) {
	case PhoneNumberUtil::PhoneNumberType::FIXED_LINE:
		return "fixed_line";
	case PhoneNumberUtil::PhoneNumberType::MOBILE:
		return "mobile";
	case PhoneNumberUtil::PhoneNumberType::FIXED_LINE_OR_MOBILE:
		return "fixed_line_or_mobile";
	case PhoneNumberUtil::PhoneNumberType::TOLL_FREE:
		return "toll_free";
	case PhoneNumberUtil::PhoneNumberType::PREMIUM_RATE:
		return "premium_rate";
	case PhoneNumberUtil::PhoneNumberType::SHARED_COST:
		return "shared_cost";
	case PhoneNumberUtil::PhoneNumberType::VOIP:
		return "voip";
	case PhoneNumberUtil::PhoneNumberType::PERSONAL_NUMBER:
		return "personal_number";
	case PhoneNumberUtil::PhoneNumberType::PAGER:
		return "pager";
	case PhoneNumberUtil::PhoneNumberType::UAN:
		return "uan";
	case PhoneNumberUtil::PhoneNumberType::VOICEMAIL:
		return "voicemail";
	case PhoneNumberUtil::PhoneNumberType::UNKNOWN:
	default:
		return "unknown";
	}
}

std::mutex &RegionMutex() {
	static std::mutex region_mutex;
	return region_mutex;
}

} // namespace

PhoneNumberManager &PhoneNumberManager::Instance() {
	static PhoneNumberManager instance;
	return instance;
}

PhoneNumberManager::PhoneNumberManager() : default_region("US") {
}

PhoneNumberManager::~PhoneNumberManager() {
}

void PhoneNumberManager::EnsureInitialized() {
	if (!initialized.load()) {
		Initialize();
	}
}

PhoneNumberParts PhoneNumberManager::Parse(const std::string &raw_number, const std::string &region_hint) {
	EnsureInitialized();
	PhoneNumber parsed;
	auto &util = GetUtil();
	PhoneNumberParts parts;

	auto region = region_hint.empty() ? GetDefaultRegion() : StringUtil::Upper(region_hint);
	auto status = util.Parse(raw_number, region, &parsed);
	parts.valid = status == PhoneNumberUtil::NO_PARSING_ERROR;
	if (!parts.valid) {
		return parts;
	}

	parts.country_code = parsed.country_code();
	parts.national_number = std::to_string(parsed.national_number());
	std::string region_code;
	util.GetRegionCodeForNumber(parsed, &region_code);
	parts.region_code = region_code;
	parts.type = ToTypeString(util.GetNumberType(parsed));
	return parts;
}

std::string PhoneNumberManager::Format(const std::string &raw_number, const std::string &region_hint,
                                       PhoneNumberFormatOption format_option) {
	EnsureInitialized();
	PhoneNumber parsed;
	auto &util = GetUtil();
	auto region = region_hint.empty() ? GetDefaultRegion() : StringUtil::Upper(region_hint);
	if (util.Parse(raw_number, region, &parsed) != PhoneNumberUtil::NO_PARSING_ERROR) {
		throw InvalidInputException("Invalid phone number: %s", raw_number);
	}

	std::string formatted;
	PhoneNumberUtil::PhoneNumberFormat lib_format = PhoneNumberUtil::PhoneNumberFormat::NATIONAL;
	switch (format_option) {
	case PhoneNumberFormatOption::E164:
		lib_format = PhoneNumberUtil::PhoneNumberFormat::E164;
		break;
	case PhoneNumberFormatOption::INTERNATIONAL:
		lib_format = PhoneNumberUtil::PhoneNumberFormat::INTERNATIONAL;
		break;
	case PhoneNumberFormatOption::RFC3966:
		lib_format = PhoneNumberUtil::PhoneNumberFormat::RFC3966;
		break;
	case PhoneNumberFormatOption::NATIONAL:
	default:
		lib_format = PhoneNumberUtil::PhoneNumberFormat::NATIONAL;
		break;
	}
	util.Format(parsed, lib_format, &formatted);
	return formatted;
}

std::string PhoneNumberManager::GetRegion(const std::string &raw_number, const std::string &region_hint) {
	EnsureInitialized();
	PhoneNumber parsed;
	auto &util = GetUtil();
	auto region = region_hint.empty() ? GetDefaultRegion() : StringUtil::Upper(region_hint);
	if (util.Parse(raw_number, region, &parsed) != PhoneNumberUtil::NO_PARSING_ERROR) {
		throw InvalidInputException("Invalid phone number: %s", raw_number);
	}
	std::string region_code;
	util.GetRegionCodeForNumber(parsed, &region_code);
	return region_code;
}

void PhoneNumberManager::SetDefaultRegion(const std::string &region) {
	EnsureInitialized();
	std::lock_guard<std::mutex> lock(RegionMutex());
	default_region = StringUtil::Upper(region);
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
	(void)GetUtil();
	initialized = true;
}

PhoneNumberFormatOption ParseFormatOption(const std::string &format_str) {
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

#include "duckdb/common/types/value.hpp"

namespace duckdb {
namespace anofox {

using phonenumber::ParseFormatOption;
using phonenumber::PhoneNumberFormatOption;
using phonenumber::PhoneNumberManager;
using phonenumber::PhoneNumberStatus;

namespace {

static void SetPhonenumberDefaultRegionOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_phonenumber_default_region cannot be NULL");
	}
	PhoneNumberManager::Instance().SetDefaultRegion(parameter.ToString());
}

void PhoneParseFunction(DataChunk &args, ExpressionState &, Vector &result) {
	PhoneNumberManager::Instance().EnsureInitialized();

	auto &numbers = args.data[0];
	auto &regions = args.data[1];

	UnifiedVectorFormat number_data;
	UnifiedVectorFormat region_data;
	numbers.ToUnifiedFormat(args.size(), number_data);
	regions.ToUnifiedFormat(args.size(), region_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &children = StructVector::GetEntries(result);

	auto &valid_vec = *children[0];
	auto &country_code_vec = *children[1];
	auto &national_number_vec = *children[2];
	auto &region_vec = *children[3];
	auto &type_vec = *children[4];

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
			for (auto &child : children) {
				FlatVector::SetNull(*child, i, true);
			}
			continue;
		}

		auto raw_number = reinterpret_cast<string_t *>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t *>(region_data.data)[reg_idx].GetString()
		                     : std::string();

		auto parts = PhoneNumberManager::Instance().Parse(raw_number, region_hint);

		FlatVector::SetNull(result, i, false);
		FlatVector::SetNull(valid_vec, i, false);
		valid_data[i] = parts.valid;

		if (parts.valid) {
			FlatVector::SetNull(country_code_vec, i, false);
			country_data[i] = parts.country_code;

			FlatVector::SetNull(national_number_vec, i, false);
			national_data[i] = StringVector::AddString(national_number_vec, parts.national_number);

			FlatVector::SetNull(region_vec, i, false);
			region_data_out[i] = StringVector::AddString(region_vec, parts.region_code);

			FlatVector::SetNull(type_vec, i, false);
			type_data[i] = StringVector::AddString(type_vec, parts.type);
		} else {
			FlatVector::SetNull(country_code_vec, i, true);
			FlatVector::SetNull(national_number_vec, i, true);
			FlatVector::SetNull(region_vec, i, true);
			FlatVector::SetNull(type_vec, i, true);
		}
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void PhoneFormatFunction(DataChunk &args, ExpressionState &, Vector &result) {
	PhoneNumberManager::Instance().EnsureInitialized();

	auto &numbers = args.data[0];
	auto &regions = args.data[1];
	auto &formats = args.data[2];

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

		if (!number_data.validity.RowIsValid(nr_idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto raw_number = reinterpret_cast<string_t *>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t *>(region_data.data)[reg_idx].GetString()
		                     : std::string();
		auto format_hint = format_data.validity.RowIsValid(fmt_idx)
		                     ? reinterpret_cast<string_t *>(format_data.data)[fmt_idx].GetString()
		                     : std::string();

		auto format = ParseFormatOption(format_hint);
		auto formatted = PhoneNumberManager::Instance().Format(raw_number, region_hint, format);
		FlatVector::SetNull(result, i, false);
		result_data[i] = StringVector::AddString(result, formatted);
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void PhoneRegionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	PhoneNumberManager::Instance().EnsureInitialized();

	auto &numbers = args.data[0];
	auto &regions = args.data[1];

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

		auto raw_number = reinterpret_cast<string_t *>(number_data.data)[nr_idx].GetString();
		auto region_hint = region_data.validity.RowIsValid(reg_idx)
		                     ? reinterpret_cast<string_t *>(region_data.data)[reg_idx].GetString()
		                     : std::string();

		auto region_code = PhoneNumberManager::Instance().GetRegion(raw_number, region_hint);
		FlatVector::SetNull(result, i, false);
		result_data[i] = StringVector::AddString(result, region_code);
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

struct PhoneStatusState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<GlobalTableFunctionState> PhoneStatusInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PhoneStatusState>();
}

unique_ptr<FunctionData> PhoneStatusBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                         vector<string> &names) {
	names.emplace_back("initialized");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("default_region");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

void PhoneStatusFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PhoneStatusState>();
	if (state.done) {
		return;
	}

	auto status = PhoneNumberManager::Instance().GetStatus();
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(status.initialized));
	output.SetValue(1, 0, Value(status.default_region));
	state.done = true;
}

ScalarFunction CreateParseScalar(const string &name) {
	ScalarFunction function(name, {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                        LogicalType::STRUCT({{"valid", LogicalType::BOOLEAN},
	                                             {"country_code", LogicalType::INTEGER},
	                                             {"national_number", LogicalType::VARCHAR},
	                                             {"region", LogicalType::VARCHAR},
	                                             {"type", LogicalType::VARCHAR}}),
	                        PhoneParseFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateFormatScalar(const string &name) {
	ScalarFunction function(name, {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                        LogicalType::VARCHAR, PhoneFormatFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

ScalarFunction CreateRegionScalar(const string &name) {
	ScalarFunction function(name, {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                        PhoneRegionFunction);
	function.stability = FunctionStability::CONSISTENT;
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	return function;
}

TableFunction CreateStatusTable(const string &name) {
	return TableFunction(name, {}, PhoneStatusFunction, PhoneStatusBind, PhoneStatusInit);
}

} // namespace


void RegisterPhonenumberOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("anofox_phonenumber_default_region",
	                          "Default region code used when the region hint is NULL",
	                          LogicalType::VARCHAR, Value("US"), SetPhonenumberDefaultRegionOption);
}

void RegisterPhonenumberFunctions(ExtensionLoader &loader) {
	RegisterPhonenumberOptions(loader);
	loader.RegisterFunction(CreateParseScalar("anofox_phonenumber_parse"));
	loader.RegisterFunction(CreateFormatScalar("anofox_phonenumber_format"));
	loader.RegisterFunction(CreateRegionScalar("anofox_phonenumber_region"));
	loader.RegisterFunction(CreateStatusTable("anofox_phonenumber_status"));
	// Variable-like setter handled elsewhere
}

} // namespace anofox
} // namespace duckdb

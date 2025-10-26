#include "anofox_postal.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "anofox_trace.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/database.hpp"

#include <cstdlib>
#include <fstream>
#include <libpostal/libpostal.h>
#include <mutex>
#include <string>

namespace duckdb {
namespace anofox {
namespace postal {

namespace {
constexpr const char *POSTAL_BASE_URL = "https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/";
const std::vector<std::string> POSTAL_ASSETS = {"language_classifier.tar.gz", "libpostal_data.tar.gz",
                                                "parser.tar.gz"};
constexpr const char *DEFAULT_POSTAL_DIR = ".duckdb/extensions/libpostal";
} // namespace

PostalManager &PostalManager::Instance() {
	static PostalManager instance;
	return instance;
}

PostalManager::PostalManager() : data_directory(DEFAULT_POSTAL_DIR) {
}

PostalManager::~PostalManager() {
	libpostal_teardown_language_classifier();
	libpostal_teardown_parser();
	libpostal_teardown();
}

void PostalManager::EnsureInitialized(ClientContext &context) {
	if (!initialized.load()) {
		AnofoxTrace(AnofoxLogLevel::Debug, "Postal EnsureInitialized triggered");
		std::lock_guard<std::mutex> lock(init_lock);
		if (!initialized.load()) {
			Initialize(context);
		}
	}
	if (!initialized.load()) {
		AnofoxTrace(AnofoxLogLevel::Error, "Postal initialization failed");
		throw IOException("Failed to initialize libpostal. Use anofox_postal_load_data() to download assets.");
	}
AnofoxTrace(AnofoxLogLevel::Debug, "Postal already initialized");
}

void PostalManager::SetDataDirectory(const std::string &path) {
	std::lock_guard<std::mutex> lock(init_lock);
	if (initialized.load()) {
		throw InvalidInputException("Cannot change postal data directory after initialization");
	}
	data_directory = path.empty() ? DEFAULT_POSTAL_DIR : path;
	AnofoxTrace(AnofoxLogLevel::Debug,
           "Postal data directory set to " + data_directory);
}

std::string PostalManager::GetDataDirectory() const {
	return data_directory;
}

std::vector<PostalComponent> PostalManager::ParseAddress(const std::string &input) {
	libpostal_address_parser_options_t options = libpostal_get_address_parser_default_options();
	libpostal_address_parser_response_t *parsed = libpostal_parse_address(const_cast<char *>(input.c_str()), options);
	if (!parsed) {
		AnofoxTrace(AnofoxLogLevel::Warn, "Postal parse failed");
		throw IOException("libpostal_parse_address failed");
	}
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "Postal parsed address components=" + std::to_string(parsed->num_components) + " ");

	std::vector<PostalComponent> components;
	components.reserve(parsed->num_components);
	for (size_t i = 0; i < parsed->num_components; i++) {
		components.push_back({parsed->labels[i], parsed->components[i]});
	}
	libpostal_address_parser_response_destroy(parsed);
	return components;
}

std::vector<std::string> PostalManager::ExpandAddress(const std::string &input) {
	libpostal_normalize_options_t options = libpostal_get_default_options();
	size_t num_expansions = 0;
	char **expansions = libpostal_expand_address(const_cast<char *>(input.c_str()), options, &num_expansions);
	if (!expansions) {
		AnofoxTrace(AnofoxLogLevel::Warn, "Postal expand failed");
		throw IOException("libpostal_expand_address failed");
	}
	AnofoxTrace(AnofoxLogLevel::Debug,
	           "Postal expand generated " + std::to_string(num_expansions) + " variants");

	std::vector<std::string> result;
	result.reserve(num_expansions);
	for (size_t i = 0; i < num_expansions; i++) {
		result.emplace_back(expansions[i]);
	}
	libpostal_expansion_array_destroy(expansions, num_expansions);
	return result;
}

void PostalManager::LoadData(ClientContext &context) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto data_dir = fs.ExpandPath(data_directory);
	if (!fs.DirectoryExists(data_dir)) {
		fs.CreateDirectory(data_dir);
	}
	if (GetStatus(context).data_present) {
		return;
	}

	for (const auto &asset : POSTAL_ASSETS) {
		auto url = std::string(POSTAL_BASE_URL) + asset;
		auto destination = fs.JoinPath(data_dir, asset);
		AnofoxTrace(AnofoxLogLevel::Info, "Postal downloading " + asset + " from " + url);

		// Get database instance and HTTP utility
		auto &db = DatabaseInstance::GetDatabase(context);
		auto &http_util = HTTPUtil::Get(db);

		// Initialize HTTP parameters with robust settings
		auto params = http_util.InitializeParameters(db, url);
		params->timeout = 60;  // 60 seconds (larger files)
		params->retries = 5;   // More retries for reliability
		params->retry_wait_ms = 200;
		params->retry_backoff = 3.0;

		// Create HTTP headers
		HTTPHeaders headers(db);

		// Create GET request
		GetRequestInfo request(url, headers, *params, nullptr, nullptr);

		// Execute download
		auto response = http_util.Request(request);

		// Check response
		if (!response->Success()) {
			AnofoxTrace(AnofoxLogLevel::Error,
				"Postal download failed: HTTP " + std::to_string(static_cast<int>(response->status)));
			throw IOException("Failed to download '" + asset + "' from " + url +
			                  ": HTTP " + std::to_string(static_cast<int>(response->status)));
		}

		// Write response body to file
		auto file_handle = fs.OpenFile(destination,
			FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		file_handle->Write((void*)response->body.data(), response->body.size());
		file_handle->Close();

		AnofoxTrace(AnofoxLogLevel::Info,
			"Postal downloaded " + asset + " (" + std::to_string(response->body.size()) + " bytes)");

		// Extract the tarball
		std::string extract_command = "tar -xzf \"" + destination + "\" -C \"" + data_dir + "\"";
		if (std::system(extract_command.c_str()) != 0) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal extract failed for " + destination);
			throw IOException("Failed to extract '" + destination + "'");
		}
		fs.RemoveFile(destination);
	}
	AnofoxTrace(AnofoxLogLevel::Info, "Postal data download complete");
}

PostalStatus PostalManager::GetStatus(ClientContext &context) {
	PostalStatus status;
	status.initialized = initialized.load();
	auto &fs = FileSystem::GetFileSystem(context);
	status.data_dir = fs.ExpandPath(data_directory);
	if (!fs.DirectoryExists(status.data_dir)) {
		status.data_present = false;
		return status;
	}
	const auto classifier_file = fs.JoinPath(status.data_dir, "language_classifier/language_classifier.dat");
	const auto parser_file = fs.JoinPath(status.data_dir, "address_parser/address_parser_phrases.dat");
	std::ifstream classifier_stream(classifier_file);
	std::ifstream parser_stream(parser_file);
	status.data_present = classifier_stream.good() && parser_stream.good();
	return status;
}

void PostalManager::Initialize(ClientContext &context) {
	auto status = GetStatus(context);
	if (!status.data_present) {
		AnofoxTrace(AnofoxLogLevel::Info,
		           "Libpostal data not found in '" + status.data_dir + "', downloading automatically...");
		LoadData(context);
		status = GetStatus(context);  // Refresh status after download
		if (!status.data_present) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal data download failed");
			throw IOException("Failed to download libpostal data to '" + status.data_dir + "'");
		}
	}

	auto data_dir = status.data_dir;
	auto data_dir_c = const_cast<char *>(data_dir.c_str());
	if (!libpostal_setup_datadir(data_dir_c) || !libpostal_setup()) {
		AnofoxTrace(AnofoxLogLevel::Error, "Postal setup core failed path=" + data_dir + " ");
		throw IOException("Failed to initialize libpostal core data in '" + data_dir + "'");
	}
	if (!libpostal_setup_parser_datadir(data_dir_c) || !libpostal_setup_parser()) {
		AnofoxTrace(AnofoxLogLevel::Error, "Postal setup parser failed path=" + data_dir);
		throw IOException("Failed to initialize libpostal parser data in '" + data_dir + "'");
	}
	if (!libpostal_setup_language_classifier_datadir(data_dir_c) || !libpostal_setup_language_classifier()) {
		AnofoxTrace(AnofoxLogLevel::Error,
		           "Postal setup language classifier failed path=" + data_dir);
		throw IOException("Failed to initialize libpostal language classifier data in '" + data_dir + "'");
	}
	initialized = true;
	AnofoxTrace(AnofoxLogLevel::Info, "Postal initialization complete");
}

} // namespace postal
} // namespace anofox
} // namespace duckdb

namespace duckdb {
namespace anofox {

using postal::PostalComponent;
using postal::PostalManager;
using postal::PostalStatus;

namespace {

static void SetPostalDataPathOption(ClientContext &, SetScope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_postal_data_path cannot be NULL");
    }
    PostalManager::Instance().SetDataDirectory(parameter.ToString());
}

void PostalParseAddressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &manager = PostalManager::Instance();
	manager.EnsureInitialized(context);

	auto &input = args.data[0];
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(args.size(), input_data);
	auto inputs = reinterpret_cast<string_t *>(input_data.data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &children = StructVector::GetEntries(result);

	auto &house_number_vec = *children[0];
	auto &road_vec = *children[1];
	auto &city_vec = *children[2];
	auto &state_vec = *children[3];
	auto &postcode_vec = *children[4];
	auto &country_vec = *children[5];

	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input_data.sel->get_index(i);

		if (!input_data.validity.RowIsValid(idx)) {
			for (auto &child : children) {
				FlatVector::SetNull(*child, i, true);
			}
			continue;
		}

		auto input_str = inputs[idx].GetString();
		auto components = manager.ParseAddress(input_str);

		for (auto &child : children) {
			FlatVector::SetNull(*child, i, true);
		}

		for (const auto &component : components) {
			if (component.label == "house_number") {
				house_number_vec.SetValue(i, component.value);
			} else if (component.label == "road") {
				road_vec.SetValue(i, component.value);
			} else if (component.label == "city") {
				city_vec.SetValue(i, component.value);
			} else if (component.label == "state") {
				state_vec.SetValue(i, component.value);
			} else if (component.label == "postcode") {
				postcode_vec.SetValue(i, component.value);
			} else if (component.label == "country") {
				country_vec.SetValue(i, component.value);
			}
		}
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void PostalExpandAddressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &manager = PostalManager::Instance();
	manager.EnsureInitialized(context);

	auto &input = args.data[0];
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(args.size(), input_data);
	auto inputs = reinterpret_cast<string_t *>(input_data.data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);

	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input_data.sel->get_index(i);

		if (!input_data.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		auto input_str = inputs[idx].GetString();
		auto expansions = manager.ExpandAddress(input_str);

		list_entries[i].offset = ListVector::GetListSize(result);
		list_entries[i].length = expansions.size();

		for (auto &expansion : expansions) {
			ListVector::PushBack(result, Value(expansion));
		}
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunction CreateParseFunction(const string &name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR},
	                        LogicalType::STRUCT({{"house_number", LogicalTypeId::VARCHAR},
	                                             {"road", LogicalTypeId::VARCHAR},
	                                             {"city", LogicalTypeId::VARCHAR},
	                                             {"state", LogicalTypeId::VARCHAR},
	                                             {"postcode", LogicalTypeId::VARCHAR},
	                                             {"country", LogicalTypeId::VARCHAR}}),
	                        PostalParseAddressFunction);
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	function.stability = FunctionStability::CONSISTENT;
	return function;
}

ScalarFunction CreateExpandFunction(const string &name) {
	ScalarFunction function(name, {LogicalTypeId::VARCHAR}, LogicalType::LIST(LogicalTypeId::VARCHAR),
	                        PostalExpandAddressFunction);
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	function.stability = FunctionStability::CONSISTENT;
	return function;
}

struct PostalStatusState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<GlobalTableFunctionState> PostalStatusInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PostalStatusState>();
}

unique_ptr<FunctionData> PostalStatusBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                          vector<string> &names) {
	names.emplace_back("initialized");
	return_types.emplace_back(LogicalTypeId::BOOLEAN);
	names.emplace_back("data_present");
	return_types.emplace_back(LogicalTypeId::BOOLEAN);
	names.emplace_back("data_dir");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	return nullptr;
}

void PostalStatusFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<PostalStatusState>();
	if (state.done) {
		return;
	}

	auto status = PostalManager::Instance().GetStatus(context);
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(status.initialized));
	output.SetValue(1, 0, Value::BOOLEAN(status.data_present));
	output.SetValue(2, 0, Value(status.data_dir));
	state.done = true;
}

TableFunction CreateStatusFunction(const string &name) {
	return TableFunction(name, {}, PostalStatusFunction, PostalStatusBind, PostalStatusInit);
}

void PostalLoadDataFunction(DataChunk &, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	PostalManager::Instance().LoadData(context);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto out = ConstantVector::GetData<bool>(result);
	out[0] = true;
	ConstantVector::SetNull(result, false);
}

ScalarFunction CreateLoadFunction() {
	ScalarFunction function("anofox_postal_load_data", {}, LogicalTypeId::BOOLEAN, PostalLoadDataFunction);
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	function.stability = FunctionStability::VOLATILE;
	return function;
}

} // namespace

void RegisterPostalOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("anofox_postal_data_path",
	                          "Directory storing libpostal assets",
	                          LogicalTypeId::VARCHAR, Value(postal::DEFAULT_POSTAL_DIR), SetPostalDataPathOption);
}

void RegisterPostalFunctions(ExtensionLoader &loader) {
	RegisterPostalOptions(loader);
	loader.RegisterFunction(CreateParseFunction("anofox_postal_parse_address"));
	loader.RegisterFunction(CreateExpandFunction("anofox_postal_expand_address"));
	loader.RegisterFunction(CreateStatusFunction("anofox_postal_status"));
	loader.RegisterFunction(CreateLoadFunction());
}

} // namespace anofox
} // namespace duckdb

#include "anofox_postal.hpp"

#if HAVE_LIBPOSTAL

#include "anofox_function_alias.hpp"
#include "telemetry.hpp"
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

#include <cerrno>
#include <chrono>
#include <cstring>
#include <curl/curl.h>
#include <fstream>
#include <libpostal/libpostal.h>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>

extern char **environ;

namespace duckdb {
namespace anofox {
namespace postal {

namespace {
constexpr const char *POSTAL_BASE_URL = "https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/";
const std::vector<std::string> POSTAL_ASSETS = {"language_classifier.tar.gz", "libpostal_data.tar.gz",
                                                "parser.tar.gz"};
constexpr const char *DEFAULT_POSTAL_DIR = ".duckdb/extensions/libpostal";

// Callback for libcurl to write data to file
static size_t WriteDataToFile(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	return fwrite(ptr, size, nmemb, stream);
}

// RAII deleters so FILE* / CURL* are released on every exit path.
struct FileCloser {
	void operator()(FILE *file) const {
		if (file) {
			fclose(file);
		}
	}
};
struct CurlEasyCleanup {
	void operator()(CURL *curl) const {
		if (curl) {
			curl_easy_cleanup(curl);
		}
	}
};

//! Initializes libcurl's global state exactly once for the process lifetime.
//! curl_global_init/curl_global_cleanup are not reference counted in a way
//! that is safe to pair per call when other libraries use curl, so the state
//! lives until process exit.
static void EnsureCurlGlobalInit() {
	struct CurlGlobalState {
		CurlGlobalState() {
			curl_global_init(CURL_GLOBAL_DEFAULT);
		}
		~CurlGlobalState() {
			curl_global_cleanup();
		}
	};
	static CurlGlobalState curl_global_state;
	(void)curl_global_state;
}

//! Defense in depth: paths are passed as discrete argv entries (no shell),
//! but reject control characters and quotes anyway so a hostile setting can
//! never smuggle anything resembling shell metacharacters into a child process.
static void ValidateExtractionPath(const std::string &path) {
	for (const unsigned char character : path) {
		if (character < 0x20 || character == 0x7f || character == '"' || character == '\'' || character == '`') {
			throw InvalidInputException(
			    "Postal data path contains forbidden control or quote characters: '%s'", path);
		}
	}
}

//! Extracts a gzipped tarball by spawning `tar` directly with an argv vector.
//! No shell is involved, so the paths are never re-interpreted.
static void ExtractTarball(const std::string &archive_path, const std::string &target_dir) {
	ValidateExtractionPath(archive_path);
	ValidateExtractionPath(target_dir);

	std::vector<std::string> arguments = {"tar", "-xzf", archive_path, "-C", target_dir};
	std::vector<char *> argv;
	argv.reserve(arguments.size() + 1);
	for (auto &argument : arguments) {
		argv.push_back(const_cast<char *>(argument.c_str()));
	}
	argv.push_back(nullptr);

	pid_t pid = -1;
	const int spawn_result = posix_spawnp(&pid, "tar", nullptr, nullptr, argv.data(), environ);
	if (spawn_result != 0) {
		throw IOException("Failed to launch tar for '" + archive_path + "': " + std::strerror(spawn_result));
	}

	int status = 0;
	pid_t waited;
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited == -1 && errno == EINTR);
	if (waited == -1) {
		throw IOException("Failed to wait for tar extracting '" + archive_path + "': " + std::strerror(errno));
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		throw IOException("tar failed to extract '" + archive_path + "' into '" + target_dir + "'");
	}
}
} // namespace

PostalManager &PostalManager::Instance() {
	static PostalManager instance;
	return instance;
}

PostalManager::PostalManager() : data_directory(DEFAULT_POSTAL_DIR) {
}

PostalManager::~PostalManager() {
	TeardownInitializedStages();
}

void PostalManager::TeardownInitializedStages() {
	if (classifier_ready) {
		libpostal_teardown_language_classifier();
		classifier_ready = false;
	}
	if (parser_ready) {
		libpostal_teardown_parser();
		parser_ready = false;
	}
	if (core_ready) {
		libpostal_teardown();
		core_ready = false;
	}
	initialized = false;
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
		throw IOException("Failed to initialize libpostal. Use anofox_tab_postal_load_data() to download assets.");
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
	std::lock_guard<std::mutex> lock(init_lock);
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
	// Serialize with initialization and concurrent load calls so parallel
	// downloads cannot clobber each other's files.
	std::lock_guard<std::mutex> lock(init_lock);
	LoadDataInternal(context);
}

void PostalManager::LoadDataInternal(ClientContext &context) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto data_dir = fs.ExpandPath(data_directory);
	if (!fs.DirectoryExists(data_dir)) {
		fs.CreateDirectory(data_dir);
	}
	if (GetStatusInternal(context).data_present) {
		return;
	}

	// Initialize libcurl global state once per process (RAII, process lifetime)
	EnsureCurlGlobalInit();

	for (const auto &asset : POSTAL_ASSETS) {
		auto url = std::string(POSTAL_BASE_URL) + asset;
		auto destination = fs.JoinPath(data_dir, asset);

		// Robust libcurl-based download with retry logic
		const int max_retries = 5;
		bool download_success = false;

		for (int attempt = 1; attempt <= max_retries && !download_success; attempt++) {
			if (attempt > 1) {
				AnofoxTrace(AnofoxLogLevel::Info,
					"Postal retrying download attempt " + std::to_string(attempt) + "/" + std::to_string(max_retries));
			} else {
				AnofoxTrace(AnofoxLogLevel::Info, "Postal downloading " + asset + " from " + url);
			}

			// Open file for writing (RAII-managed)
			std::unique_ptr<FILE, FileCloser> file(fopen(destination.c_str(), "wb"));
			if (!file) {
				AnofoxTrace(AnofoxLogLevel::Error, "Postal failed to open file for writing: " + destination);
				throw IOException("Failed to open file for writing: " + destination);
			}

			// Initialize curl handle (RAII-managed)
			std::unique_ptr<CURL, CurlEasyCleanup> curl(curl_easy_init());
			if (!curl) {
				AnofoxTrace(AnofoxLogLevel::Error, "Postal failed to initialize curl");
				throw IOException("Failed to initialize libcurl");
			}

			// Configure curl options
			curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteDataToFile);
			curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, file.get());
			curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);  // Follow redirects
			curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);  // 10s connection timeout
			curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 300L);  // 5 minute total timeout
			curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);  // Fail on HTTP error codes
			curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);  // Verify SSL certificates
			curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);  // Verify hostname matches cert

			// Perform the download
			CURLcode res = curl_easy_perform(curl.get());
			long http_code = 0;
			curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

			// Release handles before inspecting the result so the file is flushed
			curl.reset();
			file.reset();

			if (res == CURLE_OK) {
				download_success = true;
				AnofoxTrace(AnofoxLogLevel::Info, "Postal download complete for " + asset);
			} else {
				const char *error_msg = curl_easy_strerror(res);
				AnofoxTrace(AnofoxLogLevel::Warn,
					"Postal download attempt " + std::to_string(attempt) + " failed: " +
					std::string(error_msg) + " (HTTP " + std::to_string(http_code) + ")");

				// Remove partially downloaded file
				fs.RemoveFile(destination);

				if (attempt < max_retries) {
					// Exponential backoff: wait 2s, 4s, 8s, 16s between retry attempts
					int wait_time = 1 << attempt;  // 2^attempt
					AnofoxTrace(AnofoxLogLevel::Info, "Waiting " + std::to_string(wait_time) + "s before retry");
					std::this_thread::sleep_for(std::chrono::seconds(wait_time));
				}
			}
		}

		if (!download_success) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal download failed after " + std::to_string(max_retries) + " attempts");
			throw IOException("Failed to download '" + asset + "' from " + url + " after " + std::to_string(max_retries) + " attempts");
		}

		// Extract the tarball without involving a shell
		AnofoxTrace(AnofoxLogLevel::Debug, "Postal extracting " + asset);
		try {
			ExtractTarball(destination, data_dir);
		} catch (const std::exception &) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal extract failed for " + destination);
			throw;
		}
		fs.RemoveFile(destination);
	}

	AnofoxTrace(AnofoxLogLevel::Info, "Postal data download complete");
}

PostalStatus PostalManager::GetStatus(ClientContext &context) {
	std::lock_guard<std::mutex> lock(init_lock);
	return GetStatusInternal(context);
}

PostalStatus PostalManager::GetStatusInternal(ClientContext &context) const {
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
	// Caller holds init_lock.
	auto status = GetStatusInternal(context);
	if (!status.data_present) {
		AnofoxTrace(AnofoxLogLevel::Info,
		           "Libpostal data not found in '" + status.data_dir + "', downloading automatically...");
		LoadDataInternal(context);
		status = GetStatusInternal(context); // Refresh status after download
		if (!status.data_present) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal data download failed");
			throw IOException("Failed to download libpostal data to '" + status.data_dir + "'");
		}
	}

	auto data_dir = status.data_dir;
	auto data_dir_c = const_cast<char *>(data_dir.c_str());
	try {
		// Each stage flag is set as soon as the first setup call of that stage
		// succeeds, so a failure mid-stage still rolls the stage back.
		if (!libpostal_setup_datadir(data_dir_c)) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal setup core failed path=" + data_dir);
			throw IOException("Failed to initialize libpostal core data in '" + data_dir + "'");
		}
		core_ready = true;
		if (!libpostal_setup()) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal setup core failed path=" + data_dir);
			throw IOException("Failed to initialize libpostal core data in '" + data_dir + "'");
		}
		if (!libpostal_setup_parser_datadir(data_dir_c)) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal setup parser failed path=" + data_dir);
			throw IOException("Failed to initialize libpostal parser data in '" + data_dir + "'");
		}
		parser_ready = true;
		if (!libpostal_setup_parser()) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal setup parser failed path=" + data_dir);
			throw IOException("Failed to initialize libpostal parser data in '" + data_dir + "'");
		}
		if (!libpostal_setup_language_classifier_datadir(data_dir_c)) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal setup language classifier failed path=" + data_dir);
			throw IOException("Failed to initialize libpostal language classifier data in '" + data_dir + "'");
		}
		classifier_ready = true;
		if (!libpostal_setup_language_classifier()) {
			AnofoxTrace(AnofoxLogLevel::Error, "Postal setup language classifier failed path=" + data_dir);
			throw IOException("Failed to initialize libpostal language classifier data in '" + data_dir + "'");
		}
	} catch (...) {
		// Roll back exactly the stages that completed so a later retry starts clean.
		TeardownInitializedStages();
		throw;
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
        throw InvalidInputException("anofox_tab_postal_data_path cannot be NULL");
    }
    PostalManager::Instance().SetDataDirectory(parameter.ToString());
}

void PostalParseAddressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &manager = PostalManager::Instance();

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

	// Initialize libpostal lazily on the first non-NULL row so NULL-only
	// queries never require the data bundle.
	bool initialization_checked = false;

	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input_data.sel->get_index(i);

		if (!input_data.validity.RowIsValid(idx)) {
			// NULL input yields a NULL struct (parent validity; children follow).
			FlatVector::SetNull(result, i, true);
			continue;
		}

		if (!initialization_checked) {
			manager.EnsureInitialized(context);
			initialization_checked = true;
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

	auto &input = args.data[0];
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(args.size(), input_data);
	auto inputs = reinterpret_cast<string_t *>(input_data.data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto list_entries = FlatVector::GetData<list_entry_t>(result);

	// Initialize libpostal lazily on the first non-NULL row so NULL-only
	// queries never require the data bundle.
	bool initialization_checked = false;

	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input_data.sel->get_index(i);

		if (!input_data.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}

		if (!initialization_checked) {
			manager.EnsureInitialized(context);
			initialization_checked = true;
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
	PostHogTelemetry::Instance().CaptureFunctionExecution("postal_status");
	names.emplace_back("initialized");
	return_types.emplace_back(LogicalTypeId::BOOLEAN);
	names.emplace_back("data_present");
	return_types.emplace_back(LogicalTypeId::BOOLEAN);
	names.emplace_back("data_dir");
	return_types.emplace_back(LogicalTypeId::VARCHAR);
	return nullptr;
}

// Telemetry bind functions for scalar functions
unique_ptr<FunctionData> PostalParseAddressBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("postal_parse_address");
	return nullptr;
}

unique_ptr<FunctionData> PostalExpandAddressBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("postal_expand_address");
	return nullptr;
}

unique_ptr<FunctionData> PostalLoadDataBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("postal_load_data");
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
	ScalarFunction function("anofox_tab_postal_load_data", {}, LogicalTypeId::BOOLEAN, PostalLoadDataFunction);
	function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	function.stability = FunctionStability::VOLATILE;
	return function;
}

} // namespace

void RegisterPostalOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("anofox_tab_postal_data_path",
	                          "Directory storing libpostal assets",
	                          LogicalTypeId::VARCHAR, Value(postal::DEFAULT_POSTAL_DIR), SetPostalDataPathOption);
}

void RegisterPostalFunctions(ExtensionLoader &loader) {
	// Note: RegisterPostalOptions is called separately by the extension entry
	// point (LoadInternal); do not register options twice here.

	// Register postal_parse_address
	{
		FunctionDescription desc;
		desc.description = "Parses a free-form postal address string into its structural components (house_number, road, city, state, postcode, country).";
		desc.parameter_names = {"address"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT postal_parse_address('123 Main St, Springfield, IL 62701');"};
		desc.categories = {"postal", "address"};
		ScalarFunction parse_func = CreateParseFunction("anofox_tab_postal_parse_address");
		parse_func.bind = PostalParseAddressBind;
		RegisterScalarFunctionWithAlias(loader, parse_func, "postal_parse_address", {std::move(desc)});
	}

	// Register postal_expand_address
	{
		FunctionDescription desc;
		desc.description = "Expands a postal address into all normalized variants using libpostal.";
		desc.parameter_names = {"address"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT postal_expand_address('123 main st springfield il');"};
		desc.categories = {"postal", "address"};
		ScalarFunction expand_func = CreateExpandFunction("anofox_tab_postal_expand_address");
		expand_func.bind = PostalExpandAddressBind;
		RegisterScalarFunctionWithAlias(loader, expand_func, "postal_expand_address", {std::move(desc)});
	}

	// Register postal_status (telemetry in PostalStatusBind)
	{
		FunctionDescription desc;
		desc.description = "Returns the current status of the libpostal address parser, including whether it is initialized and the data directory path.";
		desc.examples = {"SELECT * FROM postal_status();"};
		desc.categories = {"postal", "status"};
		TableFunction status_func = CreateStatusFunction("anofox_tab_postal_status");
		RegisterTableFunctionWithAlias(loader, status_func, "postal_status", {std::move(desc)});
	}

	// Register postal_load_data
	{
		FunctionDescription desc;
		desc.description = "Downloads and installs the libpostal data bundle (~500 MB) required for address parsing and expansion.";
		desc.examples = {"SELECT postal_load_data();"};
		desc.categories = {"postal", "setup"};
		ScalarFunction load_func = CreateLoadFunction();
		load_func.name = "anofox_tab_postal_load_data";
		load_func.bind = PostalLoadDataBind;
		RegisterScalarFunctionWithAlias(loader, load_func, "postal_load_data", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

#endif // HAVE_LIBPOSTAL

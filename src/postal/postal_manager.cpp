#include "anofox/postal/postal_manager.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstdlib>
#include <libpostal/libpostal.h>
#include <fstream>

namespace duckdb {
namespace anofox {
namespace postal {

namespace {
constexpr const char *POSTAL_BASE_URL = "https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/";
const std::vector<std::string> POSTAL_ASSETS = {"language_classifier.tar.gz", "libpostal_data.tar.gz", "parser.tar.gz"};
} // namespace

PostalManager &PostalManager::Instance() {
	static PostalManager instance;
	return instance;
}

void PostalManager::EnsureInitialized(ClientContext &context) {
	std::call_once(init_flag, [&]() { Initialize(context); });
	if (!initialized.load()) {
		throw IOException("Failed to initialize libpostal. Run PRAGMA postal_load_data(); to download the assets.");
	}
}

std::vector<PostalComponent> PostalManager::ParseAddress(const std::string &input) {
	libpostal_address_parser_options_t options = libpostal_get_address_parser_default_options();
	libpostal_address_parser_response_t *parsed = libpostal_parse_address(const_cast<char *>(input.c_str()), options);
	if (!parsed) {
		throw IOException("libpostal_parse_address failed");
	}

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
		throw IOException("libpostal_expand_address failed");
	}

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
	auto data_dir = ResolveDataDir();
	if (!fs.DirectoryExists(data_dir)) {
		fs.CreateDirectory(data_dir);
	}
	if (DataPresent(fs, data_dir)) {
		data_downloaded = true;
		return;
	}

	for (const auto &asset : POSTAL_ASSETS) {
		auto url = std::string(POSTAL_BASE_URL) + asset;
		auto destination = fs.JoinPath(data_dir, asset);

		std::string curl_command = "curl -L \"" + url + "\" -o \"" + destination + "\"";
		if (std::system(curl_command.c_str()) != 0) {
			throw IOException("Failed to download '" + asset + "' from " + url);
		}

		std::string extract_command = "tar -xzf \"" + destination + "\" -C \"" + data_dir + "\"";
		if (std::system(extract_command.c_str()) != 0) {
			throw IOException("Failed to extract '" + destination + "'");
		}
		fs.RemoveFile(destination);
	}
	data_downloaded = true;
}

PostalStatus PostalManager::GetStatus(ClientContext &context) {
	PostalStatus status;
	status.initialized = initialized.load();
	status.data_dir = ResolveDataDir();
	auto &fs = FileSystem::GetFileSystem(context);
	status.data_present = data_downloaded.load() || DataPresent(fs, status.data_dir);
	return status;
}

void PostalManager::Initialize(ClientContext &context) {
	auto data_dir = ResolveDataDir();
	auto &fs = FileSystem::GetFileSystem(context);

	auto data_dir_c = const_cast<char *>(data_dir.c_str());
	if (!libpostal_setup_datadir(data_dir_c) || !libpostal_setup()) {
		initialized = false;
		throw IOException("Failed to initialize libpostal core data in '" + data_dir +
		                  "'. Run PRAGMA postal_load_data(); before using postal functions.");
	}
	if (!libpostal_setup_parser_datadir(data_dir_c) || !libpostal_setup_parser()) {
		initialized = false;
		throw IOException("Failed to initialize libpostal parser data in '" + data_dir +
		                  "'. Run PRAGMA postal_load_data(); before using postal functions.");
	}
	if (!libpostal_setup_language_classifier_datadir(data_dir_c) || !libpostal_setup_language_classifier()) {
		initialized = false;
		throw IOException("Failed to initialize libpostal language classifier data in '" + data_dir +
		                  "'. Run PRAGMA postal_load_data(); before using postal functions.");
	}

	static std::once_flag exit_flag;
	std::call_once(exit_flag, []() { std::atexit(&PostalManager::AtExitCallback); });
	initialized = true;
}

void PostalManager::Teardown() {
	if (!initialized.load()) {
		return;
	}

	libpostal_teardown_language_classifier();
	libpostal_teardown_parser();
	libpostal_teardown();
	initialized = false;
}

void PostalManager::AtExitCallback() {
	PostalManager::Instance().Teardown();
}

std::string PostalManager::ResolveDataDir() const {
	const char *home_dir = std::getenv("HOME");
	if (!home_dir) {
		throw IOException("HOME environment variable not set");
	}
	return std::string(home_dir) + "/.duckdb/extensions/libpostal";
}

bool PostalManager::DataPresent(FileSystem &fs, const std::string &data_dir) const {
	if (!fs.DirectoryExists(data_dir)) {
		return false;
	}
	const auto classifier_file = fs.JoinPath(data_dir, "language_classifier/language_classifier.dat");
	const auto parser_file = fs.JoinPath(data_dir, "address_parser/address_parser_phrases.dat");
	std::ifstream classifier_stream(classifier_file);
	std::ifstream parser_stream(parser_file);
	return classifier_stream.good() && parser_stream.good();
}

} // namespace postal
} // namespace anofox
} // namespace duckdb

#define DUCKDB_EXTENSION_MAIN

#include "anofox_tabular_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/function/table_function.hpp"

#include <cstdlib>
#include <libpostal/libpostal.h>
#include <mutex>

namespace duckdb {

std::once_flag postal_init_flag;
static bool libpostal_initialized = false;

// Forward declaration
void finalize_libpostal();

void initialize_libpostal(ClientContext &context) {
	const char *home_dir = std::getenv("HOME");
	if (!home_dir) {
		throw IOException("HOME environment variable not set");
	}
	std::string datadir = std::string(home_dir) + "/.duckdb/extensions/libpostal";
	if (!libpostal_setup_datadir((char *)datadir.c_str()) || !libpostal_setup_parser_datadir((char *)datadir.c_str())) {
		throw IOException("Failed to setup libpostal. Did you run PRAGMA anofox_postal_load_data();?");
	}
	libpostal_initialized = true;
	// Register the cleanup function to be called at program exit
	atexit(finalize_libpostal);
}

void finalize_libpostal() {
	libpostal_teardown_parser();
	libpostal_teardown();
	libpostal_initialized = false;
}

inline void anofox_parse_address(DataChunk &args, ExpressionState &state, Vector &result) {
	// Ensure libpostal is initialized only once
	std::call_once(postal_init_flag, [&]() { initialize_libpostal(state.GetContext()); });

	auto &input = args.data[0];
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(args.size(), input_data);
	auto inputs = (string_t *)input_data.data;

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_children = StructVector::GetEntries(result);

	auto &house_number_vec = *result_children[0];
	auto &road_vec = *result_children[1];
	auto &city_vec = *result_children[2];
	auto &state_vec = *result_children[3];
	auto &postcode_vec = *result_children[4];
	auto &country_vec = *result_children[5];

	libpostal_address_parser_options_t options = libpostal_get_address_parser_default_options();

	for (idx_t i = 0; i < args.size(); i++) {
		auto idx = input_data.sel->get_index(i);

		if (!input_data.validity.RowIsValid(idx)) {
			for (auto &child : result_children) {
				FlatVector::SetNull(*child, i, true);
			}
			continue;
		}

		auto input_address = inputs[idx];
		auto input_str = input_address.GetString();

		libpostal_address_parser_response_t *parsed = libpostal_parse_address((char *)input_str.c_str(), options);

		// Set all fields to null initially for this row
		FlatVector::SetNull(house_number_vec, i, true);
		FlatVector::SetNull(road_vec, i, true);
		FlatVector::SetNull(city_vec, i, true);
		FlatVector::SetNull(state_vec, i, true);
		FlatVector::SetNull(postcode_vec, i, true);
		FlatVector::SetNull(country_vec, i, true);

		for (size_t j = 0; j < parsed->num_components; j++) {
			const char *label = parsed->labels[j];
			const char *value = parsed->components[j];

			if (strcmp(label, "house_number") == 0) {
				house_number_vec.SetValue(i, value);
			} else if (strcmp(label, "road") == 0) {
				road_vec.SetValue(i, value);
			} else if (strcmp(label, "city") == 0) {
				city_vec.SetValue(i, value);
			} else if (strcmp(label, "state") == 0) {
				state_vec.SetValue(i, value);
			} else if (strcmp(label, "postcode") == 0) {
				postcode_vec.SetValue(i, value);
			} else if (strcmp(label, "country") == 0) {
				country_vec.SetValue(i, value);
			}
		}

		libpostal_address_parser_response_destroy(parsed);
	}

	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void AnofoxPostalLoadData(ClientContext &context, const FunctionParameters &parameters) {
	const char *home_dir = std::getenv("HOME");
	if (!home_dir) {
		throw IOException("HOME environment variable not set");
	}
	std::string data_dir = std::string(home_dir) + "/.duckdb/extensions/libpostal";
	std::string base_url = "https://public-read-libpostal-data.s3.amazonaws.com/v1.1.0/";

	auto &fs = FileSystem::GetFileSystem(context);
	if (!fs.DirectoryExists(data_dir)) {
		fs.CreateDirectory(data_dir);
	}

	std::vector<std::string> files = {"language_classifier.tar.gz", "libpostal_data.tar.gz", "parser.tar.gz"};

	for (const auto &file : files) {
		auto url = base_url + file;
		auto dest_path = fs.JoinPath(data_dir, file);

		std::string command = "curl -L " + url + " -o " + dest_path;
		int ret = system(command.c_str());
		if (ret != 0) {
			throw IOException("Failed to download " + file);
		}

		command = "tar -xzf " + dest_path + " -C " + data_dir;
		ret = system(command.c_str());
		if (ret != 0) {
			throw IOException("Failed to extract " + file);
		}
		fs.RemoveFile(dest_path);
	}
}

struct AnofoxPostalDataStatusData : public GlobalTableFunctionState {
	AnofoxPostalDataStatusData() : done(false) {
	}
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> AnofoxPostalDataStatusInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<AnofoxPostalDataStatusData>();
}

static unique_ptr<FunctionData> AnofoxPostalDataStatusBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types, vector<string> &names) {
	names.emplace_back("initialized");
	return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	names.emplace_back("data_present");
	return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	names.emplace_back("data_dir");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	return nullptr;
}

static void AnofoxPostalDataStatusFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = (AnofoxPostalDataStatusData &)*data_p.global_state;
	if (data.done) {
		return;
	}

	const char *home_dir = std::getenv("HOME");
	if (!home_dir) {
		throw IOException("HOME environment variable not set");
	}
	std::string data_dir = std::string(home_dir) + "/.duckdb/extensions/libpostal";

	auto &fs = FileSystem::GetFileSystem(context);

	bool data_present = fs.DirectoryExists(data_dir) &&
	                    fs.FileExists(fs.JoinPath(data_dir, "language_classifier/MANIFEST")) &&
	                    fs.FileExists(fs.JoinPath(data_dir, "parser/MANIFEST"));

	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(libpostal_initialized));
	output.SetValue(1, 0, Value::BOOLEAN(data_present));
	output.SetValue(2, 0, Value(data_dir));

	data.done = true;
}

static void LoadInternal(ExtensionLoader &loader) {
	auto anofox_parse_address_fun = ScalarFunction(
	    "anofox_parse_address", {LogicalTypeId::VARCHAR},
	    LogicalType::STRUCT({{"house_number", LogicalType(LogicalTypeId::VARCHAR)},         {"road", LogicalType(LogicalTypeId::VARCHAR)}, {"city", LogicalType(LogicalTypeId::VARCHAR)},       {"state", LogicalType(LogicalTypeId::VARCHAR)}, {"postcode", LogicalType(LogicalTypeId::VARCHAR)},   {"country", LogicalType(LogicalTypeId::VARCHAR)}}),
	    anofox_parse_address);
	loader.RegisterFunction(anofox_parse_address_fun);

	PragmaFunction load_pragma = PragmaFunction::PragmaCall("anofox_postal_load_data", AnofoxPostalLoadData, {});
	loader.RegisterFunction(load_pragma);

	TableFunction status_func("pragma_anofox_postal_data_status", {}, AnofoxPostalDataStatusFunc, AnofoxPostalDataStatusBind,
	                          AnofoxPostalDataStatusInit);
	loader.RegisterFunction(status_func);
}

void AnofoxTabularExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string AnofoxTabularExtension::Name() {
	return "anofox_tabular";
}

std::string AnofoxTabularExtension::Version() const {
#ifdef EXT_VERSION_ANOFOX_TABULAR
	return EXT_VERSION_ANOFOX_TABULAR;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_tabular, loader) {
	duckdb::LoadInternal(loader);
}
}
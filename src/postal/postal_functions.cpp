#include "anofox/postal/postal_functions.hpp"

#include "anofox/postal/postal_manager.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {
namespace anofox {

namespace {

using postal::PostalComponent;
using postal::PostalManager;
using postal::PostalStatus;

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

		FlatVector::SetNull(house_number_vec, i, true);
		FlatVector::SetNull(road_vec, i, true);
		FlatVector::SetNull(city_vec, i, true);
		FlatVector::SetNull(state_vec, i, true);
		FlatVector::SetNull(postcode_vec, i, true);
		FlatVector::SetNull(country_vec, i, true);

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

void PostalLoadDataPragma(ClientContext &context, const FunctionParameters &parameters) {
	PostalManager::Instance().LoadData(context);
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
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("data_present");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("data_dir");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

void PostalStatusFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<PostalStatusState>();
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

ScalarFunction CreateParseFunction(const string &name) {
	return ScalarFunction(name, {LogicalType::VARCHAR},
	                      LogicalType::STRUCT({{"house_number", LogicalType::VARCHAR},
	                                           {"road", LogicalType::VARCHAR},
	                                           {"city", LogicalType::VARCHAR},
	                                           {"state", LogicalType::VARCHAR},
	                                           {"postcode", LogicalType::VARCHAR},
	                                           {"country", LogicalType::VARCHAR}}),
	                      PostalParseAddressFunction);
}

ScalarFunction CreateExpandFunction(const string &name) {
	return ScalarFunction(name, {LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::VARCHAR),
	                      PostalExpandAddressFunction);
}

PragmaFunction CreateLoadDataPragma(const string &name) {
	return PragmaFunction::PragmaCall(name, PostalLoadDataPragma, {});
}

TableFunction CreateStatusFunction(const string &name) {
	return TableFunction(name, {}, PostalStatusFunction, PostalStatusBind, PostalStatusInit);
}

} // namespace

void RegisterPostalFunctions(ExtensionLoader &loader) {
	auto parse_function = CreateParseFunction("postal_parse_address");
	loader.RegisterFunction(parse_function);

	auto parse_alias = CreateParseFunction("anofox_parse_address");
	loader.RegisterFunction(parse_alias);

	auto expand_function = CreateExpandFunction("postal_expand_address");
	loader.RegisterFunction(expand_function);

	auto expand_alias = CreateExpandFunction("anofox_expand_address");
	loader.RegisterFunction(expand_alias);

	auto load_data_pragma = CreateLoadDataPragma("postal_load_data");
	loader.RegisterFunction(load_data_pragma);

	auto load_data_alias = CreateLoadDataPragma("anofox_postal_load_data");
	loader.RegisterFunction(load_data_alias);

	auto status_function = CreateStatusFunction("pragma_postal_data_status");
	loader.RegisterFunction(status_function);

	auto status_alias = CreateStatusFunction("pragma_anofox_postal_data_status");
	loader.RegisterFunction(status_alias);
}

} // namespace anofox
} // namespace duckdb

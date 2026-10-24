#include "anofox_contract.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_trace.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb_compat.hpp"

namespace duckdb {
namespace anofox {

namespace {

// Contract producers: function (primary name and alias) -> output schema. A function is
// listed only once its output carries every required column of its schema as it comes;
// test/sql/contract_anofox_tabular.test asserts the tags and each producer's columns.
struct ContractProducer {
	const char *primary;
	const char *alias;
	const char *output;
};

constexpr ContractProducer CONTRACT_PRODUCERS[] = {
    // check: check_name, status, value, lower_threshold, upper_threshold, identifier, message, run_ts
    {"anofox_tab_run_checks", "run_checks", "check"},
    // matrix: column_a x column_b with pearson as the value (a pair list, mirrored by the reader)
    {"anofox_tab_profile_correlations", "profile_correlations", "matrix"},
    // events: ds, kind, label, severity, value, threshold, series (the time-aware checks' warn/fail rows)
    {"anofox_tab_rel_count_change_events", "rel_count_change_events", "events"},
    {"anofox_tab_metric_anomaly_iqr_events", "metric_anomaly_iqr_events", "events"},
    {"anofox_tab_rolling_values_in_set_events", "rolling_values_in_set_events", "events"},
};

constexpr const char *CONTRACT_VERSION_FUNCTION = "anofox_contract_version";

void ContractVersionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto result_data = duckdb_compat::FlatData<string_t>(result);
	for (idx_t i = 0; i < args.size(); i++) {
		result_data[i] = StringVector::AddString(result, ANOFOX_CONTRACT_VERSION);
	}
}

} // anonymous namespace

void RegisterContractVersionFunction(ExtensionLoader &loader) {
	if (loader.TryGetFunction(duckdb_compat::FunctionName(CONTRACT_VERSION_FUNCTION))) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		            "contract: anofox_contract_version() already registered by another anofox extension");
		return;
	}
	ScalarFunction func(CONTRACT_VERSION_FUNCTION, {}, LogicalType(LogicalTypeId::VARCHAR), ContractVersionFunction);
	FunctionDescription desc;
	desc.description = "Returns the version of the anofox integration contract (output schemas and "
	                   "duckdb_functions() tags) this extension implements.";
	desc.examples = {"SELECT anofox_contract_version();"};
	desc.categories = {"metadata"};
	RegisterScalarFunction(loader, std::move(func), {std::move(desc)});
}

void ApplyContractTags(ExtensionLoader &loader) {
	for (auto &producer : CONTRACT_PRODUCERS) {
		for (auto name : {producer.primary, producer.alias}) {
			SetTableFunctionTags(loader, name,
			                     {{"anofox.output", producer.output}, {"anofox.contract", ANOFOX_CONTRACT_VERSION}});
		}
		AnofoxTrace(AnofoxLogLevel::Trace, string("contract: tagged ") + producer.primary + " as '" +
		                                       producer.output + "'");
	}
}

} // namespace anofox
} // namespace duckdb

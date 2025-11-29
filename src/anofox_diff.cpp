#include "anofox_diff.hpp"
#include "anofox_function_alias.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

constexpr const char *DIFF_TYPE_ADDED = "added";
constexpr const char *DIFF_TYPE_REMOVED = "removed";
constexpr const char *DIFF_TYPE_CHANGED = "changed";
constexpr const char *DIFF_TYPE_UNCHANGED = "unchanged";

//===--------------------------------------------------------------------===//
// Diff Function Bind Data
//===--------------------------------------------------------------------===//
struct DiffFunctionBindData : public TableFunctionData {
	vector<string> primary_keys;
	vector<string> compare_columns;
	bool include_all;

	DiffFunctionBindData(vector<string> pks, vector<string> cols, bool include_all_rows)
	    : primary_keys(std::move(pks)), compare_columns(std::move(cols)), include_all(include_all_rows) {
	}
};

//===--------------------------------------------------------------------===//
// Diff Function Local State
//===--------------------------------------------------------------------===//
struct DiffFunctionLocalState : public LocalTableFunctionState {
	DiffFunctionLocalState() : initialized(false), finished(false) {
	}

	bool initialized;
	bool finished;
	idx_t current_row;
};

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//
static unique_ptr<LocalTableFunctionState> DiffLocalInit(ExecutionContext &context, TableFunctionInitInput &input,
                                                         GlobalTableFunctionState *global_state) {
	return make_uniq<DiffFunctionLocalState>();
}

static vector<string> ParseColumnList(const Value &column_value) {
	vector<string> result;
	if (column_value.IsNull()) {
		return result;
	}

	if (column_value.type().id() == LogicalTypeId::VARCHAR) {
		// Single column name
		result.push_back(column_value.ToString());
	} else if (column_value.type().id() == LogicalTypeId::LIST) {
		// List of column names
		auto &list_children = ListValue::GetChildren(column_value);
		for (const auto &child : list_children) {
			if (!child.IsNull()) {
				result.push_back(child.ToString());
			}
		}
	}

	return result;
}

static void ValidatePrimaryKeys(const vector<string> &primary_keys, const vector<string> &available_columns,
                               const string &table_name) {
	if (primary_keys.empty()) {
		throw InvalidInputException("Primary key(s) must be specified for data diff");
	}

	std::unordered_set<string> available_set(available_columns.begin(), available_columns.end());

	for (const auto &pk : primary_keys) {
		if (available_set.find(pk) == available_set.end()) {
			throw InvalidInputException("Primary key column '%s' not found in %s table", pk, table_name);
		}
	}
}

static void ValidateCompareColumns(const vector<string> &compare_columns, const vector<string> &available_columns,
                                  const vector<string> &primary_keys) {
	if (compare_columns.empty()) {
		return; // Will use all columns
	}

	std::unordered_set<string> available_set(available_columns.begin(), available_columns.end());

	for (const auto &col : compare_columns) {
		if (available_set.find(col) == available_set.end()) {
			throw InvalidInputException("Compare column '%s' not found in table", col);
		}
	}
}

//===--------------------------------------------------------------------===//
// JoinDiff Bind Function
//===--------------------------------------------------------------------===//
static unique_ptr<FunctionData> JoinDiffBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	// Parameters:
	// 0: source_table (TABLE)
	// 1: target_table (TABLE)
	// 2: primary_key(s) (VARCHAR or LIST<VARCHAR>)
	// 3: compare_columns (LIST<VARCHAR>, optional, default NULL)
	// 4: include_all (BOOLEAN, optional, default false)

	if (input.inputs.size() < 3) {
		throw InvalidInputException("anofox_diff_joindiff requires at least 3 arguments: source_table, target_table, primary_key(s)");
	}

	// Parse primary keys
	vector<string> primary_keys = ParseColumnList(input.inputs[2]);
	if (primary_keys.empty()) {
		throw InvalidInputException("Primary key(s) cannot be empty");
	}

	// Parse compare columns (optional)
	vector<string> compare_columns;
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		compare_columns = ParseColumnList(input.inputs[3]);
	}

	// Parse include_all (optional, default false)
	bool include_all = false;
	if (input.inputs.size() > 4 && !input.inputs[4].IsNull()) {
		include_all = input.inputs[4].GetValue<bool>();
	}

	// Output schema: diff_type + primary keys + compared columns
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("diff_type");

	// For now, we'll determine the actual schema dynamically during execution
	// This is a simplified bind - in practice, we need access to the input table schemas

	// LogMessage(context, "diff", "Binding JoinDiff with %zu primary key(s), %zu compare column(s), include_all=%d",
	//           primary_keys.size(), compare_columns.size(), include_all);

	return make_uniq<DiffFunctionBindData>(std::move(primary_keys), std::move(compare_columns), include_all);
}

//===--------------------------------------------------------------------===//
// JoinDiff In-Out Function
//===--------------------------------------------------------------------===//
static OperatorResultType JoinDiffFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                          DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<DiffFunctionBindData>();
	auto &local_state = data_p.local_state->Cast<DiffFunctionLocalState>();

	if (local_state.finished) {
		output.SetCardinality(0);
		return OperatorResultType::FINISHED;
	}

	if (!local_state.initialized) {
		// LogMessage(context.client, "diff", "Initializing JoinDiff function");
		local_state.initialized = true;
		local_state.current_row = 0;
	}

	// TODO: Implement actual FULL OUTER JOIN logic
	// For now, return empty result set
	output.SetCardinality(0);
	local_state.finished = true;

	return OperatorResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// JoinDiff Finalize Function
//===--------------------------------------------------------------------===//
static OperatorFinalizeResultType JoinDiffFinalize(ExecutionContext &context, TableFunctionInput &data_p,
                                                   DataChunk &output) {
	auto &local_state = data_p.local_state->Cast<DiffFunctionLocalState>();

	if (local_state.finished) {
		return OperatorFinalizeResultType::FINISHED;
	}

	output.SetCardinality(0);
	local_state.finished = true;

	return OperatorFinalizeResultType::FINISHED;
}

} // anonymous namespace

//===--------------------------------------------------------------------===//
// JoinDiff Bind Replace - Generate SQL Query
//===--------------------------------------------------------------------===//

static unique_ptr<SubqueryRef> ParseSubquery(const string &query, const ParserOptions &options, const string &err_msg) {
	Parser parser(options);
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto select_stmt = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select_stmt));
}

static string GenerateJoinDiffSQL(const string &source_table, const string &target_table,
                                  const vector<string> &primary_keys, const vector<string> &compare_columns,
                                  bool include_all) {
	// Build the primary key join condition
	string pk_join_condition;
	for (size_t i = 0; i < primary_keys.size(); i++) {
		if (i > 0) {
			pk_join_condition += " AND ";
		}
		pk_join_condition += "s." + primary_keys[i] + " = t." + primary_keys[i];
	}

	// Build the COALESCE expressions for primary keys
	string pk_select;
	for (size_t i = 0; i < primary_keys.size(); i++) {
		if (i > 0) {
			pk_select += ", ";
		}
		pk_select += "COALESCE(s." + primary_keys[i] + ", t." + primary_keys[i] + ") AS " + primary_keys[i];
	}

	// Build column list for comparison
	string compare_list;
	if (!compare_columns.empty()) {
		for (size_t i = 0; i < compare_columns.size(); i++) {
			if (i > 0) {
				compare_list += ", ";
			}
			compare_list += compare_columns[i];
		}
	}

	// Generate the diff query
	string sql = "SELECT ";
	sql += "CASE ";
	sql += "WHEN s." + primary_keys[0] + " IS NULL THEN 'added' ";
	sql += "WHEN t." + primary_keys[0] + " IS NULL THEN 'removed' ";

	// For comparison, we need to check if any non-PK columns differ
	// Simplified: use a hash-based comparison
	if (compare_columns.empty()) {
		// Compare all columns (approximation using struct comparison)
		sql += "WHEN s IS DISTINCT FROM t THEN 'changed' ";
	} else {
		// Compare specific columns
		sql += "WHEN (";
		for (size_t i = 0; i < compare_columns.size(); i++) {
			if (i > 0) {
				sql += " OR ";
			}
			sql += "s." + compare_columns[i] + " IS DISTINCT FROM t." + compare_columns[i];
		}
		sql += ") THEN 'changed' ";
	}

	sql += "ELSE 'unchanged' END AS diff_type, ";
	sql += pk_select;

	// Add all columns from target except primary keys (showing target values for changed rows)
	sql += ", t.* EXCLUDE (";
	for (size_t i = 0; i < primary_keys.size(); i++) {
		if (i > 0) {
			sql += ", ";
		}
		sql += primary_keys[i];
	}
	sql += ")";

	// Parse table names
	auto source_qname = QualifiedName::Parse(source_table);
	auto target_qname = QualifiedName::Parse(target_table);

	sql += " FROM " + source_qname.ToString() + " s ";
	sql += "FULL OUTER JOIN " + target_qname.ToString() + " t ";
	sql += "ON " + pk_join_condition;

	if (!include_all) {
		sql += " WHERE diff_type != 'unchanged'";
	}

	return sql;
}

static unique_ptr<TableRef> JoinDiffBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	// Parameters:
	// 0: source_table (VARCHAR)
	// 1: target_table (VARCHAR)
	// 2: primary_key(s) (VARCHAR or LIST<VARCHAR>)
	// 3: compare_columns (LIST<VARCHAR>, optional)
	// 4: include_all (BOOLEAN, optional)

	if (input.inputs.size() < 3) {
		throw BinderException("anofox_diff_joindiff requires at least 3 arguments: source_table, target_table, primary_key(s)");
	}

	// Parse source and target table names
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("source_table and target_table cannot be NULL");
	}

	string source_table = input.inputs[0].ToString();
	string target_table = input.inputs[1].ToString();

	// Parse primary keys
	vector<string> primary_keys = ParseColumnList(input.inputs[2]);
	if (primary_keys.empty()) {
		throw BinderException("Primary key(s) cannot be empty");
	}

	// Parse compare columns (optional)
	// Note: param 3 could also be bisection_threshold (BIGINT) for HashDiff compat
	vector<string> compare_columns;
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		// Check type - LIST means compare_columns, BIGINT means bisection_threshold (ignore)
		if (input.inputs[3].type().id() == LogicalTypeId::LIST) {
			compare_columns = ParseColumnList(input.inputs[3]);
		}
	}

	// Parse include_all (optional, default false)
	// Note: param 4 could also be bisection_factor (BIGINT) for HashDiff compat
	bool include_all = false;
	if (input.inputs.size() > 4 && !input.inputs[4].IsNull()) {
		// Check type - BOOLEAN means include_all, BIGINT means bisection_factor (ignore)
		if (input.inputs[4].type().id() == LogicalTypeId::BOOLEAN) {
			include_all = input.inputs[4].GetValue<bool>();
		}
	}

	// Generate SQL query
	string sql = GenerateJoinDiffSQL(source_table, target_table, primary_keys, compare_columns, include_all);

	// Parse and return as SubqueryRef
	auto subquery_ref = ParseSubquery(sql, context.GetParserOptions(),
	                                  "Failed to parse generated diff query");
	return std::move(subquery_ref);
}

void RegisterDiffFunctions(ExtensionLoader &loader) {
	// Register anofox_tab_diff_joindiff using bind_replace for SQL generation
	// This accepts table NAMES (VARCHAR) and generates the diff SQL query

	// Register anofox_tab_diff_joindiff with all overloads (alias: diff_joindiff)
	TableFunctionSet joindiff_set("anofox_tab_diff_joindiff");
	
	// Single primary key overload (VARCHAR primary_key)
	TableFunction joindiff_single("anofox_tab_diff_joindiff",
	                             {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                              LogicalType(LogicalTypeId::VARCHAR)},
	                             nullptr, nullptr);
	joindiff_single.bind_replace = JoinDiffBindReplace;
	joindiff_single.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_set.AddFunction(joindiff_single);

	// Add optional parameters for single PK overload
	TableFunction joindiff_single_compare("anofox_tab_diff_joindiff",
	                                     {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                      LogicalType(LogicalTypeId::VARCHAR),
	                                      LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                                     nullptr, nullptr);
	joindiff_single_compare.bind_replace = JoinDiffBindReplace;
	joindiff_single_compare.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single_compare.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single_compare.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single_compare.named_parameters["compare_columns"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	joindiff_set.AddFunction(joindiff_single_compare);

	// Full parameters for single PK overload
	TableFunction joindiff_single_full("anofox_tab_diff_joindiff",
	                                  {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                   LogicalType(LogicalTypeId::VARCHAR),
	                                   LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
	                                   LogicalType(LogicalTypeId::BOOLEAN)},
	                                  nullptr, nullptr);
	joindiff_single_full.bind_replace = JoinDiffBindReplace;
	joindiff_single_full.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single_full.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single_full.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_single_full.named_parameters["compare_columns"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	joindiff_single_full.named_parameters["include_all"] = LogicalType(LogicalTypeId::BOOLEAN);
	joindiff_set.AddFunction(joindiff_single_full);

	// Compound primary keys overload (LIST<VARCHAR> primary_keys)
	TableFunction joindiff_compound("anofox_tab_diff_joindiff",
	                               {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                               nullptr, nullptr);
	joindiff_compound.bind_replace = JoinDiffBindReplace;
	joindiff_compound.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_compound.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_compound.named_parameters["primary_keys"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	joindiff_set.AddFunction(joindiff_compound);

	// Compound PK with compare_columns
	TableFunction joindiff_compound_compare("anofox_tab_diff_joindiff",
	                                       {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                        LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
	                                        LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                                       nullptr, nullptr);
	joindiff_compound_compare.bind_replace = JoinDiffBindReplace;
	joindiff_compound_compare.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_compound_compare.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_compound_compare.named_parameters["primary_keys"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	joindiff_compound_compare.named_parameters["compare_columns"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	joindiff_set.AddFunction(joindiff_compound_compare);

	// Compound PK with all parameters
	TableFunction joindiff_compound_full("anofox_tab_diff_joindiff",
	                                    {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                     LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
	                                     LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
	                                     LogicalType(LogicalTypeId::BOOLEAN)},
	                                    nullptr, nullptr);
	joindiff_compound_full.bind_replace = JoinDiffBindReplace;
	joindiff_compound_full.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_compound_full.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	joindiff_compound_full.named_parameters["primary_keys"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	joindiff_compound_full.named_parameters["compare_columns"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	joindiff_compound_full.named_parameters["include_all"] = LogicalType(LogicalTypeId::BOOLEAN);
	joindiff_set.AddFunction(joindiff_compound_full);
	
	loader.RegisterFunction(joindiff_set);
	
	// Register alias
	TableFunctionSet alias_joindiff_set("diff_joindiff");
	for (const auto &func : joindiff_set.functions) {
		TableFunction alias_func("diff_joindiff", func.arguments, func.function, func.bind, func.init_global, func.init_local);
		alias_func.init_global = func.init_global;
		alias_func.init_local = func.init_local;
		alias_func.bind_replace = func.bind_replace;
		alias_func.named_parameters = func.named_parameters;
		alias_joindiff_set.AddFunction(alias_func);
	}
	CreateTableFunctionInfo alias_joindiff_info(alias_joindiff_set);
	alias_joindiff_info.alias_of = "anofox_tab_diff_joindiff";
	loader.RegisterFunction(alias_joindiff_info);

	//===--------------------------------------------------------------------===//
	// HashDiff Function Registration
	// Note: Currently uses bind_replace like JoinDiff
	// Full bisection with iterative checksum comparison would require
	// a different architecture (e.g., external process or UDF)
	//===--------------------------------------------------------------------===//

	// For now, HashDiff is an alias to JoinDiff
	// Future: Implement SQL-based bisection using recursive CTEs

	// Register anofox_tab_diff_hashdiff with all overloads (alias: diff_hashdiff)
	TableFunctionSet hashdiff_set("anofox_tab_diff_hashdiff");
	
	// Single PK, basic
	TableFunction hashdiff_single("anofox_tab_diff_hashdiff",
	                             {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                              LogicalType(LogicalTypeId::VARCHAR)},
	                             nullptr, nullptr);
	hashdiff_single.bind_replace = JoinDiffBindReplace;  // Reuse JoinDiff for now
	hashdiff_single.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_set.AddFunction(hashdiff_single);

	// Single PK with bisection_threshold (ignored for now)
	TableFunction hashdiff_single_threshold("anofox_tab_diff_hashdiff",
	                                       {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                        LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT)},
	                                       nullptr, nullptr);
	hashdiff_single_threshold.bind_replace = JoinDiffBindReplace;
	hashdiff_single_threshold.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_threshold.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_threshold.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_threshold.named_parameters["bisection_threshold"] = LogicalType(LogicalTypeId::BIGINT);
	hashdiff_set.AddFunction(hashdiff_single_threshold);

	// Single PK with all parameters (ignored for now)
	TableFunction hashdiff_single_full("anofox_tab_diff_hashdiff",
	                                  {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                   LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	                                   LogicalType(LogicalTypeId::BIGINT)},
	                                  nullptr, nullptr);
	hashdiff_single_full.bind_replace = JoinDiffBindReplace;
	hashdiff_single_full.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_full.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_full.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_full.named_parameters["bisection_threshold"] = LogicalType(LogicalTypeId::BIGINT);
	hashdiff_single_full.named_parameters["bisection_factor"] = LogicalType(LogicalTypeId::BIGINT);
	hashdiff_set.AddFunction(hashdiff_single_full);

	// Compound PK, basic
	TableFunction hashdiff_compound("anofox_tab_diff_hashdiff",
	                               {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                               nullptr, nullptr);
	hashdiff_compound.bind_replace = JoinDiffBindReplace;
	hashdiff_compound.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_compound.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_compound.named_parameters["primary_keys"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	hashdiff_set.AddFunction(hashdiff_compound);
	
	loader.RegisterFunction(hashdiff_set);
	
	// Register alias
	TableFunctionSet alias_hashdiff_set("diff_hashdiff");
	for (const auto &func : hashdiff_set.functions) {
		TableFunction alias_func("diff_hashdiff", func.arguments, func.function, func.bind, func.init_global, func.init_local);
		alias_func.init_global = func.init_global;
		alias_func.init_local = func.init_local;
		alias_func.bind_replace = func.bind_replace;
		alias_func.named_parameters = func.named_parameters;
		alias_hashdiff_set.AddFunction(alias_func);
	}
	CreateTableFunctionInfo alias_hashdiff_info(alias_hashdiff_set);
	alias_hashdiff_info.alias_of = "anofox_tab_diff_hashdiff";
	loader.RegisterFunction(alias_hashdiff_info);
}

} // namespace anofox
} // namespace duckdb

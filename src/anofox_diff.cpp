#include "anofox_diff.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "telemetry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"

#include <string>
#include <type_traits>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

//===--------------------------------------------------------------------===//
// Helper Functions
//===--------------------------------------------------------------------===//

// Marker columns used to track row presence on each side of the FULL OUTER
// JOIN. Unlike "first primary key IS NULL" checks, the markers stay correct
// when primary key values themselves are NULL.
constexpr const char *SOURCE_PRESENT_MARKER = "__anofox_diff_s_present";
constexpr const char *TARGET_PRESENT_MARKER = "__anofox_diff_t_present";

vector<string> ParseColumnList(const Value &column_value) {
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

// DuckDB v1.5 resolves view columns via BindView()/GetColumnInfo(); v1.4 stores
// the bound names directly on the catalog entry. No version macro is visible to
// extension builds, so the available API is detected at compile time.
template <typename T, typename = void>
struct ViewHasGetColumnInfo : std::false_type {};
template <typename T>
struct ViewHasGetColumnInfo<T, std::void_t<decltype(std::declval<T &>().GetColumnInfo())>> : std::true_type {};

template <typename VIEW>
typename std::enable_if<ViewHasGetColumnInfo<VIEW>::value, vector<string>>::type
ResolveViewColumnNames(ClientContext &context, VIEW &view) {
	view.BindView(context);
	auto column_info = view.GetColumnInfo();
	if (!column_info) {
		return {};
	}
	vector<string> result;
	for (idx_t i = 0; i < column_info->names.size(); i++) {
		result.push_back(i < view.aliases.size() ? view.aliases[i] : column_info->names[i]);
	}
	return result;
}

template <typename VIEW>
typename std::enable_if<!ViewHasGetColumnInfo<VIEW>::value, vector<string>>::type
ResolveViewColumnNames(ClientContext &, VIEW &view) {
	vector<string> result;
	for (idx_t i = 0; i < view.names.size(); i++) {
		result.push_back(i < view.aliases.size() ? view.aliases[i] : view.names[i]);
	}
	return result;
}

// Resolve the column names of a table or view through the catalog so schema
// problems surface as clear binder errors instead of confusing errors from
// the generated SQL.
vector<string> GetRelationColumnNames(ClientContext &context, const string &function_name, const string &table_name) {
	auto qname = QualifiedName::Parse(table_name);
	EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, qname.name);
	auto entry = Catalog::GetEntry(context, qname.catalog, qname.schema, lookup_info, OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		throw BinderException("%s: table or view '%s' does not exist", function_name, table_name);
	}

	vector<string> result;
	if (entry->type == CatalogType::TABLE_ENTRY) {
		auto &table = entry->Cast<TableCatalogEntry>();
		for (auto &col : table.GetColumns().Logical()) {
			result.push_back(col.Name());
		}
	} else if (entry->type == CatalogType::VIEW_ENTRY) {
		auto &view = entry->Cast<ViewCatalogEntry>();
		result = ResolveViewColumnNames(context, view);
		if (result.empty()) {
			throw BinderException("%s: unable to resolve the columns of view '%s'", function_name, table_name);
		}
	} else {
		throw BinderException("%s: '%s' is not a table or view", function_name, table_name);
	}
	return result;
}

// DuckDB matches identifiers case-insensitively, so validation does too.
std::unordered_set<string> MakeColumnSet(const vector<string> &columns) {
	std::unordered_set<string> result;
	for (const auto &col : columns) {
		result.insert(StringUtil::Lower(col));
	}
	return result;
}

void ValidatePrimaryKeys(const string &function_name, const vector<string> &primary_keys,
                         const std::unordered_set<string> &available_columns, const string &side,
                         const string &table_name) {
	for (const auto &pk : primary_keys) {
		if (available_columns.find(StringUtil::Lower(pk)) == available_columns.end()) {
			throw BinderException("%s: Primary key column '%s' not found in %s table '%s'", function_name, pk, side,
			                      table_name);
		}
	}
}

void ValidateCompareColumns(const string &function_name, const vector<string> &compare_columns,
                            const std::unordered_set<string> &available_columns, const string &side,
                            const string &table_name) {
	for (const auto &col : compare_columns) {
		if (available_columns.find(StringUtil::Lower(col)) == available_columns.end()) {
			throw BinderException("%s: Compare column '%s' not found in %s table '%s'", function_name, col, side,
			                      table_name);
		}
	}
}

// Validate the requested key/compare columns against both table schemas and
// resolve the effective compare set (defaults to the shared non-key columns).
vector<string> ResolveCompareColumns(ClientContext &context, const string &function_name, const string &source_table,
                                     const string &target_table, const vector<string> &primary_keys,
                                     const vector<string> &compare_columns) {
	auto source_columns = GetRelationColumnNames(context, function_name, source_table);
	auto target_columns = GetRelationColumnNames(context, function_name, target_table);
	auto source_set = MakeColumnSet(source_columns);
	auto target_set = MakeColumnSet(target_columns);

	ValidatePrimaryKeys(function_name, primary_keys, source_set, "source", source_table);
	ValidatePrimaryKeys(function_name, primary_keys, target_set, "target", target_table);

	if (!compare_columns.empty()) {
		ValidateCompareColumns(function_name, compare_columns, source_set, "source", source_table);
		ValidateCompareColumns(function_name, compare_columns, target_set, "target", target_table);
		return compare_columns;
	}

	// Default: compare every non-key column the two tables share, in target
	// column order (the output projects the target side)
	auto pk_set = MakeColumnSet(primary_keys);
	vector<string> shared_columns;
	for (const auto &col : target_columns) {
		auto lower = StringUtil::Lower(col);
		if (pk_set.find(lower) != pk_set.end()) {
			continue;
		}
		if (source_set.find(lower) != source_set.end()) {
			shared_columns.push_back(col);
		}
	}
	return shared_columns;
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
	// NULL-safe primary key join condition: rows whose key components are
	// NULL still match their counterpart on the other side
	string pk_join_condition;
	for (size_t i = 0; i < primary_keys.size(); i++) {
		if (i > 0) {
			pk_join_condition += " AND ";
		}
		pk_join_condition += "s." + QuoteSqlIdentifier(primary_keys[i]) + " IS NOT DISTINCT FROM t." +
		                     QuoteSqlIdentifier(primary_keys[i]);
	}

	// Build the COALESCE expressions for primary keys
	string pk_select;
	for (size_t i = 0; i < primary_keys.size(); i++) {
		if (i > 0) {
			pk_select += ", ";
		}
		pk_select += "COALESCE(s." + QuoteSqlIdentifier(primary_keys[i]) + ", t." + QuoteSqlIdentifier(primary_keys[i]) +
		             ") AS " + QuoteSqlIdentifier(primary_keys[i]);
	}

	// Classify rows via the side-presence markers (robust against NULL keys)
	string sql = "SELECT ";
	sql += "CASE ";
	sql += "WHEN s." + string(SOURCE_PRESENT_MARKER) + " IS NULL THEN 'added' ";
	sql += "WHEN t." + string(TARGET_PRESENT_MARKER) + " IS NULL THEN 'removed' ";

	// Compare the resolved columns one by one; the compare set was validated
	// against both schemas at bind time
	if (!compare_columns.empty()) {
		sql += "WHEN (";
		for (size_t i = 0; i < compare_columns.size(); i++) {
			if (i > 0) {
				sql += " OR ";
			}
			sql += "s." + QuoteSqlIdentifier(compare_columns[i]) + " IS DISTINCT FROM t." +
			       QuoteSqlIdentifier(compare_columns[i]);
		}
		sql += ") THEN 'changed' ";
	}

	sql += "ELSE 'unchanged' END AS diff_type, ";
	sql += pk_select;

	// Add all columns from target except primary keys and the presence marker
	// (showing target values for changed rows)
	sql += ", t.* EXCLUDE (";
	sql += string(TARGET_PRESENT_MARKER);
	for (size_t i = 0; i < primary_keys.size(); i++) {
		sql += ", " + QuoteSqlIdentifier(primary_keys[i]);
	}
	sql += ")";

	// Wrap each side with a constant presence marker; table references go
	// through query_table() so hostile and schema-qualified names are handled
	// by the shared quoting helpers
	sql += " FROM (SELECT true AS " + string(SOURCE_PRESENT_MARKER) + ", * FROM " + BuildQueryTableRef(source_table) +
	       ") s ";
	sql += "FULL OUTER JOIN (SELECT true AS " + string(TARGET_PRESENT_MARKER) + ", * FROM " +
	       BuildQueryTableRef(target_table) + ") t ";
	sql += "ON " + pk_join_condition;

	if (!include_all) {
		sql += " WHERE diff_type != 'unchanged'";
	}

	return sql;
}

static unique_ptr<TableRef> JoinDiffBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("diff_joindiff");
	// Parameters:
	// 0: source_table (VARCHAR)
	// 1: target_table (VARCHAR)
	// 2: primary_key(s) (VARCHAR or LIST<VARCHAR>)
	// 3: compare_columns (LIST<VARCHAR>, optional)
	// 4: include_all (BOOLEAN, optional)

	if (input.inputs.size() < 3) {
		throw BinderException("diff_joindiff requires at least 3 arguments: source_table, target_table, primary_key(s)");
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
	vector<string> compare_columns;
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		compare_columns = ParseColumnList(input.inputs[3]);
	}

	// Parse include_all (optional, default false)
	bool include_all = false;
	if (input.inputs.size() > 4 && !input.inputs[4].IsNull()) {
		include_all = input.inputs[4].GetValue<bool>();
	}

	// Validate both schemas and resolve the effective compare set
	compare_columns = ResolveCompareColumns(context, "diff_joindiff", source_table, target_table, primary_keys,
	                                        compare_columns);

	// Generate SQL query
	string sql = GenerateJoinDiffSQL(source_table, target_table, primary_keys, compare_columns, include_all);

	// Parse and return as SubqueryRef
	auto subquery_ref = ParseSubquery(sql, context.GetParserOptions(),
	                                  "Failed to parse generated diff query");
	return std::move(subquery_ref);
}

// HashDiff bind_replace - wraps JoinDiff with separate telemetry tracking
static unique_ptr<TableRef> HashDiffBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("diff_hashdiff");
	// HashDiff currently executes the same NULL-safe full outer join plan as
	// JoinDiff; it is tracked separately for telemetry purposes.

	if (input.inputs.size() < 3) {
		throw BinderException("diff_hashdiff requires at least 3 arguments: source_table, target_table, primary_key(s)");
	}

	// The hash/bisection algorithm behind bisection_threshold/bisection_factor
	// is not implemented; reject the parameters instead of silently ignoring them
	if (input.inputs.size() > 3) {
		throw BinderException("diff_hashdiff: the bisection_threshold/bisection_factor parameters are not implemented "
		                      "yet; remove them (diff_hashdiff currently computes the same full diff as diff_joindiff)");
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

	// Validate both schemas and resolve the compare set (all shared non-key columns)
	auto compare_columns = ResolveCompareColumns(context, "diff_hashdiff", source_table, target_table, primary_keys, {});

	// Generate SQL query using same logic as JoinDiff
	string sql = GenerateJoinDiffSQL(source_table, target_table, primary_keys, compare_columns, false);

	// Parse and return as SubqueryRef
	auto subquery_ref = ParseSubquery(sql, context.GetParserOptions(),
	                                  "Failed to parse generated hashdiff query");
	return std::move(subquery_ref);
}

void RegisterDiffFunctions(ExtensionLoader &loader) {
	// The diff functions are implemented purely as bind_replace SQL rewrites:
	// they accept table NAMES (VARCHAR) and expand into a FULL OUTER JOIN query
	// that DuckDB optimizes and executes natively.

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

	{
		FunctionDescription desc;
		desc.description = "Computes a row-level diff between two tables using a primary key (VARCHAR or VARCHAR[]), returning rows that were added, removed, or changed. Primary keys are matched NULL-safely (IS NOT DISTINCT FROM).";
		desc.parameter_names = {"source_table", "target_table", "primary_key"};
		desc.examples = {"SELECT * FROM diff_joindiff('orders_v1', 'orders_v2', 'order_id');",
		                 "SELECT * FROM diff_joindiff('orders_v1', 'orders_v2', ['order_id', 'line_id']);"};
		desc.categories = {"diff", "data-quality"};

		RegisterTableFunctionSetWithAlias(loader, joindiff_set, "diff_joindiff", {std::move(desc)});
	}

	//===--------------------------------------------------------------------===//
	// HashDiff Function Registration
	// Note: Currently uses the same bind_replace rewrite as JoinDiff. The
	// bisection_threshold/bisection_factor overloads remain registered so that
	// callers get a clear "not implemented" binder error instead of a generic
	// overload-resolution failure or silently ignored parameters.
	//===--------------------------------------------------------------------===//

	// Register anofox_tab_diff_hashdiff with all overloads (alias: diff_hashdiff)
	TableFunctionSet hashdiff_set("anofox_tab_diff_hashdiff");

	// Single PK, basic
	TableFunction hashdiff_single("anofox_tab_diff_hashdiff",
	                             {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                              LogicalType(LogicalTypeId::VARCHAR)},
	                             nullptr, nullptr);
	hashdiff_single.bind_replace = HashDiffBindReplace;
	hashdiff_single.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_set.AddFunction(hashdiff_single);

	// Single PK with bisection_threshold (rejected as not implemented)
	TableFunction hashdiff_single_threshold("anofox_tab_diff_hashdiff",
	                                       {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                        LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT)},
	                                       nullptr, nullptr);
	hashdiff_single_threshold.bind_replace = HashDiffBindReplace;
	hashdiff_single_threshold.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_threshold.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_threshold.named_parameters["primary_key"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_single_threshold.named_parameters["bisection_threshold"] = LogicalType(LogicalTypeId::BIGINT);
	hashdiff_set.AddFunction(hashdiff_single_threshold);

	// Single PK with bisection_threshold and bisection_factor (rejected as not implemented)
	TableFunction hashdiff_single_full("anofox_tab_diff_hashdiff",
	                                  {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                   LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	                                   LogicalType(LogicalTypeId::BIGINT)},
	                                  nullptr, nullptr);
	hashdiff_single_full.bind_replace = HashDiffBindReplace;
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
	hashdiff_compound.bind_replace = HashDiffBindReplace;
	hashdiff_compound.named_parameters["source_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_compound.named_parameters["target_table"] = LogicalType(LogicalTypeId::VARCHAR);
	hashdiff_compound.named_parameters["primary_keys"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));
	hashdiff_set.AddFunction(hashdiff_compound);

	{
		FunctionDescription desc;
		desc.description = "Computes a row-level diff between two tables using a primary key (VARCHAR or VARCHAR[]). Currently identical to diff_joindiff; the bisection_threshold/bisection_factor parameters are not implemented and rejected.";
		desc.parameter_names = {"source_table", "target_table", "primary_key"};
		desc.examples = {"SELECT * FROM diff_hashdiff('products_v1', 'products_v2', 'product_id');",
		                 "SELECT * FROM diff_hashdiff('products_v1', 'products_v2', ['product_id', 'sku']);"};
		desc.categories = {"diff", "data-quality"};

		RegisterTableFunctionSetWithAlias(loader, hashdiff_set, "diff_hashdiff", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

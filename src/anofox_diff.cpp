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
#include "duckdb_compat.hpp"

#include <string>
#include <type_traits>
#include <unordered_set>
#include "anofox_tabular_banner.hpp"

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

// DuckDB v1.5 and 2.0 resolve view columns via BindView()/GetColumnInfo(). The
// fallback overload (bound names stored directly on the catalog entry) served
// DuckDB v1.4, which is no longer supported; it is only instantiated when
// GetColumnInfo() is missing. The available API is detected at compile time.
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
		result.push_back(i < view.aliases.size() ? duckdb_compat::Str(view.aliases[i])
		                                         : duckdb_compat::Str(column_info->names[i]));
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
	auto entry_name = duckdb_compat::QualifiedLocalName(qname);
	EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, duckdb_compat::EntryName(entry_name));
	auto entry = Catalog::GetEntry(context, duckdb_compat::Ident(duckdb_compat::QualifiedCatalog(qname)),
	                               duckdb_compat::Ident(duckdb_compat::QualifiedSchema(qname)), lookup_info,
	                               OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		throw BinderException("%s: table or view '%s' does not exist", function_name, table_name);
	}

	vector<string> result;
	if (entry->type == CatalogType::TABLE_ENTRY) {
		auto &table = entry->Cast<TableCatalogEntry>();
		for (auto &col : table.GetColumns().Logical()) {
			result.push_back(duckdb_compat::Str(col.Name()));
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
// Argument resolution
//===--------------------------------------------------------------------===//

// Every parameter can be passed positionally or by name (`include_all := true`).
// Positional values are read first; a named value overrides the positional
// value of the same parameter.
struct DiffArguments {
	string source_table;
	string target_table;
	vector<string> primary_keys;
};

// The value of parameter `name` at positional `index`, or nullptr when the call
// passes it neither by name nor positionally.
static const Value *ResolveArgument(const TableFunctionBindInput &input, idx_t index, const string &name) {
	auto named = duckdb_compat::FindNamedParameter(input.named_parameters, name);
	if (named) {
		return named;
	}
	return index < input.inputs.size() ? &input.inputs[index] : nullptr;
}

// Resolve source_table, target_table and the primary key, which both diff
// functions share. The key has two names: primary_key (one column) and
// primary_keys (a list), both at positional index 2.
static DiffArguments ResolveDiffArguments(const string &function_name, const TableFunctionBindInput &input) {
	DiffArguments args;
	auto source_table = ResolveArgument(input, 0, "source_table");
	auto target_table = ResolveArgument(input, 1, "target_table");
	if (!source_table || source_table->IsNull()) {
		throw BinderException("%s: source_table is required and cannot be NULL", function_name);
	}
	if (!target_table || target_table->IsNull()) {
		throw BinderException("%s: target_table is required and cannot be NULL", function_name);
	}
	args.source_table = source_table->ToString();
	args.target_table = target_table->ToString();

	auto named_key = duckdb_compat::FindNamedParameter(input.named_parameters, "primary_key");
	auto named_keys = duckdb_compat::FindNamedParameter(input.named_parameters, "primary_keys");
	if (named_key && named_keys) {
		throw BinderException("%s: pass either primary_key or primary_keys, not both", function_name);
	}
	const Value *primary_key = named_key ? named_key : named_keys;
	if (!primary_key && input.inputs.size() > 2) {
		primary_key = &input.inputs[2];
	}
	if (!primary_key) {
		throw BinderException("%s: primary_key is required (a column name, or a list of names as primary_keys)",
		                      function_name);
	}
	args.primary_keys = ParseColumnList(*primary_key);
	if (args.primary_keys.empty()) {
		throw BinderException("%s: Primary key(s) cannot be empty", function_name);
	}
	return args;
}

//===--------------------------------------------------------------------===//
// JoinDiff Bind Replace - Generate SQL Query
//===--------------------------------------------------------------------===//

// ParseSubquery is a shared helper from anofox_sql_utils.hpp.

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
	// Parameters (positional index, or by name; a named value overrides):
	// 0: source_table (VARCHAR)
	// 1: target_table (VARCHAR)
	// 2: primary_key (VARCHAR) / primary_keys (LIST<VARCHAR>)
	// 3: compare_columns (LIST<VARCHAR>, optional)
	// 4: include_all (BOOLEAN, optional)
	auto args = ResolveDiffArguments("diff_joindiff", input);

	// Parse compare columns (optional, NULL means the default set)
	vector<string> compare_columns;
	auto compare_value = ResolveArgument(input, 3, "compare_columns");
	if (compare_value && !compare_value->IsNull()) {
		compare_columns = ParseColumnList(*compare_value);
	}

	// Parse include_all (optional, default false)
	bool include_all = false;
	auto include_all_value = ResolveArgument(input, 4, "include_all");
	if (include_all_value && !include_all_value->IsNull()) {
		include_all = include_all_value->GetValue<bool>();
	}

	// Validate both schemas and resolve the effective compare set
	compare_columns = ResolveCompareColumns(context, "diff_joindiff", args.source_table, args.target_table,
	                                        args.primary_keys, compare_columns);

	// Generate SQL query
	string sql =
	    GenerateJoinDiffSQL(args.source_table, args.target_table, args.primary_keys, compare_columns, include_all);

	// Parse and return as SubqueryRef
	auto subquery_ref = ParseSubquery(sql, context,
	                                  "Failed to parse generated diff query");
	return std::move(subquery_ref);
}

// HashDiff bind_replace - wraps JoinDiff with separate telemetry tracking
static unique_ptr<TableRef> HashDiffBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("diff_hashdiff");
	// HashDiff currently executes the same NULL-safe full outer join plan as
	// JoinDiff; it is tracked separately for telemetry purposes.

	// The hash/bisection algorithm behind bisection_threshold/bisection_factor
	// (positional 3 and 4) is not implemented; reject the parameters, passed
	// positionally or by name, instead of silently ignoring them
	if (ResolveArgument(input, 3, "bisection_threshold") || ResolveArgument(input, 4, "bisection_factor")) {
		throw BinderException("diff_hashdiff: the bisection_threshold/bisection_factor parameters are not implemented "
		                      "yet; remove them (diff_hashdiff currently computes the same full diff as diff_joindiff)");
	}

	auto args = ResolveDiffArguments("diff_hashdiff", input);

	// Validate both schemas and resolve the compare set (all shared non-key columns)
	auto compare_columns =
	    ResolveCompareColumns(context, "diff_hashdiff", args.source_table, args.target_table, args.primary_keys, {});

	// Generate SQL query using same logic as JoinDiff
	string sql = GenerateJoinDiffSQL(args.source_table, args.target_table, args.primary_keys, compare_columns, false);

	// Parse and return as SubqueryRef
	auto subquery_ref = ParseSubquery(sql, context,
	                                  "Failed to parse generated hashdiff query");
	return std::move(subquery_ref);
}

// Named parameters shared by both diff functions. Every overload declares all
// of them, so any parameter can be passed by name next to positional ones.
static void AddCommonDiffNamedParameters(TableFunction &fn) {
	duckdb_compat::AddNamedParameter(fn, "source_table", LogicalType(LogicalTypeId::VARCHAR));
	duckdb_compat::AddNamedParameter(fn, "target_table", LogicalType(LogicalTypeId::VARCHAR));
	duckdb_compat::AddNamedParameter(fn, "primary_key", LogicalType(LogicalTypeId::VARCHAR));
	duckdb_compat::AddNamedParameter(fn, "primary_keys", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)));
}

static void AddJoinDiffNamedParameters(TableFunction &fn) {
	AddCommonDiffNamedParameters(fn);
	duckdb_compat::AddNamedParameter(fn, "compare_columns", LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)));
	duckdb_compat::AddNamedParameter(fn, "include_all", LogicalType(LogicalTypeId::BOOLEAN));
}

static void AddHashDiffNamedParameters(TableFunction &fn) {
	AddCommonDiffNamedParameters(fn);
	// Declared so they reach the bind, which rejects them as not implemented
	duckdb_compat::AddNamedParameter(fn, "bisection_threshold", LogicalType(LogicalTypeId::BIGINT));
	duckdb_compat::AddNamedParameter(fn, "bisection_factor", LogicalType(LogicalTypeId::BIGINT));
}

// Add one overload per positional signature to `set`, all sharing the same
// bind_replace and named parameters.
static void AddDiffOverloads(TableFunctionSet &set, const char *name, decltype(TableFunction::bind_replace) bind_replace,
                             const vector<vector<LogicalType>> &signatures, void (*add_named_parameters)(TableFunction &)) {
	for (const auto &arguments : signatures) {
		TableFunction fn(name, arguments, nullptr, nullptr);
		fn.bind_replace = bind_replace;
		add_named_parameters(fn);
		set.AddFunction(std::move(fn));
	}
}

void RegisterDiffFunctions(ExtensionLoader &loader) {
	// The diff functions are implemented purely as bind_replace SQL rewrites:
	// they accept table NAMES (VARCHAR) and expand into a FULL OUTER JOIN query
	// that DuckDB optimizes and executes natively.

	const LogicalType varchar_type(LogicalTypeId::VARCHAR);
	const LogicalType list_type = LogicalType::LIST(varchar_type);
	const LogicalType boolean_type(LogicalTypeId::BOOLEAN);
	const LogicalType bigint_type(LogicalTypeId::BIGINT);

	// Register anofox_tab_diff_joindiff with all overloads (alias: diff_joindiff).
	// The overloads with fewer than three positional arguments exist for calls
	// that pass the remaining parameters by name.
	TableFunctionSet joindiff_set("anofox_tab_diff_joindiff");
	AddDiffOverloads(joindiff_set, "anofox_tab_diff_joindiff", DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, JoinDiffBindReplace),
	                 {{},
	                  {varchar_type},
	                  {varchar_type, varchar_type},
	                  // Single primary key (VARCHAR primary_key)
	                  {varchar_type, varchar_type, varchar_type},
	                  {varchar_type, varchar_type, varchar_type, list_type},
	                  {varchar_type, varchar_type, varchar_type, list_type, boolean_type},
	                  // Compound primary keys (LIST<VARCHAR> primary_keys)
	                  {varchar_type, varchar_type, list_type},
	                  {varchar_type, varchar_type, list_type, list_type},
	                  {varchar_type, varchar_type, list_type, list_type, boolean_type}},
	                 AddJoinDiffNamedParameters);

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
	// bisection_threshold/bisection_factor overloads and named parameters
	// remain registered so that callers get a clear "not implemented" binder
	// error instead of a generic overload-resolution failure or silently
	// ignored parameters. compare_columns/include_all are not declared, so
	// passing them by name is a binder error.
	//===--------------------------------------------------------------------===//

	// Register anofox_tab_diff_hashdiff with all overloads (alias: diff_hashdiff)
	TableFunctionSet hashdiff_set("anofox_tab_diff_hashdiff");
	AddDiffOverloads(hashdiff_set, "anofox_tab_diff_hashdiff", DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, HashDiffBindReplace),
	                 {{},
	                  {varchar_type},
	                  {varchar_type, varchar_type},
	                  {varchar_type, varchar_type, varchar_type},
	                  // bisection_threshold / bisection_factor (rejected as not implemented)
	                  {varchar_type, varchar_type, varchar_type, bigint_type},
	                  {varchar_type, varchar_type, varchar_type, bigint_type, bigint_type},
	                  {varchar_type, varchar_type, list_type}},
	                 AddHashDiffNamedParameters);

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

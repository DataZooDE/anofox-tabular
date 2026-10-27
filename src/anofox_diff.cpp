#include "anofox_diff.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb_compat.hpp"

#include <string>
#include <unordered_map>
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

// Resolve the columns (names and types) of a table or view through the catalog of the
// calling context so schema problems surface as clear binder errors instead of confusing
// errors from the generated SQL.
vector<ResolvedColumn> GetRelationColumns(ClientContext &context, const string &function_name,
                                          const string &table_name) {
	auto relation = LookupRelation(context, table_name);
	if (!relation.found) {
		throw BinderException("%s: table or view '%s' does not exist", function_name, table_name);
	}
	if (relation.columns.empty()) {
		throw BinderException("%s: unable to resolve the columns of view '%s'", function_name, table_name);
	}
	return relation.columns;
}

vector<string> ColumnNames(const vector<ResolvedColumn> &columns) {
	vector<string> names;
	for (auto &column : columns) {
		names.push_back(column.name);
	}
	return names;
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

// The type of `name` (case-insensitive) in `columns`; the column was validated before
const LogicalType &ColumnType(const vector<ResolvedColumn> &columns, const string &name) {
	for (auto &column : columns) {
		if (StringUtil::CIEquals(column.name, name)) {
			return column.type;
		}
	}
	throw InternalException("diff: column '%s' was not resolved", name);
}

// How a key or compare column whose type differs between source and target is compared.
// TEXT: on the text form (CAST AS VARCHAR) when the types are not both numeric: an implicit
// cast would abort the diff on a value like 'n/a' (INTEGER -> VARCHAR) or hide a change like
// 7 -> '007'. Numeric type changes (INTEGER -> BIGINT, DECIMAL -> DOUBLE) compare by value
// (RAW): 5000.00 equals 5000.0. A TIMESTAMP WITH TIME ZONE never depends on the session
// TimeZone: against a plain TIMESTAMP both sides compare as UTC TIMESTAMPs (UTC_TIMESTAMP, the
// TIMESTAMP is read as UTC), and in the text form it is rendered as its UTC instant with a
// '+00' suffix, the text DuckDB prints for it under TimeZone UTC.
enum class ComparisonForm { RAW, TEXT, UTC_TIMESTAMP };

bool IsPlainTimestamp(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return true;
	default:
		return false;
	}
}

ComparisonForm ChooseComparisonForm(const LogicalType &source_type, const LogicalType &target_type) {
	if (source_type == target_type || (source_type.IsNumeric() && target_type.IsNumeric())) {
		return ComparisonForm::RAW;
	}
	bool source_tz = source_type.id() == LogicalTypeId::TIMESTAMP_TZ;
	bool target_tz = target_type.id() == LogicalTypeId::TIMESTAMP_TZ;
	if ((source_tz && IsPlainTimestamp(target_type)) || (target_tz && IsPlainTimestamp(source_type))) {
		return ComparisonForm::UTC_TIMESTAMP;
	}
	return ComparisonForm::TEXT;
}

// A retyped key or compare column: how it is compared and its type on each side.
struct RetypedColumn {
	ComparisonForm form;
	LogicalType source_type;
	LogicalType target_type;
};

// The resolved column plan of a diff: the effective compare set and the retyped key and
// compare columns (by lower-case name).
struct DiffColumnPlan {
	vector<string> compare_columns;
	std::unordered_map<string, RetypedColumn> retyped_columns;
};

// Note the columns of `names` whose type differs between the two tables, with a trace warning.
void PlanTypeChanges(const string &function_name, const vector<string> &names, const string &role,
                     const vector<ResolvedColumn> &source_columns, const vector<ResolvedColumn> &target_columns,
                     DiffColumnPlan &plan) {
	for (const auto &name : names) {
		auto &source_type = ColumnType(source_columns, name);
		auto &target_type = ColumnType(target_columns, name);
		if (source_type == target_type) {
			continue;
		}
		auto form = ChooseComparisonForm(source_type, target_type);
		plan.retyped_columns[StringUtil::Lower(name)] = {form, source_type, target_type};
		const char *how = form == ComparisonForm::TEXT            ? "; compared as VARCHAR"
		                  : form == ComparisonForm::UTC_TIMESTAMP ? "; compared as UTC TIMESTAMP"
		                                                          : "; compared by numeric value";
		AnofoxTrace(AnofoxLogLevel::Warn, function_name + ": " + role + " column '" + name + "' changed type " +
		                                      source_type.ToString() + " -> " + target_type.ToString() + how);
	}
}

// Validate the requested key/compare columns against both table schemas and
// resolve the effective compare set (defaults to the shared non-key columns).
DiffColumnPlan ResolveCompareColumns(ClientContext &context, const string &function_name, const string &source_table,
                                     const string &target_table, const vector<string> &primary_keys,
                                     const vector<string> &compare_columns) {
	auto source_columns = GetRelationColumns(context, function_name, source_table);
	auto target_columns = GetRelationColumns(context, function_name, target_table);
	auto source_set = MakeColumnSet(ColumnNames(source_columns));
	auto target_set = MakeColumnSet(ColumnNames(target_columns));

	ValidatePrimaryKeys(function_name, primary_keys, source_set, "source", source_table);
	ValidatePrimaryKeys(function_name, primary_keys, target_set, "target", target_table);

	DiffColumnPlan plan;
	if (!compare_columns.empty()) {
		ValidateCompareColumns(function_name, compare_columns, source_set, "source", source_table);
		ValidateCompareColumns(function_name, compare_columns, target_set, "target", target_table);
		plan.compare_columns = compare_columns;
	} else {
		// Default: compare every non-key column the two tables share, in target
		// column order (the output projects the target side)
		auto pk_set = MakeColumnSet(primary_keys);
		vector<string> target_only;
		for (const auto &column : target_columns) {
			auto lower = StringUtil::Lower(column.name);
			if (pk_set.find(lower) != pk_set.end()) {
				continue;
			}
			if (source_set.find(lower) != source_set.end()) {
				plan.compare_columns.push_back(column.name);
			} else {
				target_only.push_back(column.name);
			}
		}
		vector<string> source_only;
		for (const auto &column : source_columns) {
			if (target_set.find(StringUtil::Lower(column.name)) == target_set.end()) {
				source_only.push_back(column.name);
			}
		}
		// Schema drift is not a row change: columns on one side only are not compared
		if (!source_only.empty() || !target_only.empty()) {
			AnofoxTrace(AnofoxLogLevel::Warn,
			            function_name + ": columns present on one side only are not compared (source only: [" +
			                StringUtil::Join(source_only, ", ") + "], target only: [" +
			                StringUtil::Join(target_only, ", ") + "])");
		}
	}
	PlanTypeChanges(function_name, primary_keys, "key", source_columns, target_columns, plan);
	PlanTypeChanges(function_name, plan.compare_columns, "compare", source_columns, target_columns, plan);
	return plan;
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
	args.source_table = RequireNonNullArgument(source_table, function_name, "source_table");
	args.target_table = RequireNonNullArgument(target_table, function_name, "target_table");

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
	RequireNonNullArgument(primary_key, function_name, named_keys ? "primary_keys" : "primary_key");
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

// A diff needs a unique primary key on both sides: with a repeated key every
// source row joins every target row of that key and the diff reports false
// changes. The guard is a filter on each side of the join whose uncorrelated
// scalar subquery looks for one repeated key per table (GROUP BY groups NULLs
// together, matching the NULL-safe join) and raises an error() naming it;
// without a repeated key it is NULL and the filter keeps every row. Both
// tables are checked in one subquery that prefers the source table and the
// smallest key, and error() runs only on that single row, so the message is
// the same on every run (two separate guards raced and named either table).
// It runs inside the diff query itself, so it sees everything the diff sees
// (TEMP tables, views, uncommitted rows); the cost is one GROUP BY over the
// key per table.
static string BuildRepeatedKeyMessages(const string &function_name, const string &table_name, const string &side,
                                       int side_order, const vector<string> &primary_keys) {
	string key_cols;
	string key_desc = "''";
	for (size_t i = 0; i < primary_keys.size(); i++) {
		auto col = QuoteSqlIdentifier(primary_keys[i]);
		key_cols += (i > 0 ? ", " : "") + col;
		key_desc += " || '" + string(i > 0 ? ", " : "") + EscapeSqlStringLiteral(primary_keys[i]) +
		            " = ' || COALESCE(CAST(" + col + " AS VARCHAR), 'NULL')";
	}
	string prefix = function_name + ": primary key is not unique in " + side + " table '" + table_name + "' (";
	return "(SELECT " + std::to_string(side_order) + " AS side, '" + EscapeSqlStringLiteral(prefix) + "' || " +
	       key_desc + " || ' appears ' || CAST(COUNT(*) AS VARCHAR) || ' times)' AS msg FROM " +
	       BuildQueryTableRef(table_name) + " GROUP BY " + key_cols + " HAVING COUNT(*) > 1 ORDER BY " + key_cols +
	       " LIMIT 1)";
}

static string BuildUniqueKeyGuard(const string &function_name, const string &source_table,
                                  const string &target_table, const vector<string> &primary_keys) {
	return "(SELECT error(msg) FROM (SELECT msg FROM (" +
	       BuildRepeatedKeyMessages(function_name, source_table, "source", 0, primary_keys) + " UNION ALL " +
	       BuildRepeatedKeyMessages(function_name, target_table, "target", 1, primary_keys) +
	       ") ORDER BY side LIMIT 1)) IS NULL";
}

// `side`.`column` ("s" source, "t" target) in the form it is compared in (see ComparisonForm)
static string SideColumnSQL(const string &side, const string &column, const DiffColumnPlan &plan) {
	string ref = side + "." + QuoteSqlIdentifier(column);
	auto entry = plan.retyped_columns.find(StringUtil::Lower(column));
	if (entry == plan.retyped_columns.end() || entry->second.form == ComparisonForm::RAW) {
		return ref;
	}
	auto &type = side == "s" ? entry->second.source_type : entry->second.target_type;
	if (entry->second.form == ComparisonForm::UTC_TIMESTAMP) {
		return UtcTimestampSQL(ref, type);
	}
	if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		return "(CAST(" + UtcTimestampSQL(ref, type) + " AS VARCHAR) || '+00')";
	}
	return "CAST(" + ref + " AS VARCHAR)";
}

// The changed_columns output column: for a 'changed' row the compare columns that differ, in
// compare order; [] for an 'unchanged' row; NULL for an 'added' or 'removed' row, which has no
// counterpart to compare.
static string ChangedColumnsSQL(const DiffColumnPlan &plan) {
	string list = "[]::VARCHAR[]";
	if (!plan.compare_columns.empty()) {
		list = "list_filter([";
		for (size_t i = 0; i < plan.compare_columns.size(); i++) {
			auto &column = plan.compare_columns[i];
			list += (i > 0 ? ", " : "") + string("CASE WHEN ") + SideColumnSQL("s", column, plan) +
			        " IS DISTINCT FROM " + SideColumnSQL("t", column, plan) + " THEN '" +
			        EscapeSqlStringLiteral(column) + "' END";
		}
		list += "]::VARCHAR[], lambda x: x IS NOT NULL)";
	}
	return "CASE WHEN s." + string(SOURCE_PRESENT_MARKER) + " IS NULL OR t." + string(TARGET_PRESENT_MARKER) +
	       " IS NULL THEN NULL::VARCHAR[] ELSE " + list + " END AS changed_columns";
}

static string GenerateJoinDiffSQL(const string &function_name, const string &source_table,
                                  const string &target_table, const vector<string> &primary_keys,
                                  const DiffColumnPlan &plan, bool include_all) {
	const auto &compare_columns = plan.compare_columns;
	// NULL-safe primary key join condition: rows whose key components are
	// NULL still match their counterpart on the other side
	string pk_join_condition;
	for (size_t i = 0; i < primary_keys.size(); i++) {
		if (i > 0) {
			pk_join_condition += " AND ";
		}
		pk_join_condition += SideColumnSQL("s", primary_keys[i], plan) + " IS NOT DISTINCT FROM " +
		                     SideColumnSQL("t", primary_keys[i], plan);
	}

	// Build the COALESCE expressions for primary keys
	string pk_select;
	for (size_t i = 0; i < primary_keys.size(); i++) {
		if (i > 0) {
			pk_select += ", ";
		}
		pk_select += "COALESCE(" + SideColumnSQL("s", primary_keys[i], plan) + ", " +
		             SideColumnSQL("t", primary_keys[i], plan) + ") AS " + QuoteSqlIdentifier(primary_keys[i]);
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
			sql += SideColumnSQL("s", compare_columns[i], plan) + " IS DISTINCT FROM " +
			       SideColumnSQL("t", compare_columns[i], plan);
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
	sql += "), " + ChangedColumnsSQL(plan);

	// Wrap each side with a constant presence marker; table references go
	// through query_table() so hostile and schema-qualified names are handled
	// by the shared quoting helpers
	sql += " FROM (SELECT true AS " + string(SOURCE_PRESENT_MARKER) + ", * FROM " + BuildQueryTableRef(source_table) +
	       " WHERE " + BuildUniqueKeyGuard(function_name, source_table, target_table, primary_keys) + ") s ";
	sql += "FULL OUTER JOIN (SELECT true AS " + string(TARGET_PRESENT_MARKER) + ", * FROM " +
	       BuildQueryTableRef(target_table) + " WHERE " +
	       BuildUniqueKeyGuard(function_name, source_table, target_table, primary_keys) + ") t ";
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
	auto plan = ResolveCompareColumns(context, "diff_joindiff", args.source_table, args.target_table,
	                                  args.primary_keys, compare_columns);

	// Generate SQL query
	string sql = GenerateJoinDiffSQL("diff_joindiff", args.source_table, args.target_table, args.primary_keys,
	                                 plan, include_all);

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
	auto plan =
	    ResolveCompareColumns(context, "diff_hashdiff", args.source_table, args.target_table, args.primary_keys, {});

	// Generate SQL query using same logic as JoinDiff
	string sql = GenerateJoinDiffSQL("diff_hashdiff", args.source_table, args.target_table, args.primary_keys,
	                                 plan, false);

	// Parse and return as SubqueryRef
	auto subquery_ref = ParseSubquery(sql, context,
	                                  "Failed to parse generated hashdiff query");
	return std::move(subquery_ref);
}

//===--------------------------------------------------------------------===//
// diff_schema - column-level schema drift between two tables
//===--------------------------------------------------------------------===//

// One output row of diff_schema; an empty type means the column is absent on that side.
struct SchemaDriftRow {
	string column_name;
	string source_type;
	string target_type;
	const char *drift;
};

// Compare the columns of both relations (case-insensitively, like the row diffs): the source
// columns in source order, then the target-only columns in target order.
static vector<SchemaDriftRow> ComputeSchemaDrift(const vector<ResolvedColumn> &source_columns,
                                                 const vector<ResolvedColumn> &target_columns) {
	vector<SchemaDriftRow> rows;
	for (auto &source : source_columns) {
		auto source_type = source.type.ToString();
		const ResolvedColumn *target = nullptr;
		for (auto &candidate : target_columns) {
			if (StringUtil::CIEquals(candidate.name, source.name)) {
				target = &candidate;
				break;
			}
		}
		if (!target) {
			rows.push_back({source.name, source_type, "", "removed"});
			continue;
		}
		auto target_type = target->type.ToString();
		rows.push_back({source.name, source_type, target_type, source_type == target_type ? "same" : "retyped"});
	}
	auto source_set = MakeColumnSet(ColumnNames(source_columns));
	for (auto &target : target_columns) {
		if (source_set.find(StringUtil::Lower(target.name)) == source_set.end()) {
			rows.push_back({target.name, "", target.type.ToString(), "added"});
		}
	}
	return rows;
}

static string SqlStringOrNull(const string &value) {
	return value.empty() ? "NULL" : "'" + EscapeSqlStringLiteral(value) + "'";
}

static unique_ptr<TableRef> SchemaDiffBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("diff_schema");
	const string function_name = "diff_schema";
	auto source_table = RequireNonNullArgument(ResolveArgument(input, 0, "source_table"), function_name, "source_table");
	auto target_table = RequireNonNullArgument(ResolveArgument(input, 1, "target_table"), function_name, "target_table");

	// The catalog lookup runs through the caller's context, so TEMP tables and views resolve
	auto source_columns = GetRelationColumns(context, function_name, source_table);
	auto target_columns = GetRelationColumns(context, function_name, target_table);
	auto rows = ComputeSchemaDrift(source_columns, target_columns);

	idx_t drift_count = 0;
	string values;
	for (auto &row : rows) {
		values += values.empty() ? "" : ", ";
		values += "(" + SqlStringOrNull(row.column_name) + ", " + SqlStringOrNull(row.source_type) + ", " +
		          SqlStringOrNull(row.target_type) + ", '" + row.drift + "')";
		drift_count += string(row.drift) != "same";
	}
	AnofoxTrace(AnofoxLogLevel::Debug, function_name + ": '" + source_table + "' -> '" + target_table + "': " +
	                                       std::to_string(rows.size()) + " columns, " + std::to_string(drift_count) +
	                                       " with drift");

	// Every relation has at least one column, so VALUES is never empty; the casts fix the
	// column types when one side is NULL in every row
	string sql = "SELECT CAST(column_name AS VARCHAR) AS column_name, CAST(source_type AS VARCHAR) AS source_type, "
	             "CAST(target_type AS VARCHAR) AS target_type, CAST(drift AS VARCHAR) AS drift FROM (VALUES " +
	             values + ") AS drift_rows(column_name, source_type, target_type, drift)";
	return ParseSubquery(sql, context, "Failed to parse generated diff_schema query");
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
		desc.parameter_names = {"source_table", "target_table", "primary_key", "compare_columns", "include_all"};
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
		desc.parameter_names = {"source_table", "target_table", "primary_key", "bisection_threshold", "bisection_factor"};
		desc.examples = {"SELECT * FROM diff_hashdiff('products_v1', 'products_v2', 'product_id');",
		                 "SELECT * FROM diff_hashdiff('products_v1', 'products_v2', ['product_id', 'sku']);"};
		desc.categories = {"diff", "data-quality"};

		RegisterTableFunctionSetWithAlias(loader, hashdiff_set, "diff_hashdiff", {std::move(desc)});
	}

	// Register anofox_tab_diff_schema (alias: diff_schema). The overloads with fewer than two
	// positional arguments exist for calls that pass the tables by name.
	TableFunctionSet schema_set("anofox_tab_diff_schema");
	AddDiffOverloads(schema_set, "anofox_tab_diff_schema", DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, SchemaDiffBindReplace),
	                 {{}, {varchar_type}, {varchar_type, varchar_type}}, [](TableFunction &fn) {
		                 duckdb_compat::AddNamedParameter(fn, "source_table", LogicalType(LogicalTypeId::VARCHAR));
		                 duckdb_compat::AddNamedParameter(fn, "target_table", LogicalType(LogicalTypeId::VARCHAR));
	                 });

	{
		FunctionDescription desc;
		desc.description = "Lists the schema drift between two tables: one row per column of either table with its "
		                   "source and target type and the drift (same, added, removed, retyped). Columns match "
		                   "case-insensitively.";
		desc.parameter_names = {"source_table", "target_table"};
		desc.examples = {"SELECT * FROM diff_schema('orders_v1', 'orders_v2');",
		                 "SELECT * FROM diff_schema('orders_v1', 'orders_v2') WHERE drift <> 'same';"};
		desc.categories = {"diff", "data-quality"};

		RegisterTableFunctionSetWithAlias(loader, schema_set, "diff_schema", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

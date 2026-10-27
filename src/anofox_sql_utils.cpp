#include "anofox_sql_utils.hpp"

#include "anofox_trace.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb_compat.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace duckdb {
namespace anofox {

string EscapeSqlStringLiteral(const string &value) {
	string escaped;
	escaped.reserve(value.size());
	for (char c : value) {
		if (c == '\'') {
			escaped += "''";
		} else {
			escaped += c;
		}
	}
	return escaped;
}

string QuoteSqlIdentifier(const string &identifier) {
	string escaped;
	escaped.reserve(identifier.size() + 2);
	for (char c : identifier) {
		if (c == '"') {
			escaped += "\"\"";
		} else {
			escaped += c;
		}
	}
	return "\"" + escaped + "\"";
}

string BuildQueryTableRef(const string &table_name) {
	return "query_table('" + EscapeSqlStringLiteral(table_name) + "')";
}

namespace {
// 2.0: Parser(ClientContext &)
template <class P = Parser, class CONTEXT>
auto ParseWith(CONTEXT &context, const string &sql, int) -> decltype(P(context), vector<unique_ptr<SQLStatement>>()) {
	P parser(context);
	parser.ParseQuery(sql);
	return std::move(parser.statements);
}
// 1.5.6: Parser(ParserOptions), from the context's settings
template <class P = Parser, class CONTEXT>
vector<unique_ptr<SQLStatement>> ParseWith(CONTEXT &context, const string &sql, long) {
	P parser(context.GetParserOptions());
	parser.ParseQuery(sql);
	return std::move(parser.statements);
}
// 2.0: Connection::Submit
template <class CONNECTION>
auto StreamWith(CONNECTION &con, const string &sql, int) -> decltype(con.Submit(sql)) {
	return con.Submit(sql);
}
// before 2.0: Connection::SendQuery
template <class CONNECTION>
auto StreamWith(CONNECTION &con, const string &sql, long) -> decltype(con.SendQuery(sql)) {
	return con.SendQuery(sql);
}
} // namespace

unique_ptr<QueryResult> StreamQuery(Connection &con, const string &sql) {
	return StreamWith(con, sql, 0);
}

vector<unique_ptr<SQLStatement>> ParseWithContextSettings(ClientContext &context, const string &sql) {
	return ParseWith(context, sql, 0);
}

unique_ptr<SubqueryRef> ParseSubquery(const string &query, ClientContext &context, const string &err_msg) {
	auto statements = ParseWithContextSettings(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto select_stmt = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(statements[0]));
	return make_uniq<SubqueryRef>(std::move(select_stmt));
}

void ValidateFiniteDouble(const Value &value, const string &function_name, const string &parameter_name) {
	if (value.IsNull()) {
		return;
	}
	double val = value.GetValue<double>();
	if (!std::isfinite(val)) {
		throw BinderException(function_name + ": " + parameter_name + " must be a finite number");
	}
}

string RequireNonNullArgument(const Value *value, const string &function_name, const string &argument_name) {
	if (!value || value->IsNull()) {
		throw BinderException("%s: %s must not be NULL", function_name, argument_name);
	}
	return value->ToString();
}

string RequireNonNullArgument(const TableFunctionBindInput &input, idx_t index, const string &function_name,
                              const string &argument_name) {
	return RequireNonNullArgument(index < input.inputs.size() ? &input.inputs[index] : nullptr, function_name,
	                              argument_name);
}

string RequireNameArgument(const TableFunctionBindInput &input, idx_t index, const string &function_name,
                           const string &argument_name) {
	auto name = RequireNonNullArgument(input, index, function_name, argument_name);
	if (name.find_first_not_of(" \t\n\r") == string::npos) {
		throw BinderException("%s: %s must not be empty", function_name, argument_name);
	}
	return name;
}

void ValidateThresholdOrder(const Value &lower, const Value &upper, const string &function_name,
                            const string &lower_name, const string &upper_name) {
	if (lower.IsNull() || upper.IsNull()) {
		return;
	}
	if (lower.GetValue<double>() > upper.GetValue<double>()) {
		throw BinderException("%s: %s (%s) must not be greater than %s (%s)", function_name, lower_name,
		                      lower.ToString(), upper_name, upper.ToString());
	}
}

void ValidateNonNegative(const Value &value, const string &function_name, const string &parameter_name) {
	if (value.IsNull()) {
		return;
	}
	if (value.GetValue<double>() < 0) {
		throw BinderException("%s: %s must not be negative (got %s)", function_name, parameter_name,
		                      value.ToString());
	}
}

void ValidateRate(const Value &value, const string &function_name, const string &parameter_name) {
	ValidateFiniteDouble(value, function_name, parameter_name);
	if (value.IsNull()) {
		return;
	}
	double rate = value.GetValue<double>();
	if (rate < 0 || rate > 1) {
		throw BinderException("%s: %s must be between 0 and 1 (got %s)", function_name, parameter_name,
		                      value.ToString());
	}
}

string RoundedNumberSQL(const string &double_expr) {
	return "CAST(CASE WHEN abs(" + double_expr + ") < 0.001 THEN " + double_expr + " ELSE round(" + double_expr +
	       ", 3) END AS VARCHAR)";
}

string LowerOption(const string &value) {
	return StringUtil::Lower(value);
}

vector<string> ParseCommaSeparatedColumns(const string &columns_str) {
	vector<string> column_names;
	size_t start = 0;
	for (size_t i = 0; i <= columns_str.length(); ++i) {
		if (i == columns_str.length() || columns_str[i] == ',') {
			string col = columns_str.substr(start, i - start);
			size_t col_start = col.find_first_not_of(" \t\n\r");
			size_t col_end = col.find_last_not_of(" \t\n\r");
			if (col_start != string::npos) {
				col = col.substr(col_start, col_end - col_start + 1);
				if (col.length() >= 2 && ((col.front() == '"' && col.back() == '"') ||
				                          (col.front() == '\'' && col.back() == '\''))) {
					col = col.substr(1, col.length() - 2);
				}
				column_names.push_back(col);
			}
			start = i + 1;
		}
	}
	return column_names;
}

vector<idx_t> MatchColumnNames(const string &function_name, const string &table_name,
                               const vector<string> &available_names, const vector<string> &requested) {
	vector<idx_t> indexes;
	indexes.reserve(requested.size());
	for (const auto &name : requested) {
		auto match = std::find_if(available_names.begin(), available_names.end(),
		                          [&](const string &candidate) { return StringUtil::CIEquals(candidate, name); });
		if (match == available_names.end()) {
			throw BinderException("%s: column '%s' not found in table '%s' (available columns: %s)", function_name,
			                      name, table_name, StringUtil::Join(available_names, ", "));
		}
		indexes.push_back(static_cast<idx_t>(match - available_names.begin()));
	}
	return indexes;
}

vector<ResolvedColumn> ResolveColumns(ClientContext &context, const string &function_name, const string &table_name,
                                      const vector<string> &requested) {
	RejectTempTable(context, function_name, table_name);
	RejectUncommittedChanges(context, function_name);
	Connection con(*context.db);
	auto result = con.Query("SELECT * FROM " + BuildQueryTableRef(table_name) + " LIMIT 0");
	if (result->HasError()) {
		throw BinderException("%s: cannot read table '%s': %s", function_name, table_name, result->GetError());
	}
	auto result_names = duckdb_compat::ResultNames(*result);
	vector<string> available_names(result_names.begin(), result_names.end());
	auto &result_types = duckdb_compat::ResultTypes(*result);

	vector<ResolvedColumn> columns;
	for (auto idx : MatchColumnNames(function_name, table_name, available_names, requested)) {
		columns.push_back({available_names[idx], result_types[idx]});
	}
	AnofoxTrace(AnofoxLogLevel::Debug, function_name + ": resolved " + std::to_string(columns.size()) +
	                                       " column(s) of '" + table_name + "'");
	return columns;
}

namespace {
// DuckDB v1.5 and 2.0 resolve view columns via BindView()/GetColumnInfo(); the fallback
// (bound names/types stored on the entry) is only instantiated when GetColumnInfo() is
// missing. The available API is detected at compile time.
template <typename T, typename = void>
struct ViewHasGetColumnInfo : std::false_type {};
template <typename T>
struct ViewHasGetColumnInfo<T, std::void_t<decltype(std::declval<T &>().GetColumnInfo())>> : std::true_type {};

template <typename VIEW>
typename std::enable_if<ViewHasGetColumnInfo<VIEW>::value, vector<ResolvedColumn>>::type
ResolveViewColumns(ClientContext &context, VIEW &view) {
	view.BindView(context);
	auto column_info = view.GetColumnInfo();
	if (!column_info) {
		return {};
	}
	vector<ResolvedColumn> result;
	for (idx_t i = 0; i < column_info->names.size(); i++) {
		string name = i < view.aliases.size() ? duckdb_compat::Str(view.aliases[i])
		                                      : duckdb_compat::Str(column_info->names[i]);
		result.push_back({name, i < column_info->types.size() ? column_info->types[i] : LogicalType()});
	}
	return result;
}

template <typename VIEW>
typename std::enable_if<!ViewHasGetColumnInfo<VIEW>::value, vector<ResolvedColumn>>::type
ResolveViewColumns(ClientContext &, VIEW &view) {
	vector<ResolvedColumn> result;
	for (idx_t i = 0; i < view.names.size(); i++) {
		result.push_back({i < view.aliases.size() ? view.aliases[i] : view.names[i],
		                  i < view.types.size() ? view.types[i] : LogicalType()});
	}
	return result;
}
} // namespace

namespace {
// The table or view `name` in `catalog`.`schema` (either may be empty: the search path
// decides), or nullptr. Never throws.
optional_ptr<CatalogEntry> TryGetRelationEntry(ClientContext &context, const string &catalog, const string &schema,
                                               const string &name, const string &table_name) {
	try {
		EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, duckdb_compat::EntryName(name));
		return Catalog::GetEntry(context, duckdb_compat::Ident(catalog), duckdb_compat::Ident(schema), lookup_info,
		                         OnEntryNotFound::RETURN_NULL);
	} catch (const std::exception &ex) {
		// e.g. an unknown catalog/schema or a file path: not a catalog relation
		AnofoxTrace(AnofoxLogLevel::Debug, "catalog lookup of '" + table_name + "' as '" + catalog + "'.'" + schema +
		                                       "' failed: " + ex.what());
		return nullptr;
	}
}
} // namespace

optional_ptr<CatalogEntry> LookupRelationEntry(ClientContext &context, const string &table_name) {
	string catalog;
	string schema;
	string name;
	try {
		auto qname = QualifiedName::Parse(table_name);
		catalog = duckdb_compat::QualifiedCatalog(qname);
		schema = duckdb_compat::QualifiedSchema(qname);
		name = duckdb_compat::QualifiedLocalName(qname);
	} catch (const std::exception &ex) {
		AnofoxTrace(AnofoxLogLevel::Debug, "cannot parse relation name '" + table_name + "': " + ex.what());
		return nullptr;
	}
	auto entry = TryGetRelationEntry(context, catalog, schema, name, table_name);
	if (entry || !catalog.empty() || schema.empty()) {
		return entry;
	}
	// A two-part name x.t is schema.table in the search path first, like DuckDB's binder,
	// else table t in the default schema of the attached database x (ATTACH ... AS x)
	entry = TryGetRelationEntry(context, schema, string(), name, table_name);
	if (entry) {
		AnofoxTrace(AnofoxLogLevel::Debug, "relation '" + table_name + "' resolved as table '" + name +
		                                       "' of attached database '" + schema + "'");
	}
	return entry;
}

RelationInfo LookupRelation(ClientContext &context, const string &table_name) {
	RelationInfo info;
	try {
		auto entry = LookupRelationEntry(context, table_name);
		if (!entry) {
			return info;
		}
		if (entry->type == CatalogType::TABLE_ENTRY) {
			auto &table = entry->Cast<TableCatalogEntry>();
			for (auto &col : table.GetColumns().Logical()) {
				info.columns.push_back({duckdb_compat::Str(col.Name()), col.Type()});
			}
		} else if (entry->type == CatalogType::VIEW_ENTRY) {
			info.is_view = true;
			info.columns = ResolveViewColumns(context, entry->Cast<ViewCatalogEntry>());
		} else {
			return info;
		}
		info.found = true;
		info.is_temp = entry->ParentCatalog().IsTemporaryCatalog();
	} catch (const std::exception &ex) {
		// e.g. an unknown catalog/schema or a file path: not a catalog relation
		AnofoxTrace(AnofoxLogLevel::Debug, "catalog lookup of '" + table_name + "' failed: " + ex.what());
		info = RelationInfo();
	}
	return info;
}

const ResolvedColumn *FindColumn(const RelationInfo &relation, const string &column_name) {
	for (auto &column : relation.columns) {
		if (StringUtil::CIEquals(column.name, column_name)) {
			return &column;
		}
	}
	return nullptr;
}

void RejectTempTable(ClientContext &context, const string &function_name, const string &table_name) {
	auto relation = LookupRelation(context, table_name);
	if (relation.found && relation.is_temp) {
		AnofoxTrace(AnofoxLogLevel::Debug,
		            function_name + ": '" + table_name + "' is a TEMP table, which a sibling connection cannot read");
		throw BinderException("TEMP tables are not supported by %s; use a regular table (CREATE TABLE ... AS "
		                      "SELECT * FROM %s)",
		                      function_name, table_name);
	}
}

void RequireNumericColumn(ClientContext &context, const string &function_name, const string &table_name,
                          const string &column_name) {
	auto relation = LookupRelation(context, table_name);
	if (!relation.found || relation.columns.empty()) {
		return;
	}
	vector<string> available_names;
	for (auto &column : relation.columns) {
		available_names.push_back(column.name);
	}
	auto idx = MatchColumnNames(function_name, table_name, available_names, {column_name})[0];
	auto &column = relation.columns[idx];
	if (column.type.id() == LogicalTypeId::INVALID || column.type.id() == LogicalTypeId::UNKNOWN) {
		return;
	}
	if (!column.type.IsNumeric() && column.type.id() != LogicalTypeId::BOOLEAN) {
		throw BinderException("%s: column '%s' must be numeric, got %s", function_name, column.name,
		                      column.type.ToString());
	}
}

vector<ResolvedColumn> RequireRelationColumns(ClientContext &context, const string &function_name,
                                              const string &table_name, const vector<string> &columns) {
	if (table_name.find_first_not_of(" \t\n\r") == string::npos) {
		throw BinderException("%s: table_name must not be empty", function_name);
	}
	vector<string> names;
	vector<LogicalType> types;
	try {
		auto statements =
		    ParseWithContextSettings(context, "SELECT * FROM " + BuildQueryTableRef(table_name) + " LIMIT 0");
		if (statements.size() != 1) {
			throw BinderException("not a single relation");
		}
		auto binder = Binder::CreateBinder(context);
		auto bound = binder->Bind(*statements[0]);
		names = duckdb_compat::Strs(bound.names);
		types = bound.types;
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::FATAL ||
		    error.Type() == ExceptionType::INTERNAL) {
			throw;
		}
		auto cause = error.RawMessage();
		AnofoxTrace(AnofoxLogLevel::Debug, function_name + ": cannot bind '" + table_name + "': " + cause);
		// A catalog error for a relation the catalog does not know is a missing table; a view
		// that exists but no longer binds (e.g. its base table was dropped) is reported as is
		if (error.Type() == ExceptionType::CATALOG && !LookupRelation(context, table_name).found) {
			throw BinderException("%s: table '%s' does not exist", function_name, table_name);
		}
		auto newline = cause.find('\n');
		if (newline != string::npos) {
			cause = cause.substr(0, newline);
		}
		throw BinderException("%s: cannot read table '%s': %s", function_name, table_name, cause);
	}
	vector<ResolvedColumn> result;
	for (auto idx : MatchColumnNames(function_name, table_name, names, columns)) {
		result.push_back({names[idx], types[idx]});
	}
	return result;
}

LogicalType LookupColumnType(ClientContext &context, const string &table_name, const string &column_name) {
	auto relation = LookupRelation(context, table_name);
	auto column = FindColumn(relation, column_name);
	return column ? column->type : LogicalType(LogicalTypeId::INVALID);
}

// Whether the text `column_sql` ends in a UTC offset ('Z', '+02', '+02:00', '-0530') or a
// time zone name after a time of day. A bare date ('2026-10-26') ends in digits after '-' too,
// hence the time of day in front.
static string TextHasZoneSQL(const string &column_sql) {
	return "regexp_matches(" + column_sql +
	       ", '\\d:\\d{2}(:\\d{2}(\\.\\d*)?)?\\s*([Zz]|[+-]\\d{1,2}(:?\\d{2})?|[A-Za-z][A-Za-z0-9_/+-]*)\\s*$')";
}

string UtcTimestampSQL(const string &column_sql, const LogicalType &type, bool try_parse) {
	if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		// epoch_us of the instant is time zone independent; make_timestamp(BIGINT) reads it as UTC
		return "make_timestamp(epoch_us(" + column_sql + "))";
	}
	const string cast = try_parse ? "TRY_CAST(" : "CAST(";
	if (type.id() == LogicalTypeId::VARCHAR) {
		// Text with an offset is the instant it names (a CAST AS TIMESTAMP drops the offset);
		// text without one is UTC (a cast AS TIMESTAMPTZ would read it in the session time zone)
		return "CASE WHEN " + TextHasZoneSQL(column_sql) + " THEN make_timestamp(epoch_us(" + cast + column_sql +
		       " AS TIMESTAMPTZ))) ELSE " + cast + column_sql + " AS TIMESTAMP) END";
	}
	return cast + column_sql + " AS TIMESTAMP)";
}

string UtcDateSQL(const string &column_sql, const LogicalType &type) {
	if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		return "CAST(" + UtcTimestampSQL(column_sql, type) + " AS DATE)";
	}
	if (type.id() == LogicalTypeId::VARCHAR) {
		// a text value that is no date ('n/a') is NULL instead of a conversion error; a
		// timestamp with an offset counts on its UTC date
		return "CASE WHEN " + TextHasZoneSQL(column_sql) + " THEN CAST(" + UtcTimestampSQL(column_sql, type, true) +
		       " AS DATE) ELSE TRY_CAST(" + column_sql + " AS DATE) END";
	}
	return "CAST(" + column_sql + " AS DATE)";
}

string UtcTextSQL(const string &column_sql, const LogicalType &type) {
	if (type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		return "(CAST(" + UtcTimestampSQL(column_sql, type) + " AS VARCHAR) || '+00')";
	}
	return "CAST(" + column_sql + " AS VARCHAR)";
}

string UtcNowSQL() {
	return "make_timestamp(epoch_us(now()))";
}

string ExactCenteringType(const string &type_name) {
	auto upper = StringUtil::Upper(type_name);
	static const char *const INTEGER_TYPES[] = {"TINYINT",  "SMALLINT",  "INTEGER", "BIGINT",  "HUGEINT",
	                                            "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT"};
	for (auto integer_type : INTEGER_TYPES) {
		if (upper == integer_type) {
			return "HUGEINT";
		}
	}
	// DECIMAL(width,scale) as LogicalType::ToString renders it
	if (StringUtil::StartsWith(upper, "DECIMAL(")) {
		auto comma = upper.find(',');
		auto close = upper.find(')');
		if (comma != string::npos && close != string::npos && close > comma + 1) {
			auto scale = StringUtil::Replace(upper.substr(comma + 1, close - comma - 1), " ", "");
			return "DECIMAL(38," + scale + ")";
		}
	}
	return "";
}

string CenteringReferenceSQL(const string &column_sql, const string &centering_type) {
	string exact = "CAST(" + column_sql + " AS " + centering_type + ")";
	if (centering_type == "HUGEINT") {
		return "(MIN(" + exact + ") // 2 + MAX(" + exact + ") // 2)";
	}
	return "MIN(" + exact + ")";
}

string CenteredDoubleSQL(const string &column_sql, const string &centering_type, const string &reference_sql) {
	return "CAST(CAST(" + column_sql + " AS " + centering_type + ") - " + reference_sql + " AS DOUBLE)";
}

string DoubleThresholdSQL(const Value &value, const string &null_default) {
	string literal = value.IsNull() ? null_default : "'" + EscapeSqlStringLiteral(value.ToString()) + "'";
	return "CAST(" + literal + " AS DOUBLE)";
}

string BigintThresholdSQL(const Value &value, const string &null_default) {
	string literal = value.IsNull() ? null_default : "'" + EscapeSqlStringLiteral(value.ToString()) + "'";
	return "CAST(" + literal + " AS BIGINT)";
}

// Second-connection reads and the caller's open transaction (see the header)
void RejectUncommittedChanges(ClientContext &context, const string &function_name) {
	if (context.transaction.IsAutoCommit() || !context.transaction.HasActiveTransaction()) {
		return;
	}
	auto modified = MetaTransaction::Get(context).ModifiedDatabase();
	if (!modified) {
		return;
	}
	AnofoxTrace(AnofoxLogLevel::Debug, function_name + ": the current transaction modified database '" +
	                                       modified->GetName() + "', which a sibling connection cannot see");
	// A bind error, like the TEMP table rejection; DuckDB 2.0 aborts the transaction on any error
	throw BinderException("%s reads the last committed data, but this transaction has uncommitted changes; "
	                      "call it after COMMIT, or outside a transaction",
	                      function_name);
}

} // namespace anofox
} // namespace duckdb

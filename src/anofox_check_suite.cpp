#include "anofox_check_suite.hpp"
#include "anofox_check.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb_compat.hpp"
#include "anofox_tabular_banner.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <unordered_map>

namespace duckdb {
namespace anofox {

namespace {

// The current time as a UTC TIMESTAMP, with and without the ICU extension: CAST(now() AS
// TIMESTAMP) would follow the session time zone once ICU is loaded, and AT TIME ZONE needs ICU.
const char *const UTC_NOW_SQL = "make_timestamp(epoch_us(now()))";

// One row of the checks table, with params already extracted from JSON at read time
struct CheckSpec {
	string check_name;
	string check_type;
	string table_name;
	string column_name;
	// params (all optional, empty when absent)
	string pattern;
	vector<string> allowed_values;
	bool has_allowed_values = false;
	string agg;
	string mode;
	string expression;
	string right_table;
	string left_keys;
	string right_keys;
	string date_column;
	string count_column;
	string metric_column;
	string window_days;
	string k;
	// reference_date / reference_time as validated SQL literals (DATE '...' / TIMESTAMP '...'
	// in UTC), empty when absent
	string reference_date_sql;
	string reference_time_sql;
	// thresholds and behavior
	Value lower_threshold;
	Value upper_threshold;
	bool monitor_only = false;
	string identifier_column;
	string filter_expr;
	// A problem found while reading the check (e.g. invalid params JSON); the check then
	// yields an 'error' row instead of failing the whole run
	string config_error;
};

// The params keys each check type accepts; nullptr for an unknown check type (that error is
// reported when the check is built).
static const vector<string> *ParamsKeysFor(const string &check_type) {
	static const std::unordered_map<string, vector<string>> KEYS = {
	    {"volume", {}},
	    {"null_rate", {}},
	    {"distinct_count", {}},
	    {"duplicate_count", {}},
	    {"regex_match", {"pattern"}},
	    {"values_in_set", {"allowed_values"}},
	    {"agg", {"agg"}},
	    {"compliance", {"expression"}},
	    {"freshness", {"reference_time"}},
	    {"occurrence", {"mode"}},
	    {"match_rate", {"right_table", "right_keys", "left_keys"}},
	    {"rel_count_change", {"date_column", "count_column", "window_days", "reference_date"}},
	    {"metric_anomaly_iqr", {"date_column", "metric_column", "window_days", "k", "reference_date"}},
	    {"rolling_values_in_set", {"date_column", "allowed_values", "window_days", "reference_date"}},
	};
	auto entry = KEYS.find(check_type);
	return entry == KEYS.end() ? nullptr : &entry->second;
}

// A misspelled key (e.g. 'refernce_date') would otherwise be ignored silently and the check
// would evaluate its default instead of what was configured.
static void ValidateParamsKeys(const CheckSpec &spec, const vector<string> &keys) {
	auto *valid = ParamsKeysFor(spec.check_type);
	if (!valid) {
		return;
	}
	vector<string> unknown;
	for (auto &key : keys) {
		if (std::find(valid->begin(), valid->end(), key) == valid->end()) {
			unknown.push_back("'" + key + "'");
		}
	}
	if (unknown.empty()) {
		return;
	}
	string message = (unknown.size() == 1 ? "unknown params key " : "unknown params keys ") +
	                 StringUtil::Join(unknown, ", ");
	if (valid->empty()) {
		message += " (" + spec.check_type + " takes no params keys)";
	} else {
		message += " (valid keys for " + spec.check_type + ": " + StringUtil::Join(*valid, ", ") + ")";
	}
	throw BinderException("run_checks: check '" + spec.check_name + "': " + message);
}

// params.<key> as a DATE literal. Validated here, with DuckDB's own cast, so a malformed date
// becomes an 'error' row of this check instead of a conversion error aborting the suite.
static string DateParamLiteral(const CheckSpec &spec, const string &key, const string &text) {
	Value date;
	if (!duckdb_compat::TryCastValue(Value(text), LogicalType(LogicalTypeId::DATE), date)) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params." + key + " '" + text +
		                      "' is not a valid date (expected YYYY-MM-DD)");
	}
	return "DATE '" + date.ToString() + "'";
}

// params.<key> as a UTC TIMESTAMP literal: a value with a UTC offset is converted to UTC, a
// value without one is taken as UTC. The core casts are used (not the ICU ones, which would
// read a value without offset in the session time zone).
static string UtcTimestampParamLiteral(const CheckSpec &spec, const string &key, const string &text) {
	Value with_zone;
	Value utc;
	if (!duckdb_compat::TryCastValue(Value(text), LogicalType(LogicalTypeId::TIMESTAMP_TZ), with_zone) ||
	    !duckdb_compat::TryCastValue(with_zone, LogicalType(LogicalTypeId::TIMESTAMP), utc)) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params." + key + " '" + text +
		                      "' is not a valid timestamp (expected YYYY-MM-DD[ HH:MM[:SS]][+HH[:MM]])");
	}
	return "TIMESTAMP '" + utc.ToString() + "'";
}

// Parse the params JSON of one check with DuckDB's bundled yyjson (no dependency on the
// json SQL extension, which is not loaded in every environment).
static void ParseCheckParams(CheckSpec &spec, const string &params_json) {
	using namespace duckdb_yyjson;
	if (params_json.empty()) {
		return;
	}
	struct YyjsonDocFree {
		void operator()(yyjson_doc *doc_handle) const {
			yyjson_doc_free(doc_handle);
		}
	};
	std::unique_ptr<yyjson_doc, YyjsonDocFree> doc(yyjson_read(params_json.c_str(), params_json.size(), 0));
	yyjson_val *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
	if (!root || !yyjson_is_obj(root)) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params must be a JSON object, got '" +
		                      params_json + "'");
	}

	vector<string> keys;
	{
		size_t idx, max;
		yyjson_val *key;
		yyjson_val *ignored;
		yyjson_obj_foreach(root, idx, max, key, ignored) {
			keys.push_back(yyjson_get_str(key));
		}
	}
	ValidateParamsKeys(spec, keys);

	auto get_scalar = [&](const char *key) -> string {
		auto *val = yyjson_obj_get(root, key);
		if (!val || yyjson_is_null(val)) {
			return string();
		}
		if (yyjson_is_str(val)) {
			return yyjson_get_str(val);
		}
		if (yyjson_is_int(val)) {
			return std::to_string(yyjson_get_sint(val));
		}
		if (yyjson_is_real(val)) {
			return std::to_string(yyjson_get_real(val));
		}
		throw BinderException("run_checks: check '" + spec.check_name + "': params." + key +
		                      " must be a string or number");
	};

	spec.pattern = get_scalar("pattern");
	spec.agg = get_scalar("agg");
	spec.mode = get_scalar("mode");
	spec.expression = get_scalar("expression");
	spec.right_table = get_scalar("right_table");
	spec.left_keys = get_scalar("left_keys");
	spec.right_keys = get_scalar("right_keys");
	spec.date_column = get_scalar("date_column");
	spec.count_column = get_scalar("count_column");
	spec.metric_column = get_scalar("metric_column");
	spec.window_days = get_scalar("window_days");
	spec.k = get_scalar("k");
	auto reference_date = get_scalar("reference_date");
	if (!reference_date.empty()) {
		spec.reference_date_sql = DateParamLiteral(spec, "reference_date", reference_date);
	}
	auto reference_time = get_scalar("reference_time");
	if (!reference_time.empty()) {
		spec.reference_time_sql = UtcTimestampParamLiteral(spec, "reference_time", reference_time);
	}

	auto *allowed = yyjson_obj_get(root, "allowed_values");
	if (allowed && !yyjson_is_null(allowed)) {
		if (!yyjson_is_arr(allowed)) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.allowed_values must be a JSON array of strings");
		}
		spec.has_allowed_values = true;
		size_t idx, max;
		yyjson_val *element;
		yyjson_arr_foreach(allowed, idx, max, element) {
			if (!yyjson_is_str(element)) {
				throw BinderException("run_checks: check '" + spec.check_name +
				                      "': params.allowed_values must be a JSON array of strings");
			}
			spec.allowed_values.push_back(yyjson_get_str(element));
		}
	}
}

// Read the checks table at bind time through a sibling connection. A sibling connection
// cannot see TEMP tables or Python-registered views of the calling connection, which is
// why the checks table must be a regular table; the *target* tables are referenced via
// query_table() in the generated SQL and run in the calling connection, so they may be
// temporary.
static vector<CheckSpec> ReadCheckSpecs(ClientContext &context, const string &checks_table) {
	Connection con(*context.db);
	string sql =
	    "SELECT check_name, check_type, table_name, column_name, params, "
	    "CAST(lower_threshold AS DOUBLE), CAST(upper_threshold AS DOUBLE), "
	    "COALESCE(monitor_only, false), identifier_column, filter_expr "
	    "FROM " + BuildQueryTableRef(checks_table) + " ORDER BY check_name";
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw BinderException("run_checks: failed to read checks table '" + checks_table + "': " +
		                      result->GetError() +
		                      " (note: the checks table must be a regular, non-temporary table with the columns "
		                      "check_name, check_type, table_name, column_name, params, lower_threshold, "
		                      "upper_threshold, monitor_only, identifier_column, filter_expr)");
	}

	vector<CheckSpec> specs;
	auto rows = duckdb_compat::FetchRows(*result);
	auto value_at = [&](idx_t row, idx_t col) -> const Value & {
		return rows[row][col];
	};
	auto str_at = [&](idx_t row, idx_t col) {
		auto &val = value_at(row, col);
		return val.IsNull() ? string() : val.ToString();
	};
	for (idx_t row = 0; row < rows.size(); row++) {
		CheckSpec spec;
		spec.check_name = str_at(row, 0);
		spec.check_type = str_at(row, 1);
		std::transform(spec.check_type.begin(), spec.check_type.end(), spec.check_type.begin(), ::tolower);
		spec.table_name = str_at(row, 2);
		spec.column_name = str_at(row, 3);
		spec.lower_threshold = value_at(row, 5);
		spec.upper_threshold = value_at(row, 6);
		spec.monitor_only = !value_at(row, 7).IsNull() && value_at(row, 7).GetValue<bool>();
		spec.identifier_column = str_at(row, 8);
		spec.filter_expr = str_at(row, 9);
		if (spec.check_name.empty()) {
			throw BinderException("run_checks: every check in '" + checks_table + "' must have a check_name");
		}
		if (spec.table_name.empty()) {
			spec.config_error = "the check has no table_name";
		} else {
			try {
				ParseCheckParams(spec, str_at(row, 4));
			} catch (const std::exception &ex) {
				spec.config_error = ErrorData(ex).RawMessage();
			}
		}
		specs.push_back(std::move(spec));
	}
	if (specs.empty()) {
		throw BinderException("run_checks: checks table '" + checks_table + "' contains no checks");
	}
	return specs;
}

// Check whether an (unqualified) target table or view exists in the calling connection's
// catalogs — including the temp catalog, which a sibling connection cannot see.
// Qualified names are left to the binder.
static bool TargetTableExists(ClientContext &context, const string &name) {
	if (name.find('.') != string::npos) {
		return true;
	}
	try {
		EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, duckdb_compat::EntryName(name));
		if (Catalog::GetEntry(context, duckdb_compat::Ident(INVALID_CATALOG), duckdb_compat::Ident(INVALID_SCHEMA),
		                      lookup_info, OnEntryNotFound::RETURN_NULL)) {
			return true;
		}
	} catch (...) {
		// ambiguity or lookup errors: let the binder produce the real error
		return true;
	}
	try {
		EntryLookupInfo temp_lookup(CatalogType::TABLE_ENTRY, duckdb_compat::EntryName(name));
		if (Catalog::GetEntry(context, duckdb_compat::Ident(TEMP_CATALOG), duckdb_compat::Ident(INVALID_SCHEMA),
		                      temp_lookup, OnEntryNotFound::RETURN_NULL)) {
			return true;
		}
	} catch (...) {
		return true;
	}
	return false;
}

// Substitute ${today}, ${yesterday}, ${today+N}, ${today-N}, ${yesterday+N}, ${yesterday-N}
// tokens in a filter expression with current_date arithmetic.
static string SubstituteDateTokens(const string &expr, const string &check_name) {
	string out;
	size_t pos = 0;
	while (pos < expr.size()) {
		size_t start = expr.find("${", pos);
		if (start == string::npos) {
			out += expr.substr(pos);
			break;
		}
		size_t end = expr.find('}', start);
		if (end == string::npos) {
			throw BinderException("run_checks: unterminated ${...} token in filter_expr of check '" + check_name + "'");
		}
		out += expr.substr(pos, start - pos);
		string token = expr.substr(start + 2, end - start - 2);

		int64_t base_offset = 0;
		string rest;
		if (token.rfind("today", 0) == 0) {
			rest = token.substr(5);
		} else if (token.rfind("yesterday", 0) == 0) {
			base_offset = -1;
			rest = token.substr(9);
		} else {
			throw BinderException("run_checks: unknown token '${" + token + "}' in filter_expr of check '" +
			                      check_name + "' (supported: today, yesterday, today+N, today-N, yesterday-N)");
		}
		int64_t day_offset = base_offset;
		if (!rest.empty()) {
			if ((rest[0] != '+' && rest[0] != '-') || rest.size() < 2 ||
			    rest.find_first_not_of("0123456789", 1) != string::npos) {
				throw BinderException("run_checks: unknown token '${" + token + "}' in filter_expr of check '" +
				                      check_name + "' (supported: today, yesterday, today+N, today-N, yesterday-N)");
			}
			int64_t n = std::stoll(rest.substr(1));
			day_offset += (rest[0] == '+') ? n : -n;
		}
		// The UTC date. Not current_date/today(): with ICU loaded they follow the session
		// time zone, without it they do not resolve.
		const string today_expr = string("CAST(") + UTC_NOW_SQL + " AS DATE)";
		if (day_offset == 0) {
			out += today_expr;
		} else if (day_offset > 0) {
			out += "(" + today_expr + " + " + std::to_string(day_offset) + ")";
		} else {
			out += "(" + today_expr + " - " + std::to_string(-day_offset) + ")";
		}
		pos = end + 1;
	}
	return out;
}

// Parse an integer parameter that arrived as a JSON string, with default
static int64_t ParseWindowDays(const CheckSpec &spec, int64_t default_days) {
	if (spec.window_days.empty()) {
		return default_days;
	}
	try {
		if (spec.window_days.find_first_not_of("0123456789") != string::npos) {
			throw std::invalid_argument("not an integer");
		}
		int64_t days = std::stoll(spec.window_days);
		if (days < 1) {
			throw std::invalid_argument("not positive");
		}
		return days;
	} catch (const std::exception &) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params.window_days '" + spec.window_days +
		                      "' is not a positive integer");
	}
}

static void RequireColumn(const CheckSpec &spec) {
	if (spec.column_name.empty()) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (" + spec.check_type +
		                      ") requires a column_name");
	}
}

static void RequireParam(const CheckSpec &spec, const string &value, const string &param) {
	if (value.empty()) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (" + spec.check_type +
		                      ") requires params." + param);
	}
}

static string BuildAllowedArrayFromJson(const CheckSpec &spec) {
	if (!spec.has_allowed_values) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (" + spec.check_type +
		                      ") requires params.allowed_values");
	}
	string literal = "[]::VARCHAR[]";
	if (!spec.allowed_values.empty()) {
		string elements;
		for (auto &value : spec.allowed_values) {
			if (!elements.empty()) {
				elements += ", ";
			}
			elements += "'" + EscapeSqlStringLiteral(value) + "'";
		}
		literal = "[" + elements + "]::VARCHAR[]";
	}
	return literal;
}

// The key columns of a duplicate_count check (column_name is a comma-separated list)
static vector<string> DuplicateKeyColumns(const CheckSpec &spec) {
	auto columns = ParseCommaSeparatedColumns(spec.column_name);
	if (columns.empty()) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (duplicate_count) requires a column_name");
	}
	return columns;
}

// Bind `sql` (one SELECT) in the calling context without running it. Unlike a sibling
// connection this sees the caller's TEMP tables and uncommitted changes, like the final
// query does. Throws the binder's error for an unknown table, column or function.
static BoundStatement BindProbe(ClientContext &context, const string &sql) {
	auto statements = ParseWithContextSettings(context, sql);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException("run_checks: generated check query is not a single SELECT");
	}
	auto binder = Binder::CreateBinder(context);
	return binder->Bind(*statements[0]);
}

// The per-branch pieces the outer uniform template is assembled from. A check either
// provides literal thresholds (from the checks table) or computes them in the inner
// query (metric_anomaly_iqr exposes its IQR bounds as lower/upper threshold columns).
struct CheckBranch {
	string inner_sql;            // derived table producing: [identifier,] value [, lower_threshold, upper_threshold]
	bool computed_thresholds = false;
	bool has_identifier = false;
	// A NULL value fails the check (with this message) instead of passing trivially.
	// freshness: without a timestamp the data cannot be shown to be fresh, the same
	// outcome as the standalone freshness check.
	bool null_value_fails = false;
	string null_value_message;
	// When set, the inner query also exposes nonfinite_count (the NaN/Inf inputs of the
	// value), which the message of a NaN value reports.
	string nonfinite_count_expr;
	// The inner query also exposes samples (VARCHAR[], up to SAMPLE_LIMIT offending values),
	// which the message of a failing check lists
	bool has_samples = false;
};

// How many offending values a failing values_in_set / duplicate_count / match_rate message shows
constexpr int SAMPLE_LIMIT = 5;

// The SAMPLE_LIMIT smallest distinct values of the VARCHAR expression `value_sql` on the rows
// where `condition_sql` holds (an aggregate)
static string SampleAggregateSQL(const string &value_sql, const string &condition_sql) {
	return "min(DISTINCT " + value_sql + ", " + std::to_string(SAMPLE_LIMIT) + ") FILTER (WHERE " + condition_sql +
	       ")";
}

// The VARCHAR text of a key: the value itself for one column, the ROW text '(a, b)' for several
static string KeyTextSQL(const vector<string> &key_exprs) {
	if (key_exprs.size() == 1) {
		return "COALESCE(CAST(" + key_exprs[0] + " AS VARCHAR), 'NULL')";
	}
	return "CAST(row(" + StringUtil::Join(key_exprs, ", ") + ") AS VARCHAR)";
}

static string ThresholdLiteral(const Value &threshold) {
	return threshold.IsNull() ? "CAST(NULL AS DOUBLE)" : "CAST(" + threshold.ToString() + " AS DOUBLE)";
}

// Types that support identifier partitioning via a plain GROUP BY on the target table
static bool SupportsIdentifier(const string &check_type) {
	return check_type == "volume" || check_type == "null_rate" || check_type == "distinct_count" ||
	       check_type == "regex_match" || check_type == "values_in_set" || check_type == "agg" ||
	       check_type == "duplicate_count" || check_type == "compliance" || check_type == "freshness";
}

// Whether a user expression over the check's target table can be wrapped in TRY(). DuckDB
// rejects TRY() around a subquery or a volatile function (random()) at bind time; such an
// expression is evaluated unwrapped instead (a runtime error in it then aborts the call).
static bool ExpressionSupportsTry(ClientContext &context, const CheckSpec &spec, const string &expr) {
	try {
		BindProbe(context, "SELECT TRY((" + expr + ")) FROM " + BuildQueryTableRef(spec.table_name) + " LIMIT 0");
		return true;
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		if (StringUtil::Contains(error.RawMessage(), "TRY can not be used")) {
			AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: check '" + spec.check_name +
			                                       "': expression evaluated without TRY(): " + error.RawMessage());
			return false;
		}
		// Any other problem is reported by the probe of the whole check query
		return true;
	}
}

// `expr` as a boolean that is FALSE where it is NULL or, when TRY() can wrap it, where it fails
// at runtime
static string TolerantBooleanSQL(ClientContext &context, const CheckSpec &spec, const string &expr) {
	if (ExpressionSupportsTry(context, spec, expr)) {
		return "COALESCE(TRY((" + expr + ")), FALSE)";
	}
	return "COALESCE((" + expr + "), FALSE)";
}

// Build the inner derived table (value computation) for one check
static CheckBranch BuildCheckBranch(ClientContext &context, const CheckSpec &spec, const string &filter_sql) {
	CheckBranch branch;
	const string table_ref = BuildQueryTableRef(spec.table_name);
	const string where_clause = filter_sql.empty() ? "" : " WHERE (" + filter_sql + ")";
	const bool grouped = !spec.identifier_column.empty();
	branch.has_identifier = grouped;

	// Simple aggregate value expressions computed directly over the (filtered) target
	string value_expr;
	string samples_expr;
	if (spec.check_type == "volume") {
		value_expr = "CAST(COUNT(*) AS DOUBLE)";
	} else if (spec.check_type == "null_rate") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		value_expr = "COALESCE(CAST(SUM(CASE WHEN " + col + " IS NULL THEN 1 ELSE 0 END) AS DOUBLE) / "
		             "NULLIF(COUNT(*), 0), 0.0)";
	} else if (spec.check_type == "distinct_count") {
		RequireColumn(spec);
		value_expr = "CAST(COUNT(DISTINCT " + QuoteSqlIdentifier(spec.column_name) + ") AS DOUBLE)";
	} else if (spec.check_type == "regex_match") {
		RequireColumn(spec);
		RequireParam(spec, spec.pattern, "pattern");
		string col = QuoteSqlIdentifier(spec.column_name);
		string pattern_lit = "'" + EscapeSqlStringLiteral(spec.pattern) + "'";
		value_expr = "CAST(SUM(CASE WHEN " + col + " IS NOT NULL AND regexp_matches(CAST(" + col +
		             " AS VARCHAR), " + pattern_lit + ") THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(" + col + "), 0)";
	} else if (spec.check_type == "values_in_set") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		string arr = BuildAllowedArrayFromJson(spec);
		value_expr = "CAST(SUM(CASE WHEN " + col + " IS NOT NULL AND list_contains(" + arr + ", CAST(" + col +
		             " AS VARCHAR)) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(" + col + "), 0)";
		samples_expr = SampleAggregateSQL("CAST(" + col + " AS VARCHAR)",
		                                 col + " IS NOT NULL AND NOT list_contains(" + arr + ", CAST(" + col +
		                                     " AS VARCHAR))");
		branch.has_samples = true;
	} else if (spec.check_type == "agg") {
		RequireColumn(spec);
		RequireParam(spec, spec.agg, "agg");
		string agg = spec.agg;
		std::transform(agg.begin(), agg.end(), agg.begin(), ::tolower);
		static const vector<string> ALLOWED_AGGS = {"avg", "min", "max", "sum", "median", "stddev"};
		if (std::find(ALLOWED_AGGS.begin(), ALLOWED_AGGS.end(), agg) == ALLOWED_AGGS.end()) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.agg must be one of 'avg', 'min', 'max', 'sum', 'median', 'stddev'");
		}
		string agg_upper = agg;
		std::transform(agg_upper.begin(), agg_upper.end(), agg_upper.begin(), ::toupper);
		string double_expr = "CAST(" + QuoteSqlIdentifier(spec.column_name) + " AS DOUBLE)";
		value_expr = BuildFiniteAggregateExpr(agg_upper, double_expr);
		branch.nonfinite_count_expr = BuildNonFiniteCountExpr(double_expr);
	} else if (spec.check_type == "duplicate_count") {
		auto key_columns = DuplicateKeyColumns(spec);
		vector<string> key_exprs;
		for (auto &column : key_columns) {
			key_exprs.push_back(QuoteSqlIdentifier(column));
		}
		string value_sql = "CAST(" + BuildDuplicateCountExpr(key_columns) + " AS DOUBLE) AS value";
		string key_group = StringUtil::Join(key_exprs, ", ");
		// The duplicated keys (GROUP BY puts the NULL keys in one group, like the count)
		string dup_keys = "SELECT " + KeyTextSQL(key_exprs) + " AS k";
		if (grouped) {
			string id_col = "CAST(" + QuoteSqlIdentifier(spec.identifier_column) + " AS VARCHAR)";
			dup_keys = "SELECT " + id_col + " AS identifier, " + KeyTextSQL(key_exprs) + " AS k FROM __base GROUP BY " +
			           id_col + ", " + key_group + " HAVING COUNT(*) > 1";
			branch.inner_sql = "WITH __base AS (SELECT * FROM " + table_ref + where_clause + "), "
			                   "__dup AS (" + dup_keys + "), "
			                   "__v AS (SELECT " + id_col + " AS identifier, " + value_sql + " FROM __base GROUP BY 1) "
			                   "SELECT __v.identifier, __v.value, (SELECT min(k, " + std::to_string(SAMPLE_LIMIT) +
			                   ") FROM __dup WHERE __dup.identifier IS NOT DISTINCT FROM __v.identifier) AS samples "
			                   "FROM __v";
		} else {
			dup_keys += " FROM __base GROUP BY " + key_group + " HAVING COUNT(*) > 1";
			branch.inner_sql = "WITH __base AS (SELECT * FROM " + table_ref + where_clause + "), "
			                   "__dup AS (" + dup_keys + ") "
			                   "SELECT " + value_sql + ", (SELECT min(k, " + std::to_string(SAMPLE_LIMIT) +
			                   ") FROM __dup) AS samples FROM __base";
		}
		branch.has_samples = true;
		return branch;
	} else if (spec.check_type == "compliance") {
		RequireParam(spec, spec.expression, "expression");
		ValidateBooleanExpression(context, spec.expression, "params.expression");
		// TRY(): a row on which the user's expression fails at runtime (e.g. a
		// CAST of free text) counts as non-compliant instead of aborting the suite
		value_expr = "CAST(SUM(CASE WHEN " + TolerantBooleanSQL(context, spec, spec.expression) +
		             " THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0)";
	} else if (spec.check_type == "freshness") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		// Everything in UTC TIMESTAMPs: subtracting a TIMESTAMP from a TIMESTAMP WITH TIME ZONE
		// needs ICU, and with ICU loaded a TIMESTAMPTZ->TIMESTAMP cast follows the session time
		// zone. A TIMESTAMPTZ column goes through its epoch; other columns are taken as UTC.
		string ref_time = spec.reference_time_sql.empty() ? string(UTC_NOW_SQL) : spec.reference_time_sql;
		auto probe = BindProbe(context, "SELECT " + col + " FROM " + table_ref + " LIMIT 0");
		string ts_expr = probe.types[0].id() == LogicalTypeId::TIMESTAMP_TZ
		                     ? "make_timestamp(epoch_us(" + col + "))"
		                     // TRY_CAST: an unparseable text value must not abort the whole suite
		                     : "TRY_CAST(" + col + " AS TIMESTAMP)";
		value_expr = "CAST(EXTRACT(EPOCH FROM (" + ref_time + " - MAX(" + ts_expr + "))) AS DOUBLE)";
		branch.null_value_fails = true;
		branch.null_value_message = "No timestamp values found (table is empty or column is all NULL)";
	} else if (spec.check_type == "occurrence") {
		RequireColumn(spec);
		string mode = spec.mode.empty() ? "max" : spec.mode;
		std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
		if (mode != "max" && mode != "min") {
			throw BinderException("run_checks: check '" + spec.check_name + "': params.mode must be 'max' or 'min'");
		}
		string col = QuoteSqlIdentifier(spec.column_name);
		string extreme = (mode == "max") ? "MAX(c)" : "MIN(c)";
		branch.inner_sql = "WITH counts AS (SELECT CAST(" + col + " AS VARCHAR) AS v, COUNT(*) AS c FROM " +
		                   table_ref + " WHERE " + col + " IS NOT NULL" +
		                   (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") +
		                   " GROUP BY 1) SELECT CAST(" + extreme + " AS DOUBLE) AS value FROM counts";
		return branch;
	} else if (spec.check_type == "match_rate") {
		RequireColumn(spec);
		RequireParam(spec, spec.right_table, "right_table");
		string left_keys_str = spec.left_keys.empty() ? spec.column_name : spec.left_keys;
		RequireParam(spec, spec.right_keys, "right_keys");
		auto left_keys = ParseCommaSeparatedColumns(left_keys_str);
		auto right_keys = ParseCommaSeparatedColumns(spec.right_keys);
		if (left_keys.empty() || left_keys.size() != right_keys.size()) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.left_keys and params.right_keys must list the same number of columns");
		}
		string left_cols, right_cols, join_cond;
		vector<string> left_key_exprs;
		for (size_t i = 0; i < left_keys.size(); i++) {
			left_key_exprs.push_back("l.k" + std::to_string(i));
			string alias = "k" + std::to_string(i);
			if (i > 0) {
				left_cols += ", ";
				right_cols += ", ";
				join_cond += " AND ";
			}
			left_cols += QuoteSqlIdentifier(left_keys[i]) + " AS " + alias;
			right_cols += QuoteSqlIdentifier(right_keys[i]) + " AS " + alias;
			join_cond += "l." + alias + " = r." + alias;
		}
		branch.inner_sql = "SELECT CAST(COUNT(r.matched) AS DOUBLE) / NULLIF(COUNT(*), 0) AS value, " +
		                   SampleAggregateSQL(KeyTextSQL(left_key_exprs), "r.matched IS NULL") + " AS samples "
		                   "FROM (SELECT " + left_cols + " FROM " + table_ref + where_clause + ") l "
		                   "LEFT JOIN (SELECT DISTINCT " + right_cols + ", 1 AS matched FROM " +
		                   BuildQueryTableRef(spec.right_table) + ") r ON " + join_cond;
		branch.has_samples = true;
		return branch;
	} else if (spec.check_type == "rel_count_change") {
		RequireParam(spec, spec.date_column, "date_column");
		string date_id = QuoteSqlIdentifier(spec.date_column);
		string cnt_expr = spec.count_column.empty()
		                      ? "COUNT(*)"
		                      : "COUNT(DISTINCT " + QuoteSqlIdentifier(spec.count_column) + ")";
		string ref_lit = spec.reference_date_sql.empty() ? "NULL" : spec.reference_date_sql;
		string window_val = std::to_string(ParseWindowDays(spec, 7));
		branch.inner_sql =
		    "WITH daily AS (SELECT CAST(" + date_id + " AS DATE) AS d, " + cnt_expr + " AS c FROM " + table_ref +
		    " WHERE " + date_id + " IS NOT NULL" + (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") +
		    " GROUP BY 1), "
		    "ref AS (SELECT COALESCE(" + ref_lit + ", MAX(d)) AS ref_d FROM daily), "
		    "b AS (SELECT r.ref_d, (SELECT c FROM daily WHERE d = r.ref_d) AS ref_c, "
		    "(SELECT AVG(c) FROM daily WHERE d < r.ref_d AND d >= r.ref_d - " + window_val + ") AS baseline_avg "
		    "FROM ref r) "
		    "SELECT CAST(CASE WHEN baseline_avg IS NULL OR baseline_avg = 0 THEN NULL "
		    "ELSE (COALESCE(ref_c, 0) - baseline_avg) / baseline_avg END AS DOUBLE) AS value FROM b";
		return branch;
	} else if (spec.check_type == "metric_anomaly_iqr") {
		RequireParam(spec, spec.date_column, "date_column");
		string date_id = QuoteSqlIdentifier(spec.date_column);
		string metric_expr = spec.metric_column.empty()
		                         ? "CAST(COUNT(*) AS DOUBLE)"
		                         : "AVG(CAST(" + QuoteSqlIdentifier(spec.metric_column) + " AS DOUBLE))";
		string ref_lit = spec.reference_date_sql.empty() ? "NULL" : spec.reference_date_sql;
		string window_val = std::to_string(ParseWindowDays(spec, 30));
		string k_val = "1.5";
		if (!spec.k.empty()) {
			try {
				double k = std::stod(spec.k);
				if (!std::isfinite(k)) {
					throw std::invalid_argument("not finite");
				}
				k_val = spec.k;
			} catch (const std::exception &) {
				throw BinderException("run_checks: check '" + spec.check_name + "': params.k must be a finite number");
			}
		}
		branch.inner_sql =
		    "WITH daily AS (SELECT CAST(" + date_id + " AS DATE) AS d, " + metric_expr + " AS m FROM " + table_ref +
		    " WHERE " + date_id + " IS NOT NULL" + (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") +
		    " GROUP BY 1), "
		    "ref AS (SELECT COALESCE(" + ref_lit + ", MAX(d)) AS ref_d FROM daily), "
		    "w AS (SELECT m FROM daily, ref WHERE d < ref_d AND d >= ref_d - " + window_val + "), "
		    "s AS (SELECT QUANTILE_CONT(m, 0.25) AS q1, QUANTILE_CONT(m, 0.75) AS q3, COUNT(*) AS n FROM w) "
		    "SELECT CAST((SELECT m FROM daily, ref WHERE d = ref_d) AS DOUBLE) AS value, "
		    "CAST(CASE WHEN s.n = 0 THEN NULL ELSE s.q1 - " + k_val + " * (s.q3 - s.q1) END AS DOUBLE) AS lower_threshold, "
		    "CAST(CASE WHEN s.n = 0 THEN NULL ELSE s.q3 + " + k_val + " * (s.q3 - s.q1) END AS DOUBLE) AS upper_threshold "
		    "FROM s";
		branch.computed_thresholds = true;
		return branch;
	} else if (spec.check_type == "rolling_values_in_set") {
		RequireColumn(spec);
		RequireParam(spec, spec.date_column, "date_column");
		string col = QuoteSqlIdentifier(spec.column_name);
		string date_id = QuoteSqlIdentifier(spec.date_column);
		string arr = BuildAllowedArrayFromJson(spec);
		string ref_lit = spec.reference_date_sql.empty() ? "NULL" : spec.reference_date_sql;
		string window_val = std::to_string(ParseWindowDays(spec, 7));
		branch.inner_sql =
		    "WITH ref AS (SELECT COALESCE(" + ref_lit + ", MAX(CAST(" + date_id + " AS DATE))) AS ref_d FROM " +
		    table_ref + "), "
		    "win AS (SELECT CAST(" + col + " AS VARCHAR) AS v FROM " + table_ref + ", ref WHERE " + col +
		    " IS NOT NULL AND " + date_id + " IS NOT NULL AND CAST(" + date_id + " AS DATE) > ref_d - " + window_val +
		    " AND CAST(" + date_id + " AS DATE) <= ref_d" +
		    (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") + ") "
		    "SELECT CAST(SUM(CASE WHEN list_contains(" + arr +
		    ", v) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0) AS value FROM win";
		return branch;
	} else {
		throw BinderException("run_checks: unknown check_type '" + spec.check_type + "' in check '" + spec.check_name +
		                      "' (supported: volume, null_rate, distinct_count, regex_match, values_in_set, agg, "
		                      "duplicate_count, occurrence, match_rate, compliance, freshness, rel_count_change, "
		                      "metric_anomaly_iqr, rolling_values_in_set)");
	}

	// Simple aggregate path (value_expr set): optionally partition by the identifier column
	string select_list = value_expr + " AS value";
	if (!branch.nonfinite_count_expr.empty()) {
		select_list += ", " + branch.nonfinite_count_expr + " AS nonfinite_count";
	}
	if (!samples_expr.empty()) {
		select_list += ", " + samples_expr + " AS samples";
	}
	if (grouped) {
		string id_col = QuoteSqlIdentifier(spec.identifier_column);
		branch.inner_sql = "SELECT CAST(" + id_col + " AS VARCHAR) AS identifier, " + select_list + " FROM " +
		                   table_ref + where_clause + " GROUP BY 1";
	} else {
		branch.inner_sql = "SELECT " + select_list + " FROM " + table_ref + where_clause;
	}
	return branch;
}

// Build a constant single-row error result for a check that cannot run
static string BuildErrorRowSQL(const CheckSpec &spec, const string &error_message) {
	string col_lit = spec.column_name.empty()
	                     ? "CAST(NULL AS VARCHAR)"
	                     : "'" + EscapeSqlStringLiteral(spec.column_name) + "'";
	return string("SELECT ") + UTC_NOW_SQL + " AS run_ts,"
	       "'" + EscapeSqlStringLiteral(spec.check_name) + "' AS check_name, "
	       "'" + EscapeSqlStringLiteral(spec.check_type) + "' AS check_type, "
	       "'" + EscapeSqlStringLiteral(spec.table_name) + "' AS table_name, "
	       + col_lit + " AS column_name, "
	       "CAST(NULL AS VARCHAR) AS identifier, "
	       "CAST(NULL AS DOUBLE) AS value, "
	       "CAST(NULL AS DOUBLE) AS lower_threshold, "
	       "CAST(NULL AS DOUBLE) AS upper_threshold, "
	       "'error' AS status, "
	       "'" + EscapeSqlStringLiteral(error_message) + "' AS message";
}

// Wrap a check branch in the uniform result schema
static string BuildBranchSQL(const CheckSpec &spec, const CheckBranch &branch) {
	string col_lit = spec.column_name.empty()
	                     ? "CAST(NULL AS VARCHAR)"
	                     : "'" + EscapeSqlStringLiteral(spec.column_name) + "'";
	string identifier_expr = branch.has_identifier ? "__c.identifier" : "CAST(NULL AS VARCHAR)";
	string lower_expr = branch.computed_thresholds ? "__c.lower_threshold" : ThresholdLiteral(spec.lower_threshold);
	string upper_expr = branch.computed_thresholds ? "__c.upper_threshold" : ThresholdLiteral(spec.upper_threshold);
	string fail_status = spec.monitor_only ? "warn" : "fail";
	string below_lower = "(" + lower_expr + " IS NOT NULL AND __c.value < " + lower_expr + ")";
	string above_upper = "(" + upper_expr + " IS NOT NULL AND __c.value > " + upper_expr + ")";
	string fail_cond = below_lower + " OR " + above_upper;
	// Neither threshold configured: the row only records the value
	const bool no_thresholds =
	    !branch.computed_thresholds && spec.lower_threshold.IsNull() && spec.upper_threshold.IsNull();
	string null_status = branch.null_value_fails ? fail_status : "pass";
	string null_message = branch.null_value_fails ? EscapeSqlStringLiteral(branch.null_value_message)
	                                              : "No value to evaluate; check passed trivially";
	// A NaN value (e.g. an average over a column holding NaN) cannot be shown to be within
	// the thresholds: it fails instead of passing every comparison.
	string type_lit = EscapeSqlStringLiteral(spec.check_type);
	string nan_message = "'" + type_lit + " value is NaN'";
	if (!branch.nonfinite_count_expr.empty()) {
		nan_message += " || ': the column contains ' || CAST(__c.nonfinite_count AS VARCHAR) || ' NaN/Inf value(s)'";
	}
	string value_text = RoundedNumberSQL("__c.value");
	string value_prefix = "'" + type_lit + " value ' || " + value_text;
	// "; samples: 'a', 'b'" when the check lists offending values and found some
	string samples_suffix = branch.has_samples ? " || CASE WHEN len(__c.samples) > 0 THEN '; samples: ''' || "
	                                             "array_to_string(__c.samples, ''', ''') || '''' ELSE '' END"
	                                           : "";
	string within_message = no_thresholds ? value_prefix + " || ': no thresholds configured (monitor)'"
	                                      : value_prefix + " || ' is within thresholds'";

	return string("SELECT ")+ UTC_NOW_SQL + " AS run_ts,"
	       "'" + EscapeSqlStringLiteral(spec.check_name) + "' AS check_name, "
	       "'" + EscapeSqlStringLiteral(spec.check_type) + "' AS check_type, "
	       "'" + EscapeSqlStringLiteral(spec.table_name) + "' AS table_name, "
	       + col_lit + " AS column_name, "
	       + identifier_expr + " AS identifier, "
	       "__c.value AS value, "
	       + lower_expr + " AS lower_threshold, "
	       + upper_expr + " AS upper_threshold, "
	       "CASE WHEN __c.value IS NULL THEN '" + null_status + "' "
	       "     WHEN isnan(__c.value) THEN '" + fail_status + "' "
	       "     WHEN " + fail_cond + " THEN '" + fail_status + "' "
	       "     ELSE 'pass' END AS status, "
	       "CASE WHEN __c.value IS NULL THEN '" + null_message + "' "
	       "     WHEN isnan(__c.value) THEN " + nan_message + " "
	       "     WHEN " + below_lower + " THEN " + value_prefix + " || ' violates thresholds (' || " + value_text +
	       " || ' < lower ' || " + RoundedNumberSQL(lower_expr) + " || ')'" + samples_suffix + " "
	       "     WHEN " + above_upper + " THEN " + value_prefix + " || ' violates thresholds (' || " + value_text +
	       " || ' > upper ' || " + RoundedNumberSQL(upper_expr) + " || ')'" + samples_suffix + " "
	       "     ELSE " + within_message + " END AS message "
	       "FROM (" + branch.inner_sql + ") __c";
}

// The columns a check reads from its target table, as configured
static vector<string> ReferencedColumns(const CheckSpec &spec) {
	vector<string> columns;
	auto add = [&](const string &column) {
		if (!column.empty()) {
			columns.push_back(column);
		}
	};
	const auto &type = spec.check_type;
	if (type == "duplicate_count") {
		for (auto &column : ParseCommaSeparatedColumns(spec.column_name)) {
			add(column);
		}
	} else if (type == "match_rate") {
		for (auto &column : ParseCommaSeparatedColumns(spec.left_keys.empty() ? spec.column_name : spec.left_keys)) {
			add(column);
		}
	} else if (type != "volume" && type != "compliance" && type != "rel_count_change" &&
	           type != "metric_anomaly_iqr") {
		add(spec.column_name);
	}
	add(spec.identifier_column);
	add(spec.date_column);
	add(spec.count_column);
	add(spec.metric_column);
	return columns;
}

// Resolve every configured column against the target table (case-insensitively, like
// DuckDB identifiers) so a missing column names the column and the table's columns.
static void ValidateCheckColumns(ClientContext &context, const CheckSpec &spec) {
	auto bound = BindProbe(context, "SELECT * FROM " + BuildQueryTableRef(spec.table_name) + " LIMIT 0");
	vector<string> available = duckdb_compat::Strs(bound.names);
	MatchColumnNames("run_checks", spec.table_name, available, ReferencedColumns(spec));
	if (spec.check_type == "agg" && !spec.column_name.empty()) {
		auto idx = MatchColumnNames("run_checks", spec.table_name, available, {spec.column_name})[0];
		if (!bound.types[idx].IsNumeric()) {
			throw BinderException("run_checks: column '%s' has type %s, agg needs a numeric column",
			                      available[idx], bound.types[idx].ToString());
		}
	}
}

static bool IsRateCheck(const string &check_type) {
	return check_type == "null_rate" || check_type == "regex_match" || check_type == "values_in_set" ||
	       check_type == "compliance" || check_type == "match_rate" || check_type == "rolling_values_in_set";
}

static void ValidateCheckThresholds(const CheckSpec &spec) {
	ValidateThresholdOrder(spec.lower_threshold, spec.upper_threshold, "run_checks", "lower_threshold",
	                       "upper_threshold");
	if (!IsRateCheck(spec.check_type)) {
		return;
	}
	auto validate_rate = [&](const Value &threshold, const char *name) {
		if (threshold.IsNull()) {
			return;
		}
		double rate = threshold.GetValue<double>();
		if (!(rate >= 0 && rate <= 1)) {
			throw BinderException("run_checks: %s must be between 0 and 1 for a %s check (got %s)", name,
			                      spec.check_type, threshold.ToString());
		}
	};
	validate_rate(spec.lower_threshold, "lower_threshold");
	validate_rate(spec.upper_threshold, "upper_threshold");
}

// The SQL of one check in the uniform result schema. Every problem that keeps the check
// from running (missing table or column, invalid configuration, a query that does not
// bind) is thrown here and turned into an 'error' row by the caller.
static string BuildValidatedCheckSQL(ClientContext &context, const CheckSpec &spec) {
	if (!spec.config_error.empty()) {
		throw BinderException(spec.config_error);
	}
	if (!TargetTableExists(context, spec.table_name)) {
		throw BinderException("target table '%s' does not exist", spec.table_name);
	}
	ValidateCheckColumns(context, spec);
	if (!spec.identifier_column.empty() && !SupportsIdentifier(spec.check_type)) {
		throw BinderException("identifier_column is not supported for check_type '%s'", spec.check_type);
	}
	ValidateCheckThresholds(spec);

	string filter_sql;
	if (!spec.filter_expr.empty()) {
		filter_sql = SubstituteDateTokens(spec.filter_expr, spec.check_name);
		ValidateBooleanExpression(context, filter_sql, "filter_expr");
		// A row on which the filter fails at runtime is filtered out instead of
		// aborting the suite
		filter_sql = TolerantBooleanSQL(context, spec, filter_sql);
	}
	auto sql = BuildBranchSQL(spec, BuildCheckBranch(context, spec, filter_sql));
	BindProbe(context, sql);
	return sql;
}

// "check '<name>' (<type>) cannot run: <cause>", with the run_checks/check prefixes of the
// cause dropped and only its first line kept (binder errors append the query context)
static string CheckErrorMessage(const CheckSpec &spec, string cause) {
	auto newline = cause.find('\n');
	if (newline != string::npos) {
		cause = cause.substr(0, newline);
	}
	const string check_prefix = "check '" + spec.check_name + "'";
	for (auto &prefix : {string("run_checks: ") + check_prefix + " (" + spec.check_type + ")",
	                     string("run_checks: ") + check_prefix, string("run_checks: ")}) {
		if (StringUtil::StartsWith(cause, prefix)) {
			cause = cause.substr(prefix.size());
			break;
		}
	}
	while (!cause.empty() && (cause[0] == ':' || cause[0] == ' ')) {
		cause.erase(0, 1);
	}
	return check_prefix + " (" + spec.check_type + ") cannot run: " + cause;
}

} // anonymous namespace

//===--------------------------------------------------------------------===//
// run_checks Bind Replace
//===--------------------------------------------------------------------===//

static unique_ptr<TableRef> RunChecksBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("run_checks");
	if (input.inputs.empty()) {
		throw BinderException("anofox_tab_run_checks requires 1 argument: checks_table");
	}
	string checks_table = RequireNonNullArgument(input, 0, "run_checks", "checks_table");

	auto specs = ReadCheckSpecs(context, checks_table);
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: running " + std::to_string(specs.size()) +
	                                       " check(s) from '" + checks_table + "'");

	vector<string> branches;
	idx_t error_count = 0;
	for (auto &spec : specs) {
		// Per-check error isolation: a check that cannot be built or bound yields an
		// 'error' row; the other checks run normally.
		try {
			branches.push_back(BuildValidatedCheckSQL(context, spec));
		} catch (const std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::FATAL ||
			    error.Type() == ExceptionType::INTERNAL) {
				throw;
			}
			auto message = CheckErrorMessage(spec, error.RawMessage());
			AnofoxTrace(AnofoxLogLevel::Warn, "check_suite: " + message);
			branches.push_back(BuildErrorRowSQL(spec, message));
			error_count++;
		}
	}
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: " + std::to_string(error_count) + " of " +
	                                       std::to_string(specs.size()) + " check(s) cannot run");

	string sql;
	for (size_t i = 0; i < branches.size(); i++) {
		if (i > 0) {
			sql += " UNION ALL ";
		}
		sql += "(" + branches[i] + ")";
	}
	return ParseSubquery(sql, context, "Failed to parse check suite query");
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterCheckSuiteFunctions(ExtensionLoader &loader) {
	// anofox_tab_run_checks(checks_table) (alias: run_checks)
	TableFunction run_checks_func("anofox_tab_run_checks", {LogicalType(LogicalTypeId::VARCHAR)}, nullptr, nullptr);
	run_checks_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, RunChecksBindReplace);
	{
		FunctionDescription desc;
		desc.description =
		    "Runs every check defined in a checks table (columns: check_name, check_type, table_name, column_name, "
		    "params JSON, lower_threshold, upper_threshold, monitor_only, identifier_column, filter_expr) and returns "
		    "one uniform result row per check and identifier partition (run_ts, check_name, check_type, table_name, "
		    "column_name, identifier, value, lower_threshold, upper_threshold, status, message). The checks table "
		    "must be a regular (non-temporary) table; target tables may be temporary.";
		desc.parameter_names = {"checks_table"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT * FROM run_checks('dq_checks');",
		                 "INSERT INTO dq_results SELECT * FROM run_checks('dq_checks');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, run_checks_func, "run_checks", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

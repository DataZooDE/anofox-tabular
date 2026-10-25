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

namespace duckdb {
namespace anofox {

namespace {

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
	string reference_date;
	string reference_time;
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
	spec.reference_date = get_scalar("reference_date");
	spec.reference_time = get_scalar("reference_time");

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
		// Not current_date/today(), and not a direct TIMESTAMPTZ->DATE cast: both
		// resolve through the icu extension, which is not loaded in every environment.
		// TIMESTAMPTZ->TIMESTAMP->DATE stays within core casts.
		const string today_expr = "CAST(CAST(now() AS TIMESTAMP) AS DATE)";
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
		int64_t days = std::stoll(spec.window_days);
		if (days < 1) {
			throw std::invalid_argument("negative");
		}
		return days;
	} catch (const std::exception &) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params.window_days must be a positive integer");
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
};

static string ThresholdLiteral(const Value &threshold) {
	return threshold.IsNull() ? "CAST(NULL AS DOUBLE)" : "CAST(" + threshold.ToString() + " AS DOUBLE)";
}

// Types that support identifier partitioning via a plain GROUP BY on the target table
static bool SupportsIdentifier(const string &check_type) {
	return check_type == "volume" || check_type == "null_rate" || check_type == "distinct_count" ||
	       check_type == "regex_match" || check_type == "values_in_set" || check_type == "agg" ||
	       check_type == "duplicate_count" || check_type == "compliance" || check_type == "freshness";
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
		value_expr = "CAST(" + BuildDuplicateCountExpr(DuplicateKeyColumns(spec)) + " AS DOUBLE)";
	} else if (spec.check_type == "compliance") {
		RequireParam(spec, spec.expression, "expression");
		ValidateBooleanExpression(context, spec.expression, "params.expression");
		value_expr = "CAST(SUM(CASE WHEN COALESCE((" + spec.expression +
		             "), FALSE) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0)";
	} else if (spec.check_type == "freshness") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		// CAST(now() AS TIMESTAMP): now() is a TIMESTAMP WITH TIME ZONE, and subtracting a
		// plain TIMESTAMP from it needs the ICU extension
		string ref_time = spec.reference_time.empty()
		                      ? "CAST(now() AS TIMESTAMP)"
		                      : "'" + EscapeSqlStringLiteral(spec.reference_time) + "'::TIMESTAMP";
		// TRY_CAST: a text column with an unparseable value must not abort the whole suite
		value_expr = "CAST(EXTRACT(EPOCH FROM (" + ref_time + " - MAX(TRY_CAST(" + col + " AS TIMESTAMP)))) AS DOUBLE)";
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
		for (size_t i = 0; i < left_keys.size(); i++) {
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
		branch.inner_sql = "SELECT CAST(COUNT(r.matched) AS DOUBLE) / NULLIF(COUNT(*), 0) AS value "
		                   "FROM (SELECT " + left_cols + " FROM " + table_ref + where_clause + ") l "
		                   "LEFT JOIN (SELECT DISTINCT " + right_cols + ", 1 AS matched FROM " +
		                   BuildQueryTableRef(spec.right_table) + ") r ON " + join_cond;
		return branch;
	} else if (spec.check_type == "rel_count_change") {
		RequireParam(spec, spec.date_column, "date_column");
		string date_id = QuoteSqlIdentifier(spec.date_column);
		string cnt_expr = spec.count_column.empty()
		                      ? "COUNT(*)"
		                      : "COUNT(DISTINCT " + QuoteSqlIdentifier(spec.count_column) + ")";
		string ref_lit = spec.reference_date.empty()
		                     ? "NULL"
		                     : "DATE '" + EscapeSqlStringLiteral(spec.reference_date) + "'";
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
		string ref_lit = spec.reference_date.empty()
		                     ? "NULL"
		                     : "DATE '" + EscapeSqlStringLiteral(spec.reference_date) + "'";
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
		string ref_lit = spec.reference_date.empty()
		                     ? "NULL"
		                     : "DATE '" + EscapeSqlStringLiteral(spec.reference_date) + "'";
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
	return "SELECT CAST(now() AS TIMESTAMP) AS run_ts,"
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
	string fail_cond = "(" + lower_expr + " IS NOT NULL AND __c.value < " + lower_expr + ") OR (" + upper_expr +
	                   " IS NOT NULL AND __c.value > " + upper_expr + ")";
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

	return "SELECT CAST(now() AS TIMESTAMP) AS run_ts,"
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
	       "     WHEN " + fail_cond + " THEN '" + type_lit + " value ' || " + value_text + " || ' violates thresholds' "
	       "     ELSE '" + type_lit + " value ' || " + value_text + " || ' is within thresholds' END AS message "
	       "FROM (" + branch.inner_sql + ") __c";
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

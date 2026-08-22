#include "anofox_check_suite.hpp"
#include "anofox_check.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "anofox_tabular_banner.hpp"

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
	string allowed_values_json;
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
};

// Read the checks table at bind time through a sibling connection. A sibling connection
// cannot see TEMP tables or Python-registered views of the calling connection, which is
// why the checks table must be a regular table; the *target* tables are referenced via
// query_table() in the generated SQL and run in the calling connection, so they may be
// temporary.
static vector<CheckSpec> ReadCheckSpecs(ClientContext &context, const string &checks_table) {
	Connection con(*context.db);
	// The params extraction below uses json_extract_string; the json extension is
	// statically linked (extension_config.cmake) but not necessarily loaded in
	// every environment (e.g. the sqllogictest runner disables autoloading).
	con.Query("LOAD json");
	string sql =
	    "SELECT check_name, check_type, table_name, column_name, "
	    "json_extract_string(params, '$.pattern'), "
	    "CAST(json_extract(params, '$.allowed_values') AS VARCHAR), "
	    "json_extract_string(params, '$.agg'), "
	    "json_extract_string(params, '$.mode'), "
	    "json_extract_string(params, '$.expression'), "
	    "json_extract_string(params, '$.right_table'), "
	    "json_extract_string(params, '$.left_keys'), "
	    "json_extract_string(params, '$.right_keys'), "
	    "json_extract_string(params, '$.date_column'), "
	    "json_extract_string(params, '$.count_column'), "
	    "json_extract_string(params, '$.metric_column'), "
	    "json_extract_string(params, '$.window_days'), "
	    "json_extract_string(params, '$.k'), "
	    "json_extract_string(params, '$.reference_date'), "
	    "json_extract_string(params, '$.reference_time'), "
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
	auto str_at = [&](idx_t row, idx_t col) {
		auto val = result->GetValue(col, row);
		return val.IsNull() ? string() : val.ToString();
	};
	for (idx_t row = 0; row < result->RowCount(); row++) {
		CheckSpec spec;
		spec.check_name = str_at(row, 0);
		spec.check_type = str_at(row, 1);
		std::transform(spec.check_type.begin(), spec.check_type.end(), spec.check_type.begin(), ::tolower);
		spec.table_name = str_at(row, 2);
		spec.column_name = str_at(row, 3);
		spec.pattern = str_at(row, 4);
		spec.allowed_values_json = str_at(row, 5);
		spec.agg = str_at(row, 6);
		spec.mode = str_at(row, 7);
		spec.expression = str_at(row, 8);
		spec.right_table = str_at(row, 9);
		spec.left_keys = str_at(row, 10);
		spec.right_keys = str_at(row, 11);
		spec.date_column = str_at(row, 12);
		spec.count_column = str_at(row, 13);
		spec.metric_column = str_at(row, 14);
		spec.window_days = str_at(row, 15);
		spec.k = str_at(row, 16);
		spec.reference_date = str_at(row, 17);
		spec.reference_time = str_at(row, 18);
		spec.lower_threshold = result->GetValue(19, row);
		spec.upper_threshold = result->GetValue(20, row);
		spec.monitor_only = !result->GetValue(21, row).IsNull() && result->GetValue(21, row).GetValue<bool>();
		spec.identifier_column = str_at(row, 22);
		spec.filter_expr = str_at(row, 23);
		if (spec.check_name.empty()) {
			throw BinderException("run_checks: every check in '" + checks_table + "' must have a check_name");
		}
		if (spec.table_name.empty()) {
			throw BinderException("run_checks: check '" + spec.check_name + "' must have a table_name");
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
		EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, name);
		if (Catalog::GetEntry(context, INVALID_CATALOG, INVALID_SCHEMA, lookup_info, OnEntryNotFound::RETURN_NULL)) {
			return true;
		}
	} catch (...) {
		// ambiguity or lookup errors: let the binder produce the real error
		return true;
	}
	try {
		EntryLookupInfo temp_lookup(CatalogType::TABLE_ENTRY, name);
		if (Catalog::GetEntry(context, TEMP_CATALOG, INVALID_SCHEMA, temp_lookup, OnEntryNotFound::RETURN_NULL)) {
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
	RequireParam(spec, spec.allowed_values_json, "allowed_values");
	return "('" + EscapeSqlStringLiteral(spec.allowed_values_json) + "'::JSON)::VARCHAR[]";
}

// Build the comma-separated quoted identifier list for duplicate_count keys
static string BuildDistinctKeyExpr(const CheckSpec &spec) {
	auto columns = ParseCommaSeparatedColumns(spec.column_name);
	if (columns.empty()) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (duplicate_count) requires a column_name");
	}
	if (columns.size() == 1) {
		return QuoteSqlIdentifier(columns[0]);
	}
	string expr = "(";
	for (size_t i = 0; i < columns.size(); i++) {
		if (i > 0) {
			expr += ", ";
		}
		expr += QuoteSqlIdentifier(columns[i]);
	}
	return expr + ")";
}

// The per-branch pieces the outer uniform template is assembled from. A check either
// provides literal thresholds (from the checks table) or computes them in the inner
// query (metric_anomaly_iqr exposes its IQR bounds as lower/upper threshold columns).
struct CheckBranch {
	string inner_sql;            // derived table producing: [identifier,] value [, lower_threshold, upper_threshold]
	bool computed_thresholds = false;
	bool has_identifier = false;
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
		value_expr = "CAST(" + agg_upper + "(CAST(" + QuoteSqlIdentifier(spec.column_name) + " AS DOUBLE)) AS DOUBLE)";
	} else if (spec.check_type == "duplicate_count") {
		value_expr = "CAST(COUNT(*) - COUNT(DISTINCT " + BuildDistinctKeyExpr(spec) + ") AS DOUBLE)";
	} else if (spec.check_type == "compliance") {
		RequireParam(spec, spec.expression, "expression");
		ValidateBooleanExpression(context, spec.expression,
		                          "run_checks (params.expression of check '" + spec.check_name + "')");
		value_expr = "CAST(SUM(CASE WHEN COALESCE((" + spec.expression +
		             "), FALSE) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0)";
	} else if (spec.check_type == "freshness") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		string ref_time = spec.reference_time.empty()
		                      ? "now()"
		                      : "'" + EscapeSqlStringLiteral(spec.reference_time) + "'::TIMESTAMP";
		value_expr = "CAST(EXTRACT(EPOCH FROM (" + ref_time + " - MAX(CAST(" + col + " AS TIMESTAMP)))) AS DOUBLE)";
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
	if (grouped) {
		string id_col = QuoteSqlIdentifier(spec.identifier_column);
		branch.inner_sql = "SELECT CAST(" + id_col + " AS VARCHAR) AS identifier, " + value_expr + " AS value FROM " +
		                   table_ref + where_clause + " GROUP BY 1";
	} else {
		branch.inner_sql = "SELECT " + value_expr + " AS value FROM " + table_ref + where_clause;
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

	return "SELECT CAST(now() AS TIMESTAMP) AS run_ts,"
	       "'" + EscapeSqlStringLiteral(spec.check_name) + "' AS check_name, "
	       "'" + EscapeSqlStringLiteral(spec.check_type) + "' AS check_type, "
	       "'" + EscapeSqlStringLiteral(spec.table_name) + "' AS table_name, "
	       + col_lit + " AS column_name, "
	       + identifier_expr + " AS identifier, "
	       "__c.value AS value, "
	       + lower_expr + " AS lower_threshold, "
	       + upper_expr + " AS upper_threshold, "
	       "CASE WHEN __c.value IS NULL THEN 'pass' "
	       "     WHEN " + fail_cond + " THEN '" + fail_status + "' "
	       "     ELSE 'pass' END AS status, "
	       "CASE WHEN __c.value IS NULL THEN 'No value to evaluate; check passed trivially' "
	       "     WHEN " + fail_cond + " THEN '" + EscapeSqlStringLiteral(spec.check_type) +
	       " value ' || CAST(__c.value AS VARCHAR) || ' violates thresholds' "
	       "     ELSE '" + EscapeSqlStringLiteral(spec.check_type) +
	       " value ' || CAST(__c.value AS VARCHAR) || ' is within thresholds' END AS message "
	       "FROM (" + branch.inner_sql + ") __c";
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
	string checks_table = input.inputs[0].ToString();

	auto specs = ReadCheckSpecs(context, checks_table);
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: running " + std::to_string(specs.size()) +
	                                       " check(s) from '" + checks_table + "'");

	vector<string> branches;
	for (auto &spec : specs) {
		// Per-check error isolation: a missing target or an unsupported identifier
		// combination yields an error row instead of failing the whole run.
		if (!TargetTableExists(context, spec.table_name)) {
			branches.push_back(BuildErrorRowSQL(spec, "Target table '" + spec.table_name + "' does not exist"));
			continue;
		}
		if (!spec.identifier_column.empty() && !SupportsIdentifier(spec.check_type)) {
			branches.push_back(BuildErrorRowSQL(
			    spec, "identifier_column is not supported for check_type '" + spec.check_type + "'"));
			continue;
		}

		string filter_sql;
		if (!spec.filter_expr.empty()) {
			filter_sql = SubstituteDateTokens(spec.filter_expr, spec.check_name);
			ValidateBooleanExpression(context, filter_sql,
			                          "run_checks (filter_expr of check '" + spec.check_name + "')");
		}

		auto branch = BuildCheckBranch(context, spec, filter_sql);
		branches.push_back(BuildBranchSQL(spec, branch));
	}

	string sql;
	for (size_t i = 0; i < branches.size(); i++) {
		if (i > 0) {
			sql += " UNION ALL ";
		}
		sql += "(" + branches[i] + ")";
	}
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse check suite query");
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

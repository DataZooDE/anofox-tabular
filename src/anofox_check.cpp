#include "anofox_check.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/exception.hpp"
#include "anofox_tabular_banner.hpp"

#include <algorithm>

namespace duckdb {
namespace anofox {

namespace {

// Helper: register every prefix arity of full_args (from min_arity up) in a function set
// for bind_replace-based table functions, so all trailing parameters are optional.
static void AddPrefixAritiesBindReplace(TableFunctionSet &set, const string &name,
                                        const vector<LogicalType> &full_args, idx_t min_arity,
                                        table_function_bind_replace_t bind_replace) {
	for (idx_t arity = min_arity; arity <= full_args.size(); ++arity) {
		vector<LogicalType> args(full_args.begin(), full_args.begin() + arity);
		TableFunction func(name, std::move(args), nullptr, nullptr);
		func.bind_replace = bind_replace;
		set.AddFunction(func);
	}
}

} // anonymous namespace

// Build a ['a', 'b', ...]::VARCHAR[] literal from a LIST value
string BuildVarcharArrayLiteral(const Value &list_value) {
	string literal = "[]::VARCHAR[]";
	if (!list_value.IsNull() && list_value.type().id() == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(list_value);
		string elements;
		for (auto &child : children) {
			if (child.IsNull()) {
				continue;
			}
			if (!elements.empty()) {
				elements += ", ";
			}
			elements += "'" + EscapeSqlStringLiteral(child.ToString()) + "'";
		}
		if (!elements.empty()) {
			literal = "[" + elements + "]::VARCHAR[]";
		}
	}
	return literal;
}

// Validate that a caller-supplied string is a single SQL boolean expression.
// Rejects multi-statement payloads ("1); DROP TABLE x;--") because "SELECT (<expr>)"
// must parse to exactly one SELECT statement. The generated query is additionally
// re-validated by ParseSubquery as a single SELECT. Note that the expression is still
// executed with the caller's privileges — same trust model as running SQL directly.
void ValidateBooleanExpression(ClientContext &context, const string &expr, const string &function_name) {
	if (expr.empty()) {
		throw BinderException(function_name + ": expression must not be empty");
	}
	Parser parser(context.GetParserOptions());
	try {
		parser.ParseQuery("SELECT (" + expr + ")");
	} catch (const std::exception &ex) {
		throw BinderException(function_name + ": failed to parse expression '" + expr + "': " + ex.what());
	}
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException(function_name + ": expression must be a single SQL boolean expression");
	}
}

//===--------------------------------------------------------------------===//
// SQL generators
//===--------------------------------------------------------------------===//

// Helper: Generate SQL for regex match rate checks.
// Empty-input semantics: an empty table or all-NULL column yields match_rate = 0.0,
// total_count = 0 and passes trivially with an explicit message.
static string GenerateRegexMatchSQL(const string &table_ref, const string &column_name, const string &pattern,
                                    const Value &min_rate, const Value &max_rate) {
	string min_val = min_rate.IsNull() ? "NULL" : min_rate.ToString();
	string max_val = max_rate.IsNull() ? "NULL" : max_rate.ToString();
	string col_id = QuoteSqlIdentifier(column_name);
	string pattern_lit = "'" + EscapeSqlStringLiteral(pattern) + "'";

	return "SELECT "
		"CASE "
		"  WHEN total_count = 0 THEN 'pass' "
		"  WHEN (" + min_val + " IS NOT NULL AND match_rate < " + min_val + ") OR "
		"       (" + max_val + " IS NOT NULL AND match_rate > " + max_val + ") THEN 'fail' "
		"  ELSE 'pass' "
		"END AS status, "
		"match_rate, "
		"matched_count, "
		"total_count, "
		+ min_val + " AS min_threshold, "
		+ max_val + " AS max_threshold, "
		"CASE "
		"  WHEN total_count = 0 THEN 'No non-NULL values to evaluate; regex match check passed trivially' "
		"  WHEN (" + min_val + " IS NOT NULL AND match_rate < " + min_val + ") THEN "
		"    'Match rate ' || CAST(match_rate AS VARCHAR) || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"  WHEN (" + max_val + " IS NOT NULL AND match_rate > " + max_val + ") THEN "
		"    'Match rate ' || CAST(match_rate AS VARCHAR) || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') exceeds maximum ' || CAST(" + max_val + " AS VARCHAR) "
		"  ELSE 'Match rate ' || CAST(match_rate AS VARCHAR) || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is within acceptable range' "
		"END AS message "
		"FROM (SELECT "
		"  COALESCE(CAST(SUM(CASE WHEN regexp_matches(CAST(" + col_id + " AS VARCHAR), " + pattern_lit + ") THEN 1 ELSE 0 END) AS BIGINT), 0) AS matched_count, "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(SUM(CASE WHEN regexp_matches(CAST(" + col_id + " AS VARCHAR), " + pattern_lit + ") THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS match_rate "
		"FROM " + table_ref + " WHERE " + col_id + " IS NOT NULL)";
}

// Helper: Generate SQL for values-in-set checks.
// Empty-input semantics: an empty table or all-NULL column yields in_set_rate = 0.0,
// total_count = 0 and passes trivially with an explicit message.
static string GenerateValuesInSetSQL(const string &table_ref, const string &column_name, const string &allowed_array,
                                     const Value &min_rate) {
	string min_val = min_rate.IsNull() ? "1.0" : min_rate.ToString();
	string col_id = QuoteSqlIdentifier(column_name);
	string col_str = "CAST(" + col_id + " AS VARCHAR)";

	return "SELECT "
		"CASE "
		"  WHEN total_count = 0 THEN 'pass' "
		"  WHEN in_set_rate >= " + min_val + " THEN 'pass' "
		"  ELSE 'fail' "
		"END AS status, "
		"in_set_rate, "
		"in_set_count, "
		"total_count, "
		+ min_val + " AS threshold, "
		"(SELECT COALESCE(array_agg(v), []::VARCHAR[]) FROM ("
		"  SELECT DISTINCT " + col_str + " AS v FROM " + table_ref +
		"  WHERE " + col_id + " IS NOT NULL AND NOT list_contains(" + allowed_array + ", " + col_str + ") LIMIT 5"
		")) AS sample_violations, "
		"CASE "
		"  WHEN total_count = 0 THEN 'No non-NULL values to evaluate; values-in-set check passed trivially' "
		"  WHEN in_set_rate >= " + min_val + " THEN "
		"    'In-set rate ' || CAST(in_set_rate AS VARCHAR) || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'In-set rate ' || CAST(in_set_rate AS VARCHAR) || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"END AS message "
		"FROM (SELECT "
		"  COALESCE(CAST(SUM(CASE WHEN list_contains(" + allowed_array + ", " + col_str + ") THEN 1 ELSE 0 END) AS BIGINT), 0) AS in_set_count, "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(SUM(CASE WHEN list_contains(" + allowed_array + ", " + col_str + ") THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS in_set_rate "
		"FROM " + table_ref + " WHERE " + col_id + " IS NOT NULL)";
}

// Helper: Generate SQL for aggregate assertions (avg/min/max/sum/median/stddev vs thresholds).
// agg_keyword must come from the bind-time whitelist, never from raw user input.
// Empty-input semantics: an empty table or all-NULL column yields value = NULL and
// passes trivially with an explicit message.
static string GenerateAggCheckSQL(const string &table_ref, const string &column_name, const string &agg_keyword,
                                  const Value &lower, const Value &upper) {
	string lower_val = lower.IsNull() ? "NULL" : lower.ToString();
	string upper_val = upper.IsNull() ? "NULL" : upper.ToString();
	string col_id = QuoteSqlIdentifier(column_name);
	string agg_upper = agg_keyword;
	std::transform(agg_upper.begin(), agg_upper.end(), agg_upper.begin(), ::toupper);

	return "SELECT "
		"CASE "
		"  WHEN value IS NULL THEN 'pass' "
		"  WHEN (" + lower_val + " IS NOT NULL AND value < " + lower_val + ") OR "
		"       (" + upper_val + " IS NOT NULL AND value > " + upper_val + ") THEN 'fail' "
		"  ELSE 'pass' "
		"END AS status, "
		"'" + agg_keyword + "' AS agg, "
		"value, "
		+ lower_val + " AS lower_threshold, "
		+ upper_val + " AS upper_threshold, "
		"CASE "
		"  WHEN value IS NULL THEN 'No non-NULL values to evaluate; " + agg_keyword + " check passed trivially' "
		"  WHEN (" + lower_val + " IS NOT NULL AND value < " + lower_val + ") THEN "
		"    '" + agg_keyword + " value ' || CAST(value AS VARCHAR) || ' is below minimum ' || CAST(" + lower_val + " AS VARCHAR) "
		"  WHEN (" + upper_val + " IS NOT NULL AND value > " + upper_val + ") THEN "
		"    '" + agg_keyword + " value ' || CAST(value AS VARCHAR) || ' exceeds maximum ' || CAST(" + upper_val + " AS VARCHAR) "
		"  ELSE '" + agg_keyword + " value ' || CAST(value AS VARCHAR) || ' is within acceptable range' "
		"END AS message "
		"FROM (SELECT CAST(" + agg_upper + "(CAST(" + col_id + " AS DOUBLE)) AS DOUBLE) AS value FROM " + table_ref + ")";
}

// Helper: Generate SQL for duplicate count checks.
// Duplicates are COUNT(*) - COUNT(DISTINCT key); for a single column NULLs count as
// duplicates of each other (COUNT(DISTINCT) ignores NULL).
static string GenerateDuplicateCountSQL(const string &table_ref, const vector<string> &column_names,
                                        const Value &max_duplicates) {
	string max_val = max_duplicates.IsNull() ? "0" : max_duplicates.ToString();
	string distinct_expr;
	if (column_names.size() == 1) {
		distinct_expr = QuoteSqlIdentifier(column_names[0]);
	} else {
		distinct_expr = "(";
		for (size_t i = 0; i < column_names.size(); i++) {
			if (i > 0) {
				distinct_expr += ", ";
			}
			distinct_expr += QuoteSqlIdentifier(column_names[i]);
		}
		distinct_expr += ")";
	}

	return "SELECT "
		"CASE WHEN duplicate_count <= " + max_val + " THEN 'pass' ELSE 'fail' END AS status, "
		"duplicate_count, "
		"total_count, "
		+ max_val + " AS threshold, "
		"CASE "
		"  WHEN total_count = 0 THEN 'Table is empty (0 rows); duplicate check passed trivially' "
		"  WHEN duplicate_count <= " + max_val + " THEN "
		"    CAST(duplicate_count AS VARCHAR) || ' duplicate(s) in ' || CAST(total_count AS VARCHAR) || "
		"    ' rows is acceptable' "
		"  ELSE "
		"    CAST(duplicate_count AS VARCHAR) || ' duplicate(s) in ' || CAST(total_count AS VARCHAR) || "
		"    ' rows exceeds maximum ' || CAST(" + max_val + " AS VARCHAR) "
		"END AS message "
		"FROM (SELECT "
		"  COUNT(*) - COUNT(DISTINCT " + distinct_expr + ") AS duplicate_count, "
		"  COUNT(*) AS total_count "
		"FROM " + table_ref + ")";
}

// Helper: Generate SQL for occurrence checks (highest or lowest frequency of any single value).
// mode_keyword must come from the bind-time whitelist ('max' or 'min').
// Empty-input semantics: an empty table or all-NULL column yields occurrence = NULL and
// passes trivially with an explicit message.
static string GenerateOccurrenceSQL(const string &table_ref, const string &column_name, const string &mode_keyword,
                                    const Value &lower, const Value &upper) {
	string lower_val = lower.IsNull() ? "NULL" : lower.ToString();
	string upper_val = upper.IsNull() ? "NULL" : upper.ToString();
	string col_id = QuoteSqlIdentifier(column_name);
	bool is_max = mode_keyword == "max";
	string extreme_agg = is_max ? "MAX(c)" : "MIN(c)";
	string extreme_val_agg = is_max ? "arg_max(val, c)" : "arg_min(val, c)";

	return "WITH counts AS ("
		"SELECT CAST(" + col_id + " AS VARCHAR) AS val, COUNT(*) AS c "
		"FROM " + table_ref + " WHERE " + col_id + " IS NOT NULL GROUP BY 1"
		"), "
		"summary AS ("
		"SELECT " + extreme_agg + " AS occurrence, " + extreme_val_agg + " AS extreme_value, "
		"COUNT(*) AS distinct_values FROM counts"
		") "
		"SELECT "
		"CASE "
		"  WHEN occurrence IS NULL THEN 'pass' "
		"  WHEN (" + lower_val + " IS NOT NULL AND occurrence < " + lower_val + ") OR "
		"       (" + upper_val + " IS NOT NULL AND occurrence > " + upper_val + ") THEN 'fail' "
		"  ELSE 'pass' "
		"END AS status, "
		"'" + mode_keyword + "' AS mode, "
		"occurrence, "
		"extreme_value, "
		"distinct_values, "
		+ lower_val + " AS lower_threshold, "
		+ upper_val + " AS upper_threshold, "
		"CASE "
		"  WHEN occurrence IS NULL THEN 'No non-NULL values to evaluate; occurrence check passed trivially' "
		"  WHEN (" + lower_val + " IS NOT NULL AND occurrence < " + lower_val + ") THEN "
		"    '" + mode_keyword + " occurrence ' || CAST(occurrence AS VARCHAR) || ' (value: ' || extreme_value || "
		"    ') is below minimum ' || CAST(" + lower_val + " AS VARCHAR) "
		"  WHEN (" + upper_val + " IS NOT NULL AND occurrence > " + upper_val + ") THEN "
		"    '" + mode_keyword + " occurrence ' || CAST(occurrence AS VARCHAR) || ' (value: ' || extreme_value || "
		"    ') exceeds maximum ' || CAST(" + upper_val + " AS VARCHAR) "
		"  ELSE '" + mode_keyword + " occurrence ' || CAST(occurrence AS VARCHAR) || ' (value: ' || extreme_value || "
		"    ') is within acceptable range' "
		"END AS message "
		"FROM summary";
}

// Helper: Generate SQL for cross-table match rate checks (referential integrity).
// The right side is deduplicated on its key columns so join fan-out cannot inflate the rate.
// Empty-input semantics: an empty left table yields match_rate = 0.0, total_count = 0 and
// passes trivially with an explicit message.
static string GenerateMatchRateSQL(const string &left_ref, const string &right_ref,
                                   const vector<string> &left_keys, const vector<string> &right_keys,
                                   const Value &min_rate) {
	string min_val = min_rate.IsNull() ? "1.0" : min_rate.ToString();

	string left_cols;
	string right_cols;
	string join_cond;
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

	return "WITH l AS ("
		"SELECT " + left_cols + " FROM " + left_ref +
		"), "
		"r AS ("
		"SELECT DISTINCT " + right_cols + ", 1 AS matched FROM " + right_ref +
		"), "
		"joined AS ("
		"SELECT r.matched FROM l LEFT JOIN r ON " + join_cond +
		"), "
		"stats AS ("
		"SELECT "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(COUNT(matched) AS BIGINT), 0) AS matched_count, "
		"  COALESCE(CAST(COUNT(matched) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS match_rate "
		"FROM joined"
		") "
		"SELECT "
		"CASE "
		"  WHEN total_count = 0 THEN 'pass' "
		"  WHEN match_rate >= " + min_val + " THEN 'pass' "
		"  ELSE 'fail' "
		"END AS status, "
		"match_rate, "
		"matched_count, "
		"total_count, "
		+ min_val + " AS threshold, "
		"CASE "
		"  WHEN total_count = 0 THEN 'Left table is empty (0 rows); match rate check passed trivially' "
		"  WHEN match_rate >= " + min_val + " THEN "
		"    'Match rate ' || CAST(match_rate AS VARCHAR) || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'Match rate ' || CAST(match_rate AS VARCHAR) || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"END AS message "
		"FROM stats";
}

// Helper: Generate SQL for custom-predicate compliance checks.
// expression is caller-supplied SQL validated by ValidateBooleanExpression at bind time;
// rows where the predicate evaluates to NULL count as non-compliant.
// Empty-input semantics: an empty table yields compliance_rate = 0.0, total_count = 0 and
// passes trivially with an explicit message.
static string GenerateComplianceSQL(const string &table_ref, const string &expression, const Value &min_rate) {
	string min_val = min_rate.IsNull() ? "1.0" : min_rate.ToString();
	string predicate = "COALESCE((" + expression + "), FALSE)";

	return "SELECT "
		"CASE "
		"  WHEN total_count = 0 THEN 'pass' "
		"  WHEN compliance_rate >= " + min_val + " THEN 'pass' "
		"  ELSE 'fail' "
		"END AS status, "
		"compliance_rate, "
		"compliant_count, "
		"total_count, "
		+ min_val + " AS threshold, "
		"CASE "
		"  WHEN total_count = 0 THEN 'Table is empty (0 rows); compliance check passed trivially' "
		"  WHEN compliance_rate >= " + min_val + " THEN "
		"    'Compliance rate ' || CAST(compliance_rate AS VARCHAR) || ' (' || CAST(compliant_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'Compliance rate ' || CAST(compliance_rate AS VARCHAR) || ' (' || CAST(compliant_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"END AS message "
		"FROM (SELECT "
		"  COALESCE(CAST(SUM(CASE WHEN " + predicate + " THEN 1 ELSE 0 END) AS BIGINT), 0) AS compliant_count, "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(SUM(CASE WHEN " + predicate + " THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS compliance_rate "
		"FROM " + table_ref + ")";
}

// Helper: format an optional DATE parameter as a SQL literal (NULL keeps the
// documented default of "latest date present in the data").
static string BuildDateLiteral(const Value &date_value) {
	if (date_value.IsNull()) {
		return "NULL";
	}
	return "DATE '" + EscapeSqlStringLiteral(date_value.ToString()) + "'";
}

// Helper: Generate SQL for relative count change checks.
// Compares the (distinct) count on the reference date against the average over the
// window_days days strictly before it; rel_change = (ref - baseline) / baseline.
// Trivial-pass semantics: empty input, missing baseline days or a zero baseline pass
// with an explicit message (not enough history is not a failure).
static string GenerateRelCountChangeSQL(const string &table_ref, const string &date_column,
                                        const string &count_column, int64_t window_days,
                                        const Value &lower, const Value &upper, const Value &reference_date) {
	string lower_val = lower.IsNull() ? "NULL" : lower.ToString();
	string upper_val = upper.IsNull() ? "NULL" : upper.ToString();
	string date_id = QuoteSqlIdentifier(date_column);
	string window_val = std::to_string(window_days);
	string ref_lit = BuildDateLiteral(reference_date);
	string cnt_expr = count_column.empty() ? "COUNT(*)" : "COUNT(DISTINCT " + QuoteSqlIdentifier(count_column) + ")";

	return "WITH daily AS ("
		"SELECT CAST(" + date_id + " AS DATE) AS d, " + cnt_expr + " AS c "
		"FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL GROUP BY 1"
		"), "
		"ref AS (SELECT COALESCE(" + ref_lit + ", MAX(d)) AS ref_d FROM daily), "
		"refstats AS ("
		"SELECT r.ref_d, "
		"  (SELECT c FROM daily WHERE d = r.ref_d) AS ref_c, "
		"  (SELECT AVG(c) FROM daily WHERE d < r.ref_d AND d >= r.ref_d - " + window_val + ") AS baseline_avg, "
		"  (SELECT COUNT(*) FROM daily WHERE d < r.ref_d AND d >= r.ref_d - " + window_val + ") AS baseline_days "
		"FROM ref r"
		"), "
		"summary AS ("
		"SELECT ref_d, COALESCE(ref_c, 0) AS ref_c, baseline_avg, baseline_days, "
		"  CASE WHEN baseline_avg IS NULL OR baseline_avg = 0 THEN NULL "
		"       ELSE (COALESCE(ref_c, 0) - baseline_avg) / baseline_avg END AS rel_change "
		"FROM refstats"
		") "
		"SELECT "
		"CASE "
		"  WHEN ref_d IS NULL OR baseline_days = 0 OR rel_change IS NULL THEN 'pass' "
		"  WHEN (" + lower_val + " IS NOT NULL AND rel_change < " + lower_val + ") OR "
		"       (" + upper_val + " IS NOT NULL AND rel_change > " + upper_val + ") THEN 'fail' "
		"  ELSE 'pass' "
		"END AS status, "
		"ref_d AS reference_date, "
		"CAST(ref_c AS DOUBLE) AS reference_count, "
		"CAST(baseline_avg AS DOUBLE) AS baseline_avg, "
		"baseline_days, "
		"CAST(rel_change AS DOUBLE) AS rel_change, "
		+ lower_val + " AS lower_threshold, "
		+ upper_val + " AS upper_threshold, "
		"CASE "
		"  WHEN ref_d IS NULL THEN 'No dates to evaluate (table is empty or date column is all NULL)' "
		"  WHEN baseline_days = 0 THEN 'No baseline days before ' || CAST(ref_d AS VARCHAR) || '; check passed trivially' "
		"  WHEN rel_change IS NULL THEN 'Baseline average is 0 before ' || CAST(ref_d AS VARCHAR) || '; relative change undefined, check passed trivially' "
		"  WHEN (" + lower_val + " IS NOT NULL AND rel_change < " + lower_val + ") OR "
		"       (" + upper_val + " IS NOT NULL AND rel_change > " + upper_val + ") THEN "
		"    'Count ' || CAST(ref_c AS VARCHAR) || ' on ' || CAST(ref_d AS VARCHAR) || ' deviates by ' || "
		"    CAST(rel_change AS VARCHAR) || ' from baseline average ' || CAST(baseline_avg AS VARCHAR) "
		"  ELSE "
		"    'Count ' || CAST(ref_c AS VARCHAR) || ' on ' || CAST(ref_d AS VARCHAR) || ' is within ' || "
		"    CAST(rel_change AS VARCHAR) || ' of baseline average ' || CAST(baseline_avg AS VARCHAR) "
		"END AS message "
		"FROM summary";
}

// Helper: Generate SQL for metric-level IQR anomaly detection.
// Computes a daily metric (row count or AVG of metric_column) over the window_days days
// strictly before the reference date, derives Q1/Q3 bounds with multiplier k, and flags
// the reference day when it falls outside the bounds selected by mode.
// Trivial-pass semantics: empty input or an empty baseline window pass with a message.
static string GenerateMetricAnomalyIQRSQL(const string &table_ref, const string &date_column,
                                          const string &metric_column, int64_t window_days, const Value &k,
                                          const string &mode, const Value &reference_date) {
	string k_val = k.IsNull() ? "1.5" : k.ToString();
	string date_id = QuoteSqlIdentifier(date_column);
	string window_val = std::to_string(window_days);
	string ref_lit = BuildDateLiteral(reference_date);
	string metric_expr =
	    metric_column.empty() ? "CAST(COUNT(*) AS DOUBLE)" : "AVG(CAST(" + QuoteSqlIdentifier(metric_column) + " AS DOUBLE))";

	string lower_check = "metric_value < lower_bound";
	string upper_check = "metric_value > upper_bound";
	string fail_check;
	if (mode == "upper") {
		fail_check = upper_check;
	} else if (mode == "lower") {
		fail_check = lower_check;
	} else {
		fail_check = "(" + lower_check + " OR " + upper_check + ")";
	}

	return "WITH daily AS ("
		"SELECT CAST(" + date_id + " AS DATE) AS d, " + metric_expr + " AS m "
		"FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL GROUP BY 1"
		"), "
		"ref AS (SELECT COALESCE(" + ref_lit + ", MAX(d)) AS ref_d FROM daily), "
		"w AS (SELECT m FROM daily, ref WHERE d < ref_d AND d >= ref_d - " + window_val + "), "
		"stats AS (SELECT QUANTILE_CONT(m, 0.25) AS q1, QUANTILE_CONT(m, 0.75) AS q3, COUNT(*) AS n FROM w), "
		"summary AS ("
		"SELECT r.ref_d AS reference_date, "
		"  (SELECT m FROM daily WHERE d = r.ref_d) AS metric_value, "
		"  s.q1, s.q3, s.n AS baseline_days, "
		"  s.q1 - " + k_val + " * (s.q3 - s.q1) AS lower_bound, "
		"  s.q3 + " + k_val + " * (s.q3 - s.q1) AS upper_bound "
		"FROM ref r, stats s"
		") "
		"SELECT "
		"CASE "
		"  WHEN reference_date IS NULL OR metric_value IS NULL OR baseline_days = 0 THEN 'pass' "
		"  WHEN " + fail_check + " THEN 'fail' "
		"  ELSE 'pass' "
		"END AS status, "
		"reference_date, "
		"CAST(metric_value AS DOUBLE) AS metric_value, "
		"q1, q3, lower_bound, upper_bound, baseline_days, "
		"'" + mode + "' AS mode, "
		"CASE "
		"  WHEN reference_date IS NULL THEN 'No dates to evaluate (table is empty or date column is all NULL)' "
		"  WHEN metric_value IS NULL THEN 'No metric value on ' || CAST(reference_date AS VARCHAR) || '; check passed trivially' "
		"  WHEN baseline_days = 0 THEN 'No baseline window before ' || CAST(reference_date AS VARCHAR) || '; check passed trivially' "
		"  WHEN " + fail_check + " THEN "
		"    'Metric ' || CAST(metric_value AS VARCHAR) || ' on ' || CAST(reference_date AS VARCHAR) || "
		"    ' is outside IQR bounds [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || '] (mode: " + mode + ")' "
		"  ELSE "
		"    'Metric ' || CAST(metric_value AS VARCHAR) || ' on ' || CAST(reference_date AS VARCHAR) || "
		"    ' is within IQR bounds [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || ']' "
		"END AS message "
		"FROM summary";
}

// Helper: Generate SQL for rolling values-in-set checks.
// Same rate computation as values_in_set, restricted to the trailing window_days days
// ending at the reference date (inclusive).
static string GenerateRollingValuesInSetSQL(const string &table_ref, const string &column_name,
                                            const string &allowed_array, const string &date_column,
                                            int64_t window_days, const Value &min_rate,
                                            const Value &reference_date) {
	string min_val = min_rate.IsNull() ? "1.0" : min_rate.ToString();
	string col_id = QuoteSqlIdentifier(column_name);
	string col_str = "CAST(" + col_id + " AS VARCHAR)";
	string date_id = QuoteSqlIdentifier(date_column);
	string window_val = std::to_string(window_days);
	string ref_lit = BuildDateLiteral(reference_date);

	return "WITH ref AS ("
		"SELECT COALESCE(" + ref_lit + ", MAX(CAST(" + date_id + " AS DATE))) AS ref_d FROM " + table_ref +
		"), "
		"win AS ("
		"SELECT " + col_str + " AS v FROM " + table_ref + ", ref "
		"WHERE " + col_id + " IS NOT NULL AND " + date_id + " IS NOT NULL "
		"AND CAST(" + date_id + " AS DATE) > ref_d - " + window_val + " AND CAST(" + date_id + " AS DATE) <= ref_d"
		"), "
		"stats AS ("
		"SELECT "
		"  COALESCE(CAST(SUM(CASE WHEN list_contains(" + allowed_array + ", v) THEN 1 ELSE 0 END) AS BIGINT), 0) AS in_set_count, "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(SUM(CASE WHEN list_contains(" + allowed_array + ", v) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS in_set_rate "
		"FROM win"
		") "
		"SELECT "
		"CASE "
		"  WHEN total_count = 0 THEN 'pass' "
		"  WHEN in_set_rate >= " + min_val + " THEN 'pass' "
		"  ELSE 'fail' "
		"END AS status, "
		"(SELECT ref_d FROM ref) AS reference_date, "
		"in_set_rate, "
		"in_set_count, "
		"total_count, "
		+ min_val + " AS threshold, "
		"(SELECT COALESCE(array_agg(v), []::VARCHAR[]) FROM ("
		"  SELECT DISTINCT w.v FROM win w WHERE NOT list_contains(" + allowed_array + ", w.v) LIMIT 5"
		")) AS sample_violations, "
		"CASE "
		"  WHEN total_count = 0 THEN 'No values in the trailing window; rolling values-in-set check passed trivially' "
		"  WHEN in_set_rate >= " + min_val + " THEN "
		"    'In-set rate ' || CAST(in_set_rate AS VARCHAR) || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') over the trailing " + window_val + " day(s) is acceptable' "
		"  ELSE "
		"    'In-set rate ' || CAST(in_set_rate AS VARCHAR) || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') over the trailing " + window_val + " day(s) is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"END AS message "
		"FROM stats";
}

// Helper: read an optional positive window_days argument with a default
static int64_t ReadWindowDays(TableFunctionBindInput &input, idx_t index, int64_t default_days,
                              const string &function_name) {
	if (input.inputs.size() <= index || input.inputs[index].IsNull()) {
		return default_days;
	}
	int64_t window_days = input.inputs[index].GetValue<int64_t>();
	if (window_days < 1) {
		throw BinderException(function_name + ": window_days must be >= 1");
	}
	return window_days;
}

//===--------------------------------------------------------------------===//
// Check Bind Replace Functions
//===--------------------------------------------------------------------===//

static unique_ptr<TableRef> CheckRegexMatchBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_regex_match");
	if (input.inputs.size() < 4) {
		throw BinderException(
		    "anofox_tab_regex_match requires at least 4 arguments: table_name, column_name, pattern, min_match_rate");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	if (input.inputs[2].IsNull()) {
		throw BinderException("regex_match: pattern must not be NULL");
	}
	string pattern = input.inputs[2].ToString();
	ValidateFiniteDouble(input.inputs[3], "regex_match", "min_match_rate");
	Value max_rate = (input.inputs.size() > 4) ? input.inputs[4] : Value();
	ValidateFiniteDouble(max_rate, "regex_match", "max_match_rate");

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateRegexMatchSQL(table_ref, column_name, pattern, input.inputs[3], max_rate);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse regex match check query");
}

static unique_ptr<TableRef> CheckValuesInSetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_values_in_set");
	if (input.inputs.size() < 3) {
		throw BinderException(
		    "anofox_tab_values_in_set requires at least 3 arguments: table_name, column_name, allowed_values");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	string allowed_array = BuildVarcharArrayLiteral(input.inputs[2]);
	Value min_rate = (input.inputs.size() > 3) ? input.inputs[3] : Value();
	ValidateFiniteDouble(min_rate, "values_in_set", "min_rate");

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateValuesInSetSQL(table_ref, column_name, allowed_array, min_rate);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse values-in-set check query");
}

static unique_ptr<TableRef> CheckAggBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_agg");
	if (input.inputs.size() < 5) {
		throw BinderException(
		    "anofox_tab_agg_check requires 5 arguments: table_name, column_name, agg, lower_threshold, upper_threshold");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	string agg = input.inputs[2].IsNull() ? "" : input.inputs[2].ToString();
	std::transform(agg.begin(), agg.end(), agg.begin(), ::tolower);
	// Whitelist: the aggregate keyword is spliced into SQL, so it must never come from raw user input
	static const vector<string> ALLOWED_AGGS = {"avg", "min", "max", "sum", "median", "stddev"};
	if (std::find(ALLOWED_AGGS.begin(), ALLOWED_AGGS.end(), agg) == ALLOWED_AGGS.end()) {
		throw BinderException("agg_check: agg must be one of 'avg', 'min', 'max', 'sum', 'median', 'stddev'");
	}
	ValidateFiniteDouble(input.inputs[3], "agg_check", "lower_threshold");
	ValidateFiniteDouble(input.inputs[4], "agg_check", "upper_threshold");

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateAggCheckSQL(table_ref, column_name, agg, input.inputs[3], input.inputs[4]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse aggregate check query");
}

static unique_ptr<TableRef> CheckDuplicateCountBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_duplicate_count");
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_duplicate_count requires at least 2 arguments: table_name, column_names");
	}

	string table_name = input.inputs[0].ToString();
	auto column_names = ParseCommaSeparatedColumns(input.inputs[1].ToString());
	if (column_names.empty()) {
		throw BinderException("duplicate_count: column_names must contain at least one column");
	}
	Value max_duplicates = (input.inputs.size() > 2) ? input.inputs[2] : Value();
	if (!max_duplicates.IsNull() && max_duplicates.GetValue<int64_t>() < 0) {
		throw BinderException("duplicate_count: max_duplicates must be >= 0");
	}

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateDuplicateCountSQL(table_ref, column_names, max_duplicates);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse duplicate count check query");
}

static unique_ptr<TableRef> CheckOccurrenceBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_occurrence");
	if (input.inputs.size() < 5) {
		throw BinderException(
		    "anofox_tab_occurrence requires 5 arguments: table_name, column_name, mode, lower_threshold, upper_threshold");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	string mode = input.inputs[2].IsNull() ? "" : input.inputs[2].ToString();
	std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
	if (mode != "max" && mode != "min") {
		throw BinderException("occurrence: mode must be 'max' or 'min'");
	}

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateOccurrenceSQL(table_ref, column_name, mode, input.inputs[3], input.inputs[4]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse occurrence check query");
}

static unique_ptr<TableRef> CheckMatchRateBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_match_rate");
	if (input.inputs.size() < 4) {
		throw BinderException(
		    "anofox_tab_match_rate requires at least 4 arguments: left_table, right_table, left_keys, right_keys");
	}

	string left_table = input.inputs[0].ToString();
	string right_table = input.inputs[1].ToString();
	auto left_keys = ParseCommaSeparatedColumns(input.inputs[2].ToString());
	auto right_keys = ParseCommaSeparatedColumns(input.inputs[3].ToString());
	if (left_keys.empty()) {
		throw BinderException("match_rate: left_keys must contain at least one column");
	}
	if (left_keys.size() != right_keys.size()) {
		throw BinderException("match_rate: left_keys and right_keys must have the same number of columns (" +
		                      std::to_string(left_keys.size()) + " vs " + std::to_string(right_keys.size()) + ")");
	}
	Value min_rate = (input.inputs.size() > 4) ? input.inputs[4] : Value();
	ValidateFiniteDouble(min_rate, "match_rate", "min_rate");

	AnofoxTrace(AnofoxLogLevel::Debug, "check: match_rate left='" + left_table + "', right='" + right_table +
	                                       "', keys=" + std::to_string(left_keys.size()));
	string sql = GenerateMatchRateSQL(BuildQueryTableRef(left_table), BuildQueryTableRef(right_table), left_keys,
	                                  right_keys, min_rate);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse match rate check query");
}

static unique_ptr<TableRef> CheckComplianceBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_compliance");
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_compliance requires at least 2 arguments: table_name, expression");
	}

	string table_name = input.inputs[0].ToString();
	if (input.inputs[1].IsNull()) {
		throw BinderException("compliance: expression must not be NULL");
	}
	string expression = input.inputs[1].ToString();
	ValidateBooleanExpression(context, expression, "compliance");
	Value min_rate = (input.inputs.size() > 2) ? input.inputs[2] : Value();
	ValidateFiniteDouble(min_rate, "compliance", "min_rate");

	AnofoxTrace(AnofoxLogLevel::Debug, "check: compliance table='" + table_name + "', expression='" + expression + "'");
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateComplianceSQL(table_ref, expression, min_rate);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse compliance check query");
}

static unique_ptr<TableRef> CheckRelCountChangeBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_rel_count_change");
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_rel_count_change requires at least 2 arguments: table_name, date_column");
	}

	string table_name = input.inputs[0].ToString();
	string date_column = input.inputs[1].ToString();
	string count_column;
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		count_column = input.inputs[2].ToString();
	}
	int64_t window_days = ReadWindowDays(input, 3, 7, "rel_count_change");
	// Defaults apply only when the argument is absent; an explicit NULL means unbounded
	Value lower = (input.inputs.size() > 4) ? input.inputs[4] : Value::DOUBLE(-0.5);
	Value upper = (input.inputs.size() > 5) ? input.inputs[5] : Value::DOUBLE(0.5);
	ValidateFiniteDouble(lower, "rel_count_change", "lower_threshold");
	ValidateFiniteDouble(upper, "rel_count_change", "upper_threshold");
	Value reference_date = (input.inputs.size() > 6) ? input.inputs[6] : Value();

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateRelCountChangeSQL(table_ref, date_column, count_column, window_days, lower, upper,
	                                       reference_date);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse relative count change check query");
}

static unique_ptr<TableRef> CheckMetricAnomalyIQRBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_metric_anomaly_iqr");
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_metric_anomaly_iqr requires at least 2 arguments: table_name, date_column");
	}

	string table_name = input.inputs[0].ToString();
	string date_column = input.inputs[1].ToString();
	string metric_column;
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		metric_column = input.inputs[2].ToString();
	}
	int64_t window_days = ReadWindowDays(input, 3, 30, "metric_anomaly_iqr");
	Value k = (input.inputs.size() > 4) ? input.inputs[4] : Value();
	ValidateFiniteDouble(k, "metric_anomaly_iqr", "k");
	string mode = "both";
	if (input.inputs.size() > 5 && !input.inputs[5].IsNull()) {
		mode = input.inputs[5].ToString();
		std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
	}
	if (mode != "both" && mode != "upper" && mode != "lower") {
		throw BinderException("metric_anomaly_iqr: mode must be 'both', 'upper' or 'lower'");
	}
	Value reference_date = (input.inputs.size() > 6) ? input.inputs[6] : Value();

	string table_ref = BuildQueryTableRef(table_name);
	string sql =
	    GenerateMetricAnomalyIQRSQL(table_ref, date_column, metric_column, window_days, k, mode, reference_date);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse metric anomaly IQR check query");
}

static unique_ptr<TableRef> CheckRollingValuesInSetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_rolling_values_in_set");
	if (input.inputs.size() < 4) {
		throw BinderException("anofox_tab_rolling_values_in_set requires at least 4 arguments: "
		                      "table_name, column_name, allowed_values, date_column");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	string allowed_array = BuildVarcharArrayLiteral(input.inputs[2]);
	string date_column = input.inputs[3].ToString();
	int64_t window_days = ReadWindowDays(input, 4, 7, "rolling_values_in_set");
	Value min_rate = (input.inputs.size() > 5) ? input.inputs[5] : Value();
	ValidateFiniteDouble(min_rate, "rolling_values_in_set", "min_rate");
	Value reference_date = (input.inputs.size() > 6) ? input.inputs[6] : Value();

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateRollingValuesInSetSQL(table_ref, column_name, allowed_array, date_column, window_days,
	                                           min_rate, reference_date);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse rolling values-in-set check query");
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterCheckFunctions(ExtensionLoader &loader) {
	// Check table functions using the bind_replace pattern: each expands into a
	// generated SQL query returning a single assertion-style summary row.

	// anofox_tab_regex_match(table_name, column_name, pattern, min_match_rate [, max_match_rate])
	// (alias: regex_match)
	const vector<LogicalType> regex_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::DOUBLE)};
	TableFunctionSet regex_set("anofox_tab_regex_match");
	AddPrefixAritiesBindReplace(regex_set, "anofox_tab_regex_match", regex_args, 4,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckRegexMatchBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the share of non-NULL values matching a regex pattern is between min_match_rate and max_match_rate.";
		desc.parameter_names = {"table_name", "column_name", "pattern", "min_match_rate", "max_match_rate"};
		desc.examples = {"SELECT * FROM regex_match('users', 'email', '^[^@]+@[^@]+$', 0.99);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, regex_set, "regex_match", {std::move(desc)});
	}

	// anofox_tab_values_in_set(table_name, column_name, allowed_values [, min_rate=1.0]) (alias: values_in_set)
	const vector<LogicalType> in_set_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)), LogicalType(LogicalTypeId::DOUBLE)};
	TableFunctionSet in_set_set("anofox_tab_values_in_set");
	AddPrefixAritiesBindReplace(in_set_set, "anofox_tab_values_in_set", in_set_args, 3,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckValuesInSetBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the share of non-NULL values contained in the allowed value set is at least min_rate. "
		    "Reports up to 5 sample violations.";
		desc.parameter_names = {"table_name", "column_name", "allowed_values", "min_rate"};
		desc.examples = {"SELECT * FROM values_in_set('orders', 'status', ['completed', 'pending', 'failed']);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, in_set_set, "values_in_set", {std::move(desc)});
	}

	// anofox_tab_agg_check(table_name, column_name, agg, lower_threshold, upper_threshold) (alias: agg_check)
	TableFunction agg_func("anofox_tab_agg_check",
	                       {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                        LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE),
	                        LogicalType(LogicalTypeId::DOUBLE)},
	                       nullptr, nullptr);
	agg_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckAggBindReplace);
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that an aggregate ('avg', 'min', 'max', 'sum', 'median', 'stddev') of a numeric column is between "
		    "lower_threshold and upper_threshold (NULL = unbounded).";
		desc.parameter_names = {"table_name", "column_name", "agg", "lower_threshold", "upper_threshold"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE,
		                        LogicalType::DOUBLE};
		desc.examples = {"SELECT * FROM agg_check('orders', 'amount', 'avg', 10.0, 500.0);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, agg_func, "agg_check", {std::move(desc)});
	}

	// anofox_tab_duplicate_count(table_name, column_names [, max_duplicates=0]) (alias: duplicate_count)
	const vector<LogicalType> dup_args = {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                      LogicalType(LogicalTypeId::BIGINT)};
	TableFunctionSet dup_set("anofox_tab_duplicate_count");
	AddPrefixAritiesBindReplace(dup_set, "anofox_tab_duplicate_count", dup_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckDuplicateCountBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the number of duplicate values in a column (or comma-separated column combination) does not "
		    "exceed max_duplicates (default 0).";
		desc.parameter_names = {"table_name", "column_names", "max_duplicates"};
		desc.examples = {"SELECT * FROM duplicate_count('orders', 'order_id');",
		                 "SELECT * FROM duplicate_count('orders', 'customer_id,order_date', 10);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, dup_set, "duplicate_count", {std::move(desc)});
	}

	// anofox_tab_occurrence(table_name, column_name, mode, lower_threshold, upper_threshold) (alias: occurrence)
	TableFunction occurrence_func("anofox_tab_occurrence",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::BIGINT)},
	                              nullptr, nullptr);
	occurrence_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckOccurrenceBindReplace);
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the highest ('max') or lowest ('min') frequency of any single value in a column is between "
		    "lower_threshold and upper_threshold (NULL = unbounded).";
		desc.parameter_names = {"table_name", "column_name", "mode", "lower_threshold", "upper_threshold"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT,
		                        LogicalType::BIGINT};
		desc.examples = {"SELECT * FROM occurrence('orders', 'customer_id', 'max', NULL, 100);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, occurrence_func, "occurrence", {std::move(desc)});
	}

	// anofox_tab_match_rate(left_table, right_table, left_keys, right_keys [, min_rate=1.0]) (alias: match_rate)
	const vector<LogicalType> match_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)};
	TableFunctionSet match_set("anofox_tab_match_rate");
	AddPrefixAritiesBindReplace(match_set, "anofox_tab_match_rate", match_args, 4,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckMatchRateBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the share of left-table rows with a join partner in the right table (comma-separated key "
		    "columns) is at least min_rate. Doubles as a referential-integrity check.";
		desc.parameter_names = {"left_table", "right_table", "left_keys", "right_keys", "min_rate"};
		desc.examples = {"SELECT * FROM match_rate('orders', 'customers', 'customer_id', 'id', 1.0);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, match_set, "match_rate", {std::move(desc)});
	}

	// anofox_tab_compliance(table_name, expression [, min_rate=1.0]) (alias: compliance)
	const vector<LogicalType> compliance_args = {LogicalType(LogicalTypeId::VARCHAR),
	                                             LogicalType(LogicalTypeId::VARCHAR),
	                                             LogicalType(LogicalTypeId::DOUBLE)};
	TableFunctionSet compliance_set("anofox_tab_compliance");
	AddPrefixAritiesBindReplace(compliance_set, "anofox_tab_compliance", compliance_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckComplianceBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the share of rows satisfying a SQL boolean expression is at least min_rate. Rows where the "
		    "expression is NULL count as non-compliant. The expression is executed with the caller's privileges.";
		desc.parameter_names = {"table_name", "expression", "min_rate"};
		desc.examples = {"SELECT * FROM compliance('orders', 'amount > 0 AND status IS NOT NULL', 0.95);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, compliance_set, "compliance", {std::move(desc)});
	}

	// anofox_tab_rel_count_change(table_name, date_column [, count_column, window_days=7,
	//                             lower_threshold=-0.5, upper_threshold=0.5, reference_date])
	// (alias: rel_count_change)
	const vector<LogicalType> rel_count_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::DOUBLE),  LogicalType(LogicalTypeId::DOUBLE),
	    LogicalType(LogicalTypeId::DATE)};
	TableFunctionSet rel_count_set("anofox_tab_rel_count_change");
	AddPrefixAritiesBindReplace(rel_count_set, "anofox_tab_rel_count_change", rel_count_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckRelCountChangeBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the daily (distinct) count on the reference date deviates from the rolling window average "
		    "by a relative change within [lower_threshold, upper_threshold]. Defaults: count rows, 7-day window, "
		    "reference = latest date in the data.";
		desc.parameter_names = {"table_name", "date_column",     "count_column",   "window_days",
		                        "lower_threshold", "upper_threshold", "reference_date"};
		desc.examples = {"SELECT * FROM rel_count_change('orders', 'order_date', NULL, 7, -0.5, 0.5);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, rel_count_set, "rel_count_change", {std::move(desc)});
	}

	// anofox_tab_metric_anomaly_iqr(table_name, date_column [, metric_column, window_days=30, k=1.5,
	//                               mode='both', reference_date]) (alias: metric_anomaly_iqr)
	const vector<LogicalType> metric_iqr_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::DOUBLE),  LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::DATE)};
	TableFunctionSet metric_iqr_set("anofox_tab_metric_anomaly_iqr");
	AddPrefixAritiesBindReplace(metric_iqr_set, "anofox_tab_metric_anomaly_iqr", metric_iqr_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckMetricAnomalyIQRBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Flags the reference date as anomalous when its daily metric (row count or AVG of metric_column) falls "
		    "outside Q1/Q3 +/- k*IQR of the trailing window. Mode 'both', 'upper' or 'lower' selects the bounds.";
		desc.parameter_names = {"table_name", "date_column", "metric_column", "window_days",
		                        "k",          "mode",        "reference_date"};
		desc.examples = {"SELECT * FROM metric_anomaly_iqr('orders', 'order_date', NULL, 30, 1.5, 'both');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, metric_iqr_set, "metric_anomaly_iqr", {std::move(desc)});
	}

	// anofox_tab_rolling_values_in_set(table_name, column_name, allowed_values, date_column
	//                                  [, window_days=7, min_rate=1.0, reference_date])
	// (alias: rolling_values_in_set)
	const vector<LogicalType> rolling_set_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)), LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::DATE)};
	TableFunctionSet rolling_set_set("anofox_tab_rolling_values_in_set");
	AddPrefixAritiesBindReplace(rolling_set_set, "anofox_tab_rolling_values_in_set", rolling_set_args, 4,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckRollingValuesInSetBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the share of values contained in the allowed set over the trailing window_days days "
		    "(ending at the reference date, inclusive) is at least min_rate.";
		desc.parameter_names = {"table_name",  "column_name", "allowed_values", "date_column",
		                        "window_days", "min_rate",    "reference_date"};
		desc.examples = {
		    "SELECT * FROM rolling_values_in_set('orders', 'status', ['completed', 'pending'], 'order_date', 14);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, rolling_set_set, "rolling_values_in_set", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

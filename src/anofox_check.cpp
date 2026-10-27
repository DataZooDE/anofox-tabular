#include "anofox_check.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb_compat.hpp"
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
		TableFunction func(duckdb_compat::FunctionName(name), std::move(args), nullptr, nullptr);
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
	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = ParseWithContextSettings(context, "SELECT (" + expr + ")");
	} catch (const std::exception &ex) {
		throw BinderException(function_name + ": failed to parse expression '" + expr + "': " + ex.what());
	}
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
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
	string min_val = DoubleThresholdSQL(min_rate);
	string max_val = DoubleThresholdSQL(max_rate);
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
		"    'Match rate ' || " + RoundedNumberSQL("match_rate") + " || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"  WHEN (" + max_val + " IS NOT NULL AND match_rate > " + max_val + ") THEN "
		"    'Match rate ' || " + RoundedNumberSQL("match_rate") + " || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') exceeds maximum ' || CAST(" + max_val + " AS VARCHAR) "
		"  ELSE 'Match rate ' || " + RoundedNumberSQL("match_rate") + " || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
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
	string min_val = DoubleThresholdSQL(min_rate, "1.0");
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
		"    'In-set rate ' || " + RoundedNumberSQL("in_set_rate") + " || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'In-set rate ' || " + RoundedNumberSQL("in_set_rate") + " || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"END AS message "
		"FROM (SELECT "
		"  COALESCE(CAST(SUM(CASE WHEN list_contains(" + allowed_array + ", " + col_str + ") THEN 1 ELSE 0 END) AS BIGINT), 0) AS in_set_count, "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(SUM(CASE WHEN list_contains(" + allowed_array + ", " + col_str + ") THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS in_set_rate "
		"FROM " + table_ref + " WHERE " + col_id + " IS NOT NULL)";
}

// AGG(double_expr) over the finite values only (STDDEV and friends raise "out of range" on
// NaN/Inf); when the input holds any NaN/Inf value the result is NaN, so the check cannot
// pass on data it could not evaluate.
string BuildFiniteAggregateExpr(const string &agg_upper, const string &double_expr) {
	return "CASE WHEN " + BuildNonFiniteCountExpr(double_expr) + " > 0 THEN CAST('nan' AS DOUBLE) ELSE CAST(" +
	       agg_upper + "(" + double_expr + ") FILTER (WHERE isfinite(" + double_expr + ")) AS DOUBLE) END";
}

string BuildNonFiniteCountExpr(const string &double_expr) {
	return "COUNT(*) FILTER (WHERE NOT isfinite(" + double_expr + "))";
}

// Helper: Generate SQL for aggregate assertions (avg/min/max/sum/median/stddev vs thresholds).
// agg_keyword must come from the bind-time whitelist, never from raw user input.
// Empty-input semantics: an empty table or all-NULL column yields value = NULL and
// passes trivially with an explicit message.
static string GenerateAggCheckSQL(const string &table_ref, const string &column_name, const string &agg_keyword,
                                  const Value &lower, const Value &upper) {
	string lower_val = DoubleThresholdSQL(lower);
	string upper_val = DoubleThresholdSQL(upper);
	string col_id = QuoteSqlIdentifier(column_name);
	string agg_upper = agg_keyword;
	std::transform(agg_upper.begin(), agg_upper.end(), agg_upper.begin(), ::toupper);

	return "SELECT "
		"CASE "
		"  WHEN value IS NULL THEN 'pass' "
		"  WHEN isnan(value) THEN 'fail' "
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
		"  WHEN isnan(value) THEN '" + agg_keyword + " value is NaN: the column contains ' || "
		"    CAST(nonfinite_count AS VARCHAR) || ' NaN/Inf value(s)' "
		"  WHEN (" + lower_val + " IS NOT NULL AND value < " + lower_val + ") THEN "
		"    '" + agg_keyword + " value ' || CAST(value AS VARCHAR) || ' is below minimum ' || CAST(" + lower_val + " AS VARCHAR) "
		"  WHEN (" + upper_val + " IS NOT NULL AND value > " + upper_val + ") THEN "
		"    '" + agg_keyword + " value ' || CAST(value AS VARCHAR) || ' exceeds maximum ' || CAST(" + upper_val + " AS VARCHAR) "
		"  ELSE '" + agg_keyword + " value ' || CAST(value AS VARCHAR) || ' is within acceptable range' "
		"END AS message "
		"FROM (SELECT " + BuildFiniteAggregateExpr(agg_upper, "CAST(" + col_id + " AS DOUBLE)") + " AS value, " +
		BuildNonFiniteCountExpr("CAST(" + col_id + " AS DOUBLE)") + " AS nonfinite_count FROM " + table_ref + ")";
}

string BuildDuplicateCountExpr(const vector<string> &column_names) {
	if (column_names.size() == 1) {
		// COUNT(DISTINCT) skips NULL: the NULL keys form one extra group when present
		string col = QuoteSqlIdentifier(column_names[0]);
		return "(COUNT(*) - COUNT(DISTINCT " + col + ") - CASE WHEN COUNT(*) > COUNT(" + col + ") THEN 1 ELSE 0 END)";
	}
	// A compound key is a ROW value, never NULL itself; DISTINCT compares its NULL
	// components as equal, so (x, NULL) twice is one duplicate and once is none.
	string row = "row(";
	for (size_t i = 0; i < column_names.size(); i++) {
		if (i > 0) {
			row += ", ";
		}
		row += QuoteSqlIdentifier(column_names[i]);
	}
	return "(COUNT(*) - COUNT(DISTINCT " + row + ")))";
}

// Helper: Generate SQL for duplicate count checks.
// Duplicates are the rows beyond the first of each distinct key. NULL keys are duplicates of
// each other only: one NULL key is no duplicate, two are one (see BuildDuplicateCountExpr).
static string GenerateDuplicateCountSQL(const string &table_ref, const vector<string> &column_names,
                                        const Value &max_duplicates) {
	string max_val = BigintThresholdSQL(max_duplicates, "0");

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
		"  " + BuildDuplicateCountExpr(column_names) + " AS duplicate_count, "
		"  COUNT(*) AS total_count "
		"FROM " + table_ref + ")";
}

// Helper: Generate SQL for occurrence checks (highest or lowest frequency of any single value).
// mode_keyword must come from the bind-time whitelist ('max' or 'min').
// Empty-input semantics: an empty table or all-NULL column yields occurrence = NULL and
// passes trivially with an explicit message.
static string GenerateOccurrenceSQL(const string &table_ref, const string &column_name, const string &mode_keyword,
                                    const Value &lower, const Value &upper) {
	string lower_val = BigintThresholdSQL(lower);
	string upper_val = BigintThresholdSQL(upper);
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
	string min_val = DoubleThresholdSQL(min_rate, "1.0");

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
		"    'Match rate ' || " + RoundedNumberSQL("match_rate") + " || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'Match rate ' || " + RoundedNumberSQL("match_rate") + " || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
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
	string min_val = DoubleThresholdSQL(min_rate, "1.0");
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
		"    'Compliance rate ' || " + RoundedNumberSQL("compliance_rate") + " || ' (' || CAST(compliant_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'Compliance rate ' || " + RoundedNumberSQL("compliance_rate") + " || ' (' || CAST(compliant_count AS VARCHAR) || '/' || "
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

// The reference dates a time-aware check evaluates (#109): either one date
// (reference_date; NULL = the latest date present in the data) or every day of
// the inclusive range start_date..end_date.
struct ReferenceDates {
	Value reference_date;
	Value start_date;
	Value end_date;

	bool IsRange() const {
		return !start_date.IsNull();
	}
};

// Helper: the body of the "ref" CTE, one row (ref_d DATE) per reference date.
// latest_expr / latest_from compute the default reference date (the latest date
// in the data) of the single-date form, e.g. "MAX(d)" over "daily".
static string BuildReferenceDatesSQL(const ReferenceDates &dates, const string &latest_expr,
                                     const string &latest_from) {
	if (dates.IsRange()) {
		return "SELECT CAST(unnest(generate_series(" + BuildDateLiteral(dates.start_date) + ", " +
		       BuildDateLiteral(dates.end_date) + ", INTERVAL '1 day')) AS DATE) AS ref_d";
	}
	return "SELECT COALESCE(" + BuildDateLiteral(dates.reference_date) + ", " + latest_expr + ") AS ref_d FROM " +
	       latest_from;
}

string BuildRelCountBaselineSQL(const string &ref_expr, int64_t window_days, bool fill_missing_days) {
	string window_val = std::to_string(window_days);
	string in_window = "d < " + ref_expr + " AND d >= " + ref_expr + " - " + window_val;
	if (!fill_missing_days) {
		return "(SELECT AVG(c) FROM daily WHERE " + in_window + ") AS baseline_avg, "
		       "(SELECT COUNT(*) FROM daily WHERE " + in_window + ") AS baseline_days";
	}
	// Calendar days of the window from the first day with data on (the days before the data
	// starts are no history, not an outage); the days without rows add 0 to the sum
	string calendar_days = "GREATEST(CAST(" + ref_expr + " - GREATEST(" + ref_expr + " - " + window_val +
	                       ", (SELECT MIN(d) FROM daily)) AS BIGINT), 0)";
	return "CASE WHEN " + calendar_days + " > 0 THEN CAST(COALESCE((SELECT SUM(c) FROM daily WHERE " + in_window +
	       "), 0) AS DOUBLE) / " + calendar_days + " END AS baseline_avg, " + calendar_days + " AS baseline_days";
}

// Helper: Generate SQL for relative count change checks.
// Compares the (distinct) count on the reference date against the average over the
// window_days days strictly before it; rel_change = (ref - baseline) / baseline.
// Trivial-pass semantics: empty input, missing baseline days or a zero baseline pass
// with an explicit message (not enough history is not a failure).
// fill_missing_days: see BuildRelCountBaselineSQL.
static string GenerateRelCountChangeSQL(const string &table_ref, const string &date_column,
                                        const LogicalType &date_type, const string &count_column,
                                        int64_t window_days, const Value &lower, const Value &upper,
                                        const ReferenceDates &dates, bool fill_missing_days) {
	string lower_val = DoubleThresholdSQL(lower);
	string upper_val = DoubleThresholdSQL(upper);
	string date_id = QuoteSqlIdentifier(date_column);
	string window_val = std::to_string(window_days);
	string cnt_expr = count_column.empty() ? "COUNT(*)" : "COUNT(DISTINCT " + QuoteSqlIdentifier(count_column) + ")";

	return "WITH daily AS ("
		"SELECT " + UtcDateSQL(date_id, date_type) + " AS d, " + cnt_expr + " AS c "
		"FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL GROUP BY 1"
		"), "
		"ref AS (" + BuildReferenceDatesSQL(dates, "MAX(d)", "daily") + "), "
		"refstats AS ("
		"SELECT r.ref_d, "
		"  (SELECT c FROM daily WHERE d = r.ref_d) AS ref_c, " +
		BuildRelCountBaselineSQL("r.ref_d", window_days, fill_missing_days) + " "
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
		"    " + RoundedNumberSQL("rel_change") + " || ' from baseline average ' || " + RoundedNumberSQL("baseline_avg") + " "
		"  ELSE "
		"    'Count ' || CAST(ref_c AS VARCHAR) || ' on ' || CAST(ref_d AS VARCHAR) || ' is within ' || "
		"    " + RoundedNumberSQL("rel_change") + " || ' of baseline average ' || " + RoundedNumberSQL("baseline_avg") + " "
		"END AS message "
		"FROM summary ORDER BY reference_date";
}

// Helper: Generate SQL for metric-level IQR anomaly detection.
// Computes a daily metric (row count or AVG of metric_column) over the window_days days
// strictly before the reference date, derives Q1/Q3 bounds with multiplier k, and flags
// the reference day when it falls outside the bounds selected by mode.
// Trivial-pass semantics: empty input or an empty baseline window pass with a message.
static string GenerateMetricAnomalyIQRSQL(const string &table_ref, const string &date_column,
                                          const LogicalType &date_type, const string &metric_column,
                                          int64_t window_days, const Value &k, const string &mode,
                                          const ReferenceDates &dates) {
	string k_val = DoubleThresholdSQL(k, "1.5");
	string date_id = QuoteSqlIdentifier(date_column);
	string window_val = std::to_string(window_days);
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
		"SELECT " + UtcDateSQL(date_id, date_type) + " AS d, " + metric_expr + " AS m "
		"FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL GROUP BY 1"
		"), "
		"ref AS (" + BuildReferenceDatesSQL(dates, "MAX(d)", "daily") + "), "
		// One baseline per reference date: the days strictly before it within the window
		// (COUNT(w.d), not COUNT(w.m): a day whose metric is NULL still counts as a baseline day)
		"stats AS ("
		"SELECT r.ref_d, QUANTILE_CONT(w.m, 0.25) AS q1, QUANTILE_CONT(w.m, 0.75) AS q3, COUNT(w.d) AS n "
		"FROM ref r LEFT JOIN daily w ON w.d < r.ref_d AND w.d >= r.ref_d - " + window_val + " "
		"GROUP BY r.ref_d"
		"), "
		"summary AS ("
		"SELECT s.ref_d AS reference_date, "
		"  (SELECT m FROM daily WHERE d = s.ref_d) AS metric_value, "
		"  s.q1, s.q3, s.n AS baseline_days, "
		"  s.q1 - " + k_val + " * (s.q3 - s.q1) AS lower_bound, "
		"  s.q3 + " + k_val + " * (s.q3 - s.q1) AS upper_bound "
		"FROM stats s"
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
		"    'Metric ' || " + RoundedNumberSQL("metric_value") + " || ' on ' || CAST(reference_date AS VARCHAR) || "
		"    ' is outside IQR bounds [' || " + RoundedNumberSQL("lower_bound") + " || ', ' || " + RoundedNumberSQL("upper_bound") + " || '] (mode: " + mode + ")' "
		"  ELSE "
		"    'Metric ' || " + RoundedNumberSQL("metric_value") + " || ' on ' || CAST(reference_date AS VARCHAR) || "
		"    ' is within IQR bounds [' || " + RoundedNumberSQL("lower_bound") + " || ', ' || " + RoundedNumberSQL("upper_bound") + " || ']' "
		"END AS message "
		"FROM summary ORDER BY reference_date";
}

// Helper: Generate SQL for rolling values-in-set checks.
// Same rate computation as values_in_set, restricted to the trailing window_days days
// ending at the reference date (inclusive). The table is scanned once into per-day value
// counts, so a date range does not re-scan it per reference date.
static string GenerateRollingValuesInSetSQL(const string &table_ref, const string &column_name,
                                            const string &allowed_array, const string &date_column,
                                            const LogicalType &date_type, int64_t window_days,
                                            const Value &min_rate, const ReferenceDates &dates) {
	string min_val = DoubleThresholdSQL(min_rate, "1.0");
	string col_id = QuoteSqlIdentifier(column_name);
	string col_str = "CAST(" + col_id + " AS VARCHAR)";
	string date_id = QuoteSqlIdentifier(date_column);
	string window_val = std::to_string(window_days);

	return "WITH dv AS ("
		"SELECT " + UtcDateSQL(date_id, date_type) + " AS d, " + col_str + " AS v, COUNT(*) AS n FROM " + table_ref +
		" WHERE " + date_id + " IS NOT NULL GROUP BY 1, 2"
		"), "
		"ref AS (" + BuildReferenceDatesSQL(dates, "MAX(d)", "dv") + "), "
		"stats AS ("
		"SELECT r.ref_d, "
		"  CAST(COALESCE(SUM(CASE WHEN list_contains(" + allowed_array + ", w.v) THEN w.n ELSE 0 END), 0) AS BIGINT) AS in_set_count, "
		"  CAST(COALESCE(SUM(w.n), 0) AS BIGINT) AS total_count, "
		"  list_sort(list(DISTINCT w.v) FILTER (WHERE NOT list_contains(" + allowed_array + ", w.v))) AS violations "
		"FROM ref r LEFT JOIN dv w ON w.v IS NOT NULL AND w.d > r.ref_d - " + window_val + " AND w.d <= r.ref_d "
		"GROUP BY r.ref_d"
		"), "
		"rates AS ("
		"SELECT *, COALESCE(CAST(in_set_count AS DOUBLE) / NULLIF(total_count, 0), 0.0) AS in_set_rate FROM stats"
		") "
		"SELECT "
		"CASE "
		"  WHEN total_count = 0 THEN 'pass' "
		"  WHEN in_set_rate >= " + min_val + " THEN 'pass' "
		"  ELSE 'fail' "
		"END AS status, "
		"ref_d AS reference_date, "
		"in_set_rate, "
		"in_set_count, "
		"total_count, "
		+ min_val + " AS threshold, "
		"COALESCE(list_slice(violations, 1, 5), []::VARCHAR[]) AS sample_violations, "
		"CASE "
		"  WHEN total_count = 0 THEN 'No values in the trailing window; rolling values-in-set check passed trivially' "
		"  WHEN in_set_rate >= " + min_val + " THEN "
		"    'In-set rate ' || " + RoundedNumberSQL("in_set_rate") + " || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') over the trailing " + window_val + " day(s) is acceptable' "
		"  ELSE "
		"    'In-set rate ' || " + RoundedNumberSQL("in_set_rate") + " || ' (' || CAST(in_set_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') over the trailing " + window_val + " day(s) is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"END AS message "
		"FROM rates ORDER BY reference_date";
}

// Helper: the type of the date column of a time-aware check, so a TIMESTAMP WITH TIME ZONE
// column is bucketed by its UTC date (UtcDateSQL) whatever the session TimeZone
static LogicalType TimeColumnType(const string &function_name, const ResolvedColumn &date_column) {
	if (date_column.type.id() == LogicalTypeId::TIMESTAMP_TZ) {
		AnofoxTrace(AnofoxLogLevel::Debug, function_name + ": '" + date_column.name +
		                                       "' is TIMESTAMP WITH TIME ZONE; bucketing by its UTC date");
	}
	return date_column.type;
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

// Helper: read the reference dates of a time-aware check (#109): the positional
// reference_date at ref_index, or the named start_date/end_date range. The range is
// inclusive, daily; both bounds are required together and exclude an explicit
// (non-NULL) reference_date.
static ReferenceDates ReadReferenceDates(TableFunctionBindInput &input, idx_t ref_index,
                                         const string &function_name) {
	ReferenceDates dates;
	dates.reference_date = (input.inputs.size() > ref_index) ? input.inputs[ref_index] : Value();
	auto start = duckdb_compat::FindNamedParameter(input.named_parameters, "start_date");
	auto end = duckdb_compat::FindNamedParameter(input.named_parameters, "end_date");
	if (!start && !end) {
		return dates;
	}
	if (!start || !end) {
		throw BinderException(function_name + ": start_date and end_date must be given together");
	}
	if (start->IsNull() || end->IsNull()) {
		throw BinderException(function_name + ": start_date and end_date must not be NULL");
	}
	if (!dates.reference_date.IsNull()) {
		throw BinderException(function_name + ": reference_date cannot be combined with start_date/end_date");
	}
	dates.start_date = start->DefaultCastAs(LogicalType(LogicalTypeId::DATE));
	dates.end_date = end->DefaultCastAs(LogicalType(LogicalTypeId::DATE));
	if (dates.start_date.GetValue<date_t>() > dates.end_date.GetValue<date_t>()) {
		throw BinderException(function_name + ": start_date must not be after end_date (" +
		                      dates.start_date.ToString() + " > " + dates.end_date.ToString() + ")");
	}
	AnofoxTrace(AnofoxLogLevel::Debug, "check: " + function_name + " date range " + dates.start_date.ToString() +
	                                       " .. " + dates.end_date.ToString());
	return dates;
}

// How a time-aware check's result row maps onto the contract `events` schema (#107)
struct EventsMapping {
	string kind;           // the check name
	string series;         // the measured table / table.column
	string value_expr;     // the check's measure, over the base result columns
	string threshold_expr; // the bound the measure violated
};

// Helper: wrap a time-aware check's SQL into its events companion: one row per
// reference date whose status is 'fail' (severity 'alarm') or 'warn' ('warning'),
// ordered by ds. Passing dates are not events.
static string WrapAsEventsSQL(const string &check_sql, const EventsMapping &mapping) {
	return "SELECT reference_date AS ds, "
	       "'" + EscapeSqlStringLiteral(mapping.kind) + "' AS kind, "
	       "message AS label, "
	       "CASE status WHEN 'fail' THEN 'alarm' ELSE 'warning' END AS severity, "
	       "CAST(" + mapping.value_expr + " AS DOUBLE) AS value, "
	       "CAST(" + mapping.threshold_expr + " AS DOUBLE) AS threshold, "
	       "'" + EscapeSqlStringLiteral(mapping.series) + "' AS series "
	       "FROM (" + check_sql + ") __check WHERE status IN ('fail', 'warn') ORDER BY ds";
}

// Helper: the series label of an event: the table, or table.column for a column measure
static string EventsSeries(const string &table_name, const string &column_name) {
	return column_name.empty() ? table_name : table_name + "." + column_name;
}

// Declare the optional start_date/end_date named parameters (#109) on every overload
static void AddDateRangeParameters(TableFunctionSet &set) {
	duckdb_compat::ForEachFunction(set, [](TableFunction &func) {
		duckdb_compat::AddNamedParameter(func, "start_date", LogicalType(LogicalTypeId::DATE));
		duckdb_compat::AddNamedParameter(func, "end_date", LogicalType(LogicalTypeId::DATE));
	});
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

	string table_name = RequireNameArgument(input, 0, "regex_match", "table_name");
	string column_name = RequireNameArgument(input, 1, "regex_match", "column_name");
	string pattern = RequireNonNullArgument(input, 2, "regex_match", "pattern");
	ValidateRate(input.inputs[3], "regex_match", "min_match_rate");
	Value max_rate = (input.inputs.size() > 4) ? input.inputs[4] : Value();
	ValidateRate(max_rate, "regex_match", "max_match_rate");
	ValidateThresholdOrder(input.inputs[3], max_rate, "regex_match", "min_match_rate", "max_match_rate");
	RequireRelationColumns(context, "regex_match", table_name, {column_name});

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateRegexMatchSQL(table_ref, column_name, pattern, input.inputs[3], max_rate);
	return ParseSubquery(sql, context, "Failed to parse regex match check query");
}

static unique_ptr<TableRef> CheckValuesInSetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_values_in_set");
	if (input.inputs.size() < 3) {
		throw BinderException(
		    "anofox_tab_values_in_set requires at least 3 arguments: table_name, column_name, allowed_values");
	}

	string table_name = RequireNameArgument(input, 0, "values_in_set", "table_name");
	string column_name = RequireNameArgument(input, 1, "values_in_set", "column_name");
	RequireNonNullArgument(input, 2, "values_in_set", "allowed_values");
	string allowed_array = BuildVarcharArrayLiteral(input.inputs[2]);
	Value min_rate = (input.inputs.size() > 3) ? input.inputs[3] : Value();
	ValidateRate(min_rate, "values_in_set", "min_rate");
	RequireRelationColumns(context, "values_in_set", table_name, {column_name});

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateValuesInSetSQL(table_ref, column_name, allowed_array, min_rate);
	return ParseSubquery(sql, context, "Failed to parse values-in-set check query");
}

static unique_ptr<TableRef> CheckAggBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_agg");
	if (input.inputs.size() < 5) {
		throw BinderException(
		    "anofox_tab_agg_check requires 5 arguments: table_name, column_name, agg, lower_threshold, upper_threshold");
	}

	string table_name = RequireNameArgument(input, 0, "agg_check", "table_name");
	string column_name = RequireNameArgument(input, 1, "agg_check", "column_name");
	string agg = RequireNonNullArgument(input, 2, "agg_check", "agg");
	std::transform(agg.begin(), agg.end(), agg.begin(), ::tolower);
	// Whitelist: the aggregate keyword is spliced into SQL, so it must never come from raw user input
	static const vector<string> ALLOWED_AGGS = {"avg", "min", "max", "sum", "median", "stddev"};
	if (std::find(ALLOWED_AGGS.begin(), ALLOWED_AGGS.end(), agg) == ALLOWED_AGGS.end()) {
		throw BinderException("agg_check: agg must be one of 'avg', 'min', 'max', 'sum', 'median', 'stddev'");
	}
	ValidateFiniteDouble(input.inputs[3], "agg_check", "lower_threshold");
	ValidateFiniteDouble(input.inputs[4], "agg_check", "upper_threshold");
	ValidateThresholdOrder(input.inputs[3], input.inputs[4], "agg_check", "lower_threshold", "upper_threshold");
	RequireRelationColumns(context, "agg_check", table_name, {column_name});
	RequireNumericColumn(context, "agg_check", table_name, column_name);

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateAggCheckSQL(table_ref, column_name, agg, input.inputs[3], input.inputs[4]);
	return ParseSubquery(sql, context, "Failed to parse aggregate check query");
}

static unique_ptr<TableRef> CheckDuplicateCountBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_duplicate_count");
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_duplicate_count requires at least 2 arguments: table_name, column_names");
	}

	string table_name = RequireNameArgument(input, 0, "duplicate_count", "table_name");
	RequireNonNullArgument(input, 1, "duplicate_count", "column_names");
	// column_names: a comma-separated string or a VARCHAR[] (like diff and schema_check)
	vector<string> column_names;
	if (input.inputs[1].type().id() == LogicalTypeId::LIST) {
		for (auto &child : ListValue::GetChildren(input.inputs[1])) {
			if (!child.IsNull() && !child.ToString().empty()) {
				column_names.push_back(child.ToString());
			}
		}
	} else {
		column_names = ParseCommaSeparatedColumns(input.inputs[1].ToString());
	}
	if (column_names.empty()) {
		throw BinderException("duplicate_count: column_names must contain at least one column");
	}
	Value max_duplicates = (input.inputs.size() > 2) ? input.inputs[2] : Value();
	if (!max_duplicates.IsNull() && max_duplicates.GetValue<int64_t>() < 0) {
		throw BinderException("duplicate_count: max_duplicates must be >= 0");
	}
	RequireRelationColumns(context, "duplicate_count", table_name, column_names);

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateDuplicateCountSQL(table_ref, column_names, max_duplicates);
	return ParseSubquery(sql, context, "Failed to parse duplicate count check query");
}

static unique_ptr<TableRef> CheckOccurrenceBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_occurrence");
	if (input.inputs.size() < 5) {
		throw BinderException(
		    "anofox_tab_occurrence requires 5 arguments: table_name, column_name, mode, lower_threshold, upper_threshold");
	}

	string table_name = RequireNameArgument(input, 0, "occurrence", "table_name");
	string column_name = RequireNameArgument(input, 1, "occurrence", "column_name");
	string mode = RequireNonNullArgument(input, 2, "occurrence", "mode");
	std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
	if (mode != "max" && mode != "min") {
		throw BinderException("occurrence: mode must be 'max' or 'min'");
	}
	ValidateNonNegative(input.inputs[3], "occurrence", "lower_threshold");
	ValidateNonNegative(input.inputs[4], "occurrence", "upper_threshold");
	ValidateThresholdOrder(input.inputs[3], input.inputs[4], "occurrence", "lower_threshold", "upper_threshold");
	RequireRelationColumns(context, "occurrence", table_name, {column_name});

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateOccurrenceSQL(table_ref, column_name, mode, input.inputs[3], input.inputs[4]);
	return ParseSubquery(sql, context, "Failed to parse occurrence check query");
}

static unique_ptr<TableRef> CheckMatchRateBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_match_rate");
	if (input.inputs.size() < 4) {
		throw BinderException(
		    "anofox_tab_match_rate requires at least 4 arguments: left_table, right_table, left_keys, right_keys");
	}

	string left_table = RequireNameArgument(input, 0, "match_rate", "left_table");
	string right_table = RequireNameArgument(input, 1, "match_rate", "right_table");
	auto left_keys = ParseCommaSeparatedColumns(RequireNonNullArgument(input, 2, "match_rate", "left_keys"));
	auto right_keys = ParseCommaSeparatedColumns(RequireNonNullArgument(input, 3, "match_rate", "right_keys"));
	if (left_keys.empty()) {
		throw BinderException("match_rate: left_keys must contain at least one column");
	}
	if (left_keys.size() != right_keys.size()) {
		throw BinderException("match_rate: left_keys and right_keys must have the same number of columns (" +
		                      std::to_string(left_keys.size()) + " vs " + std::to_string(right_keys.size()) + ")");
	}
	Value min_rate = (input.inputs.size() > 4) ? input.inputs[4] : Value();
	ValidateRate(min_rate, "match_rate", "min_rate");
	RequireRelationColumns(context, "match_rate", left_table, left_keys);
	RequireRelationColumns(context, "match_rate", right_table, right_keys);

	AnofoxTrace(AnofoxLogLevel::Debug, "check: match_rate left='" + left_table + "', right='" + right_table +
	                                       "', keys=" + std::to_string(left_keys.size()));
	string sql = GenerateMatchRateSQL(BuildQueryTableRef(left_table), BuildQueryTableRef(right_table), left_keys,
	                                  right_keys, min_rate);
	return ParseSubquery(sql, context, "Failed to parse match rate check query");
}

static unique_ptr<TableRef> CheckComplianceBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_compliance");
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_compliance requires at least 2 arguments: table_name, expression");
	}

	string table_name = RequireNameArgument(input, 0, "compliance", "table_name");
	string expression = RequireNonNullArgument(input, 1, "compliance", "expression");
	ValidateBooleanExpression(context, expression, "compliance");
	Value min_rate = (input.inputs.size() > 2) ? input.inputs[2] : Value();
	ValidateRate(min_rate, "compliance", "min_rate");
	RequireRelationColumns(context, "compliance", table_name);

	AnofoxTrace(AnofoxLogLevel::Debug, "check: compliance table='" + table_name + "', expression='" + expression + "'");
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateComplianceSQL(table_ref, expression, min_rate);
	return ParseSubquery(sql, context, "Failed to parse compliance check query");
}

// Builds the rel_count_change SQL from the bind input; fills the events mapping when asked
static string BuildRelCountChangeSQL(ClientContext &context, TableFunctionBindInput &input, EventsMapping *events) {
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_rel_count_change requires at least 2 arguments: table_name, date_column");
	}

	const string function_name = events ? "rel_count_change_events" : "rel_count_change";
	string table_name = RequireNameArgument(input, 0, function_name, "table_name");
	string date_column = RequireNameArgument(input, 1, function_name, "date_column");
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
	ValidateThresholdOrder(lower, upper, "rel_count_change", "lower_threshold", "upper_threshold");
	auto dates = ReadReferenceDates(input, 6, "rel_count_change");
	bool fill_missing_days = false;
	auto fill = duckdb_compat::FindNamedParameter(input.named_parameters, "fill_missing_days");
	if (fill && !fill->IsNull()) {
		fill_missing_days = BooleanValue::Get(fill->DefaultCastAs(LogicalType(LogicalTypeId::BOOLEAN)));
	}

	if (events) {
		events->kind = "rel_count_change";
		events->series = EventsSeries(table_name, count_column);
		events->value_expr = "rel_change";
		events->threshold_expr =
		    "CASE WHEN lower_threshold IS NOT NULL AND rel_change < lower_threshold THEN lower_threshold "
		    "ELSE upper_threshold END";
	}
	string table_ref = BuildQueryTableRef(table_name);
	vector<string> columns = {date_column};
	if (!count_column.empty()) {
		columns.push_back(count_column);
	}
	auto date_type = TimeColumnType(function_name, RequireRelationColumns(context, function_name, table_name, columns)[0]);
	AnofoxTrace(AnofoxLogLevel::Debug, "check: " + function_name + " table='" + table_name + "', window_days=" +
	                                       std::to_string(window_days) +
	                                       (fill_missing_days ? ", missing days count as 0" : ""));
	return GenerateRelCountChangeSQL(table_ref, date_column, date_type, count_column, window_days, lower, upper,
	                                 dates, fill_missing_days);
}

static unique_ptr<TableRef> CheckRelCountChangeBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_rel_count_change");
	string sql = BuildRelCountChangeSQL(context, input, nullptr);
	return ParseSubquery(sql, context, "Failed to parse relative count change check query");
}

static unique_ptr<TableRef> CheckRelCountChangeEventsBindReplace(ClientContext &context,
                                                                 TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_rel_count_change_events");
	EventsMapping events;
	string sql = WrapAsEventsSQL(BuildRelCountChangeSQL(context, input, &events), events);
	return ParseSubquery(sql, context, "Failed to parse relative count change events query");
}

// Builds the metric_anomaly_iqr SQL from the bind input; fills the events mapping when asked
static string BuildMetricAnomalyIQRSQL(ClientContext &context, TableFunctionBindInput &input, EventsMapping *events) {
	if (input.inputs.size() < 2) {
		throw BinderException(
		    "anofox_tab_metric_anomaly_iqr requires at least 2 arguments: table_name, date_column");
	}

	const string function_name = events ? "metric_anomaly_iqr_events" : "metric_anomaly_iqr";
	string table_name = RequireNameArgument(input, 0, function_name, "table_name");
	string date_column = RequireNameArgument(input, 1, function_name, "date_column");
	string metric_column;
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		metric_column = input.inputs[2].ToString();
	}
	int64_t window_days = ReadWindowDays(input, 3, 30, "metric_anomaly_iqr");
	Value k = (input.inputs.size() > 4) ? input.inputs[4] : Value();
	ValidateFiniteDouble(k, "metric_anomaly_iqr", "k");
	ValidateNonNegative(k, "metric_anomaly_iqr", "k");
	string mode = "both";
	if (input.inputs.size() > 5 && !input.inputs[5].IsNull()) {
		mode = input.inputs[5].ToString();
		std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
	}
	if (mode != "both" && mode != "upper" && mode != "lower") {
		throw BinderException("metric_anomaly_iqr: mode must be 'both', 'upper' or 'lower'");
	}
	auto dates = ReadReferenceDates(input, 6, "metric_anomaly_iqr");

	if (events) {
		events->kind = "metric_anomaly_iqr";
		events->series = EventsSeries(table_name, metric_column);
		events->value_expr = "metric_value";
		events->threshold_expr = "CASE WHEN metric_value < lower_bound THEN lower_bound ELSE upper_bound END";
	}
	string table_ref = BuildQueryTableRef(table_name);
	vector<string> columns = {date_column};
	if (!metric_column.empty()) {
		columns.push_back(metric_column);
	}
	auto date_type = TimeColumnType(function_name, RequireRelationColumns(context, function_name, table_name, columns)[0]);
	if (!metric_column.empty()) {
		RequireNumericColumn(context, function_name, table_name, metric_column);
	}
	return GenerateMetricAnomalyIQRSQL(table_ref, date_column, date_type, metric_column, window_days, k, mode,
	                                   dates);
}

static unique_ptr<TableRef> CheckMetricAnomalyIQRBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_metric_anomaly_iqr");
	string sql = BuildMetricAnomalyIQRSQL(context, input, nullptr);
	return ParseSubquery(sql, context, "Failed to parse metric anomaly IQR check query");
}

static unique_ptr<TableRef> CheckMetricAnomalyIQREventsBindReplace(ClientContext &context,
                                                                   TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_metric_anomaly_iqr_events");
	EventsMapping events;
	string sql = WrapAsEventsSQL(BuildMetricAnomalyIQRSQL(context, input, &events), events);
	return ParseSubquery(sql, context, "Failed to parse metric anomaly IQR events query");
}

// Builds the rolling_values_in_set SQL from the bind input; fills the events mapping when asked
static string BuildRollingValuesInSetSQL(ClientContext &context, TableFunctionBindInput &input, EventsMapping *events) {
	if (input.inputs.size() < 4) {
		throw BinderException("anofox_tab_rolling_values_in_set requires at least 4 arguments: "
		                      "table_name, column_name, allowed_values, date_column");
	}

	const string function_name = events ? "rolling_values_in_set_events" : "rolling_values_in_set";
	string table_name = RequireNameArgument(input, 0, function_name, "table_name");
	string column_name = RequireNameArgument(input, 1, function_name, "column_name");
	RequireNonNullArgument(input, 2, function_name, "allowed_values");
	string allowed_array = BuildVarcharArrayLiteral(input.inputs[2]);
	string date_column = RequireNameArgument(input, 3, function_name, "date_column");
	int64_t window_days = ReadWindowDays(input, 4, 7, "rolling_values_in_set");
	Value min_rate = (input.inputs.size() > 5) ? input.inputs[5] : Value();
	ValidateRate(min_rate, "rolling_values_in_set", "min_rate");
	auto dates = ReadReferenceDates(input, 6, "rolling_values_in_set");

	if (events) {
		events->kind = "rolling_values_in_set";
		events->series = EventsSeries(table_name, column_name);
		events->value_expr = "in_set_rate";
		events->threshold_expr = "threshold";
	}
	string table_ref = BuildQueryTableRef(table_name);
	auto date_type =
	    TimeColumnType(function_name, RequireRelationColumns(context, function_name, table_name, {date_column, column_name})[0]);
	return GenerateRollingValuesInSetSQL(table_ref, column_name, allowed_array, date_column, date_type, window_days,
	                                     min_rate, dates);
}

static unique_ptr<TableRef> CheckRollingValuesInSetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_rolling_values_in_set");
	string sql = BuildRollingValuesInSetSQL(context, input, nullptr);
	return ParseSubquery(sql, context, "Failed to parse rolling values-in-set check query");
}

static unique_ptr<TableRef> CheckRollingValuesInSetEventsBindReplace(ClientContext &context,
                                                                     TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("check_rolling_values_in_set_events");
	EventsMapping events;
	string sql = WrapAsEventsSQL(BuildRollingValuesInSetSQL(context, input, &events), events);
	return ParseSubquery(sql, context, "Failed to parse rolling values-in-set events query");
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
	const vector<LogicalType> dup_list_args = {LogicalType(LogicalTypeId::VARCHAR),
	                                           LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
	                                           LogicalType(LogicalTypeId::BIGINT)};
	TableFunctionSet dup_set("anofox_tab_duplicate_count");
	AddPrefixAritiesBindReplace(dup_set, "anofox_tab_duplicate_count", dup_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckDuplicateCountBindReplace));
	AddPrefixAritiesBindReplace(dup_set, "anofox_tab_duplicate_count", dup_list_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckDuplicateCountBindReplace));
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the number of duplicate values in a column (or column combination, as a comma-separated "
		    "string or a VARCHAR[]) does not exceed max_duplicates (default 0).";
		desc.parameter_names = {"table_name", "column_names", "max_duplicates"};
		desc.examples = {"SELECT * FROM duplicate_count('orders', 'order_id');",
		                 "SELECT * FROM duplicate_count('orders', 'customer_id,order_date', 10);",
		                 "SELECT * FROM duplicate_count('orders', ['customer_id', 'order_date'], 10);"};
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
	AddDateRangeParameters(rel_count_set);
	duckdb_compat::ForEachFunction(rel_count_set, [](TableFunction &func) {
		duckdb_compat::AddNamedParameter(func, "fill_missing_days", LogicalType(LogicalTypeId::BOOLEAN));
	});
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the daily (distinct) count on the reference date deviates from the rolling window average "
		    "by a relative change within [lower_threshold, upper_threshold]. Defaults: count rows, 7-day window, "
		    "reference = latest date in the data. With start_date := DATE, end_date := DATE (instead of "
		    "reference_date) returns one row per day of the inclusive range, ordered by date. With "
		    "fill_missing_days := true the calendar days without rows count as 0 in the baseline (default: only "
		    "days with rows are averaged).";
		desc.parameter_names = {"table_name", "date_column",     "count_column",   "window_days",
		                        "lower_threshold", "upper_threshold", "reference_date"};
		desc.examples = {"SELECT * FROM rel_count_change('orders', 'order_date', NULL, 7, -0.5, 0.5);",
		                 "SELECT * FROM rel_count_change('orders', 'order_date', start_date := DATE '2026-01-01', "
		                 "end_date := DATE '2026-01-31');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, rel_count_set, "rel_count_change", {std::move(desc)});
	}

	// anofox_tab_rel_count_change_events(<rel_count_change arguments>) (alias: rel_count_change_events)
	TableFunctionSet rel_count_events_set("anofox_tab_rel_count_change_events");
	AddPrefixAritiesBindReplace(rel_count_events_set, "anofox_tab_rel_count_change_events", rel_count_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckRelCountChangeEventsBindReplace));
	AddDateRangeParameters(rel_count_events_set);
	duckdb_compat::ForEachFunction(rel_count_events_set, [](TableFunction &func) {
		duckdb_compat::AddNamedParameter(func, "fill_missing_days", LogicalType(LogicalTypeId::BOOLEAN));
	});
	{
		FunctionDescription desc;
		desc.description = "Events companion of rel_count_change (same arguments, including start_date/end_date "
		                   "and fill_missing_days)."
		                   " Returns the contract `events` shape (ds, kind, label, severity, value, threshold, series): one row per "
		    "reference date whose check fails (severity 'alarm') or warns ('warning'), ordered by ds; passing dates "
		    "are not events.";
		desc.parameter_names = {"table_name", "date_column",     "count_column",   "window_days",
		                        "lower_threshold", "upper_threshold", "reference_date"};
		desc.examples = {"SELECT * FROM rel_count_change_events('orders', 'order_date', start_date := DATE '2026-01-01', "
		                 "end_date := DATE '2026-01-31');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, rel_count_events_set, "rel_count_change_events", {std::move(desc)});
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
	AddDateRangeParameters(metric_iqr_set);
	{
		FunctionDescription desc;
		desc.description =
		    "Flags the reference date as anomalous when its daily metric (row count or AVG of metric_column) falls "
		    "outside Q1/Q3 +/- k*IQR of the trailing window. Mode 'both', 'upper' or 'lower' selects the bounds. "
		    "With start_date := DATE, end_date := DATE (instead of reference_date) returns one row per day of the "
		    "inclusive range, ordered by date.";
		desc.parameter_names = {"table_name", "date_column", "metric_column", "window_days",
		                        "k",          "mode",        "reference_date"};
		desc.examples = {"SELECT * FROM metric_anomaly_iqr('orders', 'order_date', NULL, 30, 1.5, 'both');",
		                 "SELECT * FROM metric_anomaly_iqr('orders', 'order_date', start_date := DATE '2026-01-01', "
		                 "end_date := DATE '2026-01-31');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, metric_iqr_set, "metric_anomaly_iqr", {std::move(desc)});
	}

	// anofox_tab_metric_anomaly_iqr_events(<metric_anomaly_iqr arguments>) (alias: metric_anomaly_iqr_events)
	TableFunctionSet metric_iqr_events_set("anofox_tab_metric_anomaly_iqr_events");
	AddPrefixAritiesBindReplace(metric_iqr_events_set, "anofox_tab_metric_anomaly_iqr_events", metric_iqr_args, 2,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckMetricAnomalyIQREventsBindReplace));
	AddDateRangeParameters(metric_iqr_events_set);
	{
		FunctionDescription desc;
		desc.description = "Events companion of metric_anomaly_iqr (same arguments, including start_date/end_date). "
		                   "value is the daily metric, threshold the IQR bound it crossed."
		                   " Returns the contract `events` shape (ds, kind, label, severity, value, threshold, series): one row per "
		    "reference date whose check fails (severity 'alarm') or warns ('warning'), ordered by ds; passing dates "
		    "are not events.";
		desc.parameter_names = {"table_name", "date_column", "metric_column", "window_days",
		                        "k",          "mode",        "reference_date"};
		desc.examples = {"SELECT * FROM metric_anomaly_iqr_events('orders', 'order_date', start_date := DATE '2026-01-01', "
		                 "end_date := DATE '2026-01-31');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, metric_iqr_events_set, "metric_anomaly_iqr_events",
		                                  {std::move(desc)});
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
	AddDateRangeParameters(rolling_set_set);
	{
		FunctionDescription desc;
		desc.description =
		    "Asserts that the share of values contained in the allowed set over the trailing window_days days "
		    "(ending at the reference date, inclusive) is at least min_rate. With start_date := DATE, "
		    "end_date := DATE (instead of reference_date) returns one row per day of the inclusive range, ordered "
		    "by date.";
		desc.parameter_names = {"table_name",  "column_name", "allowed_values", "date_column",
		                        "window_days", "min_rate",    "reference_date"};
		desc.examples = {
		    "SELECT * FROM rolling_values_in_set('orders', 'status', ['completed', 'pending'], 'order_date', 14);",
		    "SELECT * FROM rolling_values_in_set('orders', 'status', ['completed', 'pending'], 'order_date', "
		    "start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, rolling_set_set, "rolling_values_in_set", {std::move(desc)});
	}

	// anofox_tab_rolling_values_in_set_events(<rolling_values_in_set arguments>)
	// (alias: rolling_values_in_set_events)
	TableFunctionSet rolling_events_set("anofox_tab_rolling_values_in_set_events");
	AddPrefixAritiesBindReplace(rolling_events_set, "anofox_tab_rolling_values_in_set_events", rolling_set_args, 4,
	                            DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, CheckRollingValuesInSetEventsBindReplace));
	AddDateRangeParameters(rolling_events_set);
	{
		FunctionDescription desc;
		desc.description = "Events companion of rolling_values_in_set (same arguments, including "
		                   "start_date/end_date). value is the in-set rate, threshold min_rate."
		                   " Returns the contract `events` shape (ds, kind, label, severity, value, threshold, series): one row per "
		    "reference date whose check fails (severity 'alarm') or warns ('warning'), ordered by ds; passing dates "
		    "are not events.";
		desc.parameter_names = {"table_name",  "column_name", "allowed_values", "date_column",
		                        "window_days", "min_rate",    "reference_date"};
		desc.examples = {"SELECT * FROM rolling_values_in_set_events('orders', 'status', ['completed', 'pending'], "
		                 "'order_date', 7, 1.0, start_date := DATE '2026-01-01', end_date := DATE '2026-01-31');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, rolling_events_set, "rolling_values_in_set_events",
		                                  {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

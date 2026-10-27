#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace anofox {

void RegisterCheckFunctions(ExtensionLoader &loader);

// Shared with the check-suite runner (anofox_check_suite.cpp):

// Parse a caller-supplied expression (compliance, params.expression, filter_expr) on its own.
// It must be exactly one SQL expression: text that does not parse throws "<function_name>:
// failed to parse expression '<expr>': <cause>"; several statements, a list, a FROM/WHERE/
// GROUP BY/ORDER BY/... clause, a set operation or an alias throw "<function_name>: expression
// '<expr>' must be a single expression". The expression is still executed with the caller's
// privileges (same trust model as running SQL directly).
unique_ptr<ParsedExpression> ParseSingleExpression(ClientContext &context, const string &expr,
                                                   const string &function_name);
// ParseSingleExpression written back as SQL (comments dropped): the text to embed in generated
// queries, so nothing of the caller's text outside the expression can reach them
string ValidateBooleanExpression(ClientContext &context, const string &expr, const string &function_name);

// window_days of the time-aware checks: 1 to MAX_WINDOW_DAYS (100 years)
constexpr int64_t MAX_WINDOW_DAYS = 36500;

// Throw "<prefix> '<pattern>' is not a valid regular expression: <cause>" when DuckDB's
// regexp functions reject `pattern`
void ValidateRegexPattern(ClientContext &context, const string &pattern, const string &prefix);

// SQL expression counting duplicate keys over the current group: rows beyond the first of
// each distinct key; NULL keys are duplicates of each other only. A compound key compares
// NULL components as equal.
string BuildDuplicateCountExpr(const vector<string> &column_names);

// AGG(double_expr) over the finite values; NaN when the input contains a NaN/Inf value.
string BuildFiniteAggregateExpr(const string &agg_upper, const string &double_expr);
// COUNT of the NaN/Inf values of double_expr.
string BuildNonFiniteCountExpr(const string &double_expr);

// An aggregate of a numeric column, for agg_check and the run_checks 'agg' type. MIN, MAX
// and SUM of an integer or DECIMAL column and MEDIAN of a DECIMAL column are computed in that
// exact type (a HUGEINT or a DECIMAL(38,s)) and compared exactly; AVG, STDDEV and every
// aggregate of a FLOAT/DOUBLE column are DOUBLE (over the finite values, NaN for NaN/Inf).
struct AggregateSQL {
	// The aggregate as DOUBLE (the result's value column)
	string value_expr;
	// The aggregate in its exact type (exact_type); empty for a DOUBLE aggregate
	string exact_expr;
	string exact_type;
	uint8_t exact_scale = 0;
	// COUNT of the NaN/Inf values; empty when the column cannot hold them
	string nonfinite_count_expr;

	bool IsExact() const {
		return !exact_expr.empty();
	}
};
AggregateSQL BuildAggregateSQL(const string &agg_upper, const string &column_sql, const LogicalType &column_type);

// SQL predicate: value_sql (the aggregate, in the exact type when agg.IsExact(), else DOUBLE)
// is below the lower (is_lower) or above the upper bound `threshold`, a DOUBLE; FALSE for a
// NULL threshold. An exact aggregate is compared with the decimal the threshold is written as
// (918624.95, not the binary DOUBLE 918624.94999...).
string BoundViolationSQL(const string &value_sql, const Value &threshold, const AggregateSQL &agg, bool is_lower);
// The threshold of an exact aggregate as message text (SQL VARCHAR expression): the decimal it
// is written as, with at least the aggregate's scale (200 on a DECIMAL(10,2) column is 200.00)
string ThresholdTextSQL(const Value &threshold, const AggregateSQL &agg);

// A lower or upper threshold as the checks use it: NULL, -inf for a lower and +inf for an upper
// bound mean no bound and come back as NULL; NaN and the other infinity throw
// "<function_name>: <name> must not be NaN (...)".
Value NormalizeBoundThreshold(const Value &threshold, const string &function_name, const string &name, bool is_lower);

// SQL condition: the value of column_sql (a DATE, TIMESTAMP or TIMESTAMP WITH TIME ZONE column,
// or text read as a `text_cast_type` such as "DATE") is infinity or -infinity. FALSE for NULL.
string InfiniteTimeSQL(const string &column_sql, const LogicalType &type, const string &text_cast_type);

// Whether a column of `type` can hold the dates / timestamps of a time-aware check or a
// freshness check: DATE, TIMESTAMP (any precision), TIMESTAMP WITH TIME ZONE, or VARCHAR (text
// parsed per row; a value that does not parse is skipped and counted)
bool IsTimeColumnType(const LogicalType &type);
// "has type <TYPE>; expected DATE, TIMESTAMP, TIMESTAMP WITH TIME ZONE or a VARCHAR holding
// <what>" for a column that is no time column (IsTimeColumnType)
string TimeColumnTypeError(const LogicalType &type, const string &what);

// SUM and AVG of a HUGEINT or a DECIMAL column wider than 18 digits accumulate in a HUGEINT
// and raise "Overflow in HUGEINT addition" when the sum leaves its range. The bound (a SQL
// DOUBLE literal, with a margin) for SumOverflowRiskSQL; empty when `agg_upper` of `type`
// cannot overflow.
string AggregateOverflowBoundSQL(const string &agg_upper, const LogicalType &type);
// SQL condition: the exact SUM of column_sql could overflow, i.e. the DOUBLE sum of its positive
// or of its negative values reaches bound_sql (AggregateOverflowBoundSQL). Every partial sum of
// the exact aggregate lies between those two, whatever the order of the rows. over_clause is
// appended to each aggregate (" OVER (PARTITION BY ...)" for a window; empty for a plain one).
string SumOverflowRiskSQL(const string &column_sql, const string &bound_sql, const string &over_clause = "");

// The baseline columns of rel_count_change ("<avg> AS baseline_avg, <days> AS baseline_days")
// over a CTE daily(d DATE, c) for the reference date ref_expr: the window_days days strictly
// before it. By default the days with rows are averaged; with fill_missing_days the calendar
// days of the window from the first day with data on count, a day without rows as 0.
string BuildRelCountBaselineSQL(const string &ref_expr, int64_t window_days, bool fill_missing_days);

// How many offending values a failing values_in_set / duplicate_count / match_rate message shows
constexpr int SAMPLE_LIMIT = 5;
// The SAMPLE_LIMIT smallest distinct values of the VARCHAR expression `value_sql` on the rows
// where `condition_sql` holds (an aggregate)
string SampleAggregateSQL(const string &value_sql, const string &condition_sql);
// The VARCHAR text of a key: the value itself ('NULL' for NULL) for one column, the ROW text
// '(a, b)' for several
string KeyTextSQL(const vector<string> &key_exprs);
// The message suffix "; samples: 'a', 'b'" for the VARCHAR[] expression `samples_sql`; '' when
// it is empty or NULL
string SamplesSuffixSQL(const string &samples_sql);

// Build a ['a', 'b', ...]::VARCHAR[] literal from a LIST value.
string BuildVarcharArrayLiteral(const Value &list_value);

} // namespace anofox
} // namespace duckdb

#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

void RegisterCheckFunctions(ExtensionLoader &loader);

// Shared with the check-suite runner (anofox_check_suite.cpp):

// Validate that a caller-supplied string is a single SQL boolean expression.
// Rejects multi-statement payloads; the expression is still executed with the
// caller's privileges (same trust model as running SQL directly).
void ValidateBooleanExpression(ClientContext &context, const string &expr, const string &function_name);

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

// The baseline columns of rel_count_change ("<avg> AS baseline_avg, <days> AS baseline_days")
// over a CTE daily(d DATE, c) for the reference date ref_expr: the window_days days strictly
// before it. By default the days with rows are averaged; with fill_missing_days the calendar
// days of the window from the first day with data on count, a day without rows as 0.
string BuildRelCountBaselineSQL(const string &ref_expr, int64_t window_days, bool fill_missing_days);

// Build a ['a', 'b', ...]::VARCHAR[] literal from a LIST value.
string BuildVarcharArrayLiteral(const Value &list_value);

} // namespace anofox
} // namespace duckdb

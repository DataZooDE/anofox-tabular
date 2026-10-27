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

// The baseline columns of rel_count_change ("<avg> AS baseline_avg, <days> AS baseline_days")
// over a CTE daily(d DATE, c) for the reference date ref_expr: the window_days days strictly
// before it. By default the days with rows are averaged; with fill_missing_days the calendar
// days of the window from the first day with data on count, a day without rows as 0.
string BuildRelCountBaselineSQL(const string &ref_expr, int64_t window_days, bool fill_missing_days);

// Build a ['a', 'b', ...]::VARCHAR[] literal from a LIST value.
string BuildVarcharArrayLiteral(const Value &list_value);

} // namespace anofox
} // namespace duckdb

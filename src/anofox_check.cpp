#include "anofox_check.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb_compat.hpp"
#include "anofox_tabular_banner.hpp"

#include <algorithm>
#include <cmath>

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

unique_ptr<ParsedExpression> ParseSingleExpression(ClientContext &context, const string &expr,
                                                   const string &function_name) {
	if (expr.empty()) {
		throw BinderException(function_name + ": expression must not be empty");
	}
	// The expression is parsed on its own (a SELECT list): nothing after it can close a
	// surrounding parenthesis, and a trailing -- comment ends with the text
	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = ParseWithContextSettings(context, "SELECT " + expr);
	} catch (const std::exception &ex) {
		// RawMessage: what() of a DuckDB 2.0 exception is a JSON object
		auto cause = ErrorData(ex).RawMessage();
		auto newline = cause.find('\n');
		if (newline != string::npos) {
			cause = cause.substr(0, newline);
		}
		throw BinderException(function_name + ": failed to parse expression '" + expr + "': " + cause);
	}
	const string not_single = function_name + ": expression '" + expr + "' must be a single expression";
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException(not_single);
	}
	auto &select = statements[0]->Cast<SelectStatement>();
	if (!select.node || select.node->type != QueryNodeType::SELECT_NODE || !select.node->modifiers.empty() ||
	    !select.node->cte_map.map.empty()) {
		throw BinderException(not_single);
	}
	auto &node = select.node->Cast<SelectNode>();
	const bool has_from = node.from_table && node.from_table->type != TableReferenceType::EMPTY_FROM;
	if (node.select_list.size() != 1 || has_from || node.where_clause || node.having || node.qualify ||
	    node.sample || !node.groups.group_expressions.empty() || !node.groups.grouping_sets.empty() ||
	    node.select_list[0]->HasAlias()) {
		throw BinderException(not_single);
	}
	return std::move(node.select_list[0]);
}

string ValidateBooleanExpression(ClientContext &context, const string &expr, const string &function_name) {
	return ParseSingleExpression(context, expr, function_name)->ToString();
}

void ValidateRegexPattern(ClientContext &context, const string &pattern, const string &prefix) {
	try {
		auto statements =
		    ParseWithContextSettings(context, "SELECT regexp_matches('', '" + EscapeSqlStringLiteral(pattern) + "')");
		auto binder = Binder::CreateBinder(context);
		binder->Bind(*statements[0]);
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::FATAL ||
		    error.Type() == ExceptionType::INTERNAL) {
			throw;
		}
		auto cause = error.RawMessage();
		auto newline = cause.find('\n');
		if (newline != string::npos) {
			cause = cause.substr(0, newline);
		}
		throw BinderException(prefix + " '" + pattern + "' is not a valid regular expression: " + cause);
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

bool IsWideSumAggregate(const string &agg_upper, const LogicalType &type) {
	if (agg_upper != "SUM" && agg_upper != "AVG") {
		return false;
	}
	return type.id() == LogicalTypeId::HUGEINT ||
	       (type.id() == LogicalTypeId::DECIMAL && DecimalType::GetWidth(type) > 18);
}

string WideSumRiskColumnSQL(const string &column_sql, const LogicalType &type, const string &rows_sql) {
	// Below 10^38 (the DECIMAL(38) range, inside the HUGEINT one) in unscaled units, with a
	// margin for the rounding of the DOUBLE sums
	double bound = 9.9e37;
	if (type.id() == LogicalTypeId::DECIMAL) {
		bound /= std::pow(10.0, DecimalType::GetScale(type));
	}
	const string value = "CAST(" + column_sql + " AS DOUBLE)";
	return "(SELECT COALESCE(GREATEST(SUM(GREATEST(" + value + ", 0)), SUM(GREATEST(-" + value + ", 0))) >= CAST('" +
	       Value::DOUBLE(bound).ToString() + "' AS DOUBLE), FALSE) " + rows_sql + ") AS " + WIDE_SUM_RISK_COLUMN;
}

namespace {

// The pieces of an exact SUM of a HUGEINT or wide DECIMAL column that cannot overflow (for the
// rows where WIDE_SUM_RISK_COLUMN is set; NULL otherwise). Each value, as its unscaled HUGEINT
// u, is split into u >> 64 (a BIGINT-sized high part) and u & (2^64 - 1) (the low 64 bits),
// which are summed separately: neither sum can leave the HUGEINT range below 2^63 rows. The
// exact sum is high * 2^64 + low with high = high_sum + (low_sum >> 64), low = low_sum & mask.
struct SplitSumSQL {
	string present; // whether the split sums were taken (the risk column was set on some row)
	string high;
	string low;
	string fits_hugeint; // the exact sum is within the HUGEINT range
	string total;        // the exact sum as HUGEINT (only where fits_hugeint)
};

SplitSumSQL BuildSplitSumSQL(const string &column_sql, const LogicalType &type) {
	string unscaled = column_sql;
	if (type.id() == LogicalTypeId::DECIMAL) {
		// DuckDB prints a DECIMAL with all its scale digits: without the point it is the
		// unscaled integer
		unscaled = DecimalType::GetScale(type) == 0
		               ? "CAST(" + column_sql + " AS HUGEINT)"
		               : "CAST(replace(CAST(" + column_sql + " AS VARCHAR), '.', '') AS HUGEINT)";
	}
	const string mask = "CAST('18446744073709551615' AS HUGEINT)";
	const string when_risk = string("CASE WHEN ") + WIDE_SUM_RISK_COLUMN + " THEN ";
	const string high_sum = "SUM(" + when_risk + "(" + unscaled + ") >> 64 END)";
	const string low_sum = "SUM(" + when_risk + "(" + unscaled + ") & " + mask + " END)";
	SplitSumSQL result;
	result.present = "(" + high_sum + " IS NOT NULL)";
	result.high = "(" + high_sum + " + (" + low_sum + " >> 64))";
	result.low = "(" + low_sum + " & " + mask + ")";
	result.fits_hugeint = "(" + result.high + " BETWEEN -9223372036854775808 AND 9223372036854775807)";
	result.total = "(" + result.high + " * CAST('18446744073709551616' AS HUGEINT) + " + result.low + ")";
	return result;
}

} // anonymous namespace

AggregateSQL BuildAggregateSQL(const string &agg_upper, const string &column_sql, const LogicalType &column_type) {
	AggregateSQL result;
	bool is_integer = false;
	switch (column_type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		is_integer = true;
		break;
	default:
		break;
	}
	const bool is_decimal = column_type.id() == LogicalTypeId::DECIMAL;
	if (IsWideSumAggregate(agg_upper, column_type)) {
		// Rows without the risk of an overflow (WideSumRiskColumnSQL) take DuckDB's own
		// aggregate; otherwise the exact sum is assembled from the split sums
		const string plain = "CASE WHEN " + string(WIDE_SUM_RISK_COLUMN) + " THEN NULL ELSE " + column_sql + " END";
		auto split = BuildSplitSumSQL(column_sql, column_type);
		const uint8_t scale = is_decimal ? DecimalType::GetScale(column_type) : 0;
		if (agg_upper == "AVG") {
			// The average itself always fits; its exact sum must fit the HUGEINT accumulator
			// sum / (count * 10^scale) as quotient plus remainder, so the DOUBLE is rounded once
			// like DuckDB's own average (6e37 / 3 is 2e+37, not 2.0000000000000001e+37)
			string average = "CAST(" + split.total + " AS DOUBLE) / (CAST(COUNT(" + column_sql +
			                 ") AS DOUBLE) * CAST('1e" + std::to_string(scale) + "' AS DOUBLE))";
			if (scale <= 18) {
				const string divisor = "(CAST(COUNT(" + column_sql + ") AS HUGEINT) * CAST('1" + string(scale, '0') +
				                       "' AS HUGEINT))";
				average = "CAST(" + split.total + " // " + divisor + " AS DOUBLE) + CAST(" + split.total + " % " +
				          divisor + " AS DOUBLE) / CAST(" + divisor + " AS DOUBLE)";
			}
			result.value_expr = "CASE WHEN NOT " + split.present + " THEN CAST(AVG(" + plain + ") AS DOUBLE) WHEN " +
			                    split.fits_hugeint + " THEN " + average + " END";
			result.overflow_expr = "CASE WHEN NOT " + split.present + " THEN FALSE ELSE NOT " + split.fits_hugeint + " END";
			return result;
		}
		result.exact_scale = scale;
		if (is_decimal) {
			result.exact_type = "DECIMAL(38," + std::to_string(scale) + ")";
			// |sum| < 10^38 unscaled; the unscaled integer back to the DECIMAL by its exponent
			const string limit = "CAST('100000000000000000000000000000000000000' AS HUGEINT)";
			const string in_range = "(" + split.total + " < " + limit + " AND " + split.total + " > -" + limit + ")";
			// The unscaled integer back to the DECIMAL: its digits with the point inserted
			string as_decimal = "CAST(" + split.total + " AS DECIMAL(38,0))";
			if (scale > 0) {
				const string unit = "CAST('1" + string(scale, '0') + "' AS HUGEINT)";
				const string magnitude = "abs(" + split.total + ")";
				as_decimal = "CAST(CASE WHEN " + split.total + " < 0 THEN '-' ELSE '' END || CAST(" + magnitude +
				             " // " + unit + " AS VARCHAR) || '.' || lpad(CAST(" + magnitude + " % " + unit +
				             " AS VARCHAR), " + std::to_string(scale) + ", '0') AS " + result.exact_type + ")";
			}
			result.exact_expr = "CASE WHEN NOT " + split.present + " THEN CAST(SUM(" + plain + ") AS " +
			                    result.exact_type + ") WHEN NOT " + split.fits_hugeint + " THEN NULL WHEN " + in_range +
			                    " THEN " + as_decimal + " END";
			result.overflow_expr = "CASE WHEN NOT " + split.present + " THEN FALSE WHEN NOT " + split.fits_hugeint +
			                       " THEN TRUE ELSE NOT " + in_range + " END";
		} else {
			result.exact_type = "HUGEINT";
			result.exact_expr = "CASE WHEN NOT " + split.present + " THEN CAST(SUM(" + plain + ") AS HUGEINT) WHEN " +
			                    split.fits_hugeint + " THEN " + split.total + " END";
			result.overflow_expr = "CASE WHEN NOT " + split.present + " THEN FALSE ELSE NOT " + split.fits_hugeint + " END";
		}
		result.value_expr = "CAST(" + result.exact_expr + " AS DOUBLE)";
		return result;
	}
	if (is_integer || is_decimal) {
		// MEDIAN of an integer column interpolates (a DOUBLE); of a DECIMAL column it stays DECIMAL
		const bool exact =
		    agg_upper == "MIN" || agg_upper == "MAX" || agg_upper == "SUM" || (agg_upper == "MEDIAN" && is_decimal);
		if (exact) {
			result.exact_scale = is_decimal ? DecimalType::GetScale(column_type) : 0;
			result.exact_type =
			    is_decimal ? "DECIMAL(38," + std::to_string(result.exact_scale) + ")" : string("HUGEINT");
			result.exact_expr = "CAST(" + agg_upper + "(" + column_sql + ") AS " + result.exact_type + ")";
			result.value_expr = "CAST(" + result.exact_expr + " AS DOUBLE)";
		} else {
			// AVG and STDDEV of an exact column: DuckDB's own DOUBLE result (AVG sums exactly)
			result.value_expr = "CAST(" + agg_upper + "(" + column_sql + ") AS DOUBLE)";
		}
		return result;
	}
	string double_expr = "CAST(" + column_sql + " AS DOUBLE)";
	result.value_expr = BuildFiniteAggregateExpr(agg_upper, double_expr);
	result.nonfinite_count_expr = BuildNonFiniteCountExpr(double_expr);
	return result;
}

namespace {

// A finite DOUBLE as the decimal it is written as (its shortest round-trip text), without an
// exponent: 918624.95 is {false, "918624", "95"}, 1e-07 is {false, "0", "0000001"}
struct PlainDecimal {
	bool negative = false;
	string integer_digits;  // no leading zeros; "0" for none
	string fraction_digits; // no trailing zeros

	string Text(const string &fraction) const {
		return (negative ? "-" : "") + integer_digits + (fraction.empty() ? "" : "." + fraction);
	}
};

PlainDecimal ToPlainDecimal(double number) {
	string text = Value::DOUBLE(number).ToString();
	PlainDecimal result;
	size_t pos = 0;
	if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
		result.negative = text[0] == '-';
		pos = 1;
	}
	auto exponent_pos = text.find_first_of("eE", pos);
	int64_t exponent = 0;
	if (exponent_pos != string::npos) {
		exponent = std::stoll(text.substr(exponent_pos + 1));
	}
	string mantissa = text.substr(pos, exponent_pos == string::npos ? string::npos : exponent_pos - pos);
	auto point = mantissa.find('.');
	string digits = mantissa;
	int64_t point_index = static_cast<int64_t>(mantissa.size());
	if (point != string::npos) {
		digits = mantissa.substr(0, point) + mantissa.substr(point + 1);
		point_index = static_cast<int64_t>(point);
	}
	point_index += exponent;
	const auto digit_count = static_cast<int64_t>(digits.size());
	if (point_index <= 0) {
		result.integer_digits = "0";
		result.fraction_digits = string(static_cast<size_t>(-point_index), '0') + digits;
	} else if (point_index >= digit_count) {
		result.integer_digits = digits + string(static_cast<size_t>(point_index - digit_count), '0');
	} else {
		result.integer_digits = digits.substr(0, static_cast<size_t>(point_index));
		result.fraction_digits = digits.substr(static_cast<size_t>(point_index));
	}
	auto first_nonzero = result.integer_digits.find_first_not_of('0');
	result.integer_digits = first_nonzero == string::npos ? "0" : result.integer_digits.substr(first_nonzero);
	auto last_nonzero = result.fraction_digits.find_last_not_of('0');
	result.fraction_digits = last_nonzero == string::npos ? "" : result.fraction_digits.substr(0, last_nonzero + 1);
	if (result.integer_digits == "0" && result.fraction_digits.empty()) {
		result.negative = false;
	}
	return result;
}

} // anonymous namespace

string BoundViolationSQL(const string &value_sql, const Value &threshold, const AggregateSQL &agg, bool is_lower) {
	if (threshold.IsNull()) {
		return "FALSE";
	}
	if (!agg.IsExact()) {
		return "(" + value_sql + (is_lower ? " < " : " > ") + DoubleThresholdSQL(threshold) + ")";
	}
	auto decimal = ToPlainDecimal(threshold.GetValue<double>());
	const idx_t integral_digits = decimal.integer_digits == "0" ? 0 : decimal.integer_digits.size();
	if (integral_digits + agg.exact_scale > 38) {
		// Beyond every value of the exact type: a lower bound above them all is always
		// violated, an upper bound above them all never (mirrored for a negative bound)
		return (is_lower != decimal.negative) ? "TRUE" : "FALSE";
	}
	string op = is_lower ? "<" : ">";
	string fraction = decimal.fraction_digits;
	if (fraction.size() > agg.exact_scale) {
		// The value has exact_scale decimals: for such a value, v < 0.949 <=> v <= 0.94 and
		// v > -0.005 <=> v >= -0.00, comparing with the threshold truncated to that scale
		fraction = fraction.substr(0, agg.exact_scale);
		if (is_lower && !decimal.negative) {
			op = "<=";
		} else if (!is_lower && decimal.negative) {
			op = ">=";
		}
	}
	return "(" + value_sql + " " + op + " CAST('" + decimal.Text(fraction) + "' AS " + agg.exact_type + "))";
}

string ThresholdTextSQL(const Value &threshold, const AggregateSQL &agg) {
	if (threshold.IsNull()) {
		return "CAST(NULL AS VARCHAR)";
	}
	auto decimal = ToPlainDecimal(threshold.GetValue<double>());
	// At least the value's scale, as DuckDB prints the value: 310.50 > upper 200.00
	string fraction = decimal.fraction_digits;
	if (fraction.size() < agg.exact_scale) {
		fraction += string(agg.exact_scale - fraction.size(), '0');
	}
	return "'" + decimal.Text(fraction) + "'";
}

Value NormalizeBoundThreshold(const Value &threshold, const string &function_name, const string &name, bool is_lower) {
	if (threshold.IsNull()) {
		return Value(LogicalType(LogicalTypeId::DOUBLE));
	}
	double number = threshold.GetValue<double>();
	const string open_side = is_lower ? "NULL or -inf mean no lower bound" : "NULL or +inf mean no upper bound";
	if (std::isnan(number)) {
		throw BinderException("%s: %s must not be NaN (%s)", function_name, name, open_side);
	}
	if (std::isinf(number)) {
		if ((number < 0) == is_lower) {
			return Value(LogicalType(LogicalTypeId::DOUBLE));
		}
		throw BinderException("%s: %s must not be %s (%s)", function_name, name, number > 0 ? "+inf" : "-inf",
		                      open_side);
	}
	return threshold;
}

bool IsTimeColumnType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::VARCHAR:
		return true;
	default:
		return false;
	}
}

string TimeColumnTypeError(const LogicalType &type, const string &what) {
	return "has type " + type.ToString() + "; expected DATE, TIMESTAMP, TIMESTAMP WITH TIME ZONE or a VARCHAR holding " +
	       what;
}

string AggregateOverflowBoundSQL(const string &agg_upper, const LogicalType &type) {
	if (agg_upper != "SUM" && agg_upper != "AVG") {
		return string();
	}
	// The largest unscaled sum that is safe: below 10^38 (the DECIMAL(38) range, inside the
	// HUGEINT one), with a margin for the rounding of the DOUBLE sum
	double bound = 9.9e37;
	if (type.id() == LogicalTypeId::DECIMAL) {
		if (DecimalType::GetWidth(type) <= 18) {
			return string();
		}
		bound /= std::pow(10.0, DecimalType::GetScale(type));
	} else if (type.id() != LogicalTypeId::HUGEINT) {
		return string();
	}
	return "CAST('" + Value::DOUBLE(bound).ToString() + "' AS DOUBLE)";
}

string SumOverflowRiskSQL(const string &column_sql, const string &bound_sql, const string &over_clause) {
	const string value = "CAST(" + column_sql + " AS DOUBLE)";
	return "(GREATEST(COALESCE(SUM(GREATEST(" + value + ", 0))" + over_clause + ", 0), COALESCE(SUM(GREATEST(-" +
	       value + ", 0))" + over_clause + ", 0)) >= " + bound_sql + ")";
}

string InfiniteTimeSQL(const string &column_sql, const LogicalType &type, const string &text_cast_type) {
	string value =
	    type.id() == LogicalTypeId::VARCHAR ? "TRY_CAST(" + column_sql + " AS " + text_cast_type + ")" : column_sql;
	return "COALESCE(NOT isfinite(" + value + "), FALSE)";
}

// The message suffixes of the rows a time-aware check leaves out, as a SQL VARCHAR expression:
// "; <n> row(s) skipped (<date_column> is not a date)" for text that does not parse (a VARCHAR
// date column) and "; <n> row(s) with an infinite date ignored"; '' when there are none
static string InfiniteDatesSuffixSQL(const string &table_ref, const string &date_column, const LogicalType &date_type) {
	const string date_id = QuoteSqlIdentifier(date_column);
	string suffix;
	if (date_type.id() == LogicalTypeId::VARCHAR) {
		string skipped = "(SELECT COUNT(*) FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL AND (" +
		                 UtcDateSQL(date_id, date_type) + ") IS NULL)";
		suffix = "CASE WHEN " + skipped + " > 0 THEN '; ' || CAST(" + skipped + " AS VARCHAR) || ' row(s) skipped (" +
		         EscapeSqlStringLiteral(date_column) + " is not a date)' ELSE '' END || ";
	}
	string count = "(SELECT COUNT(*) FROM " + table_ref + " WHERE " + InfiniteTimeSQL(date_id, date_type, "DATE") + ")";
	return suffix + "CASE WHEN " + count + " > 0 THEN '; ' || CAST(" + count +
	       " AS VARCHAR) || ' row(s) with an infinite date ignored' ELSE '' END";
}

// Helper: Generate SQL for aggregate assertions (avg/min/max/sum/median/stddev vs thresholds).
// agg_keyword must come from the bind-time whitelist, never from raw user input. min, max, sum
// (and median of a DECIMAL) of an exact column compare exactly (BuildAggregateSQL).
// Empty-input semantics: an empty table or all-NULL column yields value = NULL and
// passes trivially with an explicit message.
static string GenerateAggCheckSQL(const string &table_ref, const string &column_name, const LogicalType &column_type,
                                  const string &agg_keyword, const Value &lower, const Value &upper) {
	string lower_val = DoubleThresholdSQL(lower);
	string upper_val = DoubleThresholdSQL(upper);
	string col_id = QuoteSqlIdentifier(column_name);
	string agg_upper = agg_keyword;
	std::transform(agg_upper.begin(), agg_upper.end(), agg_upper.begin(), ::toupper);
	// SUM/AVG of a HUGEINT or wide DECIMAL column: summed without DuckDB's overflow on a partial
	// sum (BuildAggregateSQL); an exact sum beyond its type is a clear error instead of DuckDB's
	// "Overflow in HUGEINT addition"
	string source = table_ref;
	string overflow_status = "";
	const bool wide_sum = IsWideSumAggregate(agg_upper, column_type);
	if (wide_sum) {
		source = "(SELECT *, " + WideSumRiskColumnSQL(col_id, column_type, "FROM " + table_ref) + " FROM " +
		         table_ref + ") __src";
		overflow_status = "  WHEN overflow THEN error('" +
		                  EscapeSqlStringLiteral("agg_check: " + agg_keyword + " of " + column_type.ToString() +
		                                         " column '" + column_name + "' overflows its exact sum") +
		                  "') ";
	}
	auto agg = BuildAggregateSQL(agg_upper, col_id, column_type);
	string compared = agg.IsExact() ? "exact_value" : "value";
	string below = BoundViolationSQL(compared, lower, agg, true);
	string above = BoundViolationSQL(compared, upper, agg, false);
	string value_text = "CAST(" + compared + " AS VARCHAR)";
	string lower_text = agg.IsExact() ? ThresholdTextSQL(lower, agg) : "CAST(" + lower_val + " AS VARCHAR)";
	string upper_text = agg.IsExact() ? ThresholdTextSQL(upper, agg) : "CAST(" + upper_val + " AS VARCHAR)";
	string nonfinite = agg.nonfinite_count_expr.empty() ? "0" : agg.nonfinite_count_expr;

	return "SELECT "
		"CASE " + overflow_status +
		"  WHEN value IS NULL THEN 'pass' "
		"  WHEN isnan(value) THEN 'fail' "
		"  WHEN " + below + " OR " + above + " THEN 'fail' "
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
		"  WHEN " + below + " THEN "
		"    '" + agg_keyword + " value ' || " + value_text + " || ' is below minimum ' || " + lower_text + " "
		"  WHEN " + above + " THEN "
		"    '" + agg_keyword + " value ' || " + value_text + " || ' exceeds maximum ' || " + upper_text + " "
		"  ELSE '" + agg_keyword + " value ' || " + value_text + " || ' is within acceptable range' "
		"END AS message "
		"FROM (SELECT " + agg.value_expr + " AS value, " +
		(agg.IsExact() ? agg.exact_expr : string("CAST(NULL AS DOUBLE)")) + " AS exact_value, " + nonfinite +
		" AS nonfinite_count, " + (wide_sum ? "COALESCE(" + agg.overflow_expr + ", FALSE)" : string("FALSE")) +
		" AS overflow FROM " + source + ")";
}

string SampleAggregateSQL(const string &value_sql, const string &condition_sql) {
	return "min(DISTINCT " + value_sql + ", " + std::to_string(SAMPLE_LIMIT) + ") FILTER (WHERE " + condition_sql +
	       ")";
}

string KeyTextSQL(const vector<string> &key_exprs) {
	if (key_exprs.size() == 1) {
		return "COALESCE(CAST(" + key_exprs[0] + " AS VARCHAR), 'NULL')";
	}
	return "CAST(row(" + StringUtil::Join(key_exprs, ", ") + ") AS VARCHAR)";
}

string SamplesSuffixSQL(const string &samples_sql) {
	return "CASE WHEN len(" + samples_sql + ") > 0 THEN '; samples: ''' || array_to_string(" + samples_sql +
	       ", ''', ''') || '''' ELSE '' END";
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
	vector<string> key_exprs;
	for (auto &column : column_names) {
		key_exprs.push_back(QuoteSqlIdentifier(column));
	}
	// Up to SAMPLE_LIMIT duplicated keys for the message of a failing check (as run_checks shows)
	string samples = "(SELECT min(k, " + std::to_string(SAMPLE_LIMIT) + ") FROM (SELECT " + KeyTextSQL(key_exprs) +
	                 " AS k FROM " + table_ref + " GROUP BY " + StringUtil::Join(key_exprs, ", ") +
	                 " HAVING COUNT(*) > 1))";

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
		"    ' rows exceeds maximum ' || CAST(" + max_val + " AS VARCHAR) || " + SamplesSuffixSQL(samples) + " "
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
	// A row with a NULL key column matches nothing: it is left out of the rate (null_rate
	// reports it) and counted in null_key_count
	string any_key_null;
	vector<string> key_aliases;
	for (size_t i = 0; i < left_keys.size(); i++) {
		string alias = "k" + std::to_string(i);
		key_aliases.push_back(alias);
		any_key_null += (i > 0 ? " OR " : "") + alias + " IS NULL";
		if (i > 0) {
			left_cols += ", ";
			right_cols += ", ";
			join_cond += " AND ";
		}
		left_cols += QuoteSqlIdentifier(left_keys[i]) + " AS " + alias;
		right_cols += QuoteSqlIdentifier(right_keys[i]) + " AS " + alias;
		join_cond += "l." + alias + " = r." + alias;
	}

	return "WITH l_all AS ("
		"SELECT " + left_cols + " FROM " + left_ref +
		"), "
		"l AS (SELECT * FROM l_all WHERE NOT (" + any_key_null + ")), "
		"r AS ("
		"SELECT DISTINCT " + right_cols + ", 1 AS matched FROM " + right_ref +
		"), "
		"joined AS ("
		"SELECT l.*, r.matched FROM l LEFT JOIN r ON " + join_cond +
		"), "
		"stats AS ("
		"SELECT "
		"  (SELECT COUNT(*) FROM l_all WHERE " + any_key_null + ") AS null_key_count, "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(COUNT(matched) AS BIGINT), 0) AS matched_count, "
		"  COALESCE(CAST(COUNT(matched) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS match_rate, "
		"  " + SampleAggregateSQL(KeyTextSQL(key_aliases), "matched IS NULL") + " AS samples "
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
		"  WHEN total_count = 0 AND null_key_count > 0 THEN 'No left rows with a non-NULL key (' || "
		"    CAST(null_key_count AS VARCHAR) || ' row(s) with a NULL key excluded); match rate check passed trivially' "
		"  WHEN total_count = 0 THEN 'Left table is empty (0 rows); match rate check passed trivially' "
		"  WHEN match_rate >= " + min_val + " THEN "
		"    'Match rate ' || " + RoundedNumberSQL("match_rate") + " || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'Match rate ' || " + RoundedNumberSQL("match_rate") + " || ' (' || CAST(matched_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is below minimum ' || CAST(" + min_val + " AS VARCHAR) || " +
		SamplesSuffixSQL("samples") + " "
		"END || CASE WHEN total_count > 0 AND null_key_count > 0 THEN '; ' || CAST(null_key_count AS VARCHAR) || "
		"  ' row(s) with a NULL key excluded' ELSE '' END AS message "
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
	// An infinite date (an SCD2 valid_to of the current version) is no day of data: ignored, and
	// the message says how many rows were
	string infinite = InfiniteTimeSQL(date_id, date_type, "DATE");
	string infinite_suffix = InfiniteDatesSuffixSQL(table_ref, date_column, date_type);
	string window_val = std::to_string(window_days);
	string cnt_expr = count_column.empty() ? "COUNT(*)" : "COUNT(DISTINCT " + QuoteSqlIdentifier(count_column) + ")";

	return "WITH daily AS ("
		"SELECT " + UtcDateSQL(date_id, date_type) + " AS d, " + cnt_expr + " AS c "
		"FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL AND NOT " + infinite + " GROUP BY 1"
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
		"END || " + infinite_suffix + " AS message "
		"FROM summary ORDER BY reference_date";
}

// Helper: Generate SQL for metric-level IQR anomaly detection.
// Computes a daily metric (row count or AVG of metric_column) over the window_days days
// strictly before the reference date, derives Q1/Q3 bounds with multiplier k, and flags
// the reference day when it falls outside the bounds selected by mode. In row-count mode a
// reference day without rows counts 0 rows; the baseline holds the days with rows only.
// Trivial-pass semantics: empty input, an empty baseline window or (with a metric_column) a
// reference day without a metric value pass with a message.
static string GenerateMetricAnomalyIQRSQL(const string &table_ref, const string &date_column,
                                          const LogicalType &date_type, const string &metric_column,
                                          int64_t window_days, const Value &k, const string &mode,
                                          const ReferenceDates &dates) {
	string k_val = DoubleThresholdSQL(k, "1.5");
	string date_id = QuoteSqlIdentifier(date_column);
	// An infinite date (an SCD2 valid_to of the current version) is no day of data: ignored, and
	// the message says how many rows were
	string infinite = InfiniteTimeSQL(date_id, date_type, "DATE");
	string infinite_suffix = InfiniteDatesSuffixSQL(table_ref, date_column, date_type);
	string window_val = std::to_string(window_days);
	string metric_expr =
	    metric_column.empty() ? "CAST(COUNT(*) AS DOUBLE)" : "AVG(CAST(" + QuoteSqlIdentifier(metric_column) + " AS DOUBLE))";

	// Row-count mode: a reference day without rows is a count of 0 (as in rel_count_change), so a
	// feed that stops fails instead of passing trivially. A metric_column has no value on such a day.
	string reference_metric = "(SELECT m FROM daily WHERE d = s.ref_d)";
	if (metric_column.empty()) {
		reference_metric = "CASE WHEN s.ref_d IS NULL THEN NULL ELSE COALESCE(" + reference_metric + ", 0) END";
	}

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
		"FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL AND NOT " + infinite + " GROUP BY 1"
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
		"  " + reference_metric + " AS metric_value, "
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
		"END || " + infinite_suffix + " AS message "
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
	// An infinite date (an SCD2 valid_to of the current version) is no day of data: ignored, and
	// the message says how many rows were
	string infinite = InfiniteTimeSQL(date_id, date_type, "DATE");
	string infinite_suffix = InfiniteDatesSuffixSQL(table_ref, date_column, date_type);
	string window_val = std::to_string(window_days);

	return "WITH dv AS ("
		"SELECT " + UtcDateSQL(date_id, date_type) + " AS d, " + col_str + " AS v, COUNT(*) AS n FROM " + table_ref +
		" WHERE " + date_id + " IS NOT NULL AND NOT " + infinite + " GROUP BY 1, 2"
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
		"END || " + infinite_suffix + " AS message "
		"FROM rates ORDER BY reference_date";
}

// Helper: the type of the date column of a time-aware check, so a TIMESTAMP WITH TIME ZONE
// column is bucketed by its UTC date (UtcDateSQL) whatever the session TimeZone. A column that
// cannot hold dates (a BIGINT, a TIME) is a binder error.
static LogicalType TimeColumnType(const string &function_name, const ResolvedColumn &date_column) {
	if (!IsTimeColumnType(date_column.type)) {
		throw BinderException("%s: date_column '%s' %s", function_name, date_column.name,
		                      TimeColumnTypeError(date_column.type, "dates"));
	}
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
	if (window_days > MAX_WINDOW_DAYS) {
		throw BinderException("%s: window_days must be at most %d (got %d)", function_name, MAX_WINDOW_DAYS,
		                      window_days);
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
	auto named_reference = duckdb_compat::FindNamedParameter(input.named_parameters, "reference_date");
	if (named_reference && !named_reference->IsNull()) {
		if (!dates.reference_date.IsNull()) {
			throw BinderException(function_name +
			                      ": reference_date is given twice (positionally and as reference_date :=)");
		}
		dates.reference_date = named_reference->DefaultCastAs(LogicalType(LogicalTypeId::DATE));
	}
	// A reference date at infinity has no days before it
	auto require_finite = [&](const Value &date, const char *name) {
		if (date.IsNull()) {
			return;
		}
		if (!Value::IsFinite(date.DefaultCastAs(LogicalType(LogicalTypeId::DATE)).GetValue<date_t>())) {
			throw BinderException("%s: %s must be a finite date", function_name, name);
		}
	};
	require_finite(dates.reference_date, "reference_date");
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
	require_finite(dates.start_date, "start_date");
	require_finite(dates.end_date, "end_date");
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

// Declare the optional start_date/end_date named parameters (#109) on every overload, and
// reference_date by name as an alternative to the positional argument
static void AddDateRangeParameters(TableFunctionSet &set) {
	duckdb_compat::ForEachFunction(set, [](TableFunction &func) {
		duckdb_compat::AddNamedParameter(func, "reference_date", LogicalType(LogicalTypeId::DATE));
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
	ValidateRegexPattern(context, pattern, "regex_match: pattern");
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
	auto lower = NormalizeBoundThreshold(input.inputs[3], "agg_check", "lower_threshold", true);
	auto upper = NormalizeBoundThreshold(input.inputs[4], "agg_check", "upper_threshold", false);
	ValidateThresholdOrder(lower, upper, "agg_check", "lower_threshold", "upper_threshold");
	auto column = RequireRelationColumns(context, "agg_check", table_name, {column_name})[0];
	RequireNumericColumn(context, "agg_check", table_name, column_name);

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateAggCheckSQL(table_ref, column.name, column.type, agg, lower, upper);
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
	// The parsed expression, written back as SQL: no comment or trailing text of the caller's
	// can reach the generated query
	expression = ValidateBooleanExpression(context, expression, "compliance");
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
	lower = NormalizeBoundThreshold(lower, "rel_count_change", "lower_threshold", true);
	upper = NormalizeBoundThreshold(upper, "rel_count_change", "upper_threshold", false);
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

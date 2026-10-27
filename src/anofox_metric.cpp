#include "anofox_metric.hpp"
#include "anofox_isolation_forest.hpp"
#include "anofox_dbscan.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_id_column.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb_compat.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include "anofox_tabular_banner.hpp"

namespace duckdb {
namespace anofox {

namespace {

// ParseSubquery, ValidateFiniteDouble and ParseCommaSeparatedColumns are shared
// helpers from anofox_sql_utils.hpp.

// Helper: Generate SQL for volume metrics
static string GenerateVolumeSQL(const string &table_ref, const Value &min_rows, const Value &max_rows) {
	string min_val = BigintThresholdSQL(min_rows);
	string max_val = BigintThresholdSQL(max_rows);

	return "SELECT "
		"CASE "
		"  WHEN (" + min_val + " IS NOT NULL AND row_count < " + min_val + ") OR "
		"       (" + max_val + " IS NOT NULL AND row_count > " + max_val + ") THEN 'fail' "
		"  ELSE 'pass' "
		"END AS status, "
		"row_count, "
		+ min_val + " AS min_threshold, "
		+ max_val + " AS max_threshold, "
		"CASE "
		"  WHEN (" + min_val + " IS NOT NULL AND row_count < " + min_val + ") THEN "
		"    'Row count ' || CAST(row_count AS VARCHAR) || ' is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"  WHEN (" + max_val + " IS NOT NULL AND row_count > " + max_val + ") THEN "
		"    'Row count ' || CAST(row_count AS VARCHAR) || ' exceeds maximum ' || CAST(" + max_val + " AS VARCHAR) "
		"  ELSE 'Row count ' || CAST(row_count AS VARCHAR) || ' is within acceptable range' "
		"END AS message "
		"FROM (SELECT COUNT(*) AS row_count FROM (SELECT * FROM " + table_ref + "))";
}

// Helper: Generate SQL for null rate metrics.
// Empty-input semantics: an empty table yields null_count = 0, total_count = 0, null_rate = 0.0
// and passes trivially with an explicit message (instead of NULL rate and message).
static string GenerateNullRateSQL(const string &table_ref, const string &column_name, const Value &max_null_rate) {
	string max_rate_val = DoubleThresholdSQL(max_null_rate, "1.0");
	string col_id = QuoteSqlIdentifier(column_name);

	return "SELECT "
		"CASE WHEN null_rate <= " + max_rate_val + " THEN 'pass' ELSE 'fail' END AS status, "
		"null_count, "
		"total_count, "
		"null_rate, "
		+ max_rate_val + " AS threshold, "
		"CASE "
		"  WHEN total_count = 0 THEN 'Table is empty (0 rows); null rate check passed trivially' "
		"  WHEN null_rate <= " + max_rate_val + " THEN "
		"    'Null rate ' || " + RoundedNumberSQL("null_rate") + " || ' (' || CAST(null_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'Null rate ' || " + RoundedNumberSQL("null_rate") + " || ' (' || CAST(null_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') exceeds maximum ' || CAST(" + max_rate_val + " AS VARCHAR) "
		"END AS message "
		"FROM (SELECT "
		"  COALESCE(CAST(SUM(CASE WHEN " + col_id + " IS NULL THEN 1 ELSE 0 END) AS BIGINT), 0) AS null_count, "
		"  COUNT(*) AS total_count, "
		"  COALESCE(CAST(SUM(CASE WHEN " + col_id + " IS NULL THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0), 0.0) AS null_rate "
		"FROM " + table_ref + ")";
}

// Helper: Generate SQL for distinct count metrics
static string GenerateDistinctCountSQL(const string &table_ref, const string &column_name, const Value &min_distinct, const Value &max_distinct) {
	string min_val = BigintThresholdSQL(min_distinct);
	string max_val = BigintThresholdSQL(max_distinct);
	string col_id = QuoteSqlIdentifier(column_name);

	return "SELECT "
		"CASE "
		"  WHEN (" + min_val + " IS NOT NULL AND distinct_count < " + min_val + ") OR "
		"       (" + max_val + " IS NOT NULL AND distinct_count > " + max_val + ") THEN 'fail' "
		"  ELSE 'pass' "
		"END AS status, "
		"distinct_count, "
		+ min_val + " AS min_threshold, "
		+ max_val + " AS max_threshold, "
		"CASE "
		"  WHEN " + min_val + " IS NOT NULL AND distinct_count < " + min_val + " THEN "
		"    'Distinct count ' || CAST(distinct_count AS VARCHAR) || ' is below minimum ' || CAST(" + min_val + " AS VARCHAR) "
		"  WHEN " + max_val + " IS NOT NULL AND distinct_count > " + max_val + " THEN "
		"    'Distinct count ' || CAST(distinct_count AS VARCHAR) || ' exceeds maximum ' || CAST(" + max_val + " AS VARCHAR) "
		"  ELSE 'Distinct count ' || CAST(distinct_count AS VARCHAR) || ' is within acceptable range' "
		"END AS message "
		"FROM (SELECT COUNT(DISTINCT " + col_id + ") AS distinct_count FROM " + table_ref + ")";
}

// The CTEs that read the non-NULL values of `col_id` as DOUBLE: `all_vals(val)` and, when the
// column has an exact numeric type (integers, DECIMAL), a one-row `ref(r)` with the reference
// value the values are centred on (see ExactCenteringType): val is then the exact difference to
// it, so values above 2^53 stay distinct. `offset_sql` is the DOUBLE to add to a centred
// location (mean, quantile, bound) to report it on the column's scale ("" when not centred).
struct CenteredValuesSQL {
	string ctes;
	string offset_sql;
};

static CenteredValuesSQL BuildCenteredValuesSQL(const string &table_ref, const string &col_id,
                                                const LogicalType &column_type) {
	auto centering_type =
	    column_type.id() == LogicalTypeId::INVALID ? string() : ExactCenteringType(column_type.ToString());
	if (centering_type.empty()) {
		return {"all_vals AS (SELECT CAST(" + col_id + " AS DOUBLE) AS val FROM " + table_ref + " WHERE " + col_id +
		            " IS NOT NULL), ",
		        ""};
	}
	return {"ref AS (SELECT " + CenteringReferenceSQL(col_id, centering_type) + " AS r FROM " + table_ref + "), "
	        "all_vals AS (SELECT " + CenteredDoubleSQL(col_id, centering_type, "ref.r") + " AS val FROM " + table_ref +
	            ", ref WHERE " + col_id + " IS NOT NULL), ",
	        "(SELECT CAST(r AS DOUBLE) FROM ref)"};
}

// `centred_sql` shifted back by the offset of BuildCenteredValuesSQL
static string Uncentered(const CenteredValuesSQL &values, const string &centred_sql) {
	return values.offset_sql.empty() ? centred_sql : "(" + values.offset_sql + " + " + centred_sql + ")";
}

// Helper: Generate SQL for zscore metrics using CTE to avoid nested aggregates
static string GenerateZscoreSQL(const string &table_ref, const string &column_name, const LogicalType &column_type,
                                const Value &threshold) {
	string thresh_val = DoubleThresholdSQL(threshold, "3.0");
	string col_id = QuoteSqlIdentifier(column_name);

	// Use CTEs to avoid nested aggregate error:
	// 1. vals: the non-NULL values cast to DOUBLE (exact numeric types centred first, see
	//    BuildCenteredValuesSQL; mean is reported on the column's scale)
	// 2. stats: compute mean and stddev (always exactly one row)
	// 3. with_zscore: compute z-score for each row; a constant column (stddev = 0)
	//    or a single row (stddev NULL) yields z-score 0 instead of NaN/NULL
	// 4. summary: driven by stats so exactly one row is returned even for empty input
	// Empty-input semantics: empty/all-NULL input yields one row with total_count = 0,
	// outlier_count = 0, outlier_rate = 0.0, NULL mean/stddev, status 'pass'.
	// NaN/Inf values are left out of the statistics (they would turn mean/stddev into NaN and
	// every row into an "outlier") and counted for the message.
	const string nonfinite_note =
	    "CASE WHEN nonfinite_count > 0 THEN '; ' || CAST(nonfinite_count AS VARCHAR) || ' NaN/Inf value(s) ignored' "
	    "ELSE '' END";
	auto values = BuildCenteredValuesSQL(table_ref, col_id, column_type);
	return "WITH " + values.ctes +
		"vals AS (SELECT val FROM all_vals WHERE isfinite(val)), "
		"stats AS ("
		"SELECT AVG(val) AS centred_mean, " + Uncentered(values, "AVG(val)") + " AS mean, STDDEV(val) AS stddev, COUNT(*) AS total_count, "
		"(SELECT COUNT(*) FROM all_vals WHERE NOT isfinite(val)) AS nonfinite_count FROM vals"
		"), "
		"with_zscore AS ("
		"SELECT CASE WHEN s.stddev IS NULL OR s.stddev = 0 THEN 0.0 "
		"            ELSE ABS((v.val - s.centred_mean) / s.stddev) END AS abs_zscore "
		"FROM vals v, stats s"
		"), "
		"summary AS ("
		"SELECT "
		"  s.mean, s.stddev, s.total_count, s.nonfinite_count, "
		"  (SELECT COUNT(*) FROM with_zscore z WHERE z.abs_zscore > " + thresh_val + ") AS outlier_count "
		"FROM stats s"
		") "
		"SELECT "
		"CASE WHEN outlier_count = 0 THEN 'pass' ELSE 'fail' END AS status, "
		"mean, "
		"stddev, "
		"outlier_count, "
		"total_count, "
		"COALESCE(CAST(outlier_count AS DOUBLE) / NULLIF(total_count, 0), 0.0) AS outlier_rate, "
		+ thresh_val + " AS threshold, "
		"CASE "
		"  WHEN total_count = 0 THEN "
		"    'No rows to evaluate (column is empty or all NULL)' "
		"  WHEN stddev IS NULL OR stddev = 0 THEN "
		"    'No outliers detected (constant column: standard deviation is 0 or undefined)' "
		"  WHEN outlier_count = 0 THEN "
		"    'No outliers detected (z-score threshold: ' || CAST(" + thresh_val + " AS VARCHAR) || " + nonfinite_note + " || ')' "
		"  ELSE "
		"    CAST(outlier_count AS VARCHAR) || ' outlier(s) detected (' || "
		"    " + RoundedNumberSQL("CAST(outlier_count AS DOUBLE) / total_count * 100") + " || "
		"    '% of ' || CAST(total_count AS VARCHAR) || ' rows' || " + nonfinite_note + " || ')' "
		"END AS message "
		"FROM summary";
}

// Helper: Generate SQL for IQR metrics using CTE to avoid nested aggregates
static string GenerateIQRSQL(const string &table_ref, const string &column_name, const LogicalType &column_type,
                             const Value &iqr_multiplier) {
	string mult_val = DoubleThresholdSQL(iqr_multiplier, "1.5");
	string col_id = QuoteSqlIdentifier(column_name);

	// Use CTEs to avoid nested aggregate error:
	// 1. vals: the finite non-NULL values cast to DOUBLE (exact numeric types centred first, see
	//    BuildCenteredValuesSQL; quantiles and bounds are reported on the column's scale)
	// 2. stats: compute Q1 and Q3 quantiles (always exactly one row)
	// 3. bounds: derive the outlier bounds from the quantiles
	// 4. summary: driven by bounds so exactly one row is returned even for empty input
	// Empty-input semantics: empty/all-NULL input yields one row with total_count = 0,
	// outlier_count = 0, NULL quantiles/bounds, status 'pass' and an explicit message.
	// NaN/Inf values are left out like in zscore (a NaN would become Q3 and every value an
	// "outlier") and counted for the message.
	const string nonfinite_note =
	    "CASE WHEN nonfinite_count > 0 THEN '; ' || CAST(nonfinite_count AS VARCHAR) || ' NaN/Inf value(s) ignored' "
	    "ELSE '' END";
	auto values = BuildCenteredValuesSQL(table_ref, col_id, column_type);
	return "WITH " + values.ctes +
		"vals AS (SELECT val FROM all_vals WHERE isfinite(val)), "
		"stats AS ("
		"SELECT QUANTILE_CONT(val, 0.25) AS q1, QUANTILE_CONT(val, 0.75) AS q3, COUNT(*) AS total_count, "
		"(SELECT COUNT(*) FROM all_vals WHERE NOT isfinite(val)) AS nonfinite_count FROM vals"
		"), "
		"bounds AS ("
		"SELECT "
		"  q1, "
		"  q3, "
		"  total_count, "
		"  nonfinite_count, "
		"  q1 - " + mult_val + " * (q3 - q1) AS lower_bound, "
		"  q3 + " + mult_val + " * (q3 - q1) AS upper_bound "
		"FROM stats"
		"), "
		"summary AS ("
		"SELECT "
		"  " + Uncentered(values, "b.q1") + " AS q1, "
		"  " + Uncentered(values, "b.q3") + " AS q3, "
		"  b.q3 - b.q1 AS iqr, "
		"  b.total_count, "
		"  b.nonfinite_count, "
		"  " + Uncentered(values, "b.lower_bound") + " AS lower_bound, "
		"  " + Uncentered(values, "b.upper_bound") + " AS upper_bound, "
		"  (SELECT COUNT(*) FROM vals v, bounds b2 WHERE v.val < b2.lower_bound OR v.val > b2.upper_bound) AS outlier_count "
		"FROM bounds b"
		") "
		"SELECT "
		"CASE WHEN outlier_count = 0 THEN 'pass' ELSE 'fail' END AS status, "
		"q1, "
		"q3, "
		"iqr, "
		"lower_bound, "
		"upper_bound, "
		"outlier_count, "
		"total_count, "
		+ mult_val + " AS multiplier, "
		"CASE "
		"  WHEN total_count = 0 THEN "
		"    'No rows to evaluate (column is empty or all NULL)' "
		"  WHEN outlier_count = 0 THEN "
		"    'No outliers detected by IQR method (bounds: [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || ']' || " + nonfinite_note + " || ')' "
		"  ELSE "
		"    CAST(outlier_count AS VARCHAR) || ' outlier(s) detected outside bounds [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || ']' || " + nonfinite_note + " "
		"END AS message "
		"FROM summary";
}

static string GenerateSchemaSQL(const string &table_name, const vector<string> &required_columns) {
	string required_array = "[]::VARCHAR[]";
	if (!required_columns.empty()) {
		required_array = "[";
		for (size_t i = 0; i < required_columns.size(); i++) {
			if (i > 0) {
				required_array += ", ";
			}
			required_array += "'" + EscapeSqlStringLiteral(required_columns[i]) + "'";
		}
		required_array += "]::VARCHAR[]";
	}

	return "WITH required(col) AS ("
	       "  SELECT unnest(" + required_array + ")"
	       "), actual(col) AS ("
	       "  SELECT column_name"
	       "  FROM (DESCRIBE SELECT * FROM " + BuildQueryTableRef(table_name) + ")"
	       "), missing AS ("
	       // Column names match case-insensitively, like DuckDB identifiers
	       "  SELECT DISTINCT col FROM required r"
	       "  WHERE NOT EXISTS (SELECT 1 FROM actual a WHERE lower(a.col) = lower(r.col))"
	       "), missing_stats AS ("
	       "  SELECT"
	       "    COALESCE(COUNT(*), 0) AS missing_count,"
	       "    COALESCE(array_agg(col), []::VARCHAR[]) AS missing_columns"
	       "  FROM missing"
	       ")"
	       "SELECT"
	       "  CASE WHEN missing_count = 0 THEN 'pass' ELSE 'fail' END AS status,"
	       "  missing_columns,"
	       "  'Table " + EscapeSqlStringLiteral(table_name) + "' || CASE"
	       "    WHEN missing_count = 0 THEN ' has all required columns'"
	       "    ELSE ' is missing ' || CAST(missing_count AS VARCHAR) || ' column(s)'"
	       "  END AS message"
	       " FROM missing_stats";
}

// Helper: Generate SQL for freshness metrics
// All timestamps are compared in UTC: the reference time defaults to now() in UTC (UtcNowSQL),
// and a TIMESTAMP WITH TIME ZONE column is read as its UTC instant (UtcTimestampSQL), with and
// without the ICU extension and whatever the session TimeZone. A plain TIMESTAMP column and an
// explicit reference_time are taken as UTC as they are.
static string GenerateFreshnessSQL(const string &table_ref, const string &timestamp_column,
                                   const LogicalType &column_type, const Value &max_age,
                                   const Value &reference_time) {
	// max_age is never NULL here (rejected at bind time)
	string max_age_interval = "'" + EscapeSqlStringLiteral(max_age.ToString()) + "'::INTERVAL";
	string ref_time =
	    reference_time.IsNull() ? UtcNowSQL() : "'" + EscapeSqlStringLiteral(reference_time.ToString()) + "'::TIMESTAMP";
	string col_id = QuoteSqlIdentifier(timestamp_column);
	string age_sql = "EXTRACT(EPOCH FROM (" + ref_time + " - latest_timestamp))::BIGINT";
	// A latest timestamp after the reference time passes (the data is not stale) but is
	// called out: it usually means clock skew or timestamps loaded in the wrong time zone
	string future_note = "CASE WHEN " + age_sql + " < 0 THEN '. Warning: the latest timestamp is ' || "
	                     "CAST(-" + age_sql + " AS VARCHAR) || 's after the reference time; check for clock skew or "
	                     "a time zone mismatch' ELSE '' END";

	// Empty-input semantics: an empty table or all-NULL timestamp column yields one row with
	// status 'fail', NULL metric_value/age_seconds and an explicit message (freshness cannot
	// be demonstrated without data).
	return "SELECT "
		"CASE "
		"  WHEN latest_timestamp IS NULL THEN 'fail' "
		"  WHEN latest_timestamp >= threshold_timestamp THEN 'pass' "
		"  ELSE 'fail' "
		"END AS status, "
		"latest_timestamp AS metric_value, "
		"threshold_timestamp AS threshold, "
		+ age_sql + " AS age_seconds, "
		"CASE "
		"  WHEN latest_timestamp IS NULL THEN "
		"    'No timestamp values found (table is empty or column is all NULL)' "
		"  WHEN latest_timestamp >= threshold_timestamp THEN "
		"    'Data is fresh. Latest update: ' || CAST(latest_timestamp AS VARCHAR) || ' (age: ' || CAST(" + age_sql + " AS VARCHAR) || 's)' || " + future_note + " "
		"  ELSE "
		"    'Data is stale. Latest update: ' || CAST(latest_timestamp AS VARCHAR) || ' (age: ' || CAST(" + age_sql + " AS VARCHAR) || 's, max allowed: ' || CAST(EXTRACT(EPOCH FROM " + max_age_interval + ")::BIGINT AS VARCHAR) || 's)' "
		"END AS message "
		"FROM (SELECT "
		"  MAX(" + UtcTimestampSQL(col_id, column_type) + ") AS latest_timestamp, "
		+ ref_time + " - " + max_age_interval + " AS threshold_timestamp "
		"FROM " + table_ref + ")";
}

} // anonymous namespace

//===--------------------------------------------------------------------===//
// Metric Bind Replace Functions
//===--------------------------------------------------------------------===//

static unique_ptr<TableRef> MetricVolumeBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_volume");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_volume requires 3 arguments: table_name, min_rows, max_rows");
	}

	string table_name = RequireNameArgument(input, 0, "volume", "table_name");
	RequireRelationColumns(context, "volume", table_name);
	ValidateNonNegative(input.inputs[1], "volume", "min_rows");
	ValidateNonNegative(input.inputs[2], "volume", "max_rows");
	ValidateThresholdOrder(input.inputs[1], input.inputs[2], "volume", "min_rows", "max_rows");
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateVolumeSQL(table_ref, input.inputs[1], input.inputs[2]);
	return ParseSubquery(sql, context, "Failed to parse volume metric query");
}

static unique_ptr<TableRef> MetricNullRateBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_null_rate");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_null_rate requires 3 arguments: table_name, column_name, max_null_rate");
	}

	string table_name = RequireNameArgument(input, 0, "null_rate", "table_name");
	string column_name = RequireNameArgument(input, 1, "null_rate", "column_name");
	RequireNonNullArgument(input, 2, "null_rate", "max_null_rate");
	ValidateRate(input.inputs[2], "null_rate", "max_null_rate");
	RequireRelationColumns(context, "null_rate", table_name, {column_name});
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateNullRateSQL(table_ref, column_name, input.inputs[2]);
	return ParseSubquery(sql, context, "Failed to parse null rate metric query");
}

static unique_ptr<TableRef> MetricDistinctCountBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_distinct_count");
	if (input.inputs.size() < 4) {
		throw BinderException("anofox_metric_distinct_count requires 4 arguments: table_name, column_name, min_distinct, max_distinct");
	}

	string table_name = RequireNameArgument(input, 0, "distinct_count", "table_name");
	string column_name = RequireNameArgument(input, 1, "distinct_count", "column_name");
	ValidateNonNegative(input.inputs[2], "distinct_count", "min_distinct");
	ValidateNonNegative(input.inputs[3], "distinct_count", "max_distinct");
	ValidateThresholdOrder(input.inputs[2], input.inputs[3], "distinct_count", "min_distinct", "max_distinct");
	RequireRelationColumns(context, "distinct_count", table_name, {column_name});
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateDistinctCountSQL(table_ref, column_name, input.inputs[2], input.inputs[3]);
	return ParseSubquery(sql, context, "Failed to parse distinct count metric query");
}

static unique_ptr<TableRef> MetricZscoreBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_zscore");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_zscore requires 3 arguments: table_name, column_name, threshold");
	}

	string table_name = RequireNameArgument(input, 0, "zscore", "table_name");
	string column_name = RequireNameArgument(input, 1, "zscore", "column_name");
	RequireNonNullArgument(input, 2, "zscore", "threshold");
	ValidateFiniteDouble(input.inputs[2], "zscore", "threshold");
	if (!input.inputs[2].IsNull() && input.inputs[2].GetValue<double>() <= 0) {
		throw BinderException("zscore: threshold must be positive (got %s)", input.inputs[2].ToString());
	}
	RequireRelationColumns(context, "zscore", table_name, {column_name});
	RequireNumericColumn(context, "zscore", table_name, column_name);
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateZscoreSQL(table_ref, column_name, LookupColumnType(context, table_name, column_name),
	                               input.inputs[2]);
	return ParseSubquery(sql, context, "Failed to parse zscore metric query");
}

static unique_ptr<TableRef> MetricIQRBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_iqr");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_iqr requires 3 arguments: table_name, column_name, iqr_multiplier");
	}

	string table_name = RequireNameArgument(input, 0, "iqr", "table_name");
	string column_name = RequireNameArgument(input, 1, "iqr", "column_name");
	RequireNonNullArgument(input, 2, "iqr", "iqr_multiplier");
	ValidateFiniteDouble(input.inputs[2], "iqr", "iqr_multiplier");
	ValidateNonNegative(input.inputs[2], "iqr", "iqr_multiplier");
	RequireRelationColumns(context, "iqr", table_name, {column_name});
	RequireNumericColumn(context, "iqr", table_name, column_name);
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateIQRSQL(table_ref, column_name, LookupColumnType(context, table_name, column_name),
	                            input.inputs[2]);
	return ParseSubquery(sql, context, "Failed to parse IQR metric query");
}

static unique_ptr<TableRef> MetricSchemaBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_schema");
	if (input.inputs.size() < 2) {
		throw BinderException("anofox_metric_schema requires 2 arguments: table_name, required_columns");
	}

	string table_name = RequireNameArgument(input, 0, "schema_check", "table_name");
	RequireNonNullArgument(input, 1, "schema_check", "required_columns");
	// The required columns are what the check asserts: a missing one is a 'fail' row, not an error
	RequireRelationColumns(context, "schema_check", table_name);
	vector<string> required_columns;
	if (input.inputs[1].type().id() == LogicalTypeId::LIST) {
		auto &list_children = ListValue::GetChildren(input.inputs[1]);
		for (auto &child : list_children) {
			if (!child.IsNull()) {
				required_columns.push_back(child.ToString());
			}
		}
	}
	AnofoxTrace(AnofoxLogLevel::Debug,
	            "metric_schema: table='" + table_name + "', required=" +
	                std::to_string(required_columns.size()));
	string sql = GenerateSchemaSQL(table_name, required_columns);
	return ParseSubquery(sql, context, "Failed to parse schema metric query");
}

static unique_ptr<TableRef> MetricFreshnessBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_freshness");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_freshness requires at least 3 arguments: table_name, timestamp_column, max_age");
	}

	string table_name = RequireNameArgument(input, 0, "freshness", "table_name");
	string timestamp_column = RequireNameArgument(input, 1, "freshness", "timestamp_column");
	RequireNonNullArgument(input, 2, "freshness", "max_age");
	Value max_age = input.inputs[2];
	// NULL reference_time = the current UTC time
	Value reference_time = (input.inputs.size() > 3) ? input.inputs[3] : Value();

	string table_ref = BuildQueryTableRef(table_name);
	auto column_type = RequireRelationColumns(context, "freshness", table_name, {timestamp_column})[0].type;
	string sql = GenerateFreshnessSQL(table_ref, timestamp_column, column_type, max_age, reference_time);
	return ParseSubquery(sql, context, "Failed to parse freshness metric query");
}

//===--------------------------------------------------------------------===//
// Isolation Forest Table Function - Actual C++ Execution
//===--------------------------------------------------------------------===//

// Bind data for isolation forest table function (immutable after bind)
struct IsolationForestBindData : public TableFunctionData {
	string table_name;
	vector<string> column_names;
	size_t n_trees;
	size_t sample_size;
	double contamination;
	string output_mode;  // "summary" or "scores"
	uint64_t seed;
	bool has_seed;
	bool is_multivariate;
	size_t ndim;                              // Extended IF: hyperplane dimensions (1=axis-aligned)
	anofox::CoefType coef_type;              // Extended IF: coefficient distribution
	anofox::ScoringMetric scoring_metric;    // Scoring method: depth, density, adj_depth
	string weight_column;                     // Optional weight column name (empty = uniform)
	bool has_weight_column;
	size_t ntry;                              // SCiForest: number of split candidates to evaluate (1=random IF)
	double prob_pick_avg_gain;                // SCiForest: probability of selecting best-gain split (0=random, 1=always best)
	IdColumn id_column;                       // Optional pass-through identifier ('scores' mode)

	IsolationForestBindData(string tbl, vector<string> cols, size_t trees, size_t sample,
	                        double contam, string mode, uint64_t s, bool has_s, bool force_multivariate = false,
	                        size_t dim = 1, anofox::CoefType coef = anofox::CoefType::Uniform,
	                        anofox::ScoringMetric metric = anofox::ScoringMetric::Depth,
	                        string weight_col = "", bool has_weight = false,
	                        size_t ntry_val = 1, double prob_gain = 0.0)
	    : table_name(std::move(tbl)), column_names(std::move(cols)),
	      n_trees(trees), sample_size(sample), contamination(contam),
	      output_mode(std::move(mode)), seed(s), has_seed(has_s),
	      is_multivariate(force_multivariate || column_names.size() > 1),
	      ndim(dim), coef_type(coef), scoring_metric(metric),
	      weight_column(std::move(weight_col)), has_weight_column(has_weight),
	      ntry(ntry_val), prob_pick_avg_gain(prob_gain) {}
};

// Global state for isolation forest execution - stores mutable execution state
struct IsolationForestGlobalState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t current_row = 0;

	// Model outputs computed during execution (stored here because bind_data
	// is const). The scores from the fitted model are kept once and emitted
	// incrementally; row ids and anomaly flags are derived during emission
	// instead of buffering a duplicate per-row result vector.
	std::vector<double> scores;
	std::vector<double> first_column_values;  // univariate numeric path only ("value" output column)
	std::vector<Value> id_values;              // id_column value per scored row (empty without id_column)
	std::vector<int64_t> source_row_ids;       // 1-based scan position of each scored row ("row_id")
	std::vector<bool> anomaly_flags;           // is_anomaly per scored row
	double threshold = 0.0;
	int64_t total_count = 0;
	int64_t outlier_count = 0;
	double avg_value = 0.0;
	bool avg_is_null = false;  // no scored row has a finite value
};

// Classifies the numeric feature values of one row for the isolation forest:
// a NaN in any feature skips the row like NULL; +-Inf in any feature makes the
// row an anomaly that is scored 1.0 and kept out of the trees.
enum class IsolationRowKind : uint8_t { FINITE, NON_FINITE_ANOMALY, SKIPPED_NAN };

// Merges the model scores of the finite rows with the rows forced to score 1.0
// (+-Inf), in scan order, and derives the anomaly flags and counts.
static void FinishIsolationForestScores(IsolationForestGlobalState &state, const IsolationForest *forest,
                                        const std::vector<double> &finite_scores,
                                        const std::vector<bool> &non_finite_rows, double finite_sum,
                                        bool univariate) {
	const size_t n_rows = non_finite_rows.size();
	state.total_count = static_cast<int64_t>(n_rows);
	const size_t n_finite = finite_scores.size();
	if (univariate) {
		if (n_finite > 0) {
			state.avg_value = finite_sum / static_cast<double>(n_finite);
		} else {
			// No finite value scored (empty table, all NULL/NaN, or only +-Inf): NULL, as documented
			state.avg_value = 0.0;
			state.avg_is_null = true;
		}
	}
	state.threshold = forest ? forest->ComputeThreshold(finite_scores) : std::numeric_limits<double>::infinity();

	state.scores.clear();
	state.scores.reserve(n_rows);
	state.anomaly_flags.clear();
	state.anomaly_flags.reserve(n_rows);
	size_t next_finite = 0;
	for (size_t r = 0; r < n_rows; ++r) {
		if (non_finite_rows[r]) {
			state.scores.push_back(1.0);
			state.anomaly_flags.push_back(true);
		} else {
			double score = finite_scores[next_finite++];
			state.scores.push_back(score);
			state.anomaly_flags.push_back(IsolationForest::IsAnomaly(score, state.threshold));
		}
	}
	state.outlier_count =
	    static_cast<int64_t>(std::count(state.anomaly_flags.begin(), state.anomaly_flags.end(), true));
	if (anofox::AnofoxTraceConfig::Get().ShouldLog(anofox::AnofoxLogLevel::Debug)) {
		anofox::AnofoxTrace(anofox::AnofoxLogLevel::Debug,
		                    "isolation_forest: " + std::to_string(n_rows) + " scored rows (" + std::to_string(n_finite) +
		                        " finite, " + std::to_string(n_rows - n_finite) + " +-Inf), threshold " +
		                        std::to_string(state.threshold) + ", " + std::to_string(state.outlier_count) +
		                        " anomalies");
	}
}

// Initialize global state
static unique_ptr<GlobalTableFunctionState> IsolationForestInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<IsolationForestGlobalState>();
}

// Resolve the feature columns of an anomaly function at bind time: case-insensitive names
// (the table's spelling is used from here on), unknown names are an error, and every column
// must be numeric (BOOLEAN is cast to 0/1). With `allow_categorical` VARCHAR/ENUM columns are
// accepted as categorical features (isolation_forest_mv). Without this check a VARCHAR column
// was silently scored as a category (isolation_forest) or failed with a cast error (dbscan).
static vector<string> ResolveFeatureColumns(ClientContext &context, const string &function_name,
                                            const string &table_name, const vector<string> &requested,
                                            bool allow_categorical) {
	vector<string> names;
	for (auto &column : ResolveColumns(context, function_name, table_name, requested)) {
		auto type_id = column.type.id();
		bool numeric = column.type.IsNumeric() || type_id == LogicalTypeId::BOOLEAN;
		bool categorical = type_id == LogicalTypeId::VARCHAR || type_id == LogicalTypeId::ENUM;
		if (!numeric && !(allow_categorical && categorical)) {
			if (allow_categorical) {
				throw BinderException("%s: column '%s' must be numeric or categorical (VARCHAR), got %s",
				                      function_name, column.name, column.type.ToString());
			}
			throw BinderException("%s: column '%s' must be numeric, got %s", function_name, column.name,
			                      column.type.ToString());
		}
		names.push_back(std::move(column.name));
	}
	return names;
}

// Shared optional-parameter handling and validation for both isolation forest bind functions.
// Integer parameters are read as signed int64_t and range-checked BEFORE casting to size_t,
// so negative values are rejected instead of wrapping; doubles must be finite.
// An optional positional parameter that is absent or NULL takes its default,
// like the seed does; GetValue on a NULL Value would raise an INTERNAL error.
template <class T>
static T OptionalInput(const TableFunctionBindInput &input, idx_t index, T default_value) {
	if (input.inputs.size() <= index || input.inputs[index].IsNull()) {
		return default_value;
	}
	return input.inputs[index].GetValue<T>();
}

static void ParseIsolationForestCommonParameters(TableFunctionBindInput &input, const string &function_name,
                                                 size_t &n_trees, size_t &sample_size, double &contamination,
                                                 string &output_mode, uint64_t &seed, bool &has_seed) {
	int64_t n_trees_val = OptionalInput<int64_t>(input, 2, 100);
	int64_t sample_size_val = OptionalInput<int64_t>(input, 3, 256);
	contamination = OptionalInput<double>(input, 4, 0.1);
	output_mode = LowerOption(OptionalInput<string>(input, 5, "summary"));

	// Seed parameter (optional, index 6); any signed value is an acceptable seed
	seed = 0;
	has_seed = false;
	if (input.inputs.size() > 6 && !input.inputs[6].IsNull()) {
		seed = static_cast<uint64_t>(input.inputs[6].GetValue<int64_t>());
		has_seed = true;
	}

	if (n_trees_val < 1 || n_trees_val > 500) {
		throw BinderException(function_name + ": n_trees must be between 1 and 500");
	}
	if (sample_size_val < 1 || sample_size_val > 10000) {
		throw BinderException(function_name + ": sample_size must be between 1 and 10000");
	}
	if (!std::isfinite(contamination) || contamination < 0.0 || contamination > 0.5) {
		throw BinderException(function_name + ": contamination must be between 0.0 and 0.5 (finite)");
	}
	if (output_mode != "summary" && output_mode != "scores") {
		throw BinderException(function_name + ": output_mode must be 'summary' or 'scores'");
	}

	n_trees = static_cast<size_t>(n_trees_val);
	sample_size = static_cast<size_t>(sample_size_val);
}

// Bind function for univariate isolation forest
static unique_ptr<FunctionData> IsolationForestBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("isolation_forest");
	if (input.inputs.size() < 2) {
		throw BinderException("isolation_forest requires at least 2 arguments: table_name, column_name");
	}

	string table_name = RequireNonNullArgument(input, 0, "isolation_forest", "table_name");
	string column_name = RequireNonNullArgument(input, 1, "isolation_forest", "column_name");
	vector<string> column_names = {column_name};

	size_t n_trees;
	size_t sample_size;
	double contamination;
	string output_mode;
	uint64_t seed;
	bool has_seed;
	ParseIsolationForestCommonParameters(input, "isolation_forest", n_trees, sample_size, contamination, output_mode,
	                                     seed, has_seed);

	// Define output schema based on mode
	if (output_mode == "scores") {
		names.emplace_back("row_id");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("value");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("anomaly_score");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("is_anomaly");
		return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	} else {
		// Summary mode
		names.emplace_back("status");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("outlier_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("total_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("avg_value");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("contamination");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("n_trees");
		return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		names.emplace_back("message");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}

	column_names = ResolveFeatureColumns(context, "isolation_forest", table_name, column_names, false);
	auto id_column = BindIdColumn(context, input, "isolation_forest", table_name, output_mode == "scores", "scores");
	InsertIdColumn(id_column, "isolation_forest", return_types, names);

	auto bind_data = make_uniq<IsolationForestBindData>(table_name, column_names, n_trees, sample_size,
	                                                    contamination, output_mode, seed, has_seed);
	bind_data->id_column = std::move(id_column);
	return std::move(bind_data);
}

// Bind function for multivariate isolation forest
static unique_ptr<FunctionData> IsolationForestMultivariateBind(ClientContext &context, TableFunctionBindInput &input,
                                                                 vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("isolation_forest_mv");
	if (input.inputs.size() < 2) {
		throw BinderException("isolation_forest_mv requires at least 2 arguments: table_name, column_names");
	}

	string table_name = RequireNonNullArgument(input, 0, "isolation_forest_mv", "table_name");

	// Parse comma-separated column names
	auto column_names = ParseCommaSeparatedColumns(RequireNonNullArgument(input, 1, "isolation_forest_mv", "column_names"));
	if (column_names.empty()) {
		throw BinderException("column_names must contain at least one column");
	}

	size_t n_trees;
	size_t sample_size;
	double contamination;
	string output_mode;
	uint64_t seed;
	bool has_seed;
	ParseIsolationForestCommonParameters(input, "isolation_forest_mv", n_trees, sample_size, contamination,
	                                     output_mode, seed, has_seed);

	// Extended IF: ndim parameter (optional, index 7)
	size_t ndim = 1;  // Default: axis-aligned
	if (input.inputs.size() > 7 && !input.inputs[7].IsNull()) {
		int64_t ndim_val = input.inputs[7].GetValue<int64_t>();
		if (ndim_val < 1) {
			throw BinderException("ndim must be >= 1");
		}
		ndim = static_cast<size_t>(ndim_val);
	}

	// Extended IF: coef_type parameter (optional, index 8)
	anofox::CoefType coef_type = anofox::CoefType::Uniform;  // Default: uniform
	if (input.inputs.size() > 8 && !input.inputs[8].IsNull()) {
		string coef_type_str = input.inputs[8].ToString();
		// Convert to lowercase for comparison
		std::transform(coef_type_str.begin(), coef_type_str.end(), coef_type_str.begin(), ::tolower);
		if (coef_type_str == "normal") {
			coef_type = anofox::CoefType::Normal;
		} else if (coef_type_str != "uniform") {
			throw BinderException("coef_type must be 'uniform' or 'normal'");
		}
	}

	// scoring_metric parameter (optional, index 9)
	anofox::ScoringMetric scoring_metric = anofox::ScoringMetric::Depth;  // Default: depth
	if (input.inputs.size() > 9 && !input.inputs[9].IsNull()) {
		string metric_str = input.inputs[9].ToString();
		std::transform(metric_str.begin(), metric_str.end(), metric_str.begin(), ::tolower);
		if (metric_str == "density") {
			scoring_metric = anofox::ScoringMetric::Density;
		} else if (metric_str == "adj_depth") {
			scoring_metric = anofox::ScoringMetric::AdjDepth;
		} else if (metric_str != "depth") {
			throw BinderException("scoring_metric must be 'depth', 'density', or 'adj_depth'");
		}
	}

	// weight_column parameter (optional, index 10)
	string weight_column;
	bool has_weight_column = false;
	if (input.inputs.size() > 10 && !input.inputs[10].IsNull()) {
		weight_column = input.inputs[10].ToString();
		has_weight_column = !weight_column.empty();
	}

	// SCiForest: ntry parameter (optional, index 11)
	size_t ntry = 1;  // Default: single random split (standard IF)
	if (input.inputs.size() > 11 && !input.inputs[11].IsNull()) {
		int64_t ntry_val = input.inputs[11].GetValue<int64_t>();
		if (ntry_val < 1) {
			throw BinderException("isolation_forest_mv: ntry must be >= 1");
		}
		ntry = static_cast<size_t>(ntry_val);
	}

	// SCiForest: prob_pick_avg_gain parameter (optional, index 12)
	double prob_pick_avg_gain = 0.0;  // Default: always random (standard IF)
	if (input.inputs.size() > 12 && !input.inputs[12].IsNull()) {
		prob_pick_avg_gain = input.inputs[12].GetValue<double>();
		if (!std::isfinite(prob_pick_avg_gain) || prob_pick_avg_gain < 0.0 || prob_pick_avg_gain > 1.0) {
			throw BinderException("isolation_forest_mv: prob_pick_avg_gain must be between 0.0 and 1.0 (finite)");
		}
	}

	// Define output schema based on mode
	if (output_mode == "scores") {
		names.emplace_back("row_id");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("anomaly_score");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("is_anomaly");
		return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	} else {
		// Summary mode
		names.emplace_back("status");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("outlier_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("total_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("n_columns");
		return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		names.emplace_back("contamination");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("n_trees");
		return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		names.emplace_back("message");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}

	column_names = ResolveFeatureColumns(context, "isolation_forest_mv", table_name, column_names, true);
	if (has_weight_column) {
		weight_column = ResolveFeatureColumns(context, "isolation_forest_mv", table_name, {weight_column}, false)[0];
	}
	auto id_column =
	    BindIdColumn(context, input, "isolation_forest_mv", table_name, output_mode == "scores", "scores");
	InsertIdColumn(id_column, "isolation_forest_mv", return_types, names);

	// Force multivariate mode for this function (3-column output schema)
	auto bind_data = make_uniq<IsolationForestBindData>(table_name, column_names, n_trees, sample_size,
	                                                    contamination, output_mode, seed, has_seed, true, ndim,
	                                                    coef_type, scoring_metric, weight_column, has_weight_column,
	                                                    ntry, prob_pick_avg_gain);
	bind_data->id_column = std::move(id_column);
	return std::move(bind_data);
}

// Execute isolation forest algorithm
static void IsolationForestExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<IsolationForestBindData>();
	auto &state = data_p.global_state->Cast<IsolationForestGlobalState>();

	// Execute algorithm only once
	if (!state.executed) {
		state.executed = true;

		Connection con(*context.db);

		// Step 1: Detect column types from the source table
		std::vector<bool> is_categorical(bind_data.column_names.size(), false);
		bool has_categorical = false;

		{
			// Build query to get column types (select one row with raw types)
			string type_query = "SELECT ";
			for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
				if (i > 0) type_query += ", ";
				type_query += QuoteSqlIdentifier(bind_data.column_names[i]);
			}
			type_query += " FROM " + BuildQueryTableRef(bind_data.table_name) + " LIMIT 0";

			auto type_result = con.Query(type_query);
			if (type_result->HasError()) {
				throw InvalidInputException("Failed to query source table: %s", type_result->GetError());
			}

			// Check column types from result schema
			for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
				auto col_type = duckdb_compat::ResultTypes(*type_result)[i].id();
				// VARCHAR, ENUM, and similar string types are categorical
				if (col_type == LogicalTypeId::VARCHAR ||
				    col_type == LogicalTypeId::ENUM) {
					is_categorical[i] = true;
					has_categorical = true;
				}
			}
		}

		// Step 2: Build data query - keep VARCHAR as-is, cast numeric to DOUBLE
		// Include weight column in the same query to guarantee row alignment
		string column_list;
		for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
			if (i > 0) column_list += ", ";
			if (is_categorical[i]) {
				column_list += "CAST(" + QuoteSqlIdentifier(bind_data.column_names[i]) + " AS VARCHAR)";
			} else {
				column_list += "CAST(" + QuoteSqlIdentifier(bind_data.column_names[i]) + " AS DOUBLE)";
			}
		}
		// Append weight column to query if provided (will be last column in result)
		idx_t weight_col_idx = bind_data.column_names.size();  // Index of weight column in result
		if (bind_data.has_weight_column) {
			column_list += ", CAST(" + QuoteSqlIdentifier(bind_data.weight_column) + " AS DOUBLE)";
		}
		// Feature (+ weight) columns: the ones that must be non-NULL for a row
		// to be scored. The id column (if any) is read last in the same query,
		// so each id stays with its own row; it is never a filter (NULL ids
		// are passed through).
		const idx_t n_checked_cols = bind_data.column_names.size() + (bind_data.has_weight_column ? 1 : 0);
		const idx_t id_col_idx = n_checked_cols;
		column_list += bind_data.id_column.SelectSuffix();

		// No WHERE clause: every row of the scan is read and counted, so row_id
		// is the 1-based position in the scan order (the table row number),
		// also after rows that are skipped below because a value is NULL.
		string query = "SELECT " + column_list + " FROM " + BuildQueryTableRef(bind_data.table_name);
		int64_t source_row = 0;

		// Stream the input instead of materializing the full result; model
		// fitting requires the complete dataset, so the feature columns built
		// below are the only full copy that is kept.
		auto result = StreamQuery(con, query);
		if (result->HasError()) {
			throw InvalidInputException("Failed to query source table: %s", result->GetError());
		}

		// Determine seed
		uint64_t actual_seed = bind_data.has_seed ? bind_data.seed :
			static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());

		// Use mixed-type path if:
		// - has categorical columns, OR
		// - has weight column, OR
		// - uses Extended IF (ndim > 1), OR
		// - uses non-default scoring metric, OR
		// - uses SCiForest (ntry > 1 or prob_pick_avg_gain > 0)
		// Note: FitMixed/ScoreBatchMixed are required for all advanced parameters
		bool use_mixed_path = has_categorical ||
		                      bind_data.has_weight_column ||
		                      bind_data.ndim > 1 ||
		                      bind_data.scoring_metric != anofox::ScoringMetric::Depth ||
		                      bind_data.ntry > 1 ||
		                      bind_data.prob_pick_avg_gain > 0.0;
		if (use_mixed_path) {
			// Mixed-type path: use ColumnData and FitMixed
			std::vector<ColumnInfo> column_info(bind_data.column_names.size());
			std::vector<ColumnData> data(bind_data.column_names.size());

			// Initialize column info and data
			for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
				if (is_categorical[i]) {
					column_info[i].type = FeatureType::CATEGORICAL;
					data[i].type = FeatureType::CATEGORICAL;
				} else {
					column_info[i].type = FeatureType::NUMERIC;
					data[i].type = FeatureType::NUMERIC;
				}
				column_info[i].name = bind_data.column_names[i];
			}

			double sum_first_col = 0.0;
			std::vector<double> sample_weights;  // Collected during data fetch if weight column provided
			std::vector<bool> non_finite_rows;   // per scored row: +-Inf in a numeric feature

			// Read data and build category mappings, reading chunks through
			// UnifiedVectorFormat typed accessors. The weight column (if
			// present) is the last column in the result.
			std::vector<UnifiedVectorFormat> formats;
			while (true) {
				auto chunk = result->Fetch();
				if (!chunk || chunk->size() == 0) break;

				const idx_t chunk_cols = chunk->ColumnCount();
				formats.resize(chunk_cols);
				for (idx_t col = 0; col < chunk_cols; ++col) {
					chunk->data[col].ToUnifiedFormat(chunk->size(), formats[col]);
				}

				for (idx_t row = 0; row < chunk->size(); ++row) {
					source_row++;
					bool valid_row = true;
					for (idx_t col = 0; col < n_checked_cols; ++col) {
						if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
							valid_row = false;
							break;
						}
					}
					if (!valid_row) continue;

					auto kind = IsolationRowKind::FINITE;
					for (idx_t col = 0; col < bind_data.column_names.size() && kind != IsolationRowKind::SKIPPED_NAN;
					     ++col) {
						if (is_categorical[col]) {
							continue;
						}
						double v = UnifiedVectorFormat::GetData<double>(formats[col])[formats[col].sel->get_index(row)];
						if (std::isnan(v)) {
							kind = IsolationRowKind::SKIPPED_NAN;
						} else if (std::isinf(v)) {
							kind = IsolationRowKind::NON_FINITE_ANOMALY;
						}
					}
					if (kind == IsolationRowKind::SKIPPED_NAN) continue;

					state.source_row_ids.push_back(source_row);
					if (bind_data.id_column.Enabled()) {
						state.id_values.push_back(chunk->data[id_col_idx].GetValue(row));
					}
					non_finite_rows.push_back(kind == IsolationRowKind::NON_FINITE_ANOMALY);
					// +-Inf rows are anomalies by definition and stay out of the trees
					if (kind == IsolationRowKind::NON_FINITE_ANOMALY) continue;

					// Process feature columns (not including weight column)
					for (idx_t col = 0; col < bind_data.column_names.size(); ++col) {
						auto idx = formats[col].sel->get_index(row);
						if (is_categorical[col]) {
							string cat_val = UnifiedVectorFormat::GetData<string_t>(formats[col])[idx].GetString();
							int cat_idx = column_info[col].AddCategory(cat_val);
							data[col].category_indices.push_back(cat_idx);
						} else {
							double num_val = UnifiedVectorFormat::GetData<double>(formats[col])[idx];
							data[col].numeric_values.push_back(num_val);
							if (col == 0 && !bind_data.is_multivariate) {
								sum_first_col += num_val;
							}
						}
					}

					// Extract weight value if weight column is present (last column in result)
					if (bind_data.has_weight_column) {
						auto idx = formats[weight_col_idx].sel->get_index(row);
						sample_weights.push_back(UnifiedVectorFormat::GetData<double>(formats[weight_col_idx])[idx]);
					}
				}
			}
			if (result->HasError()) {
				throw InvalidInputException("Failed to query source table: %s", result->GetError());
			}

			size_t n_rows = data.empty() ? 0 : data[0].size();

			if (n_rows == 0) {
				FinishIsolationForestScores(state, nullptr, {}, non_finite_rows, 0.0, !bind_data.is_multivariate);
			} else {
				// Create and fit isolation forest with mixed-type data (Extended IF with ndim/coef_type, scoring_metric, SCiForest with ntry/prob_pick_avg_gain)
				IsolationForest forest(bind_data.n_trees, bind_data.sample_size, bind_data.contamination,
				                       actual_seed, bind_data.ndim, bind_data.coef_type, bind_data.scoring_metric,
				                       bind_data.ntry, bind_data.prob_pick_avg_gain);

				// sample_weights was already collected during data fetch (guarantees correct row alignment)
				forest.FitMixed(data, column_info, sample_weights);

				// Score all points directly into the state; row ids and
				// anomaly flags are derived during emission
				auto finite_scores = forest.ScoreBatchMixed(data);
				FinishIsolationForestScores(state, &forest, finite_scores, non_finite_rows, sum_first_col,
				                            !bind_data.is_multivariate);
			}
		} else {
			// Numeric-only path: use original vector<vector<double>> format
			std::vector<std::vector<double>> data;
			double sum_first_col = 0.0;
			std::vector<bool> non_finite_rows;  // per scored row: +-Inf in a feature

			std::vector<UnifiedVectorFormat> formats;
			while (true) {
				auto chunk = result->Fetch();
				if (!chunk || chunk->size() == 0) break;

				const idx_t chunk_cols = chunk->ColumnCount();
				formats.resize(chunk_cols);
				for (idx_t col = 0; col < chunk_cols; ++col) {
					chunk->data[col].ToUnifiedFormat(chunk->size(), formats[col]);
				}

				for (idx_t row = 0; row < chunk->size(); ++row) {
					source_row++;
					bool valid_row = true;
					for (idx_t col = 0; col < n_checked_cols; ++col) {
						if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
							valid_row = false;
							break;
						}
					}
					if (!valid_row) continue;

					// The numeric path has no weight column: every checked column is a feature
					std::vector<double> point(n_checked_cols);
					auto kind = IsolationRowKind::FINITE;
					for (idx_t col = 0; col < n_checked_cols; ++col) {
						point[col] = UnifiedVectorFormat::GetData<double>(formats[col])[formats[col].sel->get_index(row)];
						if (std::isnan(point[col])) {
							kind = IsolationRowKind::SKIPPED_NAN;
							break;
						}
						if (std::isinf(point[col])) {
							kind = IsolationRowKind::NON_FINITE_ANOMALY;
						}
					}
					// NaN is skipped like NULL (the row keeps its row_id slot)
					if (kind == IsolationRowKind::SKIPPED_NAN) continue;

					state.source_row_ids.push_back(source_row);
					if (bind_data.id_column.Enabled()) {
						state.id_values.push_back(chunk->data[id_col_idx].GetValue(row));
					}
					if (!bind_data.is_multivariate) {
						// Retain the raw input values for the "value" output column
						state.first_column_values.push_back(point[0]);
					}
					non_finite_rows.push_back(kind == IsolationRowKind::NON_FINITE_ANOMALY);
					// +-Inf rows are anomalies by definition and stay out of the trees
					if (kind == IsolationRowKind::NON_FINITE_ANOMALY) continue;
					if (!bind_data.is_multivariate) {
						sum_first_col += point[0];
					}
					data.push_back(std::move(point));
				}
			}
			if (result->HasError()) {
				throw InvalidInputException("Failed to query source table: %s", result->GetError());
			}

			if (data.empty()) {
				FinishIsolationForestScores(state, nullptr, {}, non_finite_rows, 0.0, !bind_data.is_multivariate);
			} else {
				// Create and fit isolation forest (ndim/coef_type/scoring_metric/ntry/prob_pick_avg_gain passed but only used for multivariate)
				IsolationForest forest(bind_data.n_trees, bind_data.sample_size, bind_data.contamination,
				                       actual_seed, bind_data.ndim, bind_data.coef_type, bind_data.scoring_metric,
				                       bind_data.ntry, bind_data.prob_pick_avg_gain);
				forest.Fit(data);

				// Score all points directly into the state; row ids and
				// anomaly flags are derived during emission
				auto finite_scores = forest.ScoreBatch(data);
				FinishIsolationForestScores(state, &forest, finite_scores, non_finite_rows, sum_first_col,
				                            !bind_data.is_multivariate);
			}
		}
	}

	// Output results, streamed chunk-wise from the scores held in the state
	if (bind_data.output_mode == "scores") {
		// Scores mode - return individual rows
		idx_t count = MinValue<idx_t>(state.scores.size() - state.current_row, STANDARD_VECTOR_SIZE);
		duckdb_compat::SetChildCardinality(output, count);

		auto row_ids = duckdb_compat::FlatData<int64_t>(output.data[0]);
		idx_t col = 1;
		Vector *id_vec = nullptr;
		if (bind_data.id_column.Enabled()) {
			id_vec = &output.data[col++];
		}
		double *values = nullptr;
		if (!bind_data.is_multivariate) {
			values = duckdb_compat::FlatData<double>(output.data[col++]);
		}
		auto anomaly_scores = duckdb_compat::FlatData<double>(output.data[col++]);
		auto is_anomaly = duckdb_compat::FlatData<bool>(output.data[col]);

		for (idx_t i = 0; i < count; ++i) {
			idx_t r = state.current_row + i;
			row_ids[i] = state.source_row_ids[r];
			if (id_vec) {
				// Scores are in scan order, like the ids captured with them
				id_vec->SetValue(i, state.id_values[r]);
			}
			if (values) {
				// Mixed-type univariate input has no numeric first column; it reports 0.0
				values[i] = r < state.first_column_values.size() ? state.first_column_values[r] : 0.0;
			}
			anomaly_scores[i] = state.scores[r];
			is_anomaly[i] = state.anomaly_flags[r];
		}
		state.current_row += count;
	} else {
		// Summary mode - return single row
		if (state.current_row == 0) {
			string status = state.outlier_count > 0 ? "fail" : "pass";
			string message = state.outlier_count == 0 ?
				"No anomalies detected" :
				std::to_string(state.outlier_count) + " anomalies detected";

			if (bind_data.is_multivariate) {
				output.SetValue(0, 0, Value(status));
				output.SetValue(1, 0, Value::BIGINT(state.outlier_count));
				output.SetValue(2, 0, Value::BIGINT(state.total_count));
				output.SetValue(3, 0, Value::INTEGER(static_cast<int32_t>(bind_data.column_names.size())));
				output.SetValue(4, 0, Value::DOUBLE(bind_data.contamination));
				output.SetValue(5, 0, Value::INTEGER(static_cast<int32_t>(bind_data.n_trees)));
				output.SetValue(6, 0, Value(message));
			} else {
				output.SetValue(0, 0, Value(status));
				output.SetValue(1, 0, Value::BIGINT(state.outlier_count));
				output.SetValue(2, 0, Value::BIGINT(state.total_count));
				output.SetValue(3, 0, state.avg_is_null ? Value(LogicalType::DOUBLE) : Value::DOUBLE(state.avg_value));
				output.SetValue(4, 0, Value::DOUBLE(bind_data.contamination));
				output.SetValue(5, 0, Value::INTEGER(static_cast<int32_t>(bind_data.n_trees)));
				output.SetValue(6, 0, Value(message));
			}
			duckdb_compat::SetChildCardinality(output, 1);
			state.current_row = 1;
		} else {
			duckdb_compat::SetChildCardinality(output, 0);
		}
	}
}

//===--------------------------------------------------------------------===//
// DBSCAN Table Function - Actual C++ Execution
//===--------------------------------------------------------------------===//

// Bind data for DBSCAN table function (immutable after bind)
struct DBSCANBindData : public TableFunctionData {
	string table_name;
	vector<string> column_names;
	double eps;
	int64_t min_pts;
	string output_mode; // "summary" or "clusters"
	bool is_multivariate;
	IdColumn id_column; // Optional pass-through identifier ('clusters' mode)

	DBSCANBindData(string tbl, vector<string> cols, double eps_val, int64_t min_pts_val, string mode,
	               bool force_multivariate)
	    : table_name(std::move(tbl)), column_names(std::move(cols)), eps(eps_val), min_pts(min_pts_val),
	      output_mode(std::move(mode)), is_multivariate(force_multivariate || column_names.size() > 1) {
	}
};

// Global state for DBSCAN execution - stores mutable execution state
struct DBSCANGlobalState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t current_row = 0;

	// Results computed during execution (stored here because bind_data is const)
	std::vector<DBSCANPoint> points;
	std::vector<double> anomaly_scores;
	std::vector<double> first_column_values; // univariate only
	std::vector<Value> id_values;            // id_column value per point (empty without id_column)
	std::vector<int64_t> source_row_ids;     // 1-based scan position of each point ("row_id")
	int64_t total_count = 0;
	int64_t cluster_count = 0;
	int64_t noise_count = 0;
	int64_t largest_cluster_size = 0;
};

// Initialize global state
static unique_ptr<GlobalTableFunctionState> DBSCANInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<DBSCANGlobalState>();
}

// Shared optional-parameter handling and validation for both DBSCAN bind functions
static void ParseDBSCANParameters(TableFunctionBindInput &input, const string &function_name, double &eps,
                                  int64_t &min_pts, string &output_mode) {
	eps = OptionalInput<double>(input, 2, 0.5);
	min_pts = OptionalInput<int64_t>(input, 3, 5);
	output_mode = LowerOption(OptionalInput<string>(input, 4, "summary"));

	if (!std::isfinite(eps) || eps <= 0.0) {
		throw BinderException(function_name + ": eps must be a finite number > 0.0");
	}
	if (min_pts < 1) {
		throw BinderException(function_name + ": min_pts must be >= 1");
	}
	if (output_mode != "summary" && output_mode != "clusters") {
		throw BinderException(function_name + ": output_mode must be 'summary' or 'clusters'");
	}
}

// Shared output schema for both DBSCAN functions
static void DefineDBSCANSchema(const string &output_mode, bool include_value, bool include_n_columns,
                               vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	if (output_mode == "clusters") {
		// Per-row output: one row per non-NULL input row
		names.emplace_back("row_id");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		if (include_value) {
			names.emplace_back("value");
			return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		}
		names.emplace_back("cluster_id");
		return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		names.emplace_back("point_type");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("neighbor_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("anomaly_score");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("is_anomaly");
		return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	} else {
		// Summary output: a single row with aggregate clustering statistics
		names.emplace_back("status");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
		names.emplace_back("cluster_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("noise_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("total_count");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("noise_rate");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("largest_cluster_size");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		names.emplace_back("eps");
		return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
		names.emplace_back("min_pts");
		return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
		if (include_n_columns) {
			names.emplace_back("n_columns");
			return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
		}
		names.emplace_back("message");
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}
}

// Bind function for univariate DBSCAN
static unique_ptr<FunctionData> DBSCANBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_dbscan");
	if (input.inputs.size() < 2) {
		throw BinderException("dbscan requires at least 2 arguments: table_name, column_name");
	}

	string table_name = RequireNonNullArgument(input, 0, "dbscan", "table_name");
	vector<string> column_names = {RequireNonNullArgument(input, 1, "dbscan", "column_name")};

	double eps;
	int64_t min_pts;
	string output_mode;
	ParseDBSCANParameters(input, "dbscan", eps, min_pts, output_mode);

	DefineDBSCANSchema(output_mode, /*include_value=*/true, /*include_n_columns=*/false, return_types, names);
	column_names = ResolveFeatureColumns(context, "dbscan", table_name, column_names, false);
	auto id_column = BindIdColumn(context, input, "dbscan", table_name, output_mode == "clusters", "clusters");
	InsertIdColumn(id_column, "dbscan", return_types, names);

	auto bind_data = make_uniq<DBSCANBindData>(table_name, std::move(column_names), eps, min_pts, output_mode, false);
	bind_data->id_column = std::move(id_column);
	return std::move(bind_data);
}

// Bind function for multivariate DBSCAN
static unique_ptr<FunctionData> DBSCANMultivariateBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("metric_dbscan_mv");
	if (input.inputs.size() < 2) {
		throw BinderException("dbscan_mv requires at least 2 arguments: table_name, column_names");
	}

	string table_name = RequireNonNullArgument(input, 0, "dbscan_mv", "table_name");
	auto column_names = ParseCommaSeparatedColumns(RequireNonNullArgument(input, 1, "dbscan_mv", "column_names"));
	if (column_names.empty()) {
		throw BinderException("column_names must contain at least one column");
	}

	double eps;
	int64_t min_pts;
	string output_mode;
	ParseDBSCANParameters(input, "dbscan_mv", eps, min_pts, output_mode);

	DefineDBSCANSchema(output_mode, /*include_value=*/false, /*include_n_columns=*/true, return_types, names);
	column_names = ResolveFeatureColumns(context, "dbscan_mv", table_name, column_names, false);
	auto id_column = BindIdColumn(context, input, "dbscan_mv", table_name, output_mode == "clusters", "clusters");
	InsertIdColumn(id_column, "dbscan_mv", return_types, names);

	auto bind_data = make_uniq<DBSCANBindData>(table_name, std::move(column_names), eps, min_pts, output_mode, true);
	bind_data->id_column = std::move(id_column);
	return std::move(bind_data);
}

static const char *DBSCANPointTypeToString(PointType type) {
	switch (type) {
	case PointType::CORE:
		return "CORE";
	case PointType::BORDER:
		return "BORDER";
	case PointType::NOISE:
		return "NOISE";
	default:
		return "UNVISITED";
	}
}

// Execute DBSCAN clustering
static void DBSCANExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<DBSCANBindData>();
	auto &state = data_p.global_state->Cast<DBSCANGlobalState>();

	// Execute algorithm only once
	if (!state.executed) {
		state.executed = true;

		Connection con(*context.db);

		// Materialize the selected columns as DOUBLE, skipping rows with NULLs
		// (same query path as the isolation forest table function)
		string column_list;
		for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
			if (i > 0) {
				column_list += ", ";
			}
			column_list += "CAST(" + QuoteSqlIdentifier(bind_data.column_names[i]) + " AS DOUBLE)";
		}
		// The id column (if any) is read last in the same query, so each id
		// stays with its own point; it is never a filter (NULL ids pass through)
		const idx_t n_features = bind_data.column_names.size();
		column_list += bind_data.id_column.SelectSuffix();
		// No WHERE clause: every row of the scan is counted, so row_id is the
		// 1-based position in the scan order (the table row number), also
		// after rows that are skipped below because a value is NULL.
		string query = "SELECT " + column_list + " FROM " + BuildQueryTableRef(bind_data.table_name);
		int64_t source_row = 0;

		// Stream the input instead of materializing the full result; the
		// clustering algorithm requires the complete dataset, so the feature
		// matrix built below is the only full copy that is kept.
		auto result = StreamQuery(con, query);
		if (result->HasError()) {
			throw InvalidInputException("Failed to query source table: %s", result->GetError());
		}

		std::vector<std::vector<double>> data;
		std::vector<UnifiedVectorFormat> formats;
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}

			const idx_t chunk_cols = chunk->ColumnCount();
			formats.resize(chunk_cols);
			for (idx_t col = 0; col < chunk_cols; ++col) {
				chunk->data[col].ToUnifiedFormat(chunk->size(), formats[col]);
			}

			for (idx_t row = 0; row < chunk->size(); ++row) {
				source_row++;
				bool valid_row = true;
				for (idx_t col = 0; col < n_features; ++col) {
					if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
						valid_row = false;
						break;
					}
				}
				if (!valid_row) {
					continue;
				}

				state.source_row_ids.push_back(source_row);
				if (bind_data.id_column.Enabled()) {
					state.id_values.push_back(chunk->data[n_features].GetValue(row));
				}
				std::vector<double> point(n_features);
				for (idx_t col = 0; col < n_features; ++col) {
					point[col] = UnifiedVectorFormat::GetData<double>(formats[col])[formats[col].sel->get_index(row)];
				}
				if (!bind_data.is_multivariate) {
					state.first_column_values.push_back(point[0]);
				}
				data.push_back(std::move(point));
			}
		}
		if (result->HasError()) {
			throw InvalidInputException("Failed to query source table: %s", result->GetError());
		}

		state.total_count = static_cast<int64_t>(data.size());

		if (!data.empty()) {
			DBSCAN dbscan(bind_data.eps, static_cast<size_t>(bind_data.min_pts));
			dbscan.Fit(data);

			state.points = dbscan.GetResults();
			state.anomaly_scores = dbscan.ComputeAnomalyScores();
			state.cluster_count = static_cast<int64_t>(dbscan.GetClusterCount());
			state.noise_count = static_cast<int64_t>(dbscan.GetNoiseCount());
			state.largest_cluster_size = static_cast<int64_t>(dbscan.GetLargestClusterSize());
		}

		AnofoxTrace(AnofoxLogLevel::Debug, "metric: dbscan table='" + bind_data.table_name +
		                                       "' rows=" + std::to_string(state.total_count) +
		                                       " clusters=" + std::to_string(state.cluster_count) +
		                                       " noise=" + std::to_string(state.noise_count));
	}

	// Output results, streamed chunk-wise from the clustering results
	if (bind_data.output_mode == "clusters") {
		// Clusters mode - return one row per input point
		idx_t count = MinValue<idx_t>(state.points.size() - state.current_row, STANDARD_VECTOR_SIZE);
		duckdb_compat::SetChildCardinality(output, count);

		idx_t col = 0;
		auto row_ids = duckdb_compat::FlatData<int64_t>(output.data[col++]);
		Vector *id_vec = nullptr;
		if (bind_data.id_column.Enabled()) {
			id_vec = &output.data[col++];
		}
		double *values = nullptr;
		if (!bind_data.is_multivariate) {
			values = duckdb_compat::FlatData<double>(output.data[col++]);
		}
		auto cluster_ids = duckdb_compat::FlatData<int32_t>(output.data[col++]);
		auto &point_type_vec = output.data[col];
		auto point_types = duckdb_compat::FlatData<string_t>(output.data[col++]);
		auto neighbor_counts = duckdb_compat::FlatData<int64_t>(output.data[col++]);
		auto anomaly_scores = duckdb_compat::FlatData<double>(output.data[col++]);
		auto is_anomaly = duckdb_compat::FlatData<bool>(output.data[col]);

		for (idx_t i = 0; i < count; ++i) {
			idx_t r = state.current_row + i;
			auto &pt = state.points[r];
			row_ids[i] = state.source_row_ids[r];
			if (id_vec) {
				// Points are in scan order, like the ids captured with them
				id_vec->SetValue(i, state.id_values[r]);
			}
			if (values) {
				values[i] = state.first_column_values[r];
			}
			cluster_ids[i] = pt.cluster_id;
			point_types[i] = StringVector::AddString(point_type_vec, DBSCANPointTypeToString(pt.point_type));
			neighbor_counts[i] = static_cast<int64_t>(pt.neighbor_count);
			anomaly_scores[i] = state.anomaly_scores[r];
			is_anomaly[i] = pt.cluster_id == -1;
		}
		state.current_row += count;
	} else {
		// Summary mode - return single row
		if (state.current_row == 0) {
			string status = state.noise_count > 0 ? "fail" : "pass";
			string message = state.noise_count == 0
			                     ? "No noise points detected"
			                     : std::to_string(state.noise_count) + " noise point(s) detected";
			double noise_rate = state.total_count > 0
			                        ? static_cast<double>(state.noise_count) / static_cast<double>(state.total_count)
			                        : 0.0;

			idx_t col = 0;
			output.SetValue(col++, 0, Value(status));
			output.SetValue(col++, 0, Value::BIGINT(state.cluster_count));
			output.SetValue(col++, 0, Value::BIGINT(state.noise_count));
			output.SetValue(col++, 0, Value::BIGINT(state.total_count));
			output.SetValue(col++, 0, Value::DOUBLE(noise_rate));
			output.SetValue(col++, 0, Value::BIGINT(state.largest_cluster_size));
			output.SetValue(col++, 0, Value::DOUBLE(bind_data.eps));
			output.SetValue(col++, 0, Value::BIGINT(bind_data.min_pts));
			if (bind_data.is_multivariate) {
				output.SetValue(col++, 0, Value::INTEGER(static_cast<int32_t>(bind_data.column_names.size())));
			}
			output.SetValue(col++, 0, Value(message));
			duckdb_compat::SetChildCardinality(output, 1);
			state.current_row = 1;
		} else {
			duckdb_compat::SetChildCardinality(output, 0);
		}
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

// Helper: register every prefix arity of full_args (from min_arity up) in a function set,
// so that all trailing parameters with bind-time defaults are optional.
// Every overload takes the optional `id_column` named parameter (#108).
static void AddPrefixArities(TableFunctionSet &set, const string &name, const vector<LogicalType> &full_args,
                             idx_t min_arity, table_function_t function, table_function_bind_t bind,
                             table_function_init_global_t init_global) {
	for (idx_t arity = min_arity; arity <= full_args.size(); ++arity) {
		vector<LogicalType> args(full_args.begin(), full_args.begin() + arity);
		TableFunction fn(duckdb_compat::FunctionName(name), std::move(args), function, bind, init_global);
		AddIdColumnParameter(fn);
		set.AddFunction(std::move(fn));
	}
}

void RegisterMetricFunctions(ExtensionLoader &loader) {
	// Register metric table functions using bind_replace pattern
	// This generates SQL queries dynamically for stateless data quality validation

	// anofox_tab_volume(table_name, min_rows, max_rows) (alias: volume)
	TableFunction volume_func("anofox_tab_volume",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                           nullptr, nullptr);
	volume_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricVolumeBindReplace);
	{
		FunctionDescription desc;
		desc.description = "Asserts that a table has between min_rows and max_rows rows, returning the count and assertion status.";
		desc.parameter_names = {"table_name", "min_rows", "max_rows"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT};
		desc.examples = {"SELECT * FROM volume('orders', 100, 1000000);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, volume_func, "volume", {std::move(desc)});
	}

	// anofox_tab_null_rate(table_name, column_name, max_null_rate) (alias: null_rate)
	TableFunction null_rate_func("anofox_tab_null_rate",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                              nullptr, nullptr);
	null_rate_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricNullRateBindReplace);
	{
		FunctionDescription desc;
		desc.description = "Asserts that the fraction of NULL values in a column does not exceed max_null_rate.";
		desc.parameter_names = {"table_name", "column_name", "max_null_rate"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE};
		desc.examples = {"SELECT * FROM null_rate('orders', 'email', 0.05);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, null_rate_func, "null_rate", {std::move(desc)});
	}

	// anofox_tab_distinct_count(table_name, column_name, min_distinct, max_distinct) (alias: distinct_count)
	TableFunction distinct_func("anofox_tab_distinct_count",
	                             {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                              LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                             nullptr, nullptr);
	distinct_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricDistinctCountBindReplace);
	{
		FunctionDescription desc;
		desc.description = "Asserts that the number of distinct values in a column is between min_distinct and max_distinct.";
		desc.parameter_names = {"table_name", "column_name", "min_distinct", "max_distinct"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT};
		desc.examples = {"SELECT * FROM distinct_count('orders', 'status', 1, 10);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, distinct_func, "distinct_count", {std::move(desc)});
	}

	// anofox_tab_zscore(table_name, column_name, threshold) (alias: zscore)
	TableFunction zscore_func("anofox_tab_zscore",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                           nullptr, nullptr);
	zscore_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricZscoreBindReplace);
	{
		FunctionDescription desc;
		desc.description = "Identifies rows where a numeric column value deviates more than threshold standard deviations from the mean.";
		desc.parameter_names = {"table_name", "column_name", "threshold"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE};
		desc.examples = {"SELECT * FROM zscore('orders', 'amount', 3.0);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, zscore_func, "zscore", {std::move(desc)});
	}

	// anofox_tab_iqr(table_name, column_name, iqr_multiplier) (alias: iqr)
	TableFunction iqr_func("anofox_tab_iqr",
	                        {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                        nullptr, nullptr);
	iqr_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricIQRBindReplace);
	{
		FunctionDescription desc;
		desc.description = "Identifies rows where a numeric column value is an outlier by the IQR (interquartile range) method.";
		desc.parameter_names = {"table_name", "column_name", "iqr_multiplier"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE};
		desc.examples = {"SELECT * FROM iqr('orders', 'amount', 1.5);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, iqr_func, "iqr", {std::move(desc)});
	}

	// anofox_tab_schema_check(table_name, required_columns) (alias: schema_check)
	TableFunction schema_func("anofox_tab_schema_check",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                           nullptr, nullptr);
	schema_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricSchemaBindReplace);
	{
		FunctionDescription desc;
		desc.description = "Asserts that a table contains all the required column names.";
		desc.parameter_names = {"table_name", "required_columns"};
		desc.parameter_types = {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)};
		desc.examples = {"SELECT * FROM schema_check('orders', ['id', 'amount', 'status']);"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, schema_func, "schema_check", {std::move(desc)});
	}

	// anofox_tab_freshness(table_name, timestamp_column, max_age) or (table_name, timestamp_column, max_age, reference_time) (alias: freshness)
	TableFunctionSet freshness_set("anofox_tab_freshness");

	// Basic overload with 3 parameters
	TableFunction freshness_func_basic("anofox_tab_freshness",
	                                    {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::INTERVAL)},
	                                    nullptr, nullptr);
	freshness_func_basic.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricFreshnessBindReplace);
	freshness_set.AddFunction(freshness_func_basic);

	// Full overload with 4 parameters (includes reference_time)
	TableFunction freshness_func_full("anofox_tab_freshness",
	                                   {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                    LogicalType(LogicalTypeId::INTERVAL), LogicalType(LogicalTypeId::TIMESTAMP)},
	                                   nullptr, nullptr);
	freshness_func_full.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, MetricFreshnessBindReplace);
	freshness_set.AddFunction(freshness_func_full);

	{
		FunctionDescription desc;
		desc.description = "Returns rows where the most recent value in a timestamp column is older than the specified maximum age interval. Optionally accepts a reference time.";
		desc.parameter_names = {"table_name", "timestamp_column", "max_age", "reference_time"};
		desc.examples = {"SELECT * FROM freshness('events', 'created_at', INTERVAL '1 day');"};
		desc.categories = {"metric", "data-quality"};
		// Same description on the alias -- see the note on the outlier_tree alias.
		RegisterTableFunctionSetWithAlias(loader, std::move(freshness_set), "freshness", {std::move(desc)});
	}

	// anofox_tab_isolation_forest(table_name, column_name, n_trees=100, sample_size=256, contamination=0.1,
	//                             output_mode='summary', seed=NULL) (alias: isolation_forest)
	// All parameters after column_name are optional; defaults are applied at bind time.
	// Uses actual C++ isolation forest algorithm.
	const vector<LogicalType> iso_forest_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::DOUBLE),  LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::BIGINT)};
	TableFunctionSet iso_forest_set("anofox_tab_isolation_forest");
	AddPrefixArities(iso_forest_set, "anofox_tab_isolation_forest", iso_forest_args, 2, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, IsolationForestExecute),
	                 IsolationForestBind, IsolationForestInit);
	{
		FunctionDescription desc;
		desc.description =
		    "Detects univariate outliers in a numeric column using the Isolation Forest algorithm. Returns scores and outlier labels.";
		desc.parameter_names = {"table_name", "column_name", "n_trees", "sample_size", "contamination", "output_mode", "seed"};
		desc.examples = {"SELECT * FROM isolation_forest('sales', 'amount', 100, 256, 0.05, 'scores');"};
		desc.categories = {"metric", "anomaly-detection"};
		RegisterTableFunctionSetWithAlias(loader, iso_forest_set, "isolation_forest", {std::move(desc)});
	}

	// anofox_tab_isolation_forest_mv(table_name, column_names (comma-separated), n_trees=100, sample_size=256,
	//                                contamination=0.1, output_mode='summary', seed=NULL, ndim=1,
	//                                coef_type='uniform', scoring_metric='depth', weight_column=NULL, ntry=1,
	//                                prob_pick_avg_gain=0.0) (alias: isolation_forest_mv)
	// All parameters after column_names are optional; defaults are applied at bind time.
	const vector<LogicalType> iso_forest_mv_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::DOUBLE),  LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	    LogicalType(LogicalTypeId::DOUBLE)};
	TableFunctionSet iso_forest_mv_set("anofox_tab_isolation_forest_mv");
	AddPrefixArities(iso_forest_mv_set, "anofox_tab_isolation_forest_mv", iso_forest_mv_args, 2,
	                 DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, IsolationForestExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, IsolationForestMultivariateBind), IsolationForestInit);
	{
		FunctionDescription desc;
		desc.description =
		    "Detects multivariate outliers across multiple numeric columns (comma-separated) using the Isolation Forest algorithm.";
		desc.parameter_names = {"table_name",  "column_names",   "n_trees",       "sample_size", "contamination",
		                        "output_mode", "seed",           "ndim",          "coef_type",   "scoring_metric",
		                        "weight_column", "ntry",         "prob_pick_avg_gain"};
		desc.examples = {"SELECT * FROM isolation_forest_mv('sales', 'amount,qty', 100, 256, 0.05, 'scores');"};
		desc.categories = {"metric", "anomaly-detection"};
		RegisterTableFunctionSetWithAlias(loader, iso_forest_mv_set, "isolation_forest_mv", {std::move(desc)});
	}

	// anofox_tab_dbscan(table_name, column_name, eps=0.5, min_pts=5, output_mode='summary') (alias: dbscan)
	// All parameters after column_name are optional; defaults are applied at bind time.
	// Uses actual C++ DBSCAN algorithm.
	const vector<LogicalType> dbscan_args = {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                         LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::BIGINT),
	                                         LogicalType(LogicalTypeId::VARCHAR)};
	TableFunctionSet dbscan_set("anofox_tab_dbscan");
	AddPrefixArities(dbscan_set, "anofox_tab_dbscan", dbscan_args, 2, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, DBSCANExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, DBSCANBind), DBSCANInit);
	{
		FunctionDescription desc;
		desc.description = "Clusters rows by a numeric column using DBSCAN. Returns cluster labels and noise flags.";
		desc.parameter_names = {"table_name", "column_name", "eps", "min_pts", "output_mode"};
		desc.examples = {"SELECT * FROM dbscan('orders', 'amount', 0.5, 5, 'clusters');"};
		desc.categories = {"metric", "anomaly-detection"};
		RegisterTableFunctionSetWithAlias(loader, dbscan_set, "dbscan", {std::move(desc)});
	}

	// anofox_tab_dbscan_mv(table_name, column_names (comma-separated), eps=0.5, min_pts=5, output_mode='summary')
	// (alias: dbscan_mv)
	// All parameters after column_names are optional; defaults are applied at bind time.
	TableFunctionSet dbscan_mv_set("anofox_tab_dbscan_mv");
	AddPrefixArities(dbscan_mv_set, "anofox_tab_dbscan_mv", dbscan_args, 2, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, DBSCANExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, DBSCANMultivariateBind),
	                 DBSCANInit);
	{
		FunctionDescription desc;
		desc.description =
		    "Clusters rows by multiple numeric columns (comma-separated) using DBSCAN. Returns cluster labels and noise flags.";
		desc.parameter_names = {"table_name", "column_names", "eps", "min_pts", "output_mode"};
		desc.examples = {"SELECT * FROM dbscan_mv('orders', 'amount,qty', 0.5, 5, 'clusters');"};
		desc.categories = {"metric", "anomaly-detection"};
		RegisterTableFunctionSetWithAlias(loader, dbscan_mv_set, "dbscan_mv", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

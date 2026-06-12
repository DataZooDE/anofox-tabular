#include "anofox_metric.hpp"
#include "anofox_isolation_forest.hpp"
#include "anofox_dbscan.hpp"
#include "anofox_function_alias.hpp"
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
#include <algorithm>
#include <chrono>
#include <cmath>

namespace duckdb {
namespace anofox {

namespace {

static unique_ptr<SubqueryRef> ParseSubquery(const string &query, const ParserOptions &options, const string &err_msg) {
	Parser parser(options);
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto select_stmt = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));
	return make_uniq<SubqueryRef>(std::move(select_stmt));
}

// Helper: reject NaN/Inf double parameters at bind time with a clear error.
// NULL values are left alone (callers substitute their documented defaults).
static void ValidateFiniteDouble(const Value &value, const string &function_name, const string &parameter_name) {
	if (value.IsNull()) {
		return;
	}
	double val = value.GetValue<double>();
	if (!std::isfinite(val)) {
		throw BinderException(function_name + ": " + parameter_name + " must be a finite number");
	}
}

// Helper: parse a comma-separated column list, trimming whitespace and surrounding quotes
static vector<string> ParseCommaSeparatedColumns(const string &columns_str) {
	vector<string> column_names;
	size_t start = 0;
	for (size_t i = 0; i <= columns_str.length(); ++i) {
		if (i == columns_str.length() || columns_str[i] == ',') {
			string col = columns_str.substr(start, i - start);
			size_t col_start = col.find_first_not_of(" \t\n\r");
			size_t col_end = col.find_last_not_of(" \t\n\r");
			if (col_start != string::npos) {
				col = col.substr(col_start, col_end - col_start + 1);
				if (col.length() >= 2 && ((col.front() == '"' && col.back() == '"') ||
				                          (col.front() == '\'' && col.back() == '\''))) {
					col = col.substr(1, col.length() - 2);
				}
				column_names.push_back(col);
			}
			start = i + 1;
		}
	}
	return column_names;
}

// Helper: Generate SQL for volume metrics
static string GenerateVolumeSQL(const string &table_ref, const Value &min_rows, const Value &max_rows) {
	string min_val = min_rows.IsNull() ? "NULL" : min_rows.ToString();
	string max_val = max_rows.IsNull() ? "NULL" : max_rows.ToString();

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
	string max_rate_val = max_null_rate.IsNull() ? "1.0" : max_null_rate.ToString();
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
		"    'Null rate ' || CAST(null_rate AS VARCHAR) || ' (' || CAST(null_count AS VARCHAR) || '/' || "
		"    CAST(total_count AS VARCHAR) || ') is acceptable' "
		"  ELSE "
		"    'Null rate ' || CAST(null_rate AS VARCHAR) || ' (' || CAST(null_count AS VARCHAR) || '/' || "
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
	string min_val = min_distinct.IsNull() ? "NULL" : min_distinct.ToString();
	string max_val = max_distinct.IsNull() ? "NULL" : max_distinct.ToString();
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

// Helper: Generate SQL for zscore metrics using CTE to avoid nested aggregates
static string GenerateZscoreSQL(const string &table_ref, const string &column_name, const Value &threshold) {
	string thresh_val = threshold.IsNull() ? "3.0" : threshold.ToString();
	string col_id = QuoteSqlIdentifier(column_name);

	// Use CTEs to avoid nested aggregate error:
	// 1. vals: the non-NULL values cast to DOUBLE
	// 2. stats: compute mean and stddev (always exactly one row)
	// 3. with_zscore: compute z-score for each row; a constant column (stddev = 0)
	//    or a single row (stddev NULL) yields z-score 0 instead of NaN/NULL
	// 4. summary: driven by stats so exactly one row is returned even for empty input
	// Empty-input semantics: empty/all-NULL input yields one row with total_count = 0,
	// outlier_count = 0, outlier_rate = 0.0, NULL mean/stddev, status 'pass'.
	return "WITH vals AS ("
		"SELECT CAST(" + col_id + " AS DOUBLE) AS val FROM " + table_ref + " WHERE " + col_id + " IS NOT NULL"
		"), "
		"stats AS ("
		"SELECT AVG(val) AS mean, STDDEV(val) AS stddev, COUNT(*) AS total_count FROM vals"
		"), "
		"with_zscore AS ("
		"SELECT CASE WHEN s.stddev IS NULL OR s.stddev = 0 THEN 0.0 "
		"            ELSE ABS((v.val - s.mean) / s.stddev) END AS abs_zscore "
		"FROM vals v, stats s"
		"), "
		"summary AS ("
		"SELECT "
		"  s.mean, s.stddev, s.total_count, "
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
		"    'No outliers detected (z-score threshold: ' || CAST(" + thresh_val + " AS VARCHAR) || ')' "
		"  ELSE "
		"    CAST(outlier_count AS VARCHAR) || ' outlier(s) detected (' || "
		"    CAST((CAST(outlier_count AS DOUBLE) / total_count * 100) AS VARCHAR) || "
		"    '% of ' || CAST(total_count AS VARCHAR) || ' rows)' "
		"END AS message "
		"FROM summary";
}

// Helper: Generate SQL for IQR metrics using CTE to avoid nested aggregates
static string GenerateIQRSQL(const string &table_ref, const string &column_name, const Value &iqr_multiplier) {
	string mult_val = iqr_multiplier.IsNull() ? "1.5" : iqr_multiplier.ToString();
	string col_id = QuoteSqlIdentifier(column_name);

	// Use CTEs to avoid nested aggregate error:
	// 1. vals: the non-NULL values cast to DOUBLE
	// 2. stats: compute Q1 and Q3 quantiles (always exactly one row)
	// 3. bounds: derive the outlier bounds from the quantiles
	// 4. summary: driven by bounds so exactly one row is returned even for empty input
	// Empty-input semantics: empty/all-NULL input yields one row with total_count = 0,
	// outlier_count = 0, NULL quantiles/bounds, status 'pass' and an explicit message.
	return "WITH vals AS ("
		"SELECT CAST(" + col_id + " AS DOUBLE) AS val FROM " + table_ref + " WHERE " + col_id + " IS NOT NULL"
		"), "
		"stats AS ("
		"SELECT QUANTILE_CONT(val, 0.25) AS q1, QUANTILE_CONT(val, 0.75) AS q3, COUNT(*) AS total_count FROM vals"
		"), "
		"bounds AS ("
		"SELECT "
		"  q1, "
		"  q3, "
		"  total_count, "
		"  q1 - " + mult_val + " * (q3 - q1) AS lower_bound, "
		"  q3 + " + mult_val + " * (q3 - q1) AS upper_bound "
		"FROM stats"
		"), "
		"summary AS ("
		"SELECT "
		"  b.q1, "
		"  b.q3, "
		"  b.total_count, "
		"  b.lower_bound, "
		"  b.upper_bound, "
		"  (SELECT COUNT(*) FROM vals v, bounds b2 WHERE v.val < b2.lower_bound OR v.val > b2.upper_bound) AS outlier_count "
		"FROM bounds b"
		") "
		"SELECT "
		"CASE WHEN outlier_count = 0 THEN 'pass' ELSE 'fail' END AS status, "
		"q1, "
		"q3, "
		"q3 - q1 AS iqr, "
		"lower_bound, "
		"upper_bound, "
		"outlier_count, "
		"total_count, "
		+ mult_val + " AS multiplier, "
		"CASE "
		"  WHEN total_count = 0 THEN "
		"    'No rows to evaluate (column is empty or all NULL)' "
		"  WHEN outlier_count = 0 THEN "
		"    'No outliers detected by IQR method (bounds: [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || '])' "
		"  ELSE "
		"    CAST(outlier_count AS VARCHAR) || ' outlier(s) detected outside bounds [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || ']' "
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
	       "  SELECT col FROM required"
	       "  EXCEPT"
	       "  SELECT col FROM actual"
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
static string GenerateFreshnessSQL(const string &table_ref, const string &timestamp_column, const Value &max_age, const Value &reference_time) {
	string max_age_interval =
	    max_age.IsNull() ? "INTERVAL '1 day'" : "'" + EscapeSqlStringLiteral(max_age.ToString()) + "'::INTERVAL";
	string ref_time =
	    reference_time.IsNull() ? "now()" : "'" + EscapeSqlStringLiteral(reference_time.ToString()) + "'::TIMESTAMP";
	string col_id = QuoteSqlIdentifier(timestamp_column);

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
		"EXTRACT(EPOCH FROM (" + ref_time + " - latest_timestamp))::BIGINT AS age_seconds, "
		"CASE "
		"  WHEN latest_timestamp IS NULL THEN "
		"    'No timestamp values found (table is empty or column is all NULL)' "
		"  WHEN latest_timestamp >= threshold_timestamp THEN "
		"    'Data is fresh. Latest update: ' || CAST(latest_timestamp AS VARCHAR) || ' (age: ' || CAST(EXTRACT(EPOCH FROM (" + ref_time + " - latest_timestamp))::BIGINT AS VARCHAR) || 's)' "
		"  ELSE "
		"    'Data is stale. Latest update: ' || CAST(latest_timestamp AS VARCHAR) || ' (age: ' || CAST(EXTRACT(EPOCH FROM (" + ref_time + " - latest_timestamp))::BIGINT AS VARCHAR) || 's, max allowed: ' || CAST(EXTRACT(EPOCH FROM " + max_age_interval + ")::BIGINT AS VARCHAR) || 's)' "
		"END AS message "
		"FROM (SELECT "
		"  MAX(" + col_id + ") AS latest_timestamp, "
		+ ref_time + " - " + max_age_interval + " AS threshold_timestamp "
		"FROM " + table_ref + ")";
}

} // anonymous namespace

//===--------------------------------------------------------------------===//
// Metric Bind Replace Functions
//===--------------------------------------------------------------------===//

static unique_ptr<TableRef> MetricVolumeBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_volume");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_volume requires 3 arguments: table_name, min_rows, max_rows");
	}

	string table_name = input.inputs[0].ToString();
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateVolumeSQL(table_ref, input.inputs[1], input.inputs[2]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse volume metric query");
}

static unique_ptr<TableRef> MetricNullRateBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_null_rate");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_null_rate requires 3 arguments: table_name, column_name, max_null_rate");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	ValidateFiniteDouble(input.inputs[2], "null_rate", "max_null_rate");
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateNullRateSQL(table_ref, column_name, input.inputs[2]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse null rate metric query");
}

static unique_ptr<TableRef> MetricDistinctCountBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_distinct_count");
	if (input.inputs.size() < 4) {
		throw BinderException("anofox_metric_distinct_count requires 4 arguments: table_name, column_name, min_distinct, max_distinct");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateDistinctCountSQL(table_ref, column_name, input.inputs[2], input.inputs[3]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse distinct count metric query");
}

static unique_ptr<TableRef> MetricZscoreBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_zscore");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_zscore requires 3 arguments: table_name, column_name, threshold");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	ValidateFiniteDouble(input.inputs[2], "zscore", "threshold");
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateZscoreSQL(table_ref, column_name, input.inputs[2]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse zscore metric query");
}

static unique_ptr<TableRef> MetricIQRBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_iqr");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_iqr requires 3 arguments: table_name, column_name, iqr_multiplier");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
	ValidateFiniteDouble(input.inputs[2], "iqr", "iqr_multiplier");
	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateIQRSQL(table_ref, column_name, input.inputs[2]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse IQR metric query");
}

static unique_ptr<TableRef> MetricSchemaBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_schema");
	if (input.inputs.size() < 2) {
		throw BinderException("anofox_metric_schema requires 2 arguments: table_name, required_columns");
	}

	string table_name = input.inputs[0].ToString();
	vector<string> required_columns;
	if (!input.inputs[1].IsNull() && input.inputs[1].type().id() == LogicalTypeId::LIST) {
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
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse schema metric query");
}

static unique_ptr<TableRef> MetricFreshnessBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_freshness");
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_freshness requires at least 3 arguments: table_name, timestamp_column, max_age");
	}

	string table_name = input.inputs[0].ToString();
	string timestamp_column = input.inputs[1].ToString();
	Value max_age = input.inputs[2];
	Value reference_time = (input.inputs.size() > 3) ? input.inputs[3] : Value();

	string table_ref = BuildQueryTableRef(table_name);
	string sql = GenerateFreshnessSQL(table_ref, timestamp_column, max_age, reference_time);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse freshness metric query");
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
	double threshold = 0.0;
	int64_t total_count = 0;
	int64_t outlier_count = 0;
	double avg_value = 0.0;
};

// Initialize global state
static unique_ptr<GlobalTableFunctionState> IsolationForestInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<IsolationForestGlobalState>();
}

// Shared optional-parameter handling and validation for both isolation forest bind functions.
// Integer parameters are read as signed int64_t and range-checked BEFORE casting to size_t,
// so negative values are rejected instead of wrapping; doubles must be finite.
static void ParseIsolationForestCommonParameters(TableFunctionBindInput &input, const string &function_name,
                                                 size_t &n_trees, size_t &sample_size, double &contamination,
                                                 string &output_mode, uint64_t &seed, bool &has_seed) {
	int64_t n_trees_val = input.inputs.size() > 2 ? input.inputs[2].GetValue<int64_t>() : 100;
	int64_t sample_size_val = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 256;
	contamination = input.inputs.size() > 4 ? input.inputs[4].GetValue<double>() : 0.1;
	output_mode = input.inputs.size() > 5 ? input.inputs[5].ToString() : "summary";

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
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("isolation_forest");
	if (input.inputs.size() < 2) {
		throw BinderException("isolation_forest requires at least 2 arguments: table_name, column_name");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();
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

	return make_uniq<IsolationForestBindData>(table_name, column_names, n_trees, sample_size,
	                                          contamination, output_mode, seed, has_seed);
}

// Bind function for multivariate isolation forest
static unique_ptr<FunctionData> IsolationForestMultivariateBind(ClientContext &context, TableFunctionBindInput &input,
                                                                 vector<LogicalType> &return_types, vector<string> &names) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("isolation_forest_mv");
	if (input.inputs.size() < 2) {
		throw BinderException("isolation_forest_mv requires at least 2 arguments: table_name, column_names");
	}

	string table_name = input.inputs[0].ToString();

	// Parse comma-separated column names
	auto column_names = ParseCommaSeparatedColumns(input.inputs[1].ToString());
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

	// Force multivariate mode for this function (3-column output schema)
	return make_uniq<IsolationForestBindData>(table_name, column_names, n_trees, sample_size,
	                                          contamination, output_mode, seed, has_seed, true, ndim, coef_type,
	                                          scoring_metric, weight_column, has_weight_column,
	                                          ntry, prob_pick_avg_gain);
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
				auto col_type = type_result->types[i].id();
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

		string null_checks;
		for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
			if (i > 0) null_checks += " AND ";
			null_checks += QuoteSqlIdentifier(bind_data.column_names[i]) + " IS NOT NULL";
		}
		// Include weight column in null checks if provided (ensures data and weight queries return same rows)
		if (bind_data.has_weight_column) {
			null_checks += " AND " + QuoteSqlIdentifier(bind_data.weight_column) + " IS NOT NULL";
		}

		string query = "SELECT " + column_list + " FROM " + BuildQueryTableRef(bind_data.table_name) + " WHERE " + null_checks;

		// Stream the input instead of materializing the full result; model
		// fitting requires the complete dataset, so the feature columns built
		// below are the only full copy that is kept.
		auto result = con.SendQuery(query);
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
					bool valid_row = true;
					for (idx_t col = 0; col < chunk_cols; ++col) {
						if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
							valid_row = false;
							break;
						}
					}
					if (!valid_row) continue;

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
			state.total_count = static_cast<int64_t>(n_rows);

			if (n_rows == 0) {
				state.avg_value = 0.0;
				state.outlier_count = 0;
			} else {
				state.avg_value = sum_first_col / static_cast<double>(n_rows);

				// Create and fit isolation forest with mixed-type data (Extended IF with ndim/coef_type, scoring_metric, SCiForest with ntry/prob_pick_avg_gain)
				IsolationForest forest(bind_data.n_trees, bind_data.sample_size, bind_data.contamination,
				                       actual_seed, bind_data.ndim, bind_data.coef_type, bind_data.scoring_metric,
				                       bind_data.ntry, bind_data.prob_pick_avg_gain);

				// sample_weights was already collected during data fetch (guarantees correct row alignment)
				forest.FitMixed(data, column_info, sample_weights);

				// Score all points directly into the state; row ids and
				// anomaly flags are derived during emission
				state.scores = forest.ScoreBatchMixed(data);
				state.threshold = forest.ComputeThreshold(state.scores);
				state.outlier_count = static_cast<int64_t>(std::count_if(
				    state.scores.begin(), state.scores.end(), [&](double s) { return s > state.threshold; }));
			}
		} else {
			// Numeric-only path: use original vector<vector<double>> format
			std::vector<std::vector<double>> data;
			double sum_first_col = 0.0;

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
					bool valid_row = true;
					for (idx_t col = 0; col < chunk_cols; ++col) {
						if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
							valid_row = false;
							break;
						}
					}
					if (!valid_row) continue;

					std::vector<double> point(chunk_cols);
					for (idx_t col = 0; col < chunk_cols; ++col) {
						point[col] = UnifiedVectorFormat::GetData<double>(formats[col])[formats[col].sel->get_index(row)];
					}
					if (!bind_data.is_multivariate) {
						sum_first_col += point[0];
						// Retain the raw input values for the "value" output column
						state.first_column_values.push_back(point[0]);
					}
					data.push_back(std::move(point));
				}
			}
			if (result->HasError()) {
				throw InvalidInputException("Failed to query source table: %s", result->GetError());
			}

			state.total_count = static_cast<int64_t>(data.size());

			if (data.empty()) {
				state.avg_value = 0.0;
				state.outlier_count = 0;
			} else {
				state.avg_value = sum_first_col / static_cast<double>(data.size());

				// Create and fit isolation forest (ndim/coef_type/scoring_metric/ntry/prob_pick_avg_gain passed but only used for multivariate)
				IsolationForest forest(bind_data.n_trees, bind_data.sample_size, bind_data.contamination,
				                       actual_seed, bind_data.ndim, bind_data.coef_type, bind_data.scoring_metric,
				                       bind_data.ntry, bind_data.prob_pick_avg_gain);
				forest.Fit(data);

				// Score all points directly into the state; row ids and
				// anomaly flags are derived during emission
				state.scores = forest.ScoreBatch(data);
				state.threshold = forest.ComputeThreshold(state.scores);
				state.outlier_count = static_cast<int64_t>(std::count_if(
				    state.scores.begin(), state.scores.end(), [&](double s) { return s > state.threshold; }));
			}
		}
	}

	// Output results, streamed chunk-wise from the scores held in the state
	if (bind_data.output_mode == "scores") {
		// Scores mode - return individual rows
		idx_t count = MinValue<idx_t>(state.scores.size() - state.current_row, STANDARD_VECTOR_SIZE);
		output.SetCardinality(count);

		auto row_ids = FlatVector::GetData<int64_t>(output.data[0]);
		idx_t col = 1;
		double *values = nullptr;
		if (!bind_data.is_multivariate) {
			values = FlatVector::GetData<double>(output.data[col++]);
		}
		auto anomaly_scores = FlatVector::GetData<double>(output.data[col++]);
		auto is_anomaly = FlatVector::GetData<bool>(output.data[col]);

		for (idx_t i = 0; i < count; ++i) {
			idx_t r = state.current_row + i;
			row_ids[i] = static_cast<int64_t>(r + 1);  // 1-indexed
			if (values) {
				// Mixed-type univariate input has no numeric first column; it reports 0.0
				values[i] = r < state.first_column_values.size() ? state.first_column_values[r] : 0.0;
			}
			anomaly_scores[i] = state.scores[r];
			is_anomaly[i] = state.scores[r] > state.threshold;
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
				output.SetValue(3, 0, Value::DOUBLE(state.avg_value));
				output.SetValue(4, 0, Value::DOUBLE(bind_data.contamination));
				output.SetValue(5, 0, Value::INTEGER(static_cast<int32_t>(bind_data.n_trees)));
				output.SetValue(6, 0, Value(message));
			}
			output.SetCardinality(1);
			state.current_row = 1;
		} else {
			output.SetCardinality(0);
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
	eps = input.inputs.size() > 2 ? input.inputs[2].GetValue<double>() : 0.5;
	min_pts = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 5;
	output_mode = input.inputs.size() > 4 ? input.inputs[4].ToString() : "summary";

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
                               vector<LogicalType> &return_types, vector<string> &names) {
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
                                           vector<LogicalType> &return_types, vector<string> &names) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_dbscan");
	if (input.inputs.size() < 2) {
		throw BinderException("dbscan requires at least 2 arguments: table_name, column_name");
	}

	string table_name = input.inputs[0].ToString();
	vector<string> column_names = {input.inputs[1].ToString()};

	double eps;
	int64_t min_pts;
	string output_mode;
	ParseDBSCANParameters(input, "dbscan", eps, min_pts, output_mode);

	DefineDBSCANSchema(output_mode, /*include_value=*/true, /*include_n_columns=*/false, return_types, names);
	return make_uniq<DBSCANBindData>(table_name, std::move(column_names), eps, min_pts, output_mode, false);
}

// Bind function for multivariate DBSCAN
static unique_ptr<FunctionData> DBSCANMultivariateBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_dbscan_mv");
	if (input.inputs.size() < 2) {
		throw BinderException("dbscan_mv requires at least 2 arguments: table_name, column_names");
	}

	string table_name = input.inputs[0].ToString();
	auto column_names = ParseCommaSeparatedColumns(input.inputs[1].ToString());
	if (column_names.empty()) {
		throw BinderException("column_names must contain at least one column");
	}

	double eps;
	int64_t min_pts;
	string output_mode;
	ParseDBSCANParameters(input, "dbscan_mv", eps, min_pts, output_mode);

	DefineDBSCANSchema(output_mode, /*include_value=*/false, /*include_n_columns=*/true, return_types, names);
	return make_uniq<DBSCANBindData>(table_name, std::move(column_names), eps, min_pts, output_mode, true);
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
		string null_checks;
		for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
			if (i > 0) {
				column_list += ", ";
				null_checks += " AND ";
			}
			column_list += "CAST(" + QuoteSqlIdentifier(bind_data.column_names[i]) + " AS DOUBLE)";
			null_checks += QuoteSqlIdentifier(bind_data.column_names[i]) + " IS NOT NULL";
		}
		string query = "SELECT " + column_list + " FROM " + BuildQueryTableRef(bind_data.table_name) + " WHERE " +
		               null_checks;

		// Stream the input instead of materializing the full result; the
		// clustering algorithm requires the complete dataset, so the feature
		// matrix built below is the only full copy that is kept.
		auto result = con.SendQuery(query);
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
				bool valid_row = true;
				for (idx_t col = 0; col < chunk_cols; ++col) {
					if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
						valid_row = false;
						break;
					}
				}
				if (!valid_row) {
					continue;
				}

				std::vector<double> point(chunk_cols);
				for (idx_t col = 0; col < chunk_cols; ++col) {
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
		output.SetCardinality(count);

		idx_t col = 0;
		auto row_ids = FlatVector::GetData<int64_t>(output.data[col++]);
		double *values = nullptr;
		if (!bind_data.is_multivariate) {
			values = FlatVector::GetData<double>(output.data[col++]);
		}
		auto cluster_ids = FlatVector::GetData<int32_t>(output.data[col++]);
		auto &point_type_vec = output.data[col];
		auto point_types = FlatVector::GetData<string_t>(output.data[col++]);
		auto neighbor_counts = FlatVector::GetData<int64_t>(output.data[col++]);
		auto anomaly_scores = FlatVector::GetData<double>(output.data[col++]);
		auto is_anomaly = FlatVector::GetData<bool>(output.data[col]);

		for (idx_t i = 0; i < count; ++i) {
			idx_t r = state.current_row + i;
			auto &pt = state.points[r];
			row_ids[i] = static_cast<int64_t>(r + 1);  // 1-indexed
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
			output.SetCardinality(1);
			state.current_row = 1;
		} else {
			output.SetCardinality(0);
		}
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

// Helper: register every prefix arity of full_args (from min_arity up) in a function set,
// so that all trailing parameters with bind-time defaults are optional.
static void AddPrefixArities(TableFunctionSet &set, const string &name, const vector<LogicalType> &full_args,
                             idx_t min_arity, table_function_t function, table_function_bind_t bind,
                             table_function_init_global_t init_global) {
	for (idx_t arity = min_arity; arity <= full_args.size(); ++arity) {
		vector<LogicalType> args(full_args.begin(), full_args.begin() + arity);
		set.AddFunction(TableFunction(name, std::move(args), function, bind, init_global));
	}
}

void RegisterMetricFunctions(ExtensionLoader &loader) {
	// Register metric table functions using bind_replace pattern
	// This generates SQL queries dynamically for stateless data quality validation

	// anofox_tab_volume(table_name, min_rows, max_rows) (alias: volume)
	TableFunction volume_func("anofox_tab_volume",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                           nullptr, nullptr);
	volume_func.bind_replace = MetricVolumeBindReplace;
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
	null_rate_func.bind_replace = MetricNullRateBindReplace;
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
	distinct_func.bind_replace = MetricDistinctCountBindReplace;
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
	zscore_func.bind_replace = MetricZscoreBindReplace;
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
	iqr_func.bind_replace = MetricIQRBindReplace;
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
	schema_func.bind_replace = MetricSchemaBindReplace;
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
	freshness_func_basic.bind_replace = MetricFreshnessBindReplace;
	freshness_set.AddFunction(freshness_func_basic);

	// Full overload with 4 parameters (includes reference_time)
	TableFunction freshness_func_full("anofox_tab_freshness",
	                                   {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                    LogicalType(LogicalTypeId::INTERVAL), LogicalType(LogicalTypeId::TIMESTAMP)},
	                                   nullptr, nullptr);
	freshness_func_full.bind_replace = MetricFreshnessBindReplace;
	freshness_set.AddFunction(freshness_func_full);

	{
		FunctionDescription desc;
		desc.description = "Returns rows where the most recent value in a timestamp column is older than the specified maximum age interval. Optionally accepts a reference time.";
		desc.parameter_names = {"table_name", "timestamp_column", "max_age", "reference_time"};
		desc.examples = {"SELECT * FROM freshness('events', 'created_at', INTERVAL '1 day');"};
		desc.categories = {"metric", "data-quality"};
		CreateTableFunctionInfo freshness_info(freshness_set);
		freshness_info.descriptions = {std::move(desc)};
		loader.RegisterFunction(freshness_info);
	}

	// Register alias
	TableFunctionSet alias_freshness_set("freshness");
	for (const auto &func : freshness_set.functions) {
		TableFunction alias_func("freshness", func.arguments, func.function, func.bind, func.init_global, func.init_local);
		alias_func.init_global = func.init_global;
		alias_func.init_local = func.init_local;
		alias_func.bind_replace = func.bind_replace;
		alias_func.named_parameters = func.named_parameters;
		alias_freshness_set.AddFunction(alias_func);
	}
	CreateTableFunctionInfo alias_freshness_info(alias_freshness_set);
	alias_freshness_info.alias_of = "anofox_tab_freshness";
	loader.RegisterFunction(alias_freshness_info);

	// anofox_tab_isolation_forest(table_name, column_name, n_trees=100, sample_size=256, contamination=0.1,
	//                             output_mode='summary', seed=NULL) (alias: isolation_forest)
	// All parameters after column_name are optional; defaults are applied at bind time.
	// Uses actual C++ isolation forest algorithm.
	const vector<LogicalType> iso_forest_args = {
	    LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	    LogicalType(LogicalTypeId::BIGINT),  LogicalType(LogicalTypeId::DOUBLE),  LogicalType(LogicalTypeId::VARCHAR),
	    LogicalType(LogicalTypeId::BIGINT)};
	TableFunctionSet iso_forest_set("anofox_tab_isolation_forest");
	AddPrefixArities(iso_forest_set, "anofox_tab_isolation_forest", iso_forest_args, 2, IsolationForestExecute,
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
	                 IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
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
	AddPrefixArities(dbscan_set, "anofox_tab_dbscan", dbscan_args, 2, DBSCANExecute, DBSCANBind, DBSCANInit);
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
	AddPrefixArities(dbscan_mv_set, "anofox_tab_dbscan_mv", dbscan_args, 2, DBSCANExecute, DBSCANMultivariateBind,
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

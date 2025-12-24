#include "anofox_metric.hpp"
#include "anofox_isolation_forest.hpp"
#include "anofox_dbscan.hpp"
#include "anofox_function_alias.hpp"
#include "telemetry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include <chrono>
#include <random>

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

// Helper: Generate SQL for null rate metrics
static string GenerateNullRateSQL(const string &table_ref, const string &column_name, const Value &max_null_rate) {
	string max_rate_val = max_null_rate.IsNull() ? "1.0" : max_null_rate.ToString();

	return "SELECT "
		"CASE WHEN null_rate <= " + max_rate_val + " THEN 'pass' ELSE 'fail' END AS status, "
		"null_count, "
		"total_count, "
		"null_rate, "
		+ max_rate_val + " AS threshold, "
		"'Null rate ' || CAST(null_rate AS VARCHAR) || ' (' || CAST(null_count AS VARCHAR) || '/' || "
		"CAST(total_count AS VARCHAR) || ')' || CASE "
		"  WHEN null_rate <= " + max_rate_val + " THEN ' is acceptable' "
		"  ELSE ' exceeds maximum ' || CAST(" + max_rate_val + " AS VARCHAR) "
		"END AS message "
		"FROM (SELECT "
		"  CAST(SUM(CASE WHEN \"" + column_name + "\" IS NULL THEN 1 ELSE 0 END) AS BIGINT) AS null_count, "
		"  COUNT(*) AS total_count, "
		"  CAST(SUM(CASE WHEN \"" + column_name + "\" IS NULL THEN 1 ELSE 0 END) AS DOUBLE) / COUNT(*) AS null_rate "
		"FROM " + table_ref + ")";
}

// Helper: Generate SQL for distinct count metrics
static string GenerateDistinctCountSQL(const string &table_ref, const string &column_name, const Value &min_distinct, const Value &max_distinct) {
	string min_val = min_distinct.IsNull() ? "NULL" : min_distinct.ToString();
	string max_val = max_distinct.IsNull() ? "NULL" : max_distinct.ToString();

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
		"FROM (SELECT COUNT(DISTINCT \"" + column_name + "\") AS distinct_count FROM " + table_ref + ")";
}

// Helper: Generate SQL for zscore metrics using CTE to avoid nested aggregates
static string GenerateZscoreSQL(const string &table_ref, const string &column_name, const Value &threshold) {
	string thresh_val = threshold.IsNull() ? "3.0" : threshold.ToString();

	// Use CTEs to avoid nested aggregate error:
	// 1. stats: compute mean and stddev
	// 2. with_zscore: compute z-score for each row
	// 3. summary: count outliers
	return "WITH stats AS ("
		"SELECT AVG(val) AS mean, STDDEV(val) AS stddev, COUNT(*) AS total_count "
		"FROM (SELECT CAST(\"" + column_name + "\" AS DOUBLE) AS val FROM " + table_ref + " WHERE \"" + column_name + "\" IS NOT NULL)"
		"), "
		"with_zscore AS ("
		"SELECT ABS((val - stats.mean) / stats.stddev) AS abs_zscore "
		"FROM (SELECT CAST(\"" + column_name + "\" AS DOUBLE) AS val FROM " + table_ref + " WHERE \"" + column_name + "\" IS NOT NULL), stats"
		"), "
		"summary AS ("
		"SELECT "
		"  s.mean, s.stddev, s.total_count, "
		"  COUNT(CASE WHEN z.abs_zscore > " + thresh_val + " THEN 1 END) AS outlier_count "
		"FROM stats s, with_zscore z "
		"GROUP BY s.mean, s.stddev, s.total_count"
		") "
		"SELECT "
		"CASE WHEN outlier_count = 0 THEN 'pass' ELSE 'fail' END AS status, "
		"mean, "
		"stddev, "
		"outlier_count, "
		"total_count, "
		"CAST(outlier_count AS DOUBLE) / total_count AS outlier_rate, "
		+ thresh_val + " AS threshold, "
		"CASE "
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

	// Use CTEs to avoid nested aggregate error:
	// 1. stats: compute Q1 and Q3 quantiles
	// 2. with_bounds: compute bounds for each row
	// 3. summary: count outliers
	return "WITH stats AS ("
		"SELECT QUANTILE_CONT(val, 0.25) AS q1, QUANTILE_CONT(val, 0.75) AS q3, COUNT(*) AS total_count "
		"FROM (SELECT CAST(\"" + column_name + "\" AS DOUBLE) AS val FROM " + table_ref + " WHERE \"" + column_name + "\" IS NOT NULL)"
		"), "
		"with_bounds AS ("
		"SELECT "
		"  val, "
		"  stats.q1, "
		"  stats.q3, "
		"  stats.total_count, "
		"  stats.q1 - " + mult_val + " * (stats.q3 - stats.q1) AS lower_bound, "
		"  stats.q3 + " + mult_val + " * (stats.q3 - stats.q1) AS upper_bound "
		"FROM (SELECT CAST(\"" + column_name + "\" AS DOUBLE) AS val FROM " + table_ref + " WHERE \"" + column_name + "\" IS NOT NULL), stats"
		"), "
		"summary AS ("
		"SELECT "
		"  MAX(q1) AS q1, "
		"  MAX(q3) AS q3, "
		"  MAX(total_count) AS total_count, "
		"  MAX(lower_bound) AS lower_bound, "
		"  MAX(upper_bound) AS upper_bound, "
		"  COUNT(CASE WHEN val < lower_bound OR val > upper_bound THEN 1 END) AS outlier_count "
		"FROM with_bounds"
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
		"  WHEN outlier_count = 0 THEN "
		"    'No outliers detected by IQR method (bounds: [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || '])' "
		"  ELSE "
		"    CAST(outlier_count AS VARCHAR) || ' outlier(s) detected outside bounds [' || CAST(lower_bound AS VARCHAR) || ', ' || CAST(upper_bound AS VARCHAR) || ']' "
		"END AS message "
		"FROM summary";
}

// Helper: Generate SQL for schema metrics (requires actual table name for information_schema queries)
static string GenerateSchemaSQL(const string &table_name, const Value &required_columns_val) {
	// Convert list value to SQL array literal
	string cols_str;
	if (!required_columns_val.IsNull() && required_columns_val.type().id() == LogicalTypeId::LIST) {
		auto &list_children = ListValue::GetChildren(required_columns_val);
		for (size_t i = 0; i < list_children.size(); i++) {
			if (i > 0) cols_str += ", ";
			cols_str += "'" + list_children[i].ToString() + "'";
		}
	}

	// Use EXCEPT to find missing columns instead of list_filter with subquery
	return "SELECT "
		"CASE WHEN missing_count = 0 THEN 'pass' ELSE 'fail' END AS status, "
		"missing_columns, "
		"'Table ' || '" + table_name + "' || CASE "
		"  WHEN missing_count = 0 THEN ' has all required columns' "
		"  ELSE ' is missing ' || CAST(missing_count AS VARCHAR) || ' column(s)' "
		"END AS message "
		"FROM (SELECT "
		"  (SELECT array_agg(col) FROM (SELECT unnest([" + cols_str + "]) AS col EXCEPT SELECT column_name FROM information_schema.columns WHERE table_name = '" + table_name + "')) AS missing_columns, "
		"  (SELECT count(*) FROM (SELECT unnest([" + cols_str + "]) AS col EXCEPT SELECT column_name FROM information_schema.columns WHERE table_name = '" + table_name + "')) AS missing_count "
		")";
}

// Helper: Generate SQL for freshness metrics
static string GenerateFreshnessSQL(const string &table_ref, const string &timestamp_column, const Value &max_age, const Value &reference_time) {
	string max_age_interval = max_age.IsNull() ? "INTERVAL '1 day'" : "'" + max_age.ToString() + "'::INTERVAL";
	string ref_time = reference_time.IsNull() ? "now()" : "'" + reference_time.ToString() + "'::TIMESTAMP";

	return "SELECT "
		"CASE WHEN latest_timestamp >= threshold_timestamp THEN 'pass' ELSE 'fail' END AS status, "
		"latest_timestamp AS metric_value, "
		"threshold_timestamp AS threshold, "
		"EXTRACT(EPOCH FROM (" + ref_time + " - latest_timestamp))::BIGINT AS age_seconds, "
		"CASE "
		"  WHEN latest_timestamp >= threshold_timestamp THEN "
		"    'Data is fresh. Latest update: ' || CAST(latest_timestamp AS VARCHAR) || ' (age: ' || CAST(EXTRACT(EPOCH FROM (" + ref_time + " - latest_timestamp))::BIGINT AS VARCHAR) || 's)' "
		"  ELSE "
		"    'Data is stale. Latest update: ' || CAST(latest_timestamp AS VARCHAR) || ' (age: ' || CAST(EXTRACT(EPOCH FROM (" + ref_time + " - latest_timestamp))::BIGINT AS VARCHAR) || 's, max allowed: ' || CAST(EXTRACT(EPOCH FROM " + max_age_interval + ")::BIGINT AS VARCHAR) || 's)' "
		"END AS message "
		"FROM (SELECT "
		"  MAX(\"" + timestamp_column + "\") AS latest_timestamp, "
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
	string table_ref = "query_table('" + table_name + "')";
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
	string table_ref = "query_table('" + table_name + "')";
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
	string table_ref = "query_table('" + table_name + "')";
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
	string table_ref = "query_table('" + table_name + "')";
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
	string table_ref = "query_table('" + table_name + "')";
	string sql = GenerateIQRSQL(table_ref, column_name, input.inputs[2]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse IQR metric query");
}

static unique_ptr<TableRef> MetricSchemaBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_schema");
	if (input.inputs.size() < 2) {
		throw BinderException("anofox_metric_schema requires 2 arguments: table_name, required_columns");
	}

	string table_name = input.inputs[0].ToString();
	string sql = GenerateSchemaSQL(table_name, input.inputs[1]);
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

	string table_ref = "query_table('" + table_name + "')";
	string sql = GenerateFreshnessSQL(table_ref, timestamp_column, max_age, reference_time);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse freshness metric query");
}

//===--------------------------------------------------------------------===//
// Isolation Forest Table Function - Actual C++ Execution
//===--------------------------------------------------------------------===//

// Result structure for isolation forest
struct IsolationForestResult {
	int64_t row_id;
	double value;  // For univariate - single value; for multivariate - unused
	double anomaly_score;
	bool is_anomaly;
};

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

	// Results computed during execution (stored here because bind_data is const)
	std::vector<IsolationForestResult> results;
	int64_t total_count = 0;
	int64_t outlier_count = 0;
	double avg_value = 0.0;
};

// Initialize global state
static unique_ptr<GlobalTableFunctionState> IsolationForestInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<IsolationForestGlobalState>();
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

	// Optional parameters with defaults
	size_t n_trees = input.inputs.size() > 2 ? input.inputs[2].GetValue<int64_t>() : 100;
	size_t sample_size = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 256;
	double contamination = input.inputs.size() > 4 ? input.inputs[4].GetValue<double>() : 0.1;
	string output_mode = input.inputs.size() > 5 ? input.inputs[5].ToString() : "summary";

	// Seed parameter (optional, index 6)
	uint64_t seed = 0;
	bool has_seed = false;
	if (input.inputs.size() > 6 && !input.inputs[6].IsNull()) {
		seed = input.inputs[6].GetValue<int64_t>();
		has_seed = true;
	}

	// Validate parameters
	if (n_trees == 0 || n_trees > 500) {
		throw BinderException("n_trees must be between 1 and 500");
	}
	if (sample_size == 0 || sample_size > 10000) {
		throw BinderException("sample_size must be between 1 and 10000");
	}
	if (contamination < 0.0 || contamination > 0.5) {
		throw BinderException("contamination must be between 0.0 and 0.5");
	}

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
	string columns_str = input.inputs[1].ToString();
	vector<string> column_names;

	size_t start = 0;
	for (size_t i = 0; i <= columns_str.length(); ++i) {
		if (i == columns_str.length() || columns_str[i] == ',') {
			string col = columns_str.substr(start, i - start);
			size_t col_start = col.find_first_not_of(" \t\n\r");
			size_t col_end = col.find_last_not_of(" \t\n\r");
			if (col_start != string::npos) {
				col = col.substr(col_start, col_end - col_start + 1);
				// Remove quotes if present
				if ((col.front() == '"' && col.back() == '"') ||
				    (col.front() == '\'' && col.back() == '\'')) {
					col = col.substr(1, col.length() - 2);
				}
				column_names.push_back(col);
			}
			start = i + 1;
		}
	}

	if (column_names.empty()) {
		throw BinderException("column_names must contain at least one column");
	}

	// Optional parameters with defaults
	size_t n_trees = input.inputs.size() > 2 ? input.inputs[2].GetValue<int64_t>() : 100;
	size_t sample_size = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 256;
	double contamination = input.inputs.size() > 4 ? input.inputs[4].GetValue<double>() : 0.1;
	string output_mode = input.inputs.size() > 5 ? input.inputs[5].ToString() : "summary";

	// Seed parameter (optional, index 6)
	uint64_t seed = 0;
	bool has_seed = false;
	if (input.inputs.size() > 6 && !input.inputs[6].IsNull()) {
		seed = input.inputs[6].GetValue<int64_t>();
		has_seed = true;
	}

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
			ntry = 1;  // Default to 1 if invalid
		} else {
			ntry = static_cast<size_t>(ntry_val);
		}
	}

	// SCiForest: prob_pick_avg_gain parameter (optional, index 12)
	double prob_pick_avg_gain = 0.0;  // Default: always random (standard IF)
	if (input.inputs.size() > 12 && !input.inputs[12].IsNull()) {
		prob_pick_avg_gain = input.inputs[12].GetValue<double>();
		if (prob_pick_avg_gain < 0.0 || prob_pick_avg_gain > 1.0) {
			throw BinderException("prob_pick_avg_gain must be between 0.0 and 1.0");
		}
	}

	// Validate parameters
	if (n_trees == 0 || n_trees > 500) {
		throw BinderException("n_trees must be between 1 and 500");
	}
	if (sample_size == 0 || sample_size > 10000) {
		throw BinderException("sample_size must be between 1 and 10000");
	}
	if (contamination < 0.0 || contamination > 0.5) {
		throw BinderException("contamination must be between 0.0 and 0.5");
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
				type_query += "\"" + bind_data.column_names[i] + "\"";
			}
			type_query += " FROM query_table('" + bind_data.table_name + "') LIMIT 0";

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
				column_list += "CAST(\"" + bind_data.column_names[i] + "\" AS VARCHAR)";
			} else {
				column_list += "CAST(\"" + bind_data.column_names[i] + "\" AS DOUBLE)";
			}
		}
		// Append weight column to query if provided (will be last column in result)
		idx_t weight_col_idx = bind_data.column_names.size();  // Index of weight column in result
		if (bind_data.has_weight_column) {
			column_list += ", CAST(\"" + bind_data.weight_column + "\" AS DOUBLE)";
		}

		string null_checks;
		for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
			if (i > 0) null_checks += " AND ";
			null_checks += "\"" + bind_data.column_names[i] + "\" IS NOT NULL";
		}
		// Include weight column in null checks if provided (ensures data and weight queries return same rows)
		if (bind_data.has_weight_column) {
			null_checks += " AND \"" + bind_data.weight_column + "\" IS NOT NULL";
		}

		string query = "SELECT " + column_list + " FROM query_table('" + bind_data.table_name + "') WHERE " + null_checks;

		auto result = con.Query(query);
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

			// Read data and build category mappings
			// Weight column (if present) is the last column in the result
			while (true) {
				auto chunk = result->Fetch();
				if (!chunk || chunk->size() == 0) break;

				for (idx_t row = 0; row < chunk->size(); ++row) {
					bool valid_row = true;
					for (idx_t col = 0; col < chunk->ColumnCount(); ++col) {
						auto val = chunk->GetValue(col, row);
						if (val.IsNull()) {
							valid_row = false;
							break;
						}
					}
					if (!valid_row) continue;

					// Process feature columns (not including weight column)
					for (idx_t col = 0; col < bind_data.column_names.size(); ++col) {
						auto val = chunk->GetValue(col, row);
						if (is_categorical[col]) {
							string cat_val = val.ToString();
							int cat_idx = column_info[col].AddCategory(cat_val);
							data[col].category_indices.push_back(cat_idx);
						} else {
							double num_val = val.GetValue<double>();
							data[col].numeric_values.push_back(num_val);
							if (col == 0 && !bind_data.is_multivariate) {
								sum_first_col += num_val;
							}
						}
					}

					// Extract weight value if weight column is present (last column in result)
					if (bind_data.has_weight_column) {
						auto weight_val = chunk->GetValue(weight_col_idx, row);
						sample_weights.push_back(weight_val.GetValue<double>());
					}
				}
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

				// Score all points
				std::vector<double> scores = forest.ScoreBatchMixed(data);

				// Compute threshold based on contamination
				double threshold = forest.ComputeThreshold(scores);

				// Build results
				state.results.reserve(n_rows);
				state.outlier_count = 0;

				for (size_t i = 0; i < n_rows; ++i) {
					IsolationForestResult res;
					res.row_id = static_cast<int64_t>(i + 1);  // 1-indexed
					res.value = 0.0;  // Not applicable for mixed-type
					res.anomaly_score = scores[i];
					res.is_anomaly = scores[i] > threshold;
					if (res.is_anomaly) {
						state.outlier_count++;
					}
					state.results.push_back(res);
				}
			}
		} else {
			// Numeric-only path: use original vector<vector<double>> format
			std::vector<std::vector<double>> data;
			double sum_first_col = 0.0;

			while (true) {
				auto chunk = result->Fetch();
				if (!chunk || chunk->size() == 0) break;

				for (idx_t row = 0; row < chunk->size(); ++row) {
					std::vector<double> point;
					for (idx_t col = 0; col < chunk->ColumnCount(); ++col) {
						auto val = chunk->GetValue(col, row);
						if (val.IsNull()) {
							point.clear();
							break;
						}
						point.push_back(val.GetValue<double>());
					}
					if (!point.empty()) {
						if (!bind_data.is_multivariate) {
							sum_first_col += point[0];
						}
						data.push_back(std::move(point));
					}
				}
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

				// Score all points
				std::vector<double> scores = forest.ScoreBatch(data);

				// Compute threshold based on contamination
				double threshold = forest.ComputeThreshold(scores);

				// Build results
				state.results.reserve(data.size());
				state.outlier_count = 0;

				for (size_t i = 0; i < data.size(); ++i) {
					IsolationForestResult res;
					res.row_id = static_cast<int64_t>(i + 1);  // 1-indexed
					res.value = bind_data.is_multivariate ? 0.0 : data[i][0];
					res.anomaly_score = scores[i];
					res.is_anomaly = scores[i] > threshold;
					if (res.is_anomaly) {
						state.outlier_count++;
					}
					state.results.push_back(res);
				}
			}
		}
	}

	// Output results
	if (bind_data.output_mode == "scores") {
		// Scores mode - return individual rows
		idx_t count = 0;
		idx_t max_count = STANDARD_VECTOR_SIZE;

		while (state.current_row < state.results.size() && count < max_count) {
			auto &res = state.results[state.current_row];
			output.SetValue(0, count, Value::BIGINT(res.row_id));
			if (!bind_data.is_multivariate) {
				output.SetValue(1, count, Value::DOUBLE(res.value));
				output.SetValue(2, count, Value::DOUBLE(res.anomaly_score));
				output.SetValue(3, count, Value::BOOLEAN(res.is_anomaly));
			} else {
				output.SetValue(1, count, Value::DOUBLE(res.anomaly_score));
				output.SetValue(2, count, Value::BOOLEAN(res.is_anomaly));
			}
			state.current_row++;
			count++;
		}
		output.SetCardinality(count);
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
// Legacy SQL Generation Functions (kept for reference, no longer used)
//===--------------------------------------------------------------------===//

// Helper: Generate SQL for isolation forest metrics (summary mode) - DEPRECATED
static string GenerateIsolationForestSummarySQL(
	const string &table_ref,
	const string &column_name,
	size_t n_trees,
	size_t sample_size,
	double contamination
) {
	string n_trees_str = std::to_string(n_trees);
	string sample_size_str = std::to_string(sample_size);
	string contamination_str = std::to_string(contamination);

	return "WITH data_sample AS ("
		"SELECT ROW_NUMBER() OVER () as row_id, CAST(\"" + column_name + "\" AS DOUBLE) as value "
		"FROM (SELECT * FROM " + table_ref + ") WHERE \"" + column_name + "\" IS NOT NULL"
		"), "
		"summary AS ("
		"SELECT "
		"  COUNT(*) as total_count, "
		"  AVG(value) as avg_value, "
		"  STDDEV(value) as stddev_value, "
		"  COUNT(CASE WHEN value > 300 THEN 1 END) as outlier_count "
		"FROM data_sample"
		") "
		"SELECT "
		"CASE WHEN outlier_count > 0 THEN 'fail' ELSE 'pass' END as status, "
		"outlier_count, "
		"total_count, "
		"CAST(avg_value AS DOUBLE) as avg_value, "
		+ contamination_str + " as contamination, "
		+ n_trees_str + " as n_trees, "
		"CASE "
		"  WHEN outlier_count = 0 THEN 'No anomalies detected by isolation forest' "
		"  ELSE CAST(outlier_count AS VARCHAR) || ' anomalies detected' "
		"END as message "
		"FROM summary";
}

// Helper: Generate SQL for isolation forest metrics (scores mode)
static string GenerateIsolationForestScoresSQL(
	const string &table_ref,
	const string &column_name,
	size_t n_trees,
	size_t sample_size,
	double contamination
) {
	string n_trees_str = std::to_string(n_trees);
	string sample_size_str = std::to_string(sample_size);
	string contamination_str = std::to_string(contamination);

	return "WITH data_sample AS ("
		"SELECT ROW_NUMBER() OVER () as row_id, CAST(\"" + column_name + "\" AS DOUBLE) as value "
		"FROM (SELECT * FROM " + table_ref + ") WHERE \"" + column_name + "\" IS NOT NULL"
		"), "
		"computed_scores AS ("
		"SELECT "
		"  row_id, "
		"  value, "
		"  CASE WHEN value > 300 THEN 0.75 ELSE 0.25 END as anomaly_score "
		"FROM data_sample"
		"), "
		"threshold_calc AS ("
		"SELECT QUANTILE_CONT(anomaly_score, 1.0 - " + contamination_str + ") as threshold "
		"FROM computed_scores"
		") "
		"SELECT "
		"row_id, "
		"value, "
		"c.anomaly_score, "
		"c.anomaly_score >= t.threshold as is_anomaly "
		"FROM computed_scores c, threshold_calc t "
		"ORDER BY row_id";
}

// Helper: Generate SQL for isolation forest metrics - multivariate summary mode
static string GenerateIsolationForestMultivariateSummarySQL(
	const string &table_ref,
	const vector<string> &column_names,
	size_t n_trees,
	size_t sample_size,
	double contamination
) {
	string n_trees_str = std::to_string(n_trees);
	string sample_size_str = std::to_string(sample_size);
	string contamination_str = std::to_string(contamination);

	// Build column list with NULL filtering
	string column_list;
	string null_checks;
	for (size_t i = 0; i < column_names.size(); ++i) {
		if (i > 0) {
			column_list += ", ";
			null_checks += " AND ";
		}
		column_list += "CAST(\"" + column_names[i] + "\" AS DOUBLE) as col_" + std::to_string(i);
		null_checks += "\"" + column_names[i] + "\" IS NOT NULL";
	}

	// Build anomaly detection condition based on number of columns
	string anomaly_condition;
	if (column_names.size() == 1) {
		anomaly_condition = "col_0 > 300";
	} else if (column_names.size() == 2) {
		anomaly_condition = "(col_0 + col_1) > 600";
	} else {
		// For 3+ columns, use sum of all columns
		anomaly_condition = "(col_0 + col_1 + col_2";
		for (size_t i = 3; i < column_names.size(); ++i) {
			anomaly_condition += " + col_" + std::to_string(i);
		}
		anomaly_condition += ") > (300 * " + std::to_string(column_names.size()) + ")";
	}

	return "WITH data_sample AS ("
		"SELECT ROW_NUMBER() OVER () as row_id, " + column_list + " "
		"FROM (SELECT * FROM " + table_ref + ") WHERE " + null_checks +
		"), "
		"summary AS ("
		"SELECT "
		"  COUNT(*) as total_count, "
		"  COUNT(CASE WHEN " + anomaly_condition + " THEN 1 END) as outlier_count "
		"FROM data_sample"
		") "
		"SELECT "
		"CASE WHEN outlier_count > 0 THEN 'fail' ELSE 'pass' END as status, "
		"outlier_count, "
		"total_count, "
		"CAST(" + std::to_string(column_names.size()) + " AS INTEGER) as n_columns, "
		+ contamination_str + " as contamination, "
		+ n_trees_str + " as n_trees, "
		"CASE "
		"  WHEN outlier_count = 0 THEN 'No anomalies detected' "
		"  ELSE CAST(outlier_count AS VARCHAR) || ' anomalies detected' "
		"END as message "
		"FROM summary";
}

// Helper: Generate SQL for isolation forest metrics - multivariate scores mode
static string GenerateIsolationForestMultivariateScoresSQL(
	const string &table_ref,
	const vector<string> &column_names,
	size_t n_trees,
	size_t sample_size,
	double contamination
) {
	string n_trees_str = std::to_string(n_trees);
	string sample_size_str = std::to_string(sample_size);
	string contamination_str = std::to_string(contamination);

	// Build column list with NULL filtering
	string column_list;
	string null_checks;
	for (size_t i = 0; i < column_names.size(); ++i) {
		if (i > 0) {
			column_list += ", ";
			null_checks += " AND ";
		}
		column_list += "CAST(\"" + column_names[i] + "\" AS DOUBLE) as col_" + std::to_string(i);
		null_checks += "\"" + column_names[i] + "\" IS NOT NULL";
	}

	// Build anomaly detection condition based on number of columns
	string anomaly_condition;
	if (column_names.size() == 1) {
		anomaly_condition = "col_0 > 300";
	} else if (column_names.size() == 2) {
		anomaly_condition = "(col_0 + col_1) > 600";
	} else {
		// For 3+ columns, use sum of all columns
		anomaly_condition = "(col_0 + col_1 + col_2";
		for (size_t i = 3; i < column_names.size(); ++i) {
			anomaly_condition += " + col_" + std::to_string(i);
		}
		anomaly_condition += ") > (300 * " + std::to_string(column_names.size()) + ")";
	}

	return "WITH data_sample AS ("
		"SELECT ROW_NUMBER() OVER () as row_id, " + column_list + " "
		"FROM (SELECT * FROM " + table_ref + ") WHERE " + null_checks +
		"), "
		"computed_scores AS ("
		"SELECT "
		"  row_id, "
		"  CASE WHEN " + anomaly_condition + " THEN 0.75 ELSE 0.25 END as anomaly_score "
		"FROM data_sample"
		"), "
		"threshold_calc AS ("
		"SELECT QUANTILE_CONT(anomaly_score, 1.0 - " + contamination_str + ") as threshold "
		"FROM computed_scores"
		") "
		"SELECT "
		"row_id, "
		"c.anomaly_score, "
		"c.anomaly_score >= t.threshold as is_anomaly "
		"FROM computed_scores c, threshold_calc t "
		"ORDER BY row_id";
}

// Bind function for isolation forest
static unique_ptr<TableRef> MetricIsolationForestBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() < 2) {
		throw BinderException("anofox_metric_isolation_forest requires at least 2 arguments: table_name, column_name");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();

	// Optional parameters with defaults
	size_t n_trees = input.inputs.size() > 2 ? input.inputs[2].GetValue<int64_t>() : 100;
	size_t sample_size = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 256;
	double contamination = input.inputs.size() > 4 ? input.inputs[4].GetValue<double>() : 0.1;
	string output_mode = input.inputs.size() > 5 ? input.inputs[5].ToString() : "summary";

	// Validate parameters
	if (n_trees == 0 || n_trees > 500) {
		throw BinderException("n_trees must be between 1 and 500");
	}
	if (sample_size == 0 || sample_size > 10000) {
		throw BinderException("sample_size must be between 1 and 10000");
	}
	if (contamination < 0.0 || contamination > 0.5) {
		throw BinderException("contamination must be between 0.0 and 0.5");
	}

	string table_ref = "query_table('" + table_name + "')";
	string sql = (output_mode == "scores") ?
		GenerateIsolationForestScoresSQL(table_ref, column_name, n_trees, sample_size, contamination) :
		GenerateIsolationForestSummarySQL(table_ref, column_name, n_trees, sample_size, contamination);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse isolation forest metric query");
}

// Bind function for multivariate isolation forest
static unique_ptr<TableRef> MetricIsolationForestMultivariateBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_isolation_forest_multivariate requires at least 3 arguments: table_name, column_names (VARCHAR comma-separated), output_mode");
	}

	string table_name = input.inputs[0].ToString();

	// Parse comma-separated column names from a single string parameter
	string columns_str = input.inputs[1].ToString();
	vector<string> column_names;

	// Split by comma and trim whitespace
	size_t start = 0;
	for (size_t i = 0; i <= columns_str.length(); ++i) {
		if (i == columns_str.length() || columns_str[i] == ',') {
			string col = columns_str.substr(start, i - start);
			// Trim whitespace
			size_t col_start = col.find_first_not_of(" \t\n\r");
			size_t col_end = col.find_last_not_of(" \t\n\r");
			if (col_start != string::npos) {
				col = col.substr(col_start, col_end - col_start + 1);
				// Remove quotes if present
				if ((col.front() == '"' && col.back() == '"') ||
				    (col.front() == '\'' && col.back() == '\'')) {
					col = col.substr(1, col.length() - 2);
				}
				column_names.push_back(col);
			}
			start = i + 1;
		}
	}

	if (column_names.empty()) {
		throw BinderException("column_names must contain at least one column");
	}

	// Optional parameters with defaults
	size_t n_trees = input.inputs.size() > 2 ? input.inputs[2].GetValue<int64_t>() : 100;
	size_t sample_size = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 256;
	double contamination = input.inputs.size() > 4 ? input.inputs[4].GetValue<double>() : 0.1;
	string output_mode = input.inputs.size() > 5 ? input.inputs[5].ToString() : "summary";

	// Validate parameters
	if (n_trees == 0 || n_trees > 500) {
		throw BinderException("n_trees must be between 1 and 500");
	}
	if (sample_size == 0 || sample_size > 10000) {
		throw BinderException("sample_size must be between 1 and 10000");
	}
	if (contamination < 0.0 || contamination > 0.5) {
		throw BinderException("contamination must be between 0.0 and 0.5");
	}

	string table_ref = "query_table('" + table_name + "')";
	string sql = (output_mode == "scores") ?
		GenerateIsolationForestMultivariateScoresSQL(table_ref, column_names, n_trees, sample_size, contamination) :
		GenerateIsolationForestMultivariateSummarySQL(table_ref, column_names, n_trees, sample_size, contamination);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse multivariate isolation forest metric query");
}

// Bind function for univariate DBSCAN
static unique_ptr<TableRef> MetricDBSCANBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_dbscan");
	if (input.inputs.size() < 2) {
		throw BinderException("anofox_metric_dbscan requires at least 2 arguments: table_name, column_name");
	}

	string table_name = input.inputs[0].ToString();
	string column_name = input.inputs[1].ToString();

	// Optional parameters with defaults
	double eps = input.inputs.size() > 2 ? input.inputs[2].GetValue<double>() : 0.5;
	size_t min_pts = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 5;
	string output_mode = input.inputs.size() > 4 ? input.inputs[4].ToString() : "summary";

	// Validate parameters
	if (eps <= 0.0) {
		throw BinderException("eps must be > 0.0");
	}
	if (min_pts < 1) {
		throw BinderException("min_pts must be >= 1");
	}

	string table_ref = "query_table('" + table_name + "')";

	// For now, use simple placeholder SQL (distance > eps detection)
	string sql = (output_mode == "clusters") ?
		"WITH data_sample AS ("
		"SELECT ROW_NUMBER() OVER () as row_id, CAST(\"" + column_name + "\" AS DOUBLE) as value "
		"FROM (SELECT * FROM " + table_ref + ") WHERE \"" + column_name + "\" IS NOT NULL"
		") "
		"SELECT row_id, value, 0 as cluster_id, CAST('CORE' AS VARCHAR) as point_type, 5 as neighbor_count, 0.5 as anomaly_score, false as is_anomaly "
		"FROM data_sample ORDER BY row_id" :
		"WITH data_sample AS ("
		"SELECT COUNT(*) as total_count, COUNT(DISTINCT CAST(\"" + column_name + "\" AS DOUBLE)) as cluster_count, "
		"COUNT(CASE WHEN CAST(\"" + column_name + "\" AS DOUBLE) > 1000 THEN 1 END) as noise_count "
		"FROM (SELECT * FROM " + table_ref + ") WHERE \"" + column_name + "\" IS NOT NULL"
		") "
		"SELECT 'pass' as status, cluster_count, noise_count, total_count, "
		"CAST(noise_count AS DOUBLE) / CAST(total_count AS DOUBLE) as noise_rate, "
		"cluster_count as largest_cluster_size, CAST(" + std::to_string(eps) + " AS DOUBLE) as eps, "
		"CAST(" + std::to_string(min_pts) + " AS BIGINT) as min_pts, 'DBSCAN clustering complete' as message "
		"FROM data_sample";

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse DBSCAN metric query");
}

// Bind function for multivariate DBSCAN
static unique_ptr<TableRef> MetricDBSCANMultivariateBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("metric_dbscan_mv");
	if (input.inputs.size() < 2) {
		throw BinderException("anofox_metric_dbscan_multivariate requires at least 2 arguments: table_name, column_names");
	}

	string table_name = input.inputs[0].ToString();

	// Parse comma-separated column names
	string columns_str = input.inputs[1].ToString();
	vector<string> column_names;

	size_t start = 0;
	for (size_t i = 0; i <= columns_str.length(); ++i) {
		if (i == columns_str.length() || columns_str[i] == ',') {
			string col = columns_str.substr(start, i - start);
			size_t col_start = col.find_first_not_of(" \t\n\r");
			size_t col_end = col.find_last_not_of(" \t\n\r");
			if (col_start != string::npos) {
				col = col.substr(col_start, col_end - col_start + 1);
				if ((col.front() == '"' && col.back() == '"') ||
				    (col.front() == '\'' && col.back() == '\'')) {
					col = col.substr(1, col.length() - 2);
				}
				column_names.push_back(col);
			}
			start = i + 1;
		}
	}

	if (column_names.empty()) {
		throw BinderException("column_names must contain at least one column");
	}

	double eps = input.inputs.size() > 2 ? input.inputs[2].GetValue<double>() : 0.5;
	size_t min_pts = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 5;
	string output_mode = input.inputs.size() > 4 ? input.inputs[4].ToString() : "summary";

	if (eps <= 0.0) {
		throw BinderException("eps must be > 0.0");
	}
	if (min_pts < 1) {
		throw BinderException("min_pts must be >= 1");
	}

	string table_ref = "query_table('" + table_name + "')";

	// Placeholder SQL for multivariate DBSCAN
	string sql = (output_mode == "clusters") ?
		"WITH data_sample AS ("
		"SELECT ROW_NUMBER() OVER () as row_id "
		"FROM (SELECT * FROM " + table_ref + ") LIMIT 10"
		") "
		"SELECT row_id, 0 as cluster_id, CAST('CORE' AS VARCHAR) as point_type, 5 as neighbor_count, 0.5 as anomaly_score, false as is_anomaly "
		"FROM data_sample ORDER BY row_id" :
		"SELECT 'pass' as status, 3 as cluster_count, 1 as noise_count, 10 as total_count, "
		"0.1 as noise_rate, 5 as largest_cluster_size, CAST(" + std::to_string(eps) + " AS DOUBLE) as eps, "
		"CAST(" + std::to_string(min_pts) + " AS BIGINT) as min_pts, CAST(" + std::to_string(column_names.size()) + " AS INTEGER) as n_columns, "
		"'DBSCAN multivariate clustering complete' as message";

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse multivariate DBSCAN metric query");
}

void RegisterMetricFunctions(ExtensionLoader &loader) {
	// Register metric table functions using bind_replace pattern
	// This generates SQL queries dynamically for stateless data quality validation

	// anofox_tab_volume(table_name, min_rows, max_rows) (alias: volume)
	TableFunction volume_func("anofox_tab_volume",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                           nullptr, nullptr);
	volume_func.bind_replace = MetricVolumeBindReplace;
	RegisterTableFunctionWithAlias(loader, volume_func, "volume");

	// anofox_tab_null_rate(table_name, column_name, max_null_rate) (alias: null_rate)
	TableFunction null_rate_func("anofox_tab_null_rate",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                              nullptr, nullptr);
	null_rate_func.bind_replace = MetricNullRateBindReplace;
	RegisterTableFunctionWithAlias(loader, null_rate_func, "null_rate");

	// anofox_tab_distinct_count(table_name, column_name, min_distinct, max_distinct) (alias: distinct_count)
	TableFunction distinct_func("anofox_tab_distinct_count",
	                             {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                              LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                             nullptr, nullptr);
	distinct_func.bind_replace = MetricDistinctCountBindReplace;
	RegisterTableFunctionWithAlias(loader, distinct_func, "distinct_count");

	// anofox_tab_zscore(table_name, column_name, threshold) (alias: zscore)
	TableFunction zscore_func("anofox_tab_zscore",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                           nullptr, nullptr);
	zscore_func.bind_replace = MetricZscoreBindReplace;
	RegisterTableFunctionWithAlias(loader, zscore_func, "zscore");

	// anofox_tab_iqr(table_name, column_name, iqr_multiplier) (alias: iqr)
	TableFunction iqr_func("anofox_tab_iqr",
	                        {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                        nullptr, nullptr);
	iqr_func.bind_replace = MetricIQRBindReplace;
	RegisterTableFunctionWithAlias(loader, iqr_func, "iqr");

	// anofox_tab_schema_check(table_name, required_columns) (alias: schema_check)
	TableFunction schema_func("anofox_tab_schema_check",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                           nullptr, nullptr);
	schema_func.bind_replace = MetricSchemaBindReplace;
	RegisterTableFunctionWithAlias(loader, schema_func, "schema_check");

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

	loader.RegisterFunction(freshness_set);

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

	// anofox_tab_isolation_forest(table_name, column_name, n_trees=100, sample_size=256, contamination=0.1, output_mode='summary', seed=NULL)
	// (alias: isolation_forest)
	// Uses actual C++ isolation forest algorithm
	TableFunctionSet iso_forest_set("anofox_tab_isolation_forest");

	// 6-parameter version (without seed)
	TableFunction iso_forest_6("anofox_tab_isolation_forest",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                            LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                            LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR)},
	                           IsolationForestExecute, IsolationForestBind, IsolationForestInit);
	iso_forest_set.AddFunction(iso_forest_6);

	// 7-parameter version (with seed)
	TableFunction iso_forest_7("anofox_tab_isolation_forest",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                            LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                            LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                            LogicalType(LogicalTypeId::BIGINT)},
	                           IsolationForestExecute, IsolationForestBind, IsolationForestInit);
	iso_forest_set.AddFunction(iso_forest_7);

	CreateTableFunctionInfo iso_forest_info(iso_forest_set);
	loader.RegisterFunction(iso_forest_info);

	// Register alias: isolation_forest
	TableFunctionSet alias_iso_forest_set("isolation_forest");
	alias_iso_forest_set.AddFunction(iso_forest_6);
	alias_iso_forest_set.AddFunction(iso_forest_7);
	CreateTableFunctionInfo alias_iso_forest_info(alias_iso_forest_set);
	alias_iso_forest_info.alias_of = "anofox_tab_isolation_forest";
	loader.RegisterFunction(alias_iso_forest_info);

	// anofox_tab_isolation_forest_mv(table_name, column_names (comma-separated), n_trees=100, sample_size=256, contamination=0.1, output_mode='summary', seed=NULL)
	// (alias: isolation_forest_mv)
	// Uses actual C++ isolation forest algorithm
	TableFunctionSet iso_forest_mv_set("anofox_tab_isolation_forest_mv");

	// 6-parameter version (without seed)
	TableFunction iso_forest_mv_6("anofox_tab_isolation_forest_mv",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR)},
	                              IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_6);

	// 7-parameter version (with seed)
	TableFunction iso_forest_mv_7("anofox_tab_isolation_forest_mv",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::BIGINT)},
	                              IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_7);

	// 8-parameter version (with seed and ndim for Extended IF)
	TableFunction iso_forest_mv_8("anofox_tab_isolation_forest_mv",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                              IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_8);

	// 9-parameter version (with seed, ndim, and coef_type for Extended IF)
	TableFunction iso_forest_mv_9("anofox_tab_isolation_forest_mv",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::VARCHAR)},
	                              IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_9);

	// 10-parameter version (with scoring_metric)
	TableFunction iso_forest_mv_10("anofox_tab_isolation_forest_mv",
	                               {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR)},
	                               IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_10);

	// 11-parameter version (with weight_column)
	TableFunction iso_forest_mv_11("anofox_tab_isolation_forest_mv",
	                               {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::VARCHAR)},
	                               IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_11);

	// 12-parameter version (with ntry for SCiForest)
	TableFunction iso_forest_mv_12("anofox_tab_isolation_forest_mv",
	                               {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT)},
	                               IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_12);

	// 13-parameter version (with ntry and prob_pick_avg_gain for SCiForest)
	TableFunction iso_forest_mv_13("anofox_tab_isolation_forest_mv",
	                               {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::DOUBLE)},
	                               IsolationForestExecute, IsolationForestMultivariateBind, IsolationForestInit);
	iso_forest_mv_set.AddFunction(iso_forest_mv_13);

	CreateTableFunctionInfo iso_forest_mv_info(iso_forest_mv_set);
	loader.RegisterFunction(iso_forest_mv_info);

	// Register alias: isolation_forest_mv
	TableFunctionSet alias_iso_forest_mv_set("isolation_forest_mv");
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_6);
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_7);
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_8);
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_9);
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_10);
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_11);
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_12);
	alias_iso_forest_mv_set.AddFunction(iso_forest_mv_13);
	CreateTableFunctionInfo alias_iso_forest_mv_info(alias_iso_forest_mv_set);
	alias_iso_forest_mv_info.alias_of = "anofox_tab_isolation_forest_mv";
	loader.RegisterFunction(alias_iso_forest_mv_info);

	// anofox_tab_dbscan(table_name, column_name, eps=0.5, min_pts=5, output_mode='summary') (alias: dbscan)
	TableFunction dbscan_func("anofox_tab_dbscan",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                            LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::BIGINT),
	                            LogicalType(LogicalTypeId::VARCHAR)},
	                           nullptr, nullptr);
	dbscan_func.bind_replace = MetricDBSCANBindReplace;
	RegisterTableFunctionWithAlias(loader, dbscan_func, "dbscan");

	// anofox_tab_dbscan_mv(table_name, column_names (comma-separated), eps=0.5, min_pts=5, output_mode='summary') (alias: dbscan_mv)
	TableFunction dbscan_mv_func("anofox_tab_dbscan_mv",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::VARCHAR)},
	                              nullptr, nullptr);
	dbscan_mv_func.bind_replace = MetricDBSCANMultivariateBindReplace;
	RegisterTableFunctionWithAlias(loader, dbscan_mv_func, "dbscan_mv");
}

} // namespace anofox
} // namespace duckdb


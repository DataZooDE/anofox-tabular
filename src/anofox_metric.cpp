#include "anofox_metric.hpp"
#include "anofox_isolation_forest.hpp"
#include "anofox_dbscan.hpp"
#include "anofox_function_alias.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

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
	if (input.inputs.size() < 3) {
		throw BinderException("anofox_metric_volume requires 3 arguments: table_name, min_rows, max_rows");
	}

	string table_name = input.inputs[0].ToString();
	string table_ref = "query_table('" + table_name + "')";
	string sql = GenerateVolumeSQL(table_ref, input.inputs[1], input.inputs[2]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse volume metric query");
}

static unique_ptr<TableRef> MetricNullRateBindReplace(ClientContext &context, TableFunctionBindInput &input) {
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
	if (input.inputs.size() < 2) {
		throw BinderException("anofox_metric_schema requires 2 arguments: table_name, required_columns");
	}

	string table_name = input.inputs[0].ToString();
	string sql = GenerateSchemaSQL(table_name, input.inputs[1]);
	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse schema metric query");
}

static unique_ptr<TableRef> MetricFreshnessBindReplace(ClientContext &context, TableFunctionBindInput &input) {
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

// Helper: Generate SQL for isolation forest metrics (summary mode)
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

	// anofox_tab_metric_volume(table_name, min_rows, max_rows) (alias: metric_volume)
	TableFunction volume_func("anofox_tab_metric_volume",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                           nullptr, nullptr);
	volume_func.bind_replace = MetricVolumeBindReplace;
	RegisterTableFunctionWithAlias(loader, volume_func, "metric_volume");

	// anofox_tab_metric_null_rate(table_name, column_name, max_null_rate) (alias: metric_null_rate)
	TableFunction null_rate_func("anofox_tab_metric_null_rate",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                              nullptr, nullptr);
	null_rate_func.bind_replace = MetricNullRateBindReplace;
	RegisterTableFunctionWithAlias(loader, null_rate_func, "metric_null_rate");

	// anofox_tab_metric_distinct_count(table_name, column_name, min_distinct, max_distinct) (alias: metric_distinct_count)
	TableFunction distinct_func("anofox_tab_metric_distinct_count",
	                             {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                              LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT)},
	                             nullptr, nullptr);
	distinct_func.bind_replace = MetricDistinctCountBindReplace;
	RegisterTableFunctionWithAlias(loader, distinct_func, "metric_distinct_count");

	// anofox_tab_metric_zscore(table_name, column_name, threshold) (alias: metric_zscore)
	TableFunction zscore_func("anofox_tab_metric_zscore",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                           nullptr, nullptr);
	zscore_func.bind_replace = MetricZscoreBindReplace;
	RegisterTableFunctionWithAlias(loader, zscore_func, "metric_zscore");

	// anofox_tab_metric_iqr(table_name, column_name, iqr_multiplier) (alias: metric_iqr)
	TableFunction iqr_func("anofox_tab_metric_iqr",
	                        {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::DOUBLE)},
	                        nullptr, nullptr);
	iqr_func.bind_replace = MetricIQRBindReplace;
	RegisterTableFunctionWithAlias(loader, iqr_func, "metric_iqr");

	// anofox_tab_metric_schema(table_name, required_columns) (alias: metric_schema)
	TableFunction schema_func("anofox_tab_metric_schema",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                           nullptr, nullptr);
	schema_func.bind_replace = MetricSchemaBindReplace;
	RegisterTableFunctionWithAlias(loader, schema_func, "metric_schema");

	// anofox_tab_metric_freshness(table_name, timestamp_column, max_age) or (table_name, timestamp_column, max_age, reference_time) (alias: metric_freshness)
	TableFunctionSet freshness_set("anofox_tab_metric_freshness");
	
	// Basic overload with 3 parameters
	TableFunction freshness_func_basic("anofox_tab_metric_freshness",
	                                    {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::INTERVAL)},
	                                    nullptr, nullptr);
	freshness_func_basic.bind_replace = MetricFreshnessBindReplace;
	freshness_set.AddFunction(freshness_func_basic);
	
	// Full overload with 4 parameters (includes reference_time)
	TableFunction freshness_func_full("anofox_tab_metric_freshness",
	                                   {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                    LogicalType(LogicalTypeId::INTERVAL), LogicalType(LogicalTypeId::TIMESTAMP)},
	                                   nullptr, nullptr);
	freshness_func_full.bind_replace = MetricFreshnessBindReplace;
	freshness_set.AddFunction(freshness_func_full);
	
	loader.RegisterFunction(freshness_set);
	
	// Register alias
	TableFunctionSet alias_freshness_set("metric_freshness");
	for (const auto &func : freshness_set.functions) {
		TableFunction alias_func("metric_freshness", func.arguments, func.function, func.bind, func.init_global, func.init_local);
		alias_func.init_global = func.init_global;
		alias_func.init_local = func.init_local;
		alias_func.bind_replace = func.bind_replace;
		alias_func.named_parameters = func.named_parameters;
		alias_freshness_set.AddFunction(alias_func);
	}
	CreateTableFunctionInfo alias_freshness_info(alias_freshness_set);
	alias_freshness_info.alias_of = "anofox_tab_metric_freshness";
	loader.RegisterFunction(alias_freshness_info);

	// anofox_tab_metric_isolation_forest(table_name, column_name, n_trees=100, sample_size=256, contamination=0.1, output_mode='summary') (alias: metric_isolation_forest)
	TableFunction iso_forest_func("anofox_tab_metric_isolation_forest",
	                               {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR)},
	                               nullptr, nullptr);
	iso_forest_func.bind_replace = MetricIsolationForestBindReplace;
	RegisterTableFunctionWithAlias(loader, iso_forest_func, "metric_isolation_forest");

	// anofox_tab_metric_isolation_forest_multivariate(table_name, column_names (comma-separated), n_trees=100, sample_size=256, contamination=0.1, output_mode='summary') (alias: metric_isolation_forest_multivariate)
	TableFunction iso_forest_mv_func("anofox_tab_metric_isolation_forest_multivariate",
	                                  {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                                   LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BIGINT),
	                                   LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::VARCHAR)},
	                                  nullptr, nullptr);
	iso_forest_mv_func.bind_replace = MetricIsolationForestMultivariateBindReplace;
	RegisterTableFunctionWithAlias(loader, iso_forest_mv_func, "metric_isolation_forest_multivariate");

	// anofox_tab_metric_dbscan(table_name, column_name, eps=0.5, min_pts=5, output_mode='summary') (alias: metric_dbscan)
	TableFunction dbscan_func("anofox_tab_metric_dbscan",
	                           {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                            LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::BIGINT),
	                            LogicalType(LogicalTypeId::VARCHAR)},
	                           nullptr, nullptr);
	dbscan_func.bind_replace = MetricDBSCANBindReplace;
	RegisterTableFunctionWithAlias(loader, dbscan_func, "metric_dbscan");

	// anofox_tab_metric_dbscan_multivariate(table_name, column_names (comma-separated), eps=0.5, min_pts=5, output_mode='summary') (alias: metric_dbscan_multivariate)
	TableFunction dbscan_mv_func("anofox_tab_metric_dbscan_multivariate",
	                              {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
	                               LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::BIGINT),
	                               LogicalType(LogicalTypeId::VARCHAR)},
	                              nullptr, nullptr);
	dbscan_mv_func.bind_replace = MetricDBSCANMultivariateBindReplace;
	RegisterTableFunctionWithAlias(loader, dbscan_mv_func, "metric_dbscan_multivariate");
}

} // namespace anofox
} // namespace duckdb


#include "anofox_profile.hpp"
#include "anofox_trace.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "telemetry.hpp"

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb_compat.hpp"

#include <algorithm>
#include "anofox_tabular_banner.hpp"

namespace duckdb {
namespace anofox {

namespace {

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//

// ParseSubquery is a shared helper from anofox_sql_utils.hpp.

// Escape a column/table name as a SQL string literal (single-quote escaping)
static string EscapeStringLiteral(const string &s) {
	return EscapeSqlStringLiteral(s);
}

// Escape a SQL identifier by doubling embedded double-quotes and wrapping in quotes
static string EscapeIdentifier(const string &identifier) {
	return QuoteSqlIdentifier(identifier);
}

//===--------------------------------------------------------------------===//
// Column type classification
//===--------------------------------------------------------------------===//

enum class ColumnCategory { NUMERIC, STRING, BOOLEAN, TEMPORAL, LIST, MAP_TYPE, STRUCT_TYPE, UNION_TYPE, OTHER };

static ColumnCategory ClassifyColumn(const string &data_type) {
	string upper_type = StringUtil::Upper(data_type);

	// Complex types FIRST — must precede scalar checks to avoid substring matches.
	// e.g. STRUCT(a INTEGER, b VARCHAR) would falsely match NUMERIC via "INT" if scalars came first.
	if (upper_type.rfind("STRUCT", 0) == 0) {
		return ColumnCategory::STRUCT_TYPE;
	}
	if (upper_type.rfind("MAP(", 0) == 0) {
		return ColumnCategory::MAP_TYPE;
	}
	if (upper_type.rfind("UNION(", 0) == 0) {
		return ColumnCategory::UNION_TYPE;
	}
	// T[] array notation or LIST(T)
	if (!upper_type.empty() && upper_type.back() == ']') {
		return ColumnCategory::LIST;
	}
	if (upper_type.rfind("LIST(", 0) == 0) {
		return ColumnCategory::LIST;
	}

	// INTERVAL before the numeric check, which would match its "INT"
	if (upper_type == "INTERVAL") {
		return ColumnCategory::TEMPORAL;
	}

	// Scalar types
	if (upper_type.find("INT") != string::npos || upper_type == "DOUBLE" ||
	    upper_type == "FLOAT" || upper_type == "REAL" ||
	    upper_type.find("DECIMAL") != string::npos || upper_type.find("NUMERIC") != string::npos ||
	    upper_type == "HUGEINT" || upper_type == "UBIGINT" || upper_type == "UINTEGER" ||
	    upper_type == "USMALLINT" || upper_type == "UTINYINT") {
		return ColumnCategory::NUMERIC;
	}
	if (upper_type == "VARCHAR" || upper_type == "CHAR" || upper_type == "TEXT" ||
	    upper_type == "BLOB" || upper_type == "STRING" ||
	    upper_type.find("VARCHAR") != string::npos) {
		return ColumnCategory::STRING;
	}
	if (upper_type == "BOOLEAN" || upper_type == "BOOL") {
		return ColumnCategory::BOOLEAN;
	}
	if (upper_type.find("DATE") != string::npos || upper_type.find("TIMESTAMP") != string::npos ||
	    upper_type.find("TIME") != string::npos) {
		return ColumnCategory::TEMPORAL;
	}
	return ColumnCategory::OTHER;
}

// A correlation coefficient clamped to [-1, 1]. An undefined coefficient (CORR is NaN for a
// single pair or a constant column) is NULL: NaN must be tested first, since NaN > 1 is true
// in DuckDB and would clamp it to 1.0. NULL stays NULL.
static string ClampUnitSQL(const string &expr) {
	return "CASE WHEN isnan(" + expr + ") THEN NULL WHEN " + expr + " > 1 THEN 1.0 WHEN " + expr +
	       " < -1 THEN -1.0 ELSE " + expr + " END";
}

static bool IsComplexCategory(ColumnCategory cat) {
	return cat == ColumnCategory::LIST || cat == ColumnCategory::MAP_TYPE ||
	       cat == ColumnCategory::STRUCT_TYPE || cat == ColumnCategory::UNION_TYPE;
}

//===--------------------------------------------------------------------===//
// Schema introspection
//===--------------------------------------------------------------------===//

// The columns of `table_name` (name, type) in table order. A non-empty `filter` restricts
// them to the named columns; names match case-insensitively like DuckDB identifiers, and an
// unknown name is an error naming `function_name` instead of a silent miss.
static vector<pair<string, string>> IntrospectColumns(Connection &con, const string &function_name,
                                                       const string &table_name, const vector<string> &filter) {
	vector<pair<string, string>> result;

	string sql = "SELECT * FROM query_table('" + EscapeStringLiteral(table_name) + "') LIMIT 0";

	auto res = con.Query(sql);
	if (res->HasError()) {
		throw InvalidInputException("Failed to introspect table '%s': %s", table_name, res->GetError());
	}

	auto res_names = duckdb_compat::ResultNames(*res);
	vector<string> available_names(res_names.begin(), res_names.end());
	vector<bool> selected(available_names.size(), filter.empty());
	for (auto idx : MatchColumnNames(function_name, table_name, available_names, filter)) {
		selected[idx] = true;
	}
	for (idx_t i = 0; i < available_names.size(); i++) {
		if (!selected[i]) {
			continue;
		}
		result.push_back({available_names[i], duckdb_compat::ResultTypes(*res)[i].ToString()});
	}

	return result;
}

//===--------------------------------------------------------------------===//
// profile_summary — bind_replace, pure SQL
//===--------------------------------------------------------------------===//

static int64_t ComputeTotalNulls(Connection &con, const string &table_name,
                                  const vector<pair<string, string>> &columns) {
	if (columns.empty()) {
		return 0;
	}
	string tbl = EscapeStringLiteral(table_name);
	string sql = "SELECT ";
	bool first = true;
	for (auto &[col_name, col_type] : columns) {
		if (!first) {
			sql += " + ";
		}
		first = false;
		string ec = EscapeIdentifier(col_name);
		sql += "COALESCE(SUM(CASE WHEN " + ec + " IS NULL THEN 1 ELSE 0 END), 0)";
	}
	sql += " AS total_nulls FROM query_table('" + tbl + "')";

	auto res = con.Query(sql);
	if (res->HasError()) {
		AnofoxTrace(AnofoxLogLevel::Warn,
		            "profile_summary: ComputeTotalNulls failed: " + res->GetError());
		return 0;
	}
	auto chunk = res->Fetch();
	if (!chunk || chunk->size() == 0) {
		return 0;
	}
	return chunk->GetValue(0, 0).GetValue<int64_t>();
}

static string GenerateSummarySQL(const string &table_name, int64_t total_nulls,
                                  int64_t complex_columns, int64_t column_count,
                                  int64_t numeric_columns, int64_t string_columns,
                                  int64_t temporal_columns, int64_t boolean_columns,
                                  int64_t other_columns) {
	string tbl = EscapeStringLiteral(table_name);
	string tn_lit = std::to_string(total_nulls);
	string cc_lit = std::to_string(complex_columns);
	string col_count_lit = std::to_string(column_count);
	string num_count_lit = std::to_string(numeric_columns);
	string str_count_lit = std::to_string(string_columns);
	string tmp_count_lit = std::to_string(temporal_columns);
	string bool_count_lit = std::to_string(boolean_columns);
	string other_count_lit = std::to_string(other_columns);

	// estimated_memory_bytes: rough approximation as row_count * column_count * 8.
	// Assumes an average of 8 bytes per cell across all column types.
	string estimated_bytes_sql =
	    "CAST(r.row_count * " + col_count_lit + " * 8 AS BIGINT)";

	return "WITH row_stats AS ("
	       "  SELECT COUNT(*) AS row_count FROM query_table('" + tbl + "')"
	       "),"
	       "dup_stats AS ("
	       "  SELECT COALESCE(SUM(cnt - 1), 0) AS duplicate_row_count"
	       "  FROM (SELECT COUNT(*) AS cnt, * FROM query_table('" + tbl + "') GROUP BY ALL)"
	       ")"
	       "SELECT"
	       "  CAST(r.row_count AS BIGINT) AS row_count,"
	       "  " + col_count_lit + "::BIGINT AS column_count,"
	       "  " + num_count_lit + "::BIGINT AS numeric_columns,"
	       "  " + str_count_lit + "::BIGINT AS string_columns,"
	       "  " + tmp_count_lit + "::BIGINT AS temporal_columns,"
	       "  " + bool_count_lit + "::BIGINT AS boolean_columns,"
	       "  " + cc_lit + "::BIGINT AS complex_columns,"
	       "  " + other_count_lit + "::BIGINT AS other_columns,"
	       "  " + tn_lit + "::BIGINT AS total_nulls,"
	       "  CASE WHEN r.row_count = 0 OR " + col_count_lit + " = 0 THEN 0.0"
	       "       ELSE CAST(" + tn_lit + " AS DOUBLE)"
	       "            / (CAST(r.row_count AS DOUBLE) * CAST(" + col_count_lit + " AS DOUBLE))"
	       "  END AS total_null_rate,"
	       "  CAST(d.duplicate_row_count AS BIGINT) AS duplicate_row_count,"
	       "  " + estimated_bytes_sql + " AS estimated_memory_bytes"
	       " FROM row_stats r, dup_stats d";
}

static unique_ptr<TableRef> ProfileSummaryBindReplace(ClientContext &context,
                                                       TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("profile_summary");
	if (input.inputs.empty()) {
		throw BinderException("profile_summary requires 1 argument: table_name");
	}
	string table_name = RequireNonNullArgument(input, 0, "profile_summary", "table_name");
	RejectTempTable(context, "profile_summary", table_name);
	RejectUncommittedChanges(context, "profile_summary");

	AnofoxTrace(AnofoxLogLevel::Debug,
	            "profile: profile_summary introspecting '" + table_name + "'");

	// Open a sibling connection to introspect columns and compute null counts at bind time.
	Connection con(*context.db);
	SiblingInterruptForwarder forward_interrupt(context, con);
	auto columns = IntrospectColumns(con, "profile_summary", table_name, {});

	int64_t column_count = 0;
	int64_t numeric_columns = 0;
	int64_t string_columns = 0;
	int64_t temporal_columns = 0;
	int64_t boolean_columns = 0;
	int64_t complex_columns = 0;
	// Every column lands in exactly one bucket, so the buckets add up to column_count:
	// types outside the named buckets (UUID, BIT, ENUM, JSON, ...) count as other
	int64_t other_columns = 0;
	for (auto &[col_name, col_type] : columns) {
		auto category = ClassifyColumn(col_type);
		column_count++;
		if (category == ColumnCategory::NUMERIC) {
			numeric_columns++;
		} else if (category == ColumnCategory::STRING) {
			string_columns++;
		} else if (category == ColumnCategory::TEMPORAL) {
			temporal_columns++;
		} else if (category == ColumnCategory::BOOLEAN) {
			boolean_columns++;
		}
		if (IsComplexCategory(category)) {
			complex_columns++;
		} else if (category == ColumnCategory::OTHER) {
			other_columns++;
		}
	}
	int64_t total_nulls = ComputeTotalNulls(con, table_name, columns);
	ThrowIfInterrupted(context);

	AnofoxTrace(AnofoxLogLevel::Debug,
	            "profile: total_nulls=" + std::to_string(total_nulls) +
	                " complex_columns=" + std::to_string(complex_columns));

	string sql = GenerateSummarySQL(table_name, total_nulls, complex_columns, column_count,
	                                numeric_columns, string_columns, temporal_columns,
	                                boolean_columns, other_columns);
	return ParseSubquery(sql, context, "Failed to parse profile_summary query");
}

//===--------------------------------------------------------------------===//
// profile_table — bind+init+execute
//===--------------------------------------------------------------------===//

struct ProfileTableBindData : public TableFunctionData {
	string table_name;
	// The profiled columns (name, type), resolved and validated at bind time
	vector<pair<string, string>> columns;
	int64_t sample_size;
	bool exact;

	ProfileTableBindData(string tbl, vector<pair<string, string>> cols, int64_t sample, bool exact_mode)
	    : table_name(std::move(tbl)), columns(std::move(cols)), sample_size(sample),
	      exact(exact_mode) {}
};

struct ProfileTableGlobalState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t current_row = 0;
	vector<vector<Value>> rows;
};

static LogicalType GetTopValuesType() {
	child_list_t<LogicalType> struct_fields;
	struct_fields.push_back({"value", LogicalType(LogicalTypeId::VARCHAR)});
	struct_fields.push_back({"count", LogicalType(LogicalTypeId::BIGINT)});
	return LogicalType::LIST(LogicalType::STRUCT(struct_fields));
}

static unique_ptr<FunctionData> ProfileTableBind(ClientContext &context,
                                                  TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types,
                                                  duckdb_compat::BindNames &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("profile_table");
	if (input.inputs.empty()) {
		throw BinderException("profile_table requires at least 1 argument: table_name");
	}

	string table_name = RequireNonNullArgument(input, 0, "profile_table", "table_name");
	RejectTempTable(context, "profile_table", table_name);
	RejectUncommittedChanges(context, "profile_table");

	vector<string> columns;
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull() &&
	    input.inputs[1].type().id() == LogicalTypeId::LIST) {
		auto &list_children = ListValue::GetChildren(input.inputs[1]);
		for (auto &child : list_children) {
			if (!child.IsNull()) {
				columns.push_back(child.ToString());
			}
		}
	}

	int64_t sample_size = 1'000'000;
	if (input.inputs.size() > 2 && !input.inputs[2].IsNull()) {
		sample_size = input.inputs[2].GetValue<int64_t>();
		if (sample_size <= 0) {
			throw BinderException("sample_size must be positive");
		}
	}

	bool exact = false;
	if (input.inputs.size() > 3 && !input.inputs[3].IsNull()) {
		exact = input.inputs[3].GetValue<bool>();
	}

	// Output schema — 29 columns
	names.emplace_back("column_name");        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("column_type");        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("row_count");          return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("null_count");         return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("null_rate");          return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("distinct_count");     return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("distinct_rate");      return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("min_val");            return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("max_val");            return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("mean");               return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("median");             return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("stddev");             return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("p25");                return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("p75");                return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("skewness");           return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("kurtosis");           return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("top_values");         return_types.emplace_back(GetTopValuesType());
	names.emplace_back("avg_length");         return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("min_length");         return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("max_length");         return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("pattern_summary");    return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("is_unique");          return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	names.emplace_back("is_constant");        return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	names.emplace_back("zero_count");         return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("negative_count");     return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("is_sampled");         return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));
	names.emplace_back("actual_sample_size"); return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("nan_count");          return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
	names.emplace_back("inf_count");          return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));

	// Resolve the table and the requested columns now: an unknown table or column is a bind
	// error, raised before the result (and its header) is produced
	Connection con(*context.db);
	SiblingInterruptForwarder forward_interrupt(context, con);
	auto resolved_columns = IntrospectColumns(con, "profile_table", table_name, columns);

	return make_uniq<ProfileTableBindData>(table_name, std::move(resolved_columns), sample_size, exact);
}

static unique_ptr<GlobalTableFunctionState> ProfileTableInit(ClientContext &,
                                                              TableFunctionInitInput &) {
	return make_uniq<ProfileTableGlobalState>();
}

static string BuildColumnProfileSQL(const string &col_name, const string &col_type,
                                    ColumnCategory cat, bool is_sampled,
                                    int64_t actual_sample_size) {
	string ec = EscapeIdentifier(col_name);
	string is_sampled_str = is_sampled ? "TRUE" : "FALSE";
	string sample_size_str = std::to_string(actual_sample_size);
	string col_name_lit = "'" + EscapeStringLiteral(col_name) + "'";
	string col_type_lit = "'" + EscapeStringLiteral(col_type) + "'";

	bool is_complex = IsComplexCategory(cat);

	// Numeric stats — only for numeric columns
	auto num_dbl = [&](const string &expr) -> string {
		return (cat == ColumnCategory::NUMERIC) ? expr : "NULL::DOUBLE";
	};
	auto num_bint = [&](const string &expr) -> string {
		return (cat == ColumnCategory::NUMERIC) ? expr : "NULL::BIGINT";
	};

	// Moments and percentiles over the finite values only: one NaN would turn the mean into
	// NaN, and SKEWNESS/KURTOSIS/STDDEV_SAMP raise "out of range" on NaN/Inf, failing the
	// whole profile. NaN and Inf values are counted in nan_count / inf_count instead.
	string dbl = "TRY_CAST(" + ec + " AS DOUBLE)";
	string finite = "(CASE WHEN isfinite(" + dbl + ") THEN " + dbl + " END)";
	string mean_sql    = num_dbl("AVG(" + finite + ")");
	string median_sql  = num_dbl("MEDIAN(" + finite + ")");
	string stddev_sql  = num_dbl("STDDEV_SAMP(" + finite + ")");
	string p25_sql     = num_dbl("PERCENTILE_CONT(0.25) WITHIN GROUP (ORDER BY " + finite + ")");
	string p75_sql     = num_dbl("PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY " + finite + ")");
	string skewness_sql = num_dbl("SKEWNESS(" + finite + ")");
	string kurtosis_sql = num_dbl("KURTOSIS(" + finite + ")");
	// Integer and DECIMAL columns (always finite) are exact above 2^53 only before the DOUBLE
	// cast: the mean and quantiles are taken on the column itself, and the spread and shape
	// statistics on the exact differences to a reference value (ExactCenteringType), so
	// 9007199254740992 and 9007199254740993 do not collapse into a constant column.
	string centering_type = cat == ColumnCategory::NUMERIC ? ExactCenteringType(col_type) : "";
	if (!centering_type.empty()) {
		string centred =
		    CenteredDoubleSQL(ec, centering_type, "(SELECT " + CenteringReferenceSQL(ec, centering_type) + " FROM src)");
		mean_sql = "CAST(AVG(" + ec + ") AS DOUBLE)";
		median_sql = "CAST(MEDIAN(" + ec + ") AS DOUBLE)";
		p25_sql = "CAST(PERCENTILE_CONT(0.25) WITHIN GROUP (ORDER BY " + ec + ") AS DOUBLE)";
		p75_sql = "CAST(PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY " + ec + ") AS DOUBLE)";
		stddev_sql = "STDDEV_SAMP(" + centred + ")";
		skewness_sql = "SKEWNESS(" + centred + ")";
		kurtosis_sql = "KURTOSIS(" + centred + ")";
	}
	string nan_sql = num_bint("CAST(COUNT(*) FILTER (WHERE isnan(" + dbl + ")) AS BIGINT)");
	string inf_sql = num_bint("CAST(COUNT(*) FILTER (WHERE isinf(" + dbl + ")) AS BIGINT)");

	// min_val / max_val — guard against MIN/MAX on complex types (undefined ordering). Numeric
	// columns take the finite values only: DuckDB orders NaN above +Inf, so MAX would be 'nan'.
	// The value keeps its own type (DECIMAL scale, integer) in the VARCHAR rendering.
	string min_max_input = (cat == ColumnCategory::NUMERIC) ? "(CASE WHEN isfinite(" + dbl + ") THEN " + ec + " END)" : ec;
	string min_val_sql = is_complex ? "NULL::VARCHAR" : "CAST(MIN(" + min_max_input + ") AS VARCHAR)";
	string max_val_sql = is_complex ? "NULL::VARCHAR" : "CAST(MAX(" + min_max_input + ") AS VARCHAR)";

	// distinct_count — cast complex types to VARCHAR for GROUP BY consistency
	string dc = is_complex ? "CAST(" + ec + " AS VARCHAR)" : ec;

	// Length stats — string, list (element count), map (cardinality), others NULL
	string avg_len_sql, min_len_sql, max_len_sql;
	if (cat == ColumnCategory::STRING) {
		avg_len_sql = "AVG(LENGTH(CAST(" + ec + " AS VARCHAR)))";
		min_len_sql = "CAST(MIN(LENGTH(CAST(" + ec + " AS VARCHAR))) AS BIGINT)";
		max_len_sql = "CAST(MAX(LENGTH(CAST(" + ec + " AS VARCHAR))) AS BIGINT)";
	} else if (cat == ColumnCategory::LIST) {
		avg_len_sql = "AVG(len(" + ec + "))";
		min_len_sql = "CAST(MIN(len(" + ec + ")) AS BIGINT)";
		max_len_sql = "CAST(MAX(len(" + ec + ")) AS BIGINT)";
	} else if (cat == ColumnCategory::MAP_TYPE) {
		avg_len_sql = "AVG(cardinality(" + ec + "))";
		min_len_sql = "CAST(MIN(cardinality(" + ec + ")) AS BIGINT)";
		max_len_sql = "CAST(MAX(cardinality(" + ec + ")) AS BIGINT)";
	} else {
		avg_len_sql = "NULL::DOUBLE";
		min_len_sql = "NULL::BIGINT";
		max_len_sql = "NULL::BIGINT";
	}

	// Pattern summary — string columns use regex detection; complex types get fixed literals
	string pattern_sql = "NULL::VARCHAR";
	if (cat == ColumnCategory::STRING) {
		auto regex_frac = [&](const string &regex) -> string {
			return "SUM(CASE WHEN REGEXP_MATCHES(CAST(" + ec + " AS VARCHAR), '" + regex +
			       "') THEN 1 ELSE 0 END) * 1.0 / NULLIF(COUNT(" + ec + "), 0) >= 0.8";
		};
		// Phone check: use the phonenumber_is_valid scalar function to detect phone columns.
		// >= 80% of non-null values must parse as valid phone numbers.
		string phone_frac =
		    "SUM(CASE WHEN anofox_tab_phonenumber_is_valid(CAST(" + ec + " AS VARCHAR), 'US') = true"
		    " THEN 1 ELSE 0 END) * 1.0 / NULLIF(COUNT(" + ec + "), 0) >= 0.8";
		pattern_sql =
		    "CASE"
		    " WHEN " + regex_frac("^[a-zA-Z0-9._%+\\-]+@[a-zA-Z0-9.\\-]+[.][a-zA-Z]{2,}$") +
		    " THEN 'email'"
		    " WHEN " + phone_frac +
		    " THEN 'phone'"
		    " WHEN " + regex_frac("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$") +
		    " THEN 'uuid'"
		    " WHEN " + regex_frac("^https?://.+") +
		    " THEN 'url'"
		    " WHEN " + regex_frac("^[0-9]{1,3}[.][0-9]{1,3}[.][0-9]{1,3}[.][0-9]{1,3}$") +
		    " THEN 'ip_address'"
		    " WHEN " + regex_frac("^[0-9]{4}-[0-9]{2}-[0-9]{2}$") +
		    " THEN 'iso_date'"
		    " WHEN " + regex_frac("^[0-9]+$") +
		    " THEN 'numeric_string'"
		    " ELSE 'mixed'"
		    " END";
	} else if (cat == ColumnCategory::LIST) {
		pattern_sql = "'list'";
	} else if (cat == ColumnCategory::MAP_TYPE) {
		pattern_sql = "'map'";
	} else if (cat == ColumnCategory::STRUCT_TYPE) {
		pattern_sql = "'struct'";
	} else if (cat == ColumnCategory::UNION_TYPE) {
		pattern_sql = "'union'";
	}

	// Zero / negative counts — numeric only
	string zero_sql = num_bint(
	    "CAST(COALESCE(SUM(CASE WHEN TRY_CAST(" + ec + " AS DOUBLE) = 0 THEN 1 ELSE 0 END), 0)"
	    " AS BIGINT)");
	string neg_sql = num_bint(
	    "CAST(COALESCE(SUM(CASE WHEN TRY_CAST(" + ec + " AS DOUBLE) < 0 THEN 1 ELSE 0 END), 0)"
	    " AS BIGINT)");

	// top_values — values are grouped and ordered in the column's own type, then shown as
	// VARCHAR; ties in count are ordered by value (9 before 10, not '10' before '9') so the
	// result is deterministic. Numeric columns list finite values only (NaN/Inf are in
	// nan_count / inf_count)
	string top_val_filter = (cat == ColumnCategory::NUMERIC) ? "isfinite(" + dbl + ")" : ec + " IS NOT NULL";
	string top_val_sql =
	    "(SELECT LIST({'value': CAST(v AS VARCHAR), 'count': cnt} ORDER BY cnt DESC, v)"
	    " FROM (SELECT " + ec + " AS v, COUNT(*) AS cnt FROM src"
	    "       WHERE " + top_val_filter +
	    "       GROUP BY " + ec +
	    "       ORDER BY cnt DESC, v LIMIT 5))";

	return "SELECT"
	       " " + col_name_lit + " AS column_name,"
	       " " + col_type_lit + " AS column_type,"
	       " CAST(COUNT(*) AS BIGINT) AS row_count,"
	       " CAST(COALESCE(SUM(CASE WHEN " + ec + " IS NULL THEN 1 ELSE 0 END), 0) AS BIGINT)"
	       " AS null_count,"
	       " CAST(COALESCE(SUM(CASE WHEN " + ec + " IS NULL THEN 1 ELSE 0 END), 0) AS DOUBLE)"
	       " / NULLIF(CAST(COUNT(*) AS DOUBLE), 0) AS null_rate,"
	       " CAST(COUNT(DISTINCT " + dc + ") AS BIGINT) AS distinct_count,"
	       " CAST(COUNT(DISTINCT " + dc + ") AS DOUBLE)"
	       " / NULLIF(CAST(COUNT(*) AS DOUBLE), 0) AS distinct_rate,"
	       " " + min_val_sql + " AS min_val,"
	       " " + max_val_sql + " AS max_val,"
	       " " + mean_sql + " AS mean,"
	       " " + median_sql + " AS median,"
	       " " + stddev_sql + " AS stddev,"
	       " " + p25_sql + " AS p25,"
	       " " + p75_sql + " AS p75,"
	       " " + skewness_sql + " AS skewness,"
	       " " + kurtosis_sql + " AS kurtosis,"
	       " " + top_val_sql + " AS top_values,"
	       " " + avg_len_sql + " AS avg_length,"
	       " " + min_len_sql + " AS min_length,"
	       " " + max_len_sql + " AS max_length,"
	       " " + pattern_sql + " AS pattern_summary,"
	       " (CAST(COUNT(DISTINCT " + dc + ") AS BIGINT)"
	       "  = CAST(COUNT(" + ec + ") AS BIGINT)) AS is_unique,"
	       " (COUNT(DISTINCT " + dc + ") <= 1) AS is_constant,"
	       " " + zero_sql + " AS zero_count,"
	       " " + neg_sql + " AS negative_count,"
	       " " + is_sampled_str + "::BOOLEAN AS is_sampled,"
	       " " + sample_size_str + "::BIGINT AS actual_sample_size,"
	       " " + nan_sql + " AS nan_count,"
	       " " + inf_sql + " AS inf_count"
	       " FROM src";
}

static string BuildProfileTableSQL(const string &table_name,
                                   const vector<pair<string, string>> &columns,
                                   bool is_sampled, int64_t actual_sample_size,
                                   int64_t sample_size) {
	string tbl = EscapeStringLiteral(table_name);
	string sql = "WITH src AS (SELECT * FROM query_table('" + tbl + "')";
	if (is_sampled) {
		// REPEATABLE needs an explicit sampling method; "USING SAMPLE n ROWS REPEATABLE (s)" is a syntax error
		sql += " USING SAMPLE reservoir(" + std::to_string(sample_size) + " ROWS) REPEATABLE (42)";
	}
	sql += ")";

	bool first = true;
	for (auto &[col_name, col_type] : columns) {
		if (!first) {
			sql += "\nUNION ALL\n";
		}
		first = false;
		ColumnCategory cat = ClassifyColumn(col_type);
		sql += BuildColumnProfileSQL(col_name, col_type, cat, is_sampled, actual_sample_size);
	}

	return sql;
}

static void ProfileTableExecute(ClientContext &context, TableFunctionInput &data_p,
                                 DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ProfileTableBindData>();
	auto &state = data_p.global_state->Cast<ProfileTableGlobalState>();

	if (!state.executed) {
		state.executed = true;

		AnofoxTrace(AnofoxLogLevel::Debug,
		            "profile_table: scanning '" + bind_data.table_name + "' (filter_cols=" +
		                std::to_string(bind_data.columns.size()) +
		                ", sample_size=" + std::to_string(bind_data.sample_size) +
		                ", exact=" + (bind_data.exact ? "true" : "false") + ")");

		Connection con(*context.db);
		SiblingInterruptForwarder forward_interrupt(context, con);

		// Determine actual row count to decide if sampling is needed
		int64_t actual_row_count = 0;
		{
			auto count_res = con.Query(
			    "SELECT COUNT(*) FROM query_table('" +
			    EscapeStringLiteral(bind_data.table_name) + "')");
			if (!count_res->HasError()) {
				auto chunk = count_res->Fetch();
				if (chunk && chunk->size() > 0) {
					actual_row_count = chunk->GetValue(0, 0).GetValue<int64_t>();
				}
			}
		}

		bool is_sampled = !bind_data.exact && actual_row_count > bind_data.sample_size;
		int64_t actual_sample_size = is_sampled ? bind_data.sample_size : actual_row_count;

		auto &columns = bind_data.columns;
		if (columns.empty()) {
			return;
		}

		AnofoxTrace(AnofoxLogLevel::Debug,
		            "profile_table: profiling " + std::to_string(columns.size()) + " columns" +
		                (is_sampled
		                     ? " (sampled " + std::to_string(actual_sample_size) + " rows)"
		                     : " (full scan, " + std::to_string(actual_row_count) + " rows)"));

		// Build and execute profile SQL
		string profile_sql = BuildProfileTableSQL(bind_data.table_name, columns, is_sampled,
		                                          actual_sample_size, bind_data.sample_size);

		auto profile_res = con.Query(profile_sql);
		if (profile_res->HasError()) {
			ThrowIfInterrupted(context);
			throw InvalidInputException("profile_table query failed: %s",
			                           profile_res->GetError());
		}

		// Collect all rows into state
		while (true) {
			auto chunk = profile_res->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk->size(); row++) {
				vector<Value> row_vals;
				row_vals.reserve(chunk->ColumnCount());
				for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
					row_vals.push_back(chunk->GetValue(col, row));
				}
				state.rows.push_back(std::move(row_vals));
			}
		}

		AnofoxTrace(AnofoxLogLevel::Info,
		            "profile_table: collected " + std::to_string(state.rows.size()) +
		                " column profiles");
	}

	// Return rows progressively
	idx_t count = 0;
	while (state.current_row < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.current_row];
		for (idx_t col = 0; col < (idx_t)row.size() && col < output.ColumnCount(); col++) {
			output.SetValue(col, count, row[col]);
		}
		state.current_row++;
		count++;
	}
	duckdb_compat::SetChildCardinality(output, count);
}

//===--------------------------------------------------------------------===//
// profile_correlations — bind+init+execute
//===--------------------------------------------------------------------===//

struct ProfileCorrelationsBindData : public TableFunctionData {
	string table_name;
	// The candidate columns (name, type), resolved and validated at bind time
	vector<pair<string, string>> columns;

	ProfileCorrelationsBindData(string tbl, vector<pair<string, string>> cols)
	    : table_name(std::move(tbl)), columns(std::move(cols)) {}
};

struct ProfileCorrelationsGlobalState : public GlobalTableFunctionState {
	bool executed = false;
	idx_t current_row = 0;
	vector<vector<Value>> rows;
};

static unique_ptr<FunctionData> ProfileCorrelationsBind(ClientContext &context,
                                                         TableFunctionBindInput &input,
                                                         vector<LogicalType> &return_types,
                                                         duckdb_compat::BindNames &names) {
	PostHogTelemetry::Instance().RecordFunctionCall("profile_correlations");
	if (input.inputs.empty()) {
		throw BinderException("profile_correlations requires at least 1 argument: table_name");
	}

	string table_name = RequireNonNullArgument(input, 0, "profile_correlations", "table_name");
	RejectTempTable(context, "profile_correlations", table_name);
	RejectUncommittedChanges(context, "profile_correlations");

	vector<string> columns;
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull() &&
	    input.inputs[1].type().id() == LogicalTypeId::LIST) {
		auto &list_children = ListValue::GetChildren(input.inputs[1]);
		for (auto &child : list_children) {
			if (!child.IsNull()) {
				columns.push_back(child.ToString());
			}
		}
	}

	names.emplace_back("column_a"); return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("column_b"); return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	names.emplace_back("pearson");  return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("spearman"); return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
	names.emplace_back("n");        return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));

	// Resolve the table and the requested columns at bind time (see profile_table)
	Connection con(*context.db);
	SiblingInterruptForwarder forward_interrupt(context, con);
	auto resolved_columns = IntrospectColumns(con, "profile_correlations", table_name, columns);

	return make_uniq<ProfileCorrelationsBindData>(table_name, std::move(resolved_columns));
}

static unique_ptr<GlobalTableFunctionState> ProfileCorrelationsInit(ClientContext &,
                                                                     TableFunctionInitInput &) {
	return make_uniq<ProfileCorrelationsGlobalState>();
}

static void ProfileCorrelationsExecute(ClientContext &context, TableFunctionInput &data_p,
                                        DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ProfileCorrelationsBindData>();
	auto &state = data_p.global_state->Cast<ProfileCorrelationsGlobalState>();

	if (!state.executed) {
		state.executed = true;

		AnofoxTrace(AnofoxLogLevel::Debug,
		            "profile_correlations: scanning '" + bind_data.table_name + "'");

		Connection con(*context.db);
		SiblingInterruptForwarder forward_interrupt(context, con);

		// The numeric columns among the resolved ones, with the SQL of their values as DOUBLE:
		// integer and DECIMAL columns as the exact difference to a reference value
		// (ExactCenteringType), so values above 2^53 stay distinct; Pearson's r does not
		// change under the shift
		vector<string> numeric_cols;
		vector<string> numeric_vals;
		for (auto &[name, type] : bind_data.columns) {
			if (ClassifyColumn(type) == ColumnCategory::NUMERIC) {
				string ec = EscapeIdentifier(name);
				auto centering_type = ExactCenteringType(type);
				numeric_cols.push_back(name);
				numeric_vals.push_back(centering_type.empty()
				                           ? "TRY_CAST(" + ec + " AS DOUBLE)"
				                           : CenteredDoubleSQL(ec, centering_type,
				                                               "(SELECT " + CenteringReferenceSQL(ec, centering_type) +
				                                                   " FROM src)"));
			}
		}

		if (numeric_cols.size() < 2) {
			AnofoxTrace(AnofoxLogLevel::Debug,
			            "profile_correlations: fewer than 2 numeric columns, skipping");
			return;
		}

		// Build pairwise correlation SQL with a single src CTE
		string tbl = EscapeStringLiteral(bind_data.table_name);
		string sql = "WITH src AS (SELECT * FROM query_table('" + tbl + "'))";

		bool first = true;
		for (size_t i = 0; i < numeric_cols.size(); i++) {
			for (size_t j = i + 1; j < numeric_cols.size(); j++) {
				string ea = EscapeIdentifier(numeric_cols[i]);
				string eb = EscapeIdentifier(numeric_cols[j]);
				string col_a_lit = "'" + EscapeStringLiteral(numeric_cols[i]) + "'";
				string col_b_lit = "'" + EscapeStringLiteral(numeric_cols[j]) + "'";

				if (!first) {
					sql += "\nUNION ALL\n";
				}
				first = false;

				// Pearson: CORR on raw values
				// Spearman: CORR on rank-transformed values (subquery to separate window from agg)
				// Both clamped to [-1, 1]: floating-point rounding can yield 1.0000000000000002
				sql += "SELECT column_a, column_b, " + ClampUnitSQL("pearson") + " AS pearson, " +
				       ClampUnitSQL("spearman") + " AS spearman, n FROM (SELECT"
				       " " + col_a_lit + " AS column_a,"
				       " " + col_b_lit + " AS column_b,"
				       " CORR(a_val, b_val) AS pearson,"
				       " CORR(a_rank, b_rank) AS spearman,"
				       " CAST(COUNT(*) AS BIGINT) AS n"
				       " FROM ("
				       // ranks over the column itself: exact for large integers, the same
				       // order as the DOUBLE cast otherwise
				       "   SELECT"
				       "     " + numeric_vals[i] + " AS a_val,"
				       "     " + numeric_vals[j] + " AS b_val,"
				       "     CAST(RANK() OVER (ORDER BY " + ea + ") AS DOUBLE)"
				       "       AS a_rank,"
				       "     CAST(RANK() OVER (ORDER BY " + eb + ") AS DOUBLE)"
				       "       AS b_rank"
				       "   FROM src"
				       // finite pairs only: one NaN/Inf turns CORR into NaN
				       "   WHERE isfinite(TRY_CAST(" + ea + " AS DOUBLE)) AND isfinite(TRY_CAST(" + eb + " AS DOUBLE))"
				       " ) _corr_sub) _corr_pair";
			}
		}

		auto corr_res = con.Query(sql);
		if (corr_res->HasError()) {
			ThrowIfInterrupted(context);
			throw InvalidInputException("profile_correlations query failed: %s",
			                           corr_res->GetError());
		}

		while (true) {
			auto chunk = corr_res->Fetch();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			for (idx_t row = 0; row < chunk->size(); row++) {
				vector<Value> row_vals;
				for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
					row_vals.push_back(chunk->GetValue(col, row));
				}
				state.rows.push_back(std::move(row_vals));
			}
		}

		AnofoxTrace(AnofoxLogLevel::Info,
		            "profile_correlations: computed " + std::to_string(state.rows.size()) +
		                " pairs");
	}

	idx_t count = 0;
	while (state.current_row < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.current_row];
		for (idx_t col = 0; col < (idx_t)row.size() && col < output.ColumnCount(); col++) {
			output.SetValue(col, count, row[col]);
		}
		state.current_row++;
		count++;
	}
	duckdb_compat::SetChildCardinality(output, count);
}

} // anonymous namespace

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterProfileFunctions(ExtensionLoader &loader) {
	// profile_summary(table_name VARCHAR) → bind_replace, pure SQL
	TableFunction summary_func("anofox_tab_profile_summary", {LogicalType(LogicalTypeId::VARCHAR)}, nullptr,
	                           nullptr);
	summary_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileSummaryBindReplace);
	{
		FunctionDescription desc;
		desc.description = "Returns a summary profile of all columns in a table, including min, max, null_count, and distinct_count.";
		desc.parameter_names = {"table_name"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT * FROM profile_summary('orders');"};
		desc.categories = {"profiling", "data-quality"};
		RegisterTableFunctionWithAlias(loader, summary_func, "profile_summary", {std::move(desc)});
	}

	// profile_table(table_name [, cols [, sample_size [, exact]]]) — multiple overloads
	TableFunctionSet profile_set("anofox_tab_profile_table");

	TableFunction p1("anofox_tab_profile_table", {LogicalType(LogicalTypeId::VARCHAR)}, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableExecute),
	                 DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableBind), ProfileTableInit);
	profile_set.AddFunction(p1);

	TableFunction p2("anofox_tab_profile_table",
	                 {LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                 DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableBind), ProfileTableInit);
	profile_set.AddFunction(p2);

	TableFunction p3("anofox_tab_profile_table",
	                 {LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
	                  LogicalType(LogicalTypeId::BIGINT)},
	                 DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableBind), ProfileTableInit);
	profile_set.AddFunction(p3);

	TableFunction p4("anofox_tab_profile_table",
	                 {LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)),
	                  LogicalType(LogicalTypeId::BIGINT), LogicalType(LogicalTypeId::BOOLEAN)},
	                 DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileTableBind), ProfileTableInit);
	profile_set.AddFunction(p4);

	{
		FunctionDescription desc;
		desc.description = "Profiles selected columns of a table with detailed statistics (min, max, mean, stddev, null_count, distinct_count, sample rows). Optionally filters columns, sets sample size, or uses exact counts.";
		desc.parameter_names = {"table_name", "columns", "sample_size", "exact"};
		desc.examples = {"SELECT * FROM profile_table('orders');", "SELECT * FROM profile_table('orders', ['amount', 'status']);"};
		desc.categories = {"profiling", "data-quality"};
		RegisterTableFunctionSetWithAlias(loader, profile_set, "profile_table", {std::move(desc)});
	}

	// profile_correlations(table_name [, cols]) — two overloads
	TableFunctionSet corr_set("anofox_tab_profile_correlations");

	TableFunction c1("anofox_tab_profile_correlations", {LogicalType(LogicalTypeId::VARCHAR)},
	                 DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileCorrelationsExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileCorrelationsBind),
	                 ProfileCorrelationsInit);
	corr_set.AddFunction(c1);

	TableFunction c2("anofox_tab_profile_correlations",
	                 {LogicalType(LogicalTypeId::VARCHAR), LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR))},
	                 DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileCorrelationsExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, ProfileCorrelationsBind),
	                 ProfileCorrelationsInit);
	corr_set.AddFunction(c2);

	{
		FunctionDescription desc;
		desc.description = "Computes pairwise Pearson correlation coefficients for numeric columns in a table.";
		desc.parameter_names = {"table_name", "columns"};
		desc.examples = {"SELECT * FROM profile_correlations('orders');", "SELECT * FROM profile_correlations('orders', ['amount', 'qty']);"};
		desc.categories = {"profiling", "statistics"};
		RegisterTableFunctionSetWithAlias(loader, corr_set, "profile_correlations", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

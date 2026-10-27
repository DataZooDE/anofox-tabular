#include "anofox_check_suite.hpp"
#include "anofox_check.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb_compat.hpp"
#include "anofox_tabular_banner.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

// The current time as a UTC TIMESTAMP, with and without the ICU extension: CAST(now() AS
// TIMESTAMP) would follow the session time zone once ICU is loaded, and AT TIME ZONE needs ICU.
const char *const UTC_NOW_SQL = "make_timestamp(epoch_us(now()))";

// One row of the checks table, with params already extracted from JSON at read time
struct CheckSpec {
	string check_name;
	string check_type;
	string table_name;
	string column_name;
	// params (all optional, empty when absent)
	string pattern;
	vector<string> allowed_values;
	bool has_allowed_values = false;
	string agg;
	string mode;
	string expression;
	string right_table;
	string left_keys;
	string right_keys;
	string date_column;
	string count_column;
	string metric_column;
	string window_days;
	string k;
	// reference_date / reference_time as validated SQL (a DATE '...' / TIMESTAMP '...' literal in
	// UTC, or the UTC date arithmetic of a ${today}-style token), empty when absent
	string reference_date_sql;
	string reference_time_sql;
	// thresholds and behavior
	Value lower_threshold;
	Value upper_threshold;
	bool monitor_only = false;
	string identifier_column;
	string filter_expr;
	// params.fail_on_empty: no rows to evaluate (empty table, or a filter matching no rows) fails
	bool fail_on_empty = false;
	// params.fill_missing_days (rel_count_change): calendar days without rows count as 0
	bool fill_missing_days = false;
	// A problem found while reading the check (e.g. invalid params JSON); the check then
	// yields an 'error' row instead of failing the whole run
	string config_error;
};

// The params keys each check type accepts; nullptr for an unknown check type (that error is
// reported when the check is built).
static const vector<string> *ParamsKeysFor(const string &check_type) {
	static const std::unordered_map<string, vector<string>> KEYS = {
	    {"volume", {}},
	    {"null_rate", {}},
	    {"distinct_count", {}},
	    {"duplicate_count", {}},
	    {"regex_match", {"pattern"}},
	    {"values_in_set", {"allowed_values"}},
	    {"agg", {"agg"}},
	    {"compliance", {"expression"}},
	    {"freshness", {"reference_time"}},
	    {"occurrence", {"mode"}},
	    {"match_rate", {"right_table", "right_keys", "left_keys"}},
	    {"rel_count_change", {"date_column", "count_column", "window_days", "reference_date", "fill_missing_days"}},
	    {"metric_anomaly_iqr", {"date_column", "metric_column", "window_days", "k", "reference_date"}},
	    {"rolling_values_in_set", {"date_column", "allowed_values", "window_days", "reference_date"}},
	};
	auto entry = KEYS.find(check_type);
	return entry == KEYS.end() ? nullptr : &entry->second;
}

// The params keys every check type accepts
static const vector<string> &CommonParamsKeys() {
	static const vector<string> KEYS = {"fail_on_empty"};
	return KEYS;
}

// The check types that exist only as direct functions (they return per-row statistics or a
// column list, not one value against thresholds); a suite row of such a type is told so
// instead of being reported like a misspelled type
static string DirectOnlyCheckTypeMessage(const string &check_type) {
	static const vector<string> DIRECT_ONLY = {"zscore",
	                                           "iqr",
	                                           "schema_check",
	                                           "rel_count_change_events",
	                                           "metric_anomaly_iqr_events",
	                                           "rolling_values_in_set_events"};
	if (std::find(DIRECT_ONLY.begin(), DIRECT_ONLY.end(), check_type) != DIRECT_ONLY.end()) {
		return "check type '" + check_type + "' is not supported in run_checks (use the " + check_type +
		       "() function)";
	}
	if (check_type == "agg_check") {
		return "check type 'agg_check' is called 'agg' in run_checks (set params to {\"agg\": \"avg\"} etc.)";
	}
	return string();
}

// Reject a check type run_checks cannot evaluate, before anything else of the check is looked at
static void RequireSupportedCheckType(const CheckSpec &spec) {
	if (ParamsKeysFor(spec.check_type)) {
		return;
	}
	auto direct_only = DirectOnlyCheckTypeMessage(spec.check_type);
	if (!direct_only.empty()) {
		throw BinderException(direct_only);
	}
	throw BinderException("run_checks: unknown check_type '" + spec.check_type + "' in check '" + spec.check_name +
	                      "' (supported: volume, null_rate, distinct_count, regex_match, values_in_set, agg, "
	                      "duplicate_count, occurrence, match_rate, compliance, freshness, rel_count_change, "
	                      "metric_anomaly_iqr, rolling_values_in_set)");
}

// A misspelled key (e.g. 'refernce_date') would otherwise be ignored silently and the check
// would evaluate its default instead of what was configured.
static void ValidateParamsKeys(const CheckSpec &spec, const vector<string> &keys) {
	auto *valid = ParamsKeysFor(spec.check_type);
	if (!valid) {
		return;
	}
	vector<string> all_valid = *valid;
	all_valid.insert(all_valid.end(), CommonParamsKeys().begin(), CommonParamsKeys().end());
	vector<string> unknown;
	for (auto &key : keys) {
		if (std::find(all_valid.begin(), all_valid.end(), key) == all_valid.end()) {
			unknown.push_back("'" + key + "'");
		}
	}
	if (unknown.empty()) {
		return;
	}
	string message = (unknown.size() == 1 ? "unknown params key " : "unknown params keys ") +
	                 StringUtil::Join(unknown, ", ");
	message += " (valid keys for " + spec.check_type + ": " + StringUtil::Join(all_valid, ", ") + ")";
	throw BinderException("run_checks: check '" + spec.check_name + "': " + message);
}

// The UTC date `day_offset` days from the run's date, as SQL. Not current_date/today(): with
// ICU loaded they follow the session time zone, without it they do not resolve.
static string UtcDateTokenSQL(int64_t day_offset) {
	const string today_expr = string("CAST(") + UTC_NOW_SQL + " AS DATE)";
	if (day_offset == 0) {
		return today_expr;
	}
	if (day_offset > 0) {
		return "(" + today_expr + " + " + std::to_string(day_offset) + ")";
	}
	return "(" + today_expr + " - " + std::to_string(-day_offset) + ")";
}

// Substitute ${today}, ${yesterday}, ${today+N}, ${today-N}, ${yesterday+N}, ${yesterday-N}
// tokens in `expr` with `render(day offset)`, by default the UTC date arithmetic
// (UtcDateTokenSQL). `location` names where the tokens come from in error messages (e.g.
// "filter_expr of check 'x'", "params.reference_date").
static string SubstituteDateTokens(const string &expr, const string &location,
                                   const std::function<string(int64_t)> &render = UtcDateTokenSQL) {
	const string supported = " (supported: today, yesterday, today+N, today-N, yesterday-N)";
	string out;
	size_t pos = 0;
	while (pos < expr.size()) {
		size_t start = expr.find("${", pos);
		if (start == string::npos) {
			out += expr.substr(pos);
			break;
		}
		size_t end = expr.find('}', start);
		if (end == string::npos) {
			throw BinderException("run_checks: unterminated ${...} token in " + location);
		}
		out += expr.substr(pos, start - pos);
		string token = expr.substr(start + 2, end - start - 2);

		int64_t base_offset = 0;
		string rest;
		if (token.rfind("today", 0) == 0) {
			rest = token.substr(5);
		} else if (token.rfind("yesterday", 0) == 0) {
			base_offset = -1;
			rest = token.substr(9);
		} else {
			throw BinderException("run_checks: unknown token '${" + token + "}' in " + location + supported);
		}
		int64_t day_offset = base_offset;
		if (!rest.empty()) {
			if ((rest[0] != '+' && rest[0] != '-') || rest.size() < 2 || rest.size() > 7 ||
			    rest.find_first_not_of("0123456789", 1) != string::npos) {
				throw BinderException("run_checks: unknown token '${" + token + "}' in " + location + supported);
			}
			int64_t n = std::stoll(rest.substr(1));
			day_offset += (rest[0] == '+') ? n : -n;
		}
		out += render(day_offset);
		pos = end + 1;
	}
	return out;
}

// Whether a params value is exactly one date token such as ${yesterday} (nothing around it)
static bool IsSingleDateToken(const string &text) {
	return text.size() > 3 && StringUtil::StartsWith(text, "${") && text.find('}') == text.size() - 1;
}

// params.<key> as a DATE literal. Validated here, with DuckDB's own cast, so a malformed date
// becomes an 'error' row of this check instead of a conversion error aborting the suite.
// A cast that fails, throws (DuckDB v1.5 throws a conversion error for some strings, e.g.
// 'yesterday') or yields NULL counts as invalid
static bool TryCastParam(const string &text, const LogicalType &type, Value &result) {
	try {
		return duckdb_compat::TryCastValue(Value(text), type, result) && !result.IsNull();
	} catch (std::exception &) {
		return false;
	}
}

static string DateParamLiteral(const CheckSpec &spec, const string &key, const string &text) {
	Value date;
	if (!TryCastParam(text, LogicalType(LogicalTypeId::DATE), date)) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params." + key + " '" + text +
		                      "' is not a valid date (expected YYYY-MM-DD)");
	}
	return "DATE '" + date.ToString() + "'";
}

// params.<key> as a UTC TIMESTAMP literal: a value with a UTC offset is converted to UTC, a
// value without one is taken as UTC. The core casts are used (not the ICU ones, which would
// read a value without offset in the session time zone).
static string UtcTimestampParamLiteral(const CheckSpec &spec, const string &key, const string &text) {
	Value with_zone;
	Value utc;
	bool ok = TryCastParam(text, LogicalType(LogicalTypeId::TIMESTAMP_TZ), with_zone);
	if (ok) {
		try {
			ok = duckdb_compat::TryCastValue(with_zone, LogicalType(LogicalTypeId::TIMESTAMP), utc) && !utc.IsNull();
		} catch (std::exception &) {
			ok = false;
		}
	}
	if (!ok) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params." + key + " '" + text +
		                      "' is not a valid timestamp (expected YYYY-MM-DD[ HH:MM[:SS]][+HH[:MM]])");
	}
	return "TIMESTAMP '" + utc.ToString() + "'";
}

// Parse the params JSON of one check with DuckDB's bundled yyjson (no dependency on the
// json SQL extension, which is not loaded in every environment).
static void ParseCheckParams(CheckSpec &spec, const string &params_json) {
	using namespace duckdb_yyjson;
	if (params_json.empty()) {
		return;
	}
	struct YyjsonDocFree {
		void operator()(yyjson_doc *doc_handle) const {
			yyjson_doc_free(doc_handle);
		}
	};
	std::unique_ptr<yyjson_doc, YyjsonDocFree> doc(yyjson_read(params_json.c_str(), params_json.size(), 0));
	yyjson_val *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
	if (!root || !yyjson_is_obj(root)) {
		throw BinderException("run_checks: check '" + spec.check_name + "': params must be a JSON object, got '" +
		                      params_json + "'");
	}

	vector<string> keys;
	{
		size_t idx, max;
		yyjson_val *key;
		yyjson_val *ignored;
		yyjson_obj_foreach(root, idx, max, key, ignored) {
			keys.push_back(yyjson_get_str(key));
		}
	}
	ValidateParamsKeys(spec, keys);

	auto get_scalar = [&](const char *key) -> string {
		auto *val = yyjson_obj_get(root, key);
		if (!val || yyjson_is_null(val)) {
			return string();
		}
		if (yyjson_is_str(val)) {
			return yyjson_get_str(val);
		}
		if (yyjson_is_int(val)) {
			return std::to_string(yyjson_get_sint(val));
		}
		if (yyjson_is_real(val)) {
			return std::to_string(yyjson_get_real(val));
		}
		throw BinderException("run_checks: check '" + spec.check_name + "': params." + key +
		                      " must be a string or number");
	};

	spec.pattern = get_scalar("pattern");
	spec.agg = get_scalar("agg");
	spec.mode = get_scalar("mode");
	spec.expression = get_scalar("expression");
	// Key column lists: a comma-separated string or a JSON array of column names
	auto get_columns = [&](const char *key) -> string {
		auto *val = yyjson_obj_get(root, key);
		if (!val || !yyjson_is_arr(val)) {
			return get_scalar(key);
		}
		vector<string> columns;
		size_t idx, max;
		yyjson_val *element;
		yyjson_arr_foreach(val, idx, max, element) {
			if (!yyjson_is_str(element)) {
				throw BinderException("run_checks: check '" + spec.check_name + "': params." + key +
				                      " must be a string or a JSON array of strings");
			}
			columns.push_back(yyjson_get_str(element));
		}
		return StringUtil::Join(columns, ", ");
	};

	spec.right_table = get_scalar("right_table");
	spec.left_keys = get_columns("left_keys");
	spec.right_keys = get_columns("right_keys");
	spec.date_column = get_scalar("date_column");
	spec.count_column = get_scalar("count_column");
	spec.metric_column = get_scalar("metric_column");
	spec.window_days = get_scalar("window_days");
	spec.k = get_scalar("k");
	// A date token (${yesterday}, ${today-N}, ...) is the UTC date of the run; reference_time
	// then is midnight UTC of that date
	auto reference_date = get_scalar("reference_date");
	if (IsSingleDateToken(reference_date)) {
		spec.reference_date_sql = SubstituteDateTokens(reference_date, "params.reference_date");
	} else if (!reference_date.empty()) {
		spec.reference_date_sql = DateParamLiteral(spec, "reference_date", reference_date);
	}
	auto reference_time = get_scalar("reference_time");
	if (IsSingleDateToken(reference_time)) {
		spec.reference_time_sql =
		    "CAST(" + SubstituteDateTokens(reference_time, "params.reference_time") + " AS TIMESTAMP)";
	} else if (!reference_time.empty()) {
		spec.reference_time_sql = UtcTimestampParamLiteral(spec, "reference_time", reference_time);
	}

	auto *fail_on_empty = yyjson_obj_get(root, "fail_on_empty");
	if (fail_on_empty && !yyjson_is_null(fail_on_empty)) {
		if (!yyjson_is_bool(fail_on_empty)) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.fail_on_empty must be true or false");
		}
		spec.fail_on_empty = yyjson_get_bool(fail_on_empty);
	}
	auto *fill_missing_days = yyjson_obj_get(root, "fill_missing_days");
	if (fill_missing_days && !yyjson_is_null(fill_missing_days)) {
		if (!yyjson_is_bool(fill_missing_days)) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.fill_missing_days must be true or false");
		}
		spec.fill_missing_days = yyjson_get_bool(fill_missing_days);
	}

	auto *allowed = yyjson_obj_get(root, "allowed_values");
	if (allowed && !yyjson_is_null(allowed)) {
		if (!yyjson_is_arr(allowed)) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.allowed_values must be a JSON array of strings");
		}
		spec.has_allowed_values = true;
		size_t idx, max;
		yyjson_val *element;
		yyjson_arr_foreach(allowed, idx, max, element) {
			if (!yyjson_is_str(element)) {
				throw BinderException("run_checks: check '" + spec.check_name +
				                      "': params.allowed_values must be a JSON array of strings");
			}
			spec.allowed_values.push_back(yyjson_get_str(element));
		}
	}
}

// The columns of a checks table: the required ones must exist, a missing optional one reads as
// NULL (monitor_only as false). Other columns (an owner, a description, ...) are ignored.
static const vector<string> REQUIRED_CHECKS_COLUMNS = {"check_name", "check_type", "table_name"};
static const vector<string> OPTIONAL_CHECKS_COLUMNS = {"column_name",     "params",       "lower_threshold",
                                                       "upper_threshold", "monitor_only", "identifier_column",
                                                       "filter_expr"};

// The first line of an error message (binder errors append the query context)
static string FirstLine(const string &message) {
	auto newline = message.find('\n');
	return newline == string::npos ? message : message.substr(0, newline);
}

// The columns run_checks reads from a checks table, in this order, with the type each is cast to
struct ChecksColumn {
	const char *name;
	LogicalTypeId type;
};
static const vector<ChecksColumn> CHECKS_READ_COLUMNS = {
    {"check_name", LogicalTypeId::VARCHAR},       {"check_type", LogicalTypeId::VARCHAR},
    {"table_name", LogicalTypeId::VARCHAR},       {"column_name", LogicalTypeId::VARCHAR},
    {"params", LogicalTypeId::VARCHAR},           {"lower_threshold", LogicalTypeId::DOUBLE},
    {"upper_threshold", LogicalTypeId::DOUBLE},   {"monitor_only", LogicalTypeId::BOOLEAN},
    {"identifier_column", LogicalTypeId::VARCHAR}, {"filter_expr", LogicalTypeId::VARCHAR}};
static constexpr idx_t CHECKS_MONITOR_ONLY_COLUMN = 7;
// The rows read from a checks table, one Value per CHECKS_READ_COLUMNS entry
using ChecksRows = std::vector<std::vector<Value>>;

static void RequireChecksColumns(const string &checks_table, const vector<string> &available) {
	vector<string> missing;
	for (auto &name : REQUIRED_CHECKS_COLUMNS) {
		auto found = std::any_of(available.begin(), available.end(),
		                         [&](const string &candidate) { return StringUtil::CIEquals(candidate, name); });
		if (!found) {
			missing.push_back(name);
		}
	}
	if (!missing.empty()) {
		throw BinderException("run_checks: checks table '%s' is missing the required column%s %s (required: %s; "
		                      "optional: %s)",
		                      checks_table, missing.size() == 1 ? "" : "s", StringUtil::Join(missing, ", "),
		                      StringUtil::Join(REQUIRED_CHECKS_COLUMNS, ", "),
		                      StringUtil::Join(OPTIONAL_CHECKS_COLUMNS, ", "));
	}
}

// Read the rows of a checks table that is a DuckDB table (regular, TEMP or of an attached DuckDB
// database) directly from its storage, in the caller's transaction: definitions inserted, updated
// or deleted in an open transaction (BEGIN ... run_checks ... COMMIT) are seen, like a plain
// SELECT sees them. The bind holds the caller's context, so no query can run in it; a scan of the
// table storage with the caller's DuckTransaction is what DuckDB's own table scan does.
// Returns false when the entry is not a DuckDB table (a view, a table of another catalog type)
// or reads a generated column; the caller then reads it through a second connection.
static bool ScanChecksTableInTransaction(ClientContext &context, const string &checks_table, CatalogEntry &entry,
                                         ChecksRows &rows) {
	if (entry.type != CatalogType::TABLE_ENTRY) {
		return false;
	}
	auto &table = entry.Cast<TableCatalogEntry>();
	if (!table.IsDuckTable()) {
		return false;
	}
	vector<string> available;
	for (auto &col : table.GetColumns().Logical()) {
		available.push_back(duckdb_compat::Str(col.Name()));
	}
	RequireChecksColumns(checks_table, available);

	// The storage column of each read column; -1 for an optional column the table lacks (NULL)
	vector<int64_t> scan_position;
	vector<StorageIndex> storage_ids;
	vector<LogicalType> scan_types;
	for (auto &wanted : CHECKS_READ_COLUMNS) {
		int64_t position = -1;
		for (auto &col : table.GetColumns().Logical()) {
			if (!StringUtil::CIEquals(duckdb_compat::Str(col.Name()), wanted.name)) {
				continue;
			}
			if (col.Generated()) {
				return false;
			}
			position = static_cast<int64_t>(storage_ids.size());
			storage_ids.emplace_back(col.StorageOid());
			scan_types.push_back(col.Type());
			break;
		}
		scan_position.push_back(position);
	}

	auto &storage = table.GetStorage();
	auto &transaction = DuckTransaction::Get(context, table.ParentCatalog());
	TableScanState state;
	storage.InitializeScan(context, transaction, state, storage_ids);
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), scan_types);
	while (true) {
		chunk.Reset();
		storage.Scan(transaction, chunk, state);
		if (chunk.size() == 0) {
			break;
		}
		for (idx_t row = 0; row < chunk.size(); row++) {
			std::vector<Value> values;
			for (idx_t col = 0; col < CHECKS_READ_COLUMNS.size(); col++) {
				LogicalType target(CHECKS_READ_COLUMNS[col].type);
				if (scan_position[col] < 0) {
					values.emplace_back(target);
					continue;
				}
				auto value = chunk.GetValue(static_cast<idx_t>(scan_position[col]), row);
				try {
					values.push_back(value.CastAs(context, target));
				} catch (const std::exception &ex) {
					throw BinderException("run_checks: cannot read checks table '%s': column '%s': %s",
					                      checks_table, CHECKS_READ_COLUMNS[col].name,
					                      FirstLine(ErrorData(ex).RawMessage()));
				}
			}
			if (values[CHECKS_MONITOR_ONLY_COLUMN].IsNull()) {
				values[CHECKS_MONITOR_ONLY_COLUMN] = Value::BOOLEAN(false);
			}
			rows.push_back(std::move(values));
		}
	}
	// ORDER BY check_name, as the query of the second-connection path
	std::stable_sort(rows.begin(), rows.end(), [](const std::vector<Value> &left, const std::vector<Value> &right) {
		if (left[0].IsNull() || right[0].IsNull()) {
			return !left[0].IsNull() && right[0].IsNull();
		}
		return StringValue::Get(left[0]) < StringValue::Get(right[0]);
	});
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: read " + std::to_string(rows.size()) + " check(s) from '" +
	                                       checks_table + "' in the caller's transaction");
	return true;
}

// Read a checks table that is not a DuckDB table (a view, a table of another catalog type)
// through a second connection. That connection sees only committed data and none of the
// caller's TEMP objects, so a TEMP view is rejected, and so is a run inside a transaction with
// uncommitted changes (the view would silently show the committed state).
static ChecksRows ReadChecksThroughConnection(ClientContext &context, const string &checks_table,
                                                         const RelationInfo &relation) {
	if (relation.found && relation.is_temp) {
		throw BinderException("run_checks: the checks view '%s' is a TEMP view; a checks view must be a regular, "
		                      "non-temporary view, or use a table (TEMP tables work)",
		                      checks_table);
	}
	if (relation.found && !context.transaction.IsAutoCommit() && context.transaction.HasActiveTransaction() &&
	    MetaTransaction::Get(context).ModifiedDatabase()) {
		throw BinderException("run_checks: the checks %s '%s' is read outside the current transaction, which has "
		                      "uncommitted changes; commit them before running it, or pass the checks table "
		                      "instead of a view",
		                      relation.is_view ? "view" : "table", checks_table);
	}
	Connection con(*context.db);
	vector<string> available;
	if (relation.found && !relation.columns.empty()) {
		for (auto &column : relation.columns) {
			available.push_back(column.name);
		}
	} else {
		auto probe = con.Query("SELECT * FROM " + BuildQueryTableRef(checks_table) + " LIMIT 0");
		if (probe->HasError()) {
			auto &error = probe->GetErrorObject();
			if (!relation.found && error.Type() == ExceptionType::CATALOG) {
				throw BinderException("run_checks: checks table '%s' does not exist", checks_table);
			}
			throw BinderException("run_checks: cannot read checks table '%s': %s", checks_table,
			                      FirstLine(error.RawMessage()));
		}
		auto result_names = duckdb_compat::ResultNames(*probe);
		available.assign(result_names.begin(), result_names.end());
	}
	RequireChecksColumns(checks_table, available);
	auto has_column = [&](const string &name) {
		return std::any_of(available.begin(), available.end(),
		                   [&](const string &candidate) { return StringUtil::CIEquals(candidate, name); });
	};
	// A missing optional column reads as NULL
	string select_list;
	for (idx_t col = 0; col < CHECKS_READ_COLUMNS.size(); col++) {
		auto &column = CHECKS_READ_COLUMNS[col];
		string expr = "CAST(" + (has_column(column.name) ? QuoteSqlIdentifier(column.name) : string("NULL")) +
		              " AS " + LogicalType(column.type).ToString() + ")";
		if (col == CHECKS_MONITOR_ONLY_COLUMN) {
			expr = "COALESCE(" + expr + ", false)";
		}
		select_list += (col > 0 ? ", " : "") + expr;
	}
	string sql = "SELECT " + select_list + " FROM " + BuildQueryTableRef(checks_table) + " ORDER BY 1";
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: reading checks from '" + checks_table +
	                                       "' through a second connection");
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw BinderException("run_checks: cannot read checks table '%s': %s", checks_table,
		                      FirstLine(result->GetErrorObject().RawMessage()));
	}
	return duckdb_compat::FetchRows(*result);
}

// Read the check definitions. A DuckDB table is read in the caller's transaction (uncommitted
// definitions included); anything else through a second connection.
static vector<CheckSpec> ReadCheckSpecs(ClientContext &context, const string &checks_table) {
	ChecksRows rows;
	auto entry = LookupRelationEntry(context, checks_table);
	if (!entry || !ScanChecksTableInTransaction(context, checks_table, *entry, rows)) {
		rows = ReadChecksThroughConnection(context, checks_table, LookupRelation(context, checks_table));
	}

	vector<CheckSpec> specs;
	auto value_at = [&](idx_t row, idx_t col) -> const Value & {
		return rows[row][col];
	};
	auto str_at = [&](idx_t row, idx_t col) {
		auto &val = value_at(row, col);
		return val.IsNull() ? string() : val.ToString();
	};
	for (idx_t row = 0; row < rows.size(); row++) {
		CheckSpec spec;
		spec.check_name = str_at(row, 0);
		spec.check_type = str_at(row, 1);
		std::transform(spec.check_type.begin(), spec.check_type.end(), spec.check_type.begin(), ::tolower);
		spec.table_name = str_at(row, 2);
		spec.column_name = str_at(row, 3);
		spec.lower_threshold = value_at(row, 5);
		spec.upper_threshold = value_at(row, 6);
		spec.monitor_only = !value_at(row, 7).IsNull() && value_at(row, 7).GetValue<bool>();
		spec.identifier_column = str_at(row, 8);
		spec.filter_expr = str_at(row, 9);
		if (spec.check_name.empty()) {
			// Its result row could not be told apart from the other checks' rows
			spec.config_error = "every check needs a check_name";
		} else if (spec.table_name.empty()) {
			spec.config_error = "the check has no table_name";
		} else {
			try {
				ParseCheckParams(spec, str_at(row, 4));
			} catch (const std::exception &ex) {
				spec.config_error = ErrorData(ex).RawMessage();
			}
		}
		specs.push_back(std::move(spec));
	}
	if (specs.empty()) {
		throw BinderException("run_checks: checks table '" + checks_table + "' contains no checks");
	}
	// Checks sharing a name: their result rows (and a stored result history) would be
	// ambiguous, so none of them runs
	std::unordered_map<string, idx_t> name_counts;
	for (auto &spec : specs) {
		if (!spec.check_name.empty()) {
			name_counts[spec.check_name]++;
		}
	}
	for (auto &spec : specs) {
		auto count = spec.check_name.empty() ? 0 : name_counts[spec.check_name];
		if (count > 1) {
			spec.config_error = "check_name '" + spec.check_name + "' is used by " + std::to_string(count) +
			                    " checks; check names must be unique";
		}
	}
	return specs;
}

// Check whether an (unqualified) target table or view exists in the calling connection's
// catalogs — including the temp catalog, which a sibling connection cannot see.
// Qualified names are left to the binder.
static bool TargetTableExists(ClientContext &context, const string &name) {
	if (name.find('.') != string::npos) {
		return true;
	}
	try {
		EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, duckdb_compat::EntryName(name));
		if (Catalog::GetEntry(context, duckdb_compat::Ident(INVALID_CATALOG), duckdb_compat::Ident(INVALID_SCHEMA),
		                      lookup_info, OnEntryNotFound::RETURN_NULL)) {
			return true;
		}
	} catch (...) {
		// ambiguity or lookup errors: let the binder produce the real error
		return true;
	}
	try {
		EntryLookupInfo temp_lookup(CatalogType::TABLE_ENTRY, duckdb_compat::EntryName(name));
		if (Catalog::GetEntry(context, duckdb_compat::Ident(TEMP_CATALOG), duckdb_compat::Ident(INVALID_SCHEMA),
		                      temp_lookup, OnEntryNotFound::RETURN_NULL)) {
			return true;
		}
	} catch (...) {
		return true;
	}
	return false;
}

// Parse an integer parameter that arrived as a JSON string, with default
static int64_t ParseWindowDays(const CheckSpec &spec, int64_t default_days) {
	if (spec.window_days.empty()) {
		return default_days;
	}
	try {
		if (spec.window_days.find_first_not_of("0123456789") != string::npos) {
			throw std::invalid_argument("not an integer");
		}
		int64_t days = std::stoll(spec.window_days);
		if (days < 1) {
			throw std::invalid_argument("not positive");
		}
		if (days <= MAX_WINDOW_DAYS) {
			return days;
		}
	} catch (const std::exception &) {
		// std::stoll throws out_of_range for a number beyond BIGINT
		if (spec.window_days.find_first_not_of("0123456789") != string::npos ||
		    spec.window_days.find_first_not_of('0') == string::npos) {
			throw BinderException("run_checks: check '" + spec.check_name + "': params.window_days '" +
			                      spec.window_days + "' is not a positive integer");
		}
	}
	throw BinderException("run_checks: check '" + spec.check_name + "': params.window_days '" + spec.window_days +
	                      "' must be at most " + std::to_string(MAX_WINDOW_DAYS));
}

static void RequireColumn(const CheckSpec &spec) {
	if (spec.column_name.empty()) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (" + spec.check_type +
		                      ") requires a column_name");
	}
}

static void RequireParam(const CheckSpec &spec, const string &value, const string &param) {
	if (value.empty()) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (" + spec.check_type +
		                      ") requires params." + param);
	}
}

static string BuildAllowedArrayFromJson(const CheckSpec &spec) {
	if (!spec.has_allowed_values) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (" + spec.check_type +
		                      ") requires params.allowed_values");
	}
	string literal = "[]::VARCHAR[]";
	if (!spec.allowed_values.empty()) {
		string elements;
		for (auto &value : spec.allowed_values) {
			if (!elements.empty()) {
				elements += ", ";
			}
			elements += "'" + EscapeSqlStringLiteral(value) + "'";
		}
		literal = "[" + elements + "]::VARCHAR[]";
	}
	return literal;
}

// A column list of the checks table's column_name: a comma-separated string ('order_id,
// customer_id') or a JSON array of names ('["order_id", "customer_id"]'), as params.left_keys
static vector<string> ParseColumnList(const CheckSpec &spec, const string &text) {
	using namespace duckdb_yyjson;
	auto start = text.find_first_not_of(" \t\n\r");
	if (start == string::npos || text[start] != '[') {
		return ParseCommaSeparatedColumns(text);
	}
	struct YyjsonDocFree {
		void operator()(yyjson_doc *doc_handle) const {
			yyjson_doc_free(doc_handle);
		}
	};
	std::unique_ptr<yyjson_doc, YyjsonDocFree> doc(yyjson_read(text.c_str(), text.size(), 0));
	yyjson_val *root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
	vector<string> columns;
	bool valid = root && yyjson_is_arr(root);
	if (valid) {
		size_t idx, max;
		yyjson_val *element;
		yyjson_arr_foreach(root, idx, max, element) {
			if (!yyjson_is_str(element)) {
				valid = false;
				break;
			}
			columns.push_back(yyjson_get_str(element));
		}
	}
	if (!valid) {
		throw BinderException("run_checks: check '" + spec.check_name + "': column_name '" + text +
		                      "' must be a comma-separated list or a JSON array of column names");
	}
	return columns;
}

// The key columns of a duplicate_count check (column_name is a column list)
static vector<string> DuplicateKeyColumns(const CheckSpec &spec) {
	auto columns = ParseColumnList(spec, spec.column_name);
	if (columns.empty()) {
		throw BinderException("run_checks: check '" + spec.check_name + "' (duplicate_count) requires a column_name");
	}
	return columns;
}

// Bind `sql` (one SELECT) in the calling context without running it. Unlike a sibling
// connection this sees the caller's TEMP tables and uncommitted changes, like the final
// query does. Throws the binder's error for an unknown table, column or function.
static BoundStatement BindProbe(ClientContext &context, const string &sql) {
	auto statements = ParseWithContextSettings(context, sql);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw BinderException("run_checks: generated check query is not a single SELECT");
	}
	auto binder = Binder::CreateBinder(context);
	return binder->Bind(*statements[0]);
}

// filter_expr date tokens against a TIMESTAMP WITH TIME ZONE column. A token is a UTC DATE;
// compared with a TIMESTAMPTZ column it would be cast to the session time zone's midnight
// once ICU is loaded (ts >= ${yesterday} keeps other rows under 'America/Los_Angeles' than
// under 'UTC'). So in a comparison, BETWEEN or IN whose other operand is a TIMESTAMPTZ, the
// operand holding the token (a DATE or TIMESTAMP, e.g. ${today} - INTERVAL 6 HOUR) is turned
// into the instant it denotes in UTC. Against DATE and TIMESTAMP columns the token stays a
// DATE (a plain TIMESTAMP is UTC already).
namespace filter_tokens {

constexpr const char *TOKEN_PLACEHOLDER = "__anofox_date_token_";
constexpr const char *OPERAND_PLACEHOLDER = "__anofox_token_operand";
// The operand as a UTC instant: its text with a +00 offset, which both the core and the ICU
// casts read as UTC (to_timestamp(epoch(...)) would round through a DOUBLE)
constexpr const char *UTC_INSTANT_TEMPLATE =
    "CAST(strftime(CAST(__anofox_token_operand AS TIMESTAMP), '%Y-%m-%d %H:%M:%S.%f+00') AS TIMESTAMPTZ)";

using TokenSet = std::unordered_set<const ParsedExpression *>;

// One SQL expression parsed with the session's parser settings; nullptr when it does not parse
// as exactly one plain expression
static unique_ptr<ParsedExpression> ParseExpression(ClientContext &context, const string &sql) {
	vector<unique_ptr<SQLStatement>> statements;
	try {
		statements = ParseWithContextSettings(context, "SELECT (" + sql + ")");
	} catch (std::exception &) {
		return nullptr;
	}
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		return nullptr;
	}
	auto &select = statements[0]->Cast<SelectStatement>();
	if (!select.node || select.node->type != QueryNodeType::SELECT_NODE) {
		return nullptr;
	}
	auto &node = select.node->Cast<SelectNode>();
	if (node.select_list.size() != 1 || node.where_clause ||
	    (node.from_table && node.from_table->type != TableReferenceType::EMPTY_FROM)) {
		return nullptr;
	}
	return std::move(node.select_list[0]);
}

// Whether `expr` is the column reference `name` (a placeholder)
static bool IsPlaceholder(const ParsedExpression &expr, const string &name) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto text = expr.ToString();
	return text == name || text == "\"" + name + "\"";
}

// Replace the token placeholders by the UTC date SQL of their offset; `tokens` collects them
static void ReplaceTokenPlaceholders(ClientContext &context, unique_ptr<ParsedExpression> &expr,
                                     const vector<int64_t> &offsets, TokenSet &tokens) {
	for (idx_t i = 0; i < offsets.size(); i++) {
		if (IsPlaceholder(*expr, TOKEN_PLACEHOLDER + std::to_string(i))) {
			auto date = ParseExpression(context, UtcDateTokenSQL(offsets[i]));
			if (!date) {
				throw InternalException("run_checks: cannot parse the date token SQL");
			}
			expr = std::move(date);
			tokens.insert(expr.get());
			return;
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ReplaceTokenPlaceholders(context, child, offsets, tokens); });
}

static bool ContainsToken(const ParsedExpression &expr, const TokenSet &tokens) {
	if (tokens.count(&expr)) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		found = found || ContainsToken(child, tokens);
	});
	return found;
}

// The type `expr` binds to over the check's table; INVALID when it does not bind (the filter's
// own validation reports that)
static LogicalType ProbeType(ClientContext &context, const string &table_ref, const ParsedExpression &expr) {
	try {
		return BindProbe(context, "SELECT (" + expr.ToString() + ") FROM " + table_ref + " LIMIT 0").types[0];
	} catch (std::exception &) {
		return LogicalType(LogicalTypeId::INVALID);
	}
}

static bool IsDateOrPlainTimestamp(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return true;
	default:
		return false;
	}
}

// Replace the OPERAND_PLACEHOLDER in `wrapper` by `operand`
static void InsertOperand(unique_ptr<ParsedExpression> &wrapper, unique_ptr<ParsedExpression> &operand) {
	if (IsPlaceholder(*wrapper, OPERAND_PLACEHOLDER)) {
		wrapper = std::move(operand);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *wrapper, [&](unique_ptr<ParsedExpression> &child) { InsertOperand(child, operand); });
}

static void PinTokenComparisons(ClientContext &context, const string &table_ref, const TokenSet &tokens,
                                unique_ptr<ParsedExpression> &expr) {
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		PinTokenComparisons(context, table_ref, tokens, child);
	});
	auto expression_type = expr->GetExpressionType();
	bool compares = expr->GetExpressionClass() == ExpressionClass::COMPARISON ||
	                expr->GetExpressionClass() == ExpressionClass::BETWEEN ||
	                expression_type == ExpressionType::COMPARE_IN || expression_type == ExpressionType::COMPARE_NOT_IN;
	if (!compares) {
		return;
	}
	vector<unique_ptr<ParsedExpression> *> token_operands;
	bool against_timestamptz = false;
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &operand) {
		if (ContainsToken(*operand, tokens)) {
			token_operands.push_back(&operand);
		} else if (!against_timestamptz &&
		           ProbeType(context, table_ref, *operand).id() == LogicalTypeId::TIMESTAMP_TZ) {
			against_timestamptz = true;
		}
	});
	if (!against_timestamptz) {
		return;
	}
	for (auto operand : token_operands) {
		if (!IsDateOrPlainTimestamp(ProbeType(context, table_ref, **operand))) {
			continue;
		}
		auto wrapper = ParseExpression(context, UTC_INSTANT_TEMPLATE);
		if (!wrapper) {
			throw InternalException("run_checks: cannot parse the UTC instant template");
		}
		InsertOperand(wrapper, *operand);
		*operand = std::move(wrapper);
	}
}

} // namespace filter_tokens

// filter_expr with its date tokens expanded (see namespace filter_tokens). An expression the
// rewrite cannot handle keeps the plain token substitution.
static string ExpandFilterDateTokens(ClientContext &context, const CheckSpec &spec) {
	using namespace filter_tokens;
	const string location = "filter_expr of check '" + spec.check_name + "'";
	const string textual = SubstituteDateTokens(spec.filter_expr, location);
	vector<int64_t> offsets;
	auto with_placeholders = SubstituteDateTokens(spec.filter_expr, location, [&](int64_t day_offset) {
		offsets.push_back(day_offset);
		return string(TOKEN_PLACEHOLDER) + std::to_string(offsets.size() - 1);
	});
	if (offsets.empty()) {
		return textual;
	}
	auto root = ParseExpression(context, with_placeholders);
	if (!root) {
		return textual;
	}
	TokenSet tokens;
	ReplaceTokenPlaceholders(context, root, offsets, tokens);
	PinTokenComparisons(context, BuildQueryTableRef(spec.table_name), tokens, root);
	auto result = root->ToString();
	if (result.find(TOKEN_PLACEHOLDER) != string::npos) {
		// a token where the expression iterator does not reach (e.g. inside a subquery)
		AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: " + location + ": date tokens substituted as text");
		return textual;
	}
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: " + location + " expanded to " + result);
	return result;
}

// The per-branch pieces the outer uniform template is assembled from. A check either
// provides literal thresholds (from the checks table) or computes them in the inner
// query (metric_anomaly_iqr exposes its IQR bounds as lower/upper threshold columns).
struct CheckBranch {
	string inner_sql;            // derived table producing: [identifier,] value [, lower_threshold, upper_threshold]
	bool computed_thresholds = false;
	bool has_identifier = false;
	// The identifier_column as text (UtcTextSQL: a TIMESTAMPTZ is rendered as its UTC instant,
	// so the identifier and a stored history keyed by it do not depend on the session TimeZone)
	string identifier_sql;
	// A NULL value fails the check (with this message) instead of passing trivially.
	// freshness: without a timestamp the data cannot be shown to be fresh, the same
	// outcome as the standalone freshness check.
	bool null_value_fails = false;
	string null_value_message;
	// When set, the inner query also exposes nonfinite_count (the NaN/Inf inputs of the
	// value), which the message of a NaN value reports.
	string nonfinite_count_expr;
	// The inner query also exposes samples (VARCHAR[], up to SAMPLE_LIMIT offending values),
	// which the message of a failing check lists
	bool has_samples = false;
	// When set, the inner query also exposes __unevaluated (the rows on which the user's
	// expression failed at runtime and counted as non-compliant), which the message reports
	string unevaluated_expr;
	// SQL expressions over __c appended to the message (e.g. "; 2 row(s) skipped ...")
	vector<string> message_suffixes;
	// agg: the aggregate; when it is exact the inner query also exposes it as __exact, which
	// the thresholds are compared with and the message prints
	AggregateSQL agg;
	// Further columns of the simple aggregate path's inner query ("<expr> AS <name>")
	vector<string> extra_columns;
};

// How many offending values a failing values_in_set / duplicate_count / match_rate message shows
constexpr int SAMPLE_LIMIT = 5;

// The SAMPLE_LIMIT smallest distinct values of the VARCHAR expression `value_sql` on the rows
// where `condition_sql` holds (an aggregate)
static string SampleAggregateSQL(const string &value_sql, const string &condition_sql) {
	return "min(DISTINCT " + value_sql + ", " + std::to_string(SAMPLE_LIMIT) + ") FILTER (WHERE " + condition_sql +
	       ")";
}

// The VARCHAR text of a key: the value itself for one column, the ROW text '(a, b)' for several
static string KeyTextSQL(const vector<string> &key_exprs) {
	if (key_exprs.size() == 1) {
		return "COALESCE(CAST(" + key_exprs[0] + " AS VARCHAR), 'NULL')";
	}
	return "CAST(row(" + StringUtil::Join(key_exprs, ", ") + ") AS VARCHAR)";
}

static string ThresholdLiteral(const Value &threshold) {
	return DoubleThresholdSQL(threshold);
}

// Types that support identifier partitioning via a plain GROUP BY on the target table
static bool SupportsIdentifier(const string &check_type) {
	return check_type == "volume" || check_type == "null_rate" || check_type == "distinct_count" ||
	       check_type == "regex_match" || check_type == "values_in_set" || check_type == "agg" ||
	       check_type == "duplicate_count" || check_type == "compliance" || check_type == "freshness";
}

// Whether a user expression over the check's target table can be wrapped in TRY(). DuckDB
// rejects TRY() around a subquery or a volatile function (random()) at bind time; such an
// expression is evaluated unwrapped instead (a runtime error in it then aborts the call).
static bool ExpressionSupportsTry(ClientContext &context, const CheckSpec &spec, const string &expr) {
	try {
		BindProbe(context, "SELECT TRY((" + expr + ")) FROM " + BuildQueryTableRef(spec.table_name) + " LIMIT 0");
		return true;
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		if (StringUtil::Contains(error.RawMessage(), "TRY can not be used")) {
			AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: check '" + spec.check_name +
			                                       "': expression evaluated without TRY(): " + error.RawMessage());
			return false;
		}
		// Any other problem is reported by the probe of the whole check query
		return true;
	}
}

// `expr` as a boolean that is FALSE where it is NULL or, when TRY() can wrap it, where it fails
// at runtime
static string TolerantBooleanSQL(ClientContext &context, const CheckSpec &spec, const string &expr) {
	if (ExpressionSupportsTry(context, spec, expr)) {
		return "COALESCE(TRY((" + expr + ")), FALSE)";
	}
	return "COALESCE((" + expr + "), FALSE)";
}

// The UTC calendar date of a time-aware check's date column (UtcDateSQL, as the direct
// functions compute it): a TIMESTAMP WITH TIME ZONE column is bucketed by its UTC date without
// needing ICU, a VARCHAR column is parsed with TRY_CAST so a row holding 'n/a' is skipped
// (counted in skipped_sql) instead of aborting the suite. Another column type cannot hold a
// date: the check cannot run.
struct SuiteDate {
	string date_sql;
	// SQL counting the rows whose non-NULL date value did not parse; empty when every value does
	string skipped_sql;
	// SQL counting the rows with an infinite date (an SCD2 valid_to of the current version),
	// which date_sql turns into NULL: they are ignored like a row without a date
	string infinite_sql;
};

static SuiteDate SuiteDateSQL(ClientContext &context, const CheckSpec &spec, const string &filter_sql) {
	const string date_id = QuoteSqlIdentifier(spec.date_column);
	const string table_ref = BuildQueryTableRef(spec.table_name);
	// Bound in the caller's context: tables of attached databases, views and files included
	auto probe = BindProbe(context, "SELECT " + date_id + " FROM " + table_ref + " LIMIT 0");
	auto &type = probe.types[0];
	switch (type.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::VARCHAR:
		break;
	default:
		throw BinderException("run_checks: check '" + spec.check_name + "': params.date_column '" + spec.date_column +
		                      "' has type " + type.ToString() +
		                      "; expected DATE, TIMESTAMP, TIMESTAMP WITH TIME ZONE or a VARCHAR holding dates");
	}
	SuiteDate result;
	const string parsed_sql = UtcDateSQL(date_id, type);
	const string infinite = InfiniteTimeSQL(date_id, type, "DATE");
	const string filter_and = filter_sql.empty() ? "" : " AND (" + filter_sql + ")";
	result.date_sql = "CASE WHEN " + infinite + " THEN NULL ELSE " + parsed_sql + " END";
	result.infinite_sql = "(SELECT COUNT(*) FROM " + table_ref + " WHERE " + infinite + filter_and + ")";
	if (type.id() == LogicalTypeId::VARCHAR) {
		result.skipped_sql = "(SELECT COUNT(*) FROM " + table_ref + " WHERE " + date_id + " IS NOT NULL AND " +
		                     parsed_sql + " IS NULL" + filter_and + ")";
	}
	return result;
}

// Expose the count of rows a time-aware check skipped for an unparsable date (__skipped_dates)
// and ignored for an infinite one (__infinite_dates) next to the columns of its inner query,
// with the message suffixes that report them
static void AddSkippedDates(CheckBranch &branch, const CheckSpec &spec, const SuiteDate &date) {
	if (!date.skipped_sql.empty()) {
		branch.inner_sql =
		    "SELECT __d.*, " + date.skipped_sql + " AS __skipped_dates FROM (" + branch.inner_sql + ") __d";
		branch.message_suffixes.push_back("CASE WHEN __c.__skipped_dates > 0 THEN '; ' || CAST(__c.__skipped_dates AS "
		                                  "VARCHAR) || ' row(s) skipped (" +
		                                  EscapeSqlStringLiteral(spec.date_column) + " is not a date)' ELSE '' END");
	}
	branch.inner_sql =
	    "SELECT __e.*, " + date.infinite_sql + " AS __infinite_dates FROM (" + branch.inner_sql + ") __e";
	branch.message_suffixes.push_back("CASE WHEN __c.__infinite_dates > 0 THEN '; ' || CAST(__c.__infinite_dates AS "
	                                  "VARCHAR) || ' row(s) with an infinite date ignored' ELSE '' END");
}

// Build the inner derived table (value computation) for one check
static CheckBranch BuildCheckBranch(ClientContext &context, const CheckSpec &spec, const string &filter_sql) {
	CheckBranch branch;
	const string table_ref = BuildQueryTableRef(spec.table_name);
	const string where_clause = filter_sql.empty() ? "" : " WHERE (" + filter_sql + ")";
	const bool grouped = !spec.identifier_column.empty();
	branch.has_identifier = grouped;
	if (grouped) {
		const string id_ref = QuoteSqlIdentifier(spec.identifier_column);
		auto probe = BindProbe(context, "SELECT " + id_ref + " FROM " + table_ref + " LIMIT 0");
		branch.identifier_sql = UtcTextSQL(id_ref, probe.types[0]);
	}

	// Simple aggregate value expressions computed directly over the (filtered) target
	string value_expr;
	// The rows the simple aggregate path reads
	string source_sql = table_ref + where_clause;
	string samples_expr;
	if (spec.check_type == "volume") {
		value_expr = "CAST(COUNT(*) AS DOUBLE)";
	} else if (spec.check_type == "null_rate") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		value_expr = "COALESCE(CAST(SUM(CASE WHEN " + col + " IS NULL THEN 1 ELSE 0 END) AS DOUBLE) / "
		             "NULLIF(COUNT(*), 0), 0.0)";
	} else if (spec.check_type == "distinct_count") {
		RequireColumn(spec);
		value_expr = "CAST(COUNT(DISTINCT " + QuoteSqlIdentifier(spec.column_name) + ") AS DOUBLE)";
	} else if (spec.check_type == "regex_match") {
		RequireColumn(spec);
		RequireParam(spec, spec.pattern, "pattern");
		ValidateRegexPattern(context, spec.pattern, "params.pattern");
		string col = QuoteSqlIdentifier(spec.column_name);
		string pattern_lit = "'" + EscapeSqlStringLiteral(spec.pattern) + "'";
		value_expr = "CAST(SUM(CASE WHEN " + col + " IS NOT NULL AND regexp_matches(CAST(" + col +
		             " AS VARCHAR), " + pattern_lit + ") THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(" + col + "), 0)";
	} else if (spec.check_type == "values_in_set") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		string arr = BuildAllowedArrayFromJson(spec);
		value_expr = "CAST(SUM(CASE WHEN " + col + " IS NOT NULL AND list_contains(" + arr + ", CAST(" + col +
		             " AS VARCHAR)) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(" + col + "), 0)";
		samples_expr = SampleAggregateSQL("CAST(" + col + " AS VARCHAR)",
		                                 col + " IS NOT NULL AND NOT list_contains(" + arr + ", CAST(" + col +
		                                     " AS VARCHAR))");
		branch.has_samples = true;
	} else if (spec.check_type == "agg") {
		RequireColumn(spec);
		RequireParam(spec, spec.agg, "agg");
		string agg = spec.agg;
		std::transform(agg.begin(), agg.end(), agg.begin(), ::tolower);
		static const vector<string> ALLOWED_AGGS = {"avg", "min", "max", "sum", "median", "stddev"};
		if (std::find(ALLOWED_AGGS.begin(), ALLOWED_AGGS.end(), agg) == ALLOWED_AGGS.end()) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.agg must be one of 'avg', 'min', 'max', 'sum', 'median', 'stddev'");
		}
		string agg_upper = agg;
		std::transform(agg_upper.begin(), agg_upper.end(), agg_upper.begin(), ::toupper);
		string col = QuoteSqlIdentifier(spec.column_name);
		auto probe = BindProbe(context, "SELECT " + col + " FROM " + table_ref + " LIMIT 0");
		branch.agg = BuildAggregateSQL(agg_upper, col, probe.types[0]);
		value_expr = branch.agg.value_expr;
		branch.nonfinite_count_expr = branch.agg.nonfinite_count_expr;
		if (branch.agg.IsExact()) {
			branch.extra_columns.push_back(branch.agg.exact_expr + " AS __exact");
		}
	} else if (spec.check_type == "duplicate_count") {
		auto key_columns = DuplicateKeyColumns(spec);
		vector<string> key_exprs;
		for (auto &column : key_columns) {
			key_exprs.push_back(QuoteSqlIdentifier(column));
		}
		string value_sql = "CAST(" + BuildDuplicateCountExpr(key_columns) + " AS DOUBLE) AS value";
		string key_group = StringUtil::Join(key_exprs, ", ");
		// The duplicated keys (GROUP BY puts the NULL keys in one group, like the count)
		string dup_keys = "SELECT " + KeyTextSQL(key_exprs) + " AS k";
		if (grouped) {
			const string &id_col = branch.identifier_sql;
			dup_keys = "SELECT " + id_col + " AS identifier, " + KeyTextSQL(key_exprs) + " AS k FROM __base GROUP BY " +
			           id_col + ", " + key_group + " HAVING COUNT(*) > 1";
			branch.inner_sql = "WITH __base AS (SELECT * FROM " + table_ref + where_clause + "), "
			                   "__dup AS (" + dup_keys + "), "
			                   "__v AS (SELECT " + id_col + " AS identifier, " + value_sql + " FROM __base GROUP BY 1) "
			                   "SELECT __v.identifier, __v.value, (SELECT min(k, " + std::to_string(SAMPLE_LIMIT) +
			                   ") FROM __dup WHERE __dup.identifier IS NOT DISTINCT FROM __v.identifier) AS samples "
			                   "FROM __v";
		} else {
			dup_keys += " FROM __base GROUP BY " + key_group + " HAVING COUNT(*) > 1";
			branch.inner_sql = "WITH __base AS (SELECT * FROM " + table_ref + where_clause + "), "
			                   "__dup AS (" + dup_keys + ") "
			                   "SELECT " + value_sql + ", (SELECT min(k, " + std::to_string(SAMPLE_LIMIT) +
			                   ") FROM __dup) AS samples FROM __base";
		}
		branch.has_samples = true;
		return branch;
	} else if (spec.check_type == "compliance") {
		RequireParam(spec, spec.expression, "expression");
		ValidateBooleanExpression(context, spec.expression, "params.expression");
		// TRY(): a row on which the user's expression fails at runtime (e.g. a
		// CAST of free text) counts as non-compliant instead of aborting the suite
		if (ExpressionSupportsTry(context, spec, spec.expression)) {
			// One outcome per row: TRUE compliant, FALSE not (also for a NULL result), NULL where
			// the expression fails (TRY). The expression appears only once: DuckDB's common
			// subexpression elimination would move a repeated one out of the TRY().
			source_sql = "(SELECT *, TRY(COALESCE((" + spec.expression + "), FALSE)) AS __anofox_outcome FROM " +
			             table_ref + where_clause + ") __src";
			value_expr = "CAST(SUM(CASE WHEN __anofox_outcome THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0)";
			branch.unevaluated_expr = "COUNT(*) FILTER (WHERE __anofox_outcome IS NULL)";
		} else {
			value_expr = "CAST(SUM(CASE WHEN COALESCE((" + spec.expression +
			             "), FALSE) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0)";
		}
	} else if (spec.check_type == "freshness") {
		RequireColumn(spec);
		string col = QuoteSqlIdentifier(spec.column_name);
		// Everything in UTC TIMESTAMPs: subtracting a TIMESTAMP from a TIMESTAMP WITH TIME ZONE
		// needs ICU, and with ICU loaded a TIMESTAMPTZ->TIMESTAMP cast follows the session time
		// zone. A TIMESTAMPTZ column goes through its epoch (UtcTimestampSQL); other columns are taken as UTC.
		string ref_time = spec.reference_time_sql.empty() ? string(UTC_NOW_SQL) : spec.reference_time_sql;
		auto probe = BindProbe(context, "SELECT " + col + " FROM " + table_ref + " LIMIT 0");
		// try_parse: an unparseable text value must not abort the whole suite; text with a UTC
		// offset is the instant it names
		string ts_expr = UtcTimestampSQL(col, probe.types[0], true);
		// An infinite timestamp (an SCD2 valid_to of the current version) is no update time:
		// ignored, and the message says how many rows were
		string infinite = InfiniteTimeSQL(col, probe.types[0], "TIMESTAMP");
		value_expr = "CAST(EXTRACT(EPOCH FROM (" + ref_time + " - MAX(" + ts_expr + ") FILTER (WHERE NOT " + infinite +
		             "))) AS DOUBLE)";
		branch.extra_columns.push_back("COUNT(*) FILTER (WHERE " + infinite + ") AS __infinite");
		branch.message_suffixes.push_back("CASE WHEN __c.__infinite > 0 THEN '; ' || CAST(__c.__infinite AS "
		                                  "VARCHAR) || ' row(s) with an infinite timestamp ignored' ELSE '' END");
		branch.null_value_fails = true;
		branch.null_value_message = "No timestamp values found (table is empty or column is all NULL)";
	} else if (spec.check_type == "occurrence") {
		RequireColumn(spec);
		string mode = spec.mode.empty() ? "max" : spec.mode;
		std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
		if (mode != "max" && mode != "min") {
			throw BinderException("run_checks: check '" + spec.check_name + "': params.mode must be 'max' or 'min'");
		}
		string col = QuoteSqlIdentifier(spec.column_name);
		string extreme = (mode == "max") ? "MAX(c)" : "MIN(c)";
		branch.inner_sql = "WITH counts AS (SELECT CAST(" + col + " AS VARCHAR) AS v, COUNT(*) AS c FROM " +
		                   table_ref + " WHERE " + col + " IS NOT NULL" +
		                   (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") +
		                   " GROUP BY 1) SELECT CAST(" + extreme + " AS DOUBLE) AS value FROM counts";
		return branch;
	} else if (spec.check_type == "match_rate") {
		// The left keys: params.left_keys, else column_name
		if (spec.left_keys.empty()) {
			RequireColumn(spec);
		}
		RequireParam(spec, spec.right_table, "right_table");
		RequireParam(spec, spec.right_keys, "right_keys");
		auto left_keys = spec.left_keys.empty() ? ParseColumnList(spec, spec.column_name)
		                                        : ParseCommaSeparatedColumns(spec.left_keys);
		auto right_keys = ParseCommaSeparatedColumns(spec.right_keys);
		if (left_keys.empty() || left_keys.size() != right_keys.size()) {
			throw BinderException("run_checks: check '" + spec.check_name +
			                      "': params.left_keys and params.right_keys must list the same number of columns");
		}
		string left_cols, right_cols, join_cond;
		vector<string> left_key_exprs;
		// A NULL key matches nothing; null_rate reports it, the rate leaves it out
		vector<string> key_not_null, key_null;
		for (size_t i = 0; i < left_keys.size(); i++) {
			left_key_exprs.push_back("l.k" + std::to_string(i));
			key_not_null.push_back(QuoteSqlIdentifier(left_keys[i]) + " IS NOT NULL");
			key_null.push_back(QuoteSqlIdentifier(left_keys[i]) + " IS NULL");
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
		string filter_and = filter_sql.empty() ? "" : "(" + filter_sql + ") AND ";
		string null_keys = "(SELECT COUNT(*) FROM " + table_ref + " WHERE " + filter_and + "(" +
		                   StringUtil::Join(key_null, " OR ") + "))";
		branch.inner_sql = "SELECT CAST(COUNT(r.matched) AS DOUBLE) / NULLIF(COUNT(*), 0) AS value, " +
		                   SampleAggregateSQL(KeyTextSQL(left_key_exprs), "r.matched IS NULL") + " AS samples, " +
		                   null_keys + " AS __null_keys "
		                   "FROM (SELECT " + left_cols + " FROM " + table_ref + " WHERE " + filter_and +
		                   StringUtil::Join(key_not_null, " AND ") + ") l "
		                   "LEFT JOIN (SELECT DISTINCT " + right_cols + ", 1 AS matched FROM " +
		                   BuildQueryTableRef(spec.right_table) + ") r ON " + join_cond;
		branch.has_samples = true;
		branch.message_suffixes.push_back("CASE WHEN __c.__null_keys > 0 THEN '; ' || CAST(__c.__null_keys AS "
		                                  "VARCHAR) || ' row(s) with a NULL key excluded' ELSE '' END");
		return branch;
	} else if (spec.check_type == "rel_count_change") {
		RequireParam(spec, spec.date_column, "date_column");
		string cnt_expr = spec.count_column.empty()
		                      ? "COUNT(*)"
		                      : "COUNT(DISTINCT " + QuoteSqlIdentifier(spec.count_column) + ")";
		string ref_lit = spec.reference_date_sql.empty() ? "NULL" : spec.reference_date_sql;
		auto window_days = ParseWindowDays(spec, 7);
		auto date = SuiteDateSQL(context, spec, filter_sql);
		const string &date_sql = date.date_sql;
		branch.inner_sql =
		    "WITH daily AS (SELECT " + date_sql + " AS d, " + cnt_expr + " AS c FROM " + table_ref +
		    " WHERE " + date_sql + " IS NOT NULL" + (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") +
		    " GROUP BY 1), "
		    "ref AS (SELECT COALESCE(" + ref_lit + ", MAX(d)) AS ref_d FROM daily), "
		    "b AS (SELECT r.ref_d, (SELECT c FROM daily WHERE d = r.ref_d) AS ref_c, " +
		    BuildRelCountBaselineSQL("r.ref_d", window_days, spec.fill_missing_days) + " FROM ref r) "
		    "SELECT CAST(CASE WHEN baseline_avg IS NULL OR baseline_avg = 0 THEN NULL "
		    "ELSE (COALESCE(ref_c, 0) - baseline_avg) / baseline_avg END AS DOUBLE) AS value FROM b";
		AddSkippedDates(branch, spec, date);
		return branch;
	} else if (spec.check_type == "metric_anomaly_iqr") {
		RequireParam(spec, spec.date_column, "date_column");
		// The metric is params.metric_column or else column_name; without either the row count
		if (!spec.metric_column.empty() && !spec.column_name.empty() &&
		    !StringUtil::CIEquals(spec.metric_column, spec.column_name)) {
			throw BinderException("run_checks: check '" + spec.check_name + "': column_name '" + spec.column_name +
			                      "' and params.metric_column '" + spec.metric_column +
			                      "' name different columns; set only one");
		}
		const string &metric_column = spec.metric_column.empty() ? spec.column_name : spec.metric_column;
		string metric_expr = metric_column.empty() ? "CAST(COUNT(*) AS DOUBLE)"
		                                           : "AVG(CAST(" + QuoteSqlIdentifier(metric_column) + " AS DOUBLE))";
		string ref_lit = spec.reference_date_sql.empty() ? "NULL" : spec.reference_date_sql;
		string window_val = std::to_string(ParseWindowDays(spec, 30));
		string k_val = "1.5";
		if (!spec.k.empty()) {
			try {
				double k = std::stod(spec.k);
				if (!std::isfinite(k)) {
					throw std::invalid_argument("not finite");
				}
				k_val = spec.k;
			} catch (const std::exception &) {
				throw BinderException("run_checks: check '" + spec.check_name + "': params.k must be a finite number");
			}
		}
		auto date = SuiteDateSQL(context, spec, filter_sql);
		const string &date_sql = date.date_sql;
		branch.inner_sql =
		    "WITH daily AS (SELECT " + date_sql + " AS d, " + metric_expr + " AS m FROM " + table_ref +
		    " WHERE " + date_sql + " IS NOT NULL" + (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") +
		    " GROUP BY 1), "
		    "ref AS (SELECT COALESCE(" + ref_lit + ", MAX(d)) AS ref_d FROM daily), "
		    "w AS (SELECT m FROM daily, ref WHERE d < ref_d AND d >= ref_d - " + window_val + "), "
		    "s AS (SELECT QUANTILE_CONT(m, 0.25) AS q1, QUANTILE_CONT(m, 0.75) AS q3, COUNT(*) AS n FROM w) "
		    "SELECT CAST((SELECT m FROM daily, ref WHERE d = ref_d) AS DOUBLE) AS value, "
		    "CAST(CASE WHEN s.n = 0 THEN NULL ELSE s.q1 - " + k_val + " * (s.q3 - s.q1) END AS DOUBLE) AS lower_threshold, "
		    "CAST(CASE WHEN s.n = 0 THEN NULL ELSE s.q3 + " + k_val + " * (s.q3 - s.q1) END AS DOUBLE) AS upper_threshold "
		    "FROM s";
		branch.computed_thresholds = true;
		AddSkippedDates(branch, spec, date);
		return branch;
	} else if (spec.check_type == "rolling_values_in_set") {
		RequireColumn(spec);
		RequireParam(spec, spec.date_column, "date_column");
		string col = QuoteSqlIdentifier(spec.column_name);
		string date_id = QuoteSqlIdentifier(spec.date_column);
		string arr = BuildAllowedArrayFromJson(spec);
		string ref_lit = spec.reference_date_sql.empty() ? "NULL" : spec.reference_date_sql;
		string window_val = std::to_string(ParseWindowDays(spec, 7));
		auto date = SuiteDateSQL(context, spec, filter_sql);
		const string &date_sql = date.date_sql;
		branch.inner_sql =
		    "WITH ref AS (SELECT COALESCE(" + ref_lit + ", MAX(" + date_sql + ")) AS ref_d FROM " +
		    table_ref + "), "
		    "win AS (SELECT CAST(" + col + " AS VARCHAR) AS v FROM " + table_ref + ", ref WHERE " + col +
		    " IS NOT NULL AND " + date_id + " IS NOT NULL AND " + date_sql + " > ref_d - " + window_val +
		    " AND " + date_sql + " <= ref_d" +
		    (filter_sql.empty() ? "" : " AND (" + filter_sql + ")") + ") "
		    "SELECT CAST(SUM(CASE WHEN list_contains(" + arr +
		    ", v) THEN 1 ELSE 0 END) AS DOUBLE) / NULLIF(COUNT(*), 0) AS value FROM win";
		AddSkippedDates(branch, spec, date);
		return branch;
	} else {
		RequireSupportedCheckType(spec);
		throw InternalException("run_checks: check_type '%s' has no branch", spec.check_type);
	}

	// Simple aggregate path (value_expr set): optionally partition by the identifier column
	string select_list = value_expr + " AS value";
	if (!branch.nonfinite_count_expr.empty()) {
		select_list += ", " + branch.nonfinite_count_expr + " AS nonfinite_count";
	}
	if (!samples_expr.empty()) {
		select_list += ", " + samples_expr + " AS samples";
	}
	if (!branch.unevaluated_expr.empty()) {
		select_list += ", " + branch.unevaluated_expr + " AS __unevaluated";
	}
	for (auto &column : branch.extra_columns) {
		select_list += ", " + column;
	}
	if (grouped) {
		branch.inner_sql = "SELECT " + branch.identifier_sql + " AS identifier, " + select_list + " FROM " +
		                   source_sql + " GROUP BY 1";
	} else {
		branch.inner_sql = "SELECT " + select_list + " FROM " + source_sql;
	}
	return branch;
}

// The check_name of a result row as SQL: NULL for a check without one
static string CheckNameSQL(const CheckSpec &spec) {
	return spec.check_name.empty() ? "CAST(NULL AS VARCHAR)" : "'" + EscapeSqlStringLiteral(spec.check_name) + "'";
}

// Build a constant single-row error result for a check that cannot run
static string BuildErrorRowSQL(const CheckSpec &spec, const string &error_message) {
	string col_lit = spec.column_name.empty()
	                     ? "CAST(NULL AS VARCHAR)"
	                     : "'" + EscapeSqlStringLiteral(spec.column_name) + "'";
	return string("SELECT ") + UTC_NOW_SQL + " AS run_ts,"
	       + CheckNameSQL(spec) + " AS check_name, "
	       "'" + EscapeSqlStringLiteral(spec.check_type) + "' AS check_type, "
	       "'" + EscapeSqlStringLiteral(spec.table_name) + "' AS table_name, "
	       + col_lit + " AS column_name, "
	       "CAST(NULL AS VARCHAR) AS identifier, "
	       "CAST(NULL AS DOUBLE) AS value, "
	       "CAST(NULL AS DOUBLE) AS lower_threshold, "
	       "CAST(NULL AS DOUBLE) AS upper_threshold, "
	       "'error' AS status, "
	       "'" + EscapeSqlStringLiteral(error_message) + "' AS message";
}

// One row of the uniform result schema over `from_sql`, from the SQL expressions of its columns
static string UniformRowSQL(const CheckSpec &spec, const string &identifier_expr, const string &value_expr,
                            const string &lower_expr, const string &upper_expr, const string &status_expr,
                            const string &message_expr, const string &from_sql) {
	string col_lit = spec.column_name.empty()
	                     ? "CAST(NULL AS VARCHAR)"
	                     : "'" + EscapeSqlStringLiteral(spec.column_name) + "'";
	return string("SELECT ") + UTC_NOW_SQL + " AS run_ts,"
	       + CheckNameSQL(spec) + " AS check_name, "
	       "'" + EscapeSqlStringLiteral(spec.check_type) + "' AS check_type, "
	       "'" + EscapeSqlStringLiteral(spec.table_name) + "' AS table_name, "
	       + col_lit + " AS column_name, "
	       + identifier_expr + " AS identifier, "
	       + value_expr + " AS value, "
	       + lower_expr + " AS lower_threshold, "
	       + upper_expr + " AS upper_threshold, "
	       + status_expr + " AS status, "
	       + message_expr + " AS message "
	       "FROM " + from_sql;
}

// Wrap a check branch in the uniform result schema. `filter_sql` is the check's (tolerant)
// filter_expr, empty without one: the message then also reports how many rows it kept
// ("; filter matched <kept>/<total> rows"), and with params.fail_on_empty a check without rows
// to evaluate fails.
static string BuildBranchSQL(const CheckSpec &spec, const CheckBranch &branch, const string &filter_sql) {
	string identifier_expr = branch.has_identifier ? "__c.identifier" : "CAST(NULL AS VARCHAR)";
	string lower_expr = branch.computed_thresholds ? "__c.lower_threshold" : ThresholdLiteral(spec.lower_threshold);
	string upper_expr = branch.computed_thresholds ? "__c.upper_threshold" : ThresholdLiteral(spec.upper_threshold);
	string fail_status = spec.monitor_only ? "warn" : "fail";
	string below_lower = "(" + lower_expr + " IS NOT NULL AND __c.value < " + lower_expr + ")";
	string above_upper = "(" + upper_expr + " IS NOT NULL AND __c.value > " + upper_expr + ")";
	// An exact aggregate is compared and printed in its exact type
	const bool exact = branch.agg.IsExact();
	if (exact) {
		below_lower = BoundViolationSQL("__c.__exact", spec.lower_threshold, branch.agg, true);
		above_upper = BoundViolationSQL("__c.__exact", spec.upper_threshold, branch.agg, false);
	}
	string fail_cond = below_lower + " OR " + above_upper;
	// Neither threshold configured: the row only records the value
	const bool no_thresholds =
	    !branch.computed_thresholds && spec.lower_threshold.IsNull() && spec.upper_threshold.IsNull();
	string null_status = branch.null_value_fails ? fail_status : "pass";
	string null_message = branch.null_value_fails ? EscapeSqlStringLiteral(branch.null_value_message)
	                                              : "No value to evaluate; check passed trivially";
	// A NaN value (e.g. an average over a column holding NaN) cannot be shown to be within
	// the thresholds: it fails instead of passing every comparison.
	string type_lit = EscapeSqlStringLiteral(spec.check_type);
	string nan_message = "'" + type_lit + " value is NaN'";
	if (!branch.nonfinite_count_expr.empty()) {
		nan_message += " || ': the column contains ' || CAST(__c.nonfinite_count AS VARCHAR) || ' NaN/Inf value(s)'";
	}
	// freshness measures an age in seconds (the direct freshness() takes an INTERVAL instead)
	string unit = spec.check_type == "freshness" ? " || ' seconds'" : "";
	string value_text = exact ? "CAST(__c.__exact AS VARCHAR)" : RoundedNumberSQL("__c.value") + unit;
	string lower_text =
	    exact ? ThresholdTextSQL(spec.lower_threshold, branch.agg) : RoundedNumberSQL(lower_expr) + unit;
	string upper_text =
	    exact ? ThresholdTextSQL(spec.upper_threshold, branch.agg) : RoundedNumberSQL(upper_expr) + unit;
	string value_prefix = "'" + type_lit + " value ' || " + value_text;
	// "; samples: 'a', 'b'" when the check lists offending values and found some
	string samples_suffix = branch.has_samples ? " || CASE WHEN len(__c.samples) > 0 THEN '; samples: ''' || "
	                                             "array_to_string(__c.samples, ''', ''') || '''' ELSE '' END"
	                                           : "";
	string within_message = no_thresholds ? value_prefix + " || ': no thresholds configured (monitor)'"
	                                      : value_prefix + " || ' is within thresholds'";

	string status_sql = "CASE WHEN __c.value IS NULL THEN '" + null_status + "' "
	                    "     WHEN isnan(__c.value) THEN '" + fail_status + "' "
	                    "     WHEN " + fail_cond + " THEN '" + fail_status + "' "
	                    "     ELSE 'pass' END";
	string message_sql = "(CASE WHEN __c.value IS NULL THEN '" + null_message + "' "
	                     "     WHEN isnan(__c.value) THEN " + nan_message + " "
	                     "     WHEN " + below_lower + " THEN " + value_prefix + " || ' violates thresholds (' || " +
	                     value_text + " || ' < lower ' || " + lower_text + " || ')'" +
	                     samples_suffix + " "
	                     "     WHEN " + above_upper + " THEN " + value_prefix + " || ' violates thresholds (' || " +
	                     value_text + " || ' > upper ' || " + upper_text + " || ')'" +
	                     samples_suffix + " "
	                     "     ELSE " + within_message + " END)";
	if (!branch.unevaluated_expr.empty()) {
		message_sql += " || CASE WHEN __c.__unevaluated > 0 THEN '; ' || CAST(__c.__unevaluated AS VARCHAR) || "
		               "' row(s) could not be evaluated (counted as non-compliant)' ELSE '' END";
	}
	for (auto &suffix : branch.message_suffixes) {
		message_sql += " || " + suffix;
	}
	string value_sql = "__c.value";
	string from_sql = "(" + branch.inner_sql + ") __c";

	const bool has_filter = !filter_sql.empty();
	if (!has_filter && !spec.fail_on_empty) {
		return UniformRowSQL(spec, identifier_expr, value_sql, lower_expr, upper_expr, status_sql, message_sql,
		                     from_sql);
	}

	// The rows the check evaluates (__kept: after the filter) and, with a filter, the rows the
	// filter saw (__total); per partition for a check with an identifier_column
	const string table_ref = BuildQueryTableRef(spec.table_name);
	auto count_sql = [&](bool filtered, const string &identifier_ref) {
		vector<string> conditions;
		if (filtered) {
			conditions.push_back("(" + filter_sql + ")");
		}
		if (!identifier_ref.empty()) {
			conditions.push_back(branch.identifier_sql + " IS NOT DISTINCT FROM " + identifier_ref);
		}
		return "(SELECT COUNT(*) FROM " + table_ref +
		       (conditions.empty() ? "" : " WHERE " + StringUtil::Join(conditions, " AND ")) + ")";
	};
	auto counts_sql = [&](const string &identifier_ref) {
		string counts = count_sql(has_filter, identifier_ref) + " AS __kept";
		if (has_filter) {
			counts += ", " + count_sql(false, identifier_ref) + " AS __total";
		}
		return counts;
	};
	string filter_suffix = has_filter ? " || '; filter matched ' || CAST(__c.__kept AS VARCHAR) || '/' || "
	                                    "CAST(__c.__total AS VARCHAR) || ' rows'"
	                                  : "";
	string empty_message =
	    has_filter ? "'No rows to evaluate: filter matched 0/' || CAST(__c.__total AS VARCHAR) || ' rows (fail_on_empty)'"
	               : "'No rows to evaluate: table ''" + EscapeSqlStringLiteral(spec.table_name) +
	                     "'' is empty (fail_on_empty)'";
	message_sql += filter_suffix;
	if (spec.fail_on_empty) {
		value_sql = "CASE WHEN __c.__kept = 0 THEN NULL ELSE __c.value END";
		status_sql = "CASE WHEN __c.__kept = 0 THEN '" + fail_status + "' ELSE " + status_sql + " END";
		message_sql = "CASE WHEN __c.__kept = 0 THEN " + empty_message + " ELSE " + message_sql + " END";
	}
	from_sql = "(SELECT __i.*, " + counts_sql(branch.has_identifier ? "__i.identifier" : "") + " FROM (" +
	           branch.inner_sql + ") __i) __c";
	string sql =
	    UniformRowSQL(spec, identifier_expr, value_sql, lower_expr, upper_expr, status_sql, message_sql, from_sql);
	if (branch.has_identifier) {
		// A partitioned check has no partition when no row is left: one row still reports it
		string status = "'" + (spec.fail_on_empty ? fail_status : null_status) + "'";
		string message = spec.fail_on_empty ? empty_message : "'" + null_message + "'" + filter_suffix;
		sql += " UNION ALL " + UniformRowSQL(spec, "CAST(NULL AS VARCHAR)", "CAST(NULL AS DOUBLE)",
		                                     ThresholdLiteral(spec.lower_threshold),
		                                     ThresholdLiteral(spec.upper_threshold), status, message,
		                                     "(SELECT " + counts_sql("") + ") __c WHERE __c.__kept = 0");
	}
	return sql;
}

// The columns a check reads from its target table, as configured
static vector<string> ReferencedColumns(const CheckSpec &spec) {
	vector<string> columns;
	auto add = [&](const string &column) {
		if (!column.empty()) {
			columns.push_back(column);
		}
	};
	const auto &type = spec.check_type;
	if (type == "duplicate_count") {
		for (auto &column : ParseColumnList(spec, spec.column_name)) {
			add(column);
		}
	} else if (type == "match_rate") {
		for (auto &column : spec.left_keys.empty() ? ParseColumnList(spec, spec.column_name)
		                                           : ParseCommaSeparatedColumns(spec.left_keys)) {
			add(column);
		}
	} else if (type != "volume" && type != "compliance" && type != "rel_count_change") {
		add(spec.column_name);
	}
	add(spec.identifier_column);
	add(spec.date_column);
	add(spec.count_column);
	add(spec.metric_column);
	return columns;
}

// Resolve every configured column against the target table (case-insensitively, like
// DuckDB identifiers) so a missing column names the column and the table's columns.
static void ValidateCheckColumns(ClientContext &context, const CheckSpec &spec) {
	BoundStatement bound;
	try {
		bound = BindProbe(context, "SELECT * FROM " + BuildQueryTableRef(spec.table_name) + " LIMIT 0");
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		auto hint = QuotedTableNameHint(spec.table_name);
		if (hint.empty() || error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::FATAL ||
		    error.Type() == ExceptionType::INTERNAL) {
			throw;
		}
		if (error.Type() == ExceptionType::CATALOG) {
			throw BinderException("target table '%s' does not exist%s", spec.table_name, hint);
		}
		throw BinderException("cannot read target table '%s': %s%s", spec.table_name, FirstLine(error.RawMessage()),
		                      hint);
	}
	vector<string> available = duckdb_compat::Strs(bound.names);
	MatchColumnNames("run_checks", spec.table_name, available, ReferencedColumns(spec));
	if (spec.check_type == "agg" && !spec.column_name.empty()) {
		auto idx = MatchColumnNames("run_checks", spec.table_name, available, {spec.column_name})[0];
		if (!bound.types[idx].IsNumeric()) {
			throw BinderException("run_checks: column '%s' has type %s, agg needs a numeric column",
			                      available[idx], bound.types[idx].ToString());
		}
	}
}

static bool IsRateCheck(const string &check_type) {
	return check_type == "null_rate" || check_type == "regex_match" || check_type == "values_in_set" ||
	       check_type == "compliance" || check_type == "match_rate" || check_type == "rolling_values_in_set";
}

static void ValidateCheckThresholds(CheckSpec &spec) {
	// -inf lower / +inf upper: no bound, like NULL; NaN or the other infinity: an error row
	spec.lower_threshold = NormalizeBoundThreshold(spec.lower_threshold, "run_checks", "lower_threshold", true);
	spec.upper_threshold = NormalizeBoundThreshold(spec.upper_threshold, "run_checks", "upper_threshold", false);
	ValidateThresholdOrder(spec.lower_threshold, spec.upper_threshold, "run_checks", "lower_threshold",
	                       "upper_threshold");
	if (!IsRateCheck(spec.check_type)) {
		return;
	}
	auto validate_rate = [&](const Value &threshold, const char *name) {
		if (threshold.IsNull()) {
			return;
		}
		double rate = threshold.GetValue<double>();
		if (!(rate >= 0 && rate <= 1)) {
			throw BinderException("run_checks: %s must be between 0 and 1 for a %s check (got %s)", name,
			                      spec.check_type, threshold.ToString());
		}
	};
	validate_rate(spec.lower_threshold, "lower_threshold");
	validate_rate(spec.upper_threshold, "upper_threshold");
}

// The SQL of one check in the uniform result schema. Every problem that keeps the check
// from running (missing table or column, invalid configuration, a query that does not
// bind) is thrown here and turned into an 'error' row by the caller.
static string BuildValidatedCheckSQL(ClientContext &context, CheckSpec spec) {
	RequireSupportedCheckType(spec);
	if (!spec.config_error.empty()) {
		throw BinderException(spec.config_error);
	}
	if (!TargetTableExists(context, spec.table_name)) {
		throw BinderException("target table '%s' does not exist%s", spec.table_name,
		                      QuotedTableNameHint(spec.table_name));
	}
	ValidateCheckColumns(context, spec);
	if (!spec.identifier_column.empty() && !SupportsIdentifier(spec.check_type)) {
		throw BinderException("identifier_column is not supported for check_type '%s'", spec.check_type);
	}
	ValidateCheckThresholds(spec);
	// A reference at infinity has no days before it and no age
	if (StringUtil::Contains(spec.reference_date_sql, "infinity") ||
	    StringUtil::Contains(spec.reference_time_sql, "infinity")) {
		throw BinderException("params.%s must be a finite %s",
		                      spec.reference_date_sql.empty() ? "reference_time" : "reference_date",
		                      spec.reference_date_sql.empty() ? "timestamp" : "date");
	}

	string filter_sql;
	if (!spec.filter_expr.empty()) {
		filter_sql = ExpandFilterDateTokens(context, spec);
		ValidateBooleanExpression(context, filter_sql, "filter_expr");
		// A row on which the filter fails at runtime is filtered out instead of
		// aborting the suite
		filter_sql = TolerantBooleanSQL(context, spec, filter_sql);
	}
	auto sql = BuildBranchSQL(spec, BuildCheckBranch(context, spec, filter_sql), filter_sql);
	BindProbe(context, sql);
	return sql;
}

// "check '<name>' (<type>) cannot run: <cause>", with the run_checks/check prefixes of the
// cause dropped and only its first line kept (binder errors append the query context)
static string CheckErrorMessage(const CheckSpec &spec, string cause) {
	auto newline = cause.find('\n');
	if (newline != string::npos) {
		cause = cause.substr(0, newline);
	}
	const string check_prefix = spec.check_name.empty() ? "check without check_name" : "check '" + spec.check_name + "'";
	for (auto &prefix : {string("run_checks: ") + check_prefix + " (" + spec.check_type + ")",
	                     string("run_checks: ") + check_prefix, string("run_checks: ")}) {
		if (StringUtil::StartsWith(cause, prefix)) {
			cause = cause.substr(prefix.size());
			break;
		}
	}
	while (!cause.empty() && (cause[0] == ':' || cause[0] == ' ')) {
		cause.erase(0, 1);
	}
	return check_prefix + " (" + spec.check_type + ") cannot run: " + cause;
}

} // anonymous namespace

//===--------------------------------------------------------------------===//
// run_checks Bind Replace
//===--------------------------------------------------------------------===//

static unique_ptr<TableRef> RunChecksBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("run_checks");
	if (input.inputs.empty()) {
		throw BinderException("anofox_tab_run_checks requires 1 argument: checks_table");
	}
	string checks_table = RequireNonNullArgument(input, 0, "run_checks", "checks_table");

	auto specs = ReadCheckSpecs(context, checks_table);
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: running " + std::to_string(specs.size()) +
	                                       " check(s) from '" + checks_table + "'");

	vector<string> branches;
	idx_t error_count = 0;
	for (auto &spec : specs) {
		// Per-check error isolation: a check that cannot be built or bound yields an
		// 'error' row; the other checks run normally.
		try {
			branches.push_back(BuildValidatedCheckSQL(context, spec));
		} catch (const std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::INTERRUPT || error.Type() == ExceptionType::FATAL ||
			    error.Type() == ExceptionType::INTERNAL) {
				throw;
			}
			auto message = CheckErrorMessage(spec, error.RawMessage());
			// The message is in the result row; the trace is for debugging only
			AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: " + message);
			branches.push_back(BuildErrorRowSQL(spec, message));
			error_count++;
		}
	}
	AnofoxTrace(AnofoxLogLevel::Debug, "check_suite: " + std::to_string(error_count) + " of " +
	                                       std::to_string(specs.size()) + " check(s) cannot run");

	string sql;
	for (size_t i = 0; i < branches.size(); i++) {
		if (i > 0) {
			sql += " UNION ALL ";
		}
		sql += "(" + branches[i] + ")";
	}
	// A stable order: by check, then by partition (the NULL partition last)
	sql = "SELECT * FROM (" + sql + ") __suite ORDER BY check_name NULLS LAST, identifier NULLS LAST";
	return ParseSubquery(sql, context, "Failed to parse check suite query");
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterCheckSuiteFunctions(ExtensionLoader &loader) {
	// anofox_tab_run_checks(checks_table) (alias: run_checks)
	TableFunction run_checks_func("anofox_tab_run_checks", {LogicalType(LogicalTypeId::VARCHAR)}, nullptr, nullptr);
	run_checks_func.bind_replace = DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, RunChecksBindReplace);
	{
		FunctionDescription desc;
		desc.description =
		    "Runs every check defined in a checks table (columns: check_name, check_type, table_name, column_name, "
		    "params JSON, lower_threshold, upper_threshold, monitor_only, identifier_column, filter_expr) and returns "
		    "one uniform result row per check and identifier partition (run_ts, check_name, check_type, table_name, "
		    "column_name, identifier, value, lower_threshold, upper_threshold, status, message). The checks table "
		    "must be a regular (non-temporary) table; target tables may be temporary.";
		desc.parameter_names = {"checks_table"};
		desc.parameter_types = {LogicalType::VARCHAR};
		desc.examples = {"SELECT * FROM run_checks('dq_checks');",
		                 "INSERT INTO dq_results SELECT * FROM run_checks('dq_checks');"};
		desc.categories = {"metric", "data-quality"};
		RegisterTableFunctionWithAlias(loader, run_checks_func, "run_checks", {std::move(desc)});
	}
}

} // namespace anofox
} // namespace duckdb

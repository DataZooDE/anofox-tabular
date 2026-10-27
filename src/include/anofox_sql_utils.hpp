#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {
namespace anofox {

string EscapeSqlStringLiteral(const string &value);
string QuoteSqlIdentifier(const string &identifier);
string BuildQueryTableRef(const string &table_name);

// The statements of `sql`, parsed with the context's parser settings on every
// DuckDB line: 2.0 builds a Parser from the ClientContext (GetParserOptions is
// private there), 1.5.6 from the context's ParserOptions.
vector<unique_ptr<SQLStatement>> ParseWithContextSettings(ClientContext &context, const string &sql);

// Run `sql` on `con` with a streaming result: Connection::SendQuery before 2.0,
// Connection::Submit on 2.0 (SendQuery is gone there).
unique_ptr<QueryResult> StreamQuery(Connection &con, const string &sql);

// Parse a generated SQL query into a SubqueryRef for bind_replace table functions.
// Rejects anything that is not exactly one SELECT statement.
unique_ptr<SubqueryRef> ParseSubquery(const string &query, ClientContext &context, const string &err_msg);

// Reject NaN/Inf double parameters at bind time with a clear error.
// NULL values are left alone (callers substitute their documented defaults).
void ValidateFiniteDouble(const Value &value, const string &function_name, const string &parameter_name);

// The value of a required table-function argument as a string. A NULL value (also a NULL
// parameter of a prepared statement) is a BinderException
// "<function_name>: <argument_name> must not be NULL" instead of being read as the
// identifier NULL. A missing positional argument is reported the same way.
string RequireNonNullArgument(const TableFunctionBindInput &input, idx_t index, const string &function_name,
                              const string &argument_name);
string RequireNonNullArgument(const Value *value, const string &function_name, const string &argument_name);

// A required name argument (a table or column name): RequireNonNullArgument, and
// BinderException "<function_name>: <argument_name> must not be empty" for an empty or blank name.
string RequireNameArgument(const TableFunctionBindInput &input, idx_t index, const string &function_name,
                           const string &argument_name);

// Threshold validation shared by the check functions. NULL values are left alone (a NULL
// threshold means "no bound" or the documented default).
// "<fn>: <lower_name> (<lower>) must not be greater than <upper_name> (<upper>)"
void ValidateThresholdOrder(const Value &lower, const Value &upper, const string &function_name,
                            const string &lower_name, const string &upper_name);
// "<fn>: <name> must not be negative (got <v>)"
void ValidateNonNegative(const Value &value, const string &function_name, const string &parameter_name);
// "<fn>: <name> must be between 0 and 1 (got <v>)"; also rejects NaN/Inf
void ValidateRate(const Value &value, const string &function_name, const string &parameter_name);

// SQL expression rendering a DOUBLE expression for a human-readable message: rounded to
// three decimals (0.833 instead of 0.8333333333333334); values below 0.001 keep their full
// precision so a small non-zero rate never prints as 0.0.
string RoundedNumberSQL(const string &double_expr);

// Lowercase an option string such as output_mode ('SCORES' -> 'scores').
string LowerOption(const string &value);

// Parse a comma-separated column list, trimming whitespace and surrounding quotes.
vector<string> ParseCommaSeparatedColumns(const string &columns_str);

// A column of a table as resolved by ResolveColumns: the table's own spelling and its type.
struct ResolvedColumn {
	string name;
	LogicalType type;
};

// Match `requested` column names against the columns of a relation (`available_names`)
// case-insensitively, like DuckDB's own identifier resolution. Returns the index into
// `available_names` of each requested name, in request order. An unknown name throws
// BinderException("<function_name>: column '<name>' not found in table '<table_name>' ...").
vector<idx_t> MatchColumnNames(const string &function_name, const string &table_name,
                               const vector<string> &available_names, const vector<string> &requested);

// Resolve `requested` column names of `table_name` (see MatchColumnNames). The relation is
// bound with a sibling connection (LIMIT 0 reads no rows), so like the functions that read
// through such a connection it cannot see TEMP tables of the calling connection: a name that
// resolves to a TEMP table in the caller's context is rejected (RejectTempTable).
vector<ResolvedColumn> ResolveColumns(ClientContext &context, const string &function_name, const string &table_name,
                                      const vector<string> &requested);

// A table or view looked up through the catalog of the calling context (TEMP tables and
// the search path included, no sibling connection).
struct RelationInfo {
	//! false when the name is not a catalog table or view (e.g. a file read by a replacement scan)
	bool found = false;
	//! the relation lives in the temp catalog of the calling connection
	bool is_temp = false;
	bool is_view = false;
	vector<ResolvedColumn> columns;
};

// Look up `table_name` (optionally catalog/schema qualified) in the caller's context.
// Never throws: a missing relation, catalog or schema yields found = false.
RelationInfo LookupRelation(ClientContext &context, const string &table_name);

// The catalog entry (table or view) `table_name` resolves to in the caller's context, or
// nullptr. Never throws. Resolved like DuckDB's binder: a two-part name x.t is table t of
// schema x in the search path, else table t of the attached database x (default schema).
optional_ptr<CatalogEntry> LookupRelationEntry(ClientContext &context, const string &table_name);

// The column `column_name` (case-insensitive) of `relation`, or nullptr.
const ResolvedColumn *FindColumn(const RelationInfo &relation, const string &column_name);

// Functions that read their input through a sibling connection (Connection con(*context.db))
// cannot see the TEMP tables of the calling connection. When `table_name` resolves to a TEMP
// table in the caller's context this throws
// BinderException("TEMP tables are not supported by <function_name>; use a regular table").
void RejectTempTable(ClientContext &context, const string &function_name, const string &table_name);

// Functions that read their input through a sibling connection see the last committed data
// only. Inside an explicit transaction that has written to a database (INSERT/UPDATE/DELETE,
// CREATE TABLE, ...; writes to TEMP objects do not count) that data would silently miss the
// caller's changes, so this throws BinderException("<function_name> reads the last committed
// data, but this transaction has uncommitted changes; ..."). Autocommit statements and
// read-only transactions pass.
void RejectUncommittedChanges(ClientContext &context, const string &function_name);

// Bind-time check of a statistical check's input column: the column must exist in
// `table_name` (else the MatchColumnNames error) and be numeric or BOOLEAN (else
// "<function_name>: column '<c>' must be numeric, got <TYPE>"). Relations the catalog does
// not know (replacement scans) are not checked here.
void RequireNumericColumn(ClientContext &context, const string &function_name, const string &table_name,
                          const string &column_name);

// Bind-time validation of the input relation of a direct check (a bind_replace function that
// reads `table_name` through query_table in the caller's context). The relation is bound in the
// calling context (TEMP tables, views, files and replacement scans included; no rows are read):
// - an unknown table: BinderException "<fn>: table '<t>' does not exist"
// - any other bind failure: BinderException "<fn>: cannot read table '<t>': <cause>"
// - each of `columns` must exist (case-insensitively; else the MatchColumnNames error)
// Returns the requested columns (the table's spelling and type), in request order.
vector<ResolvedColumn> RequireRelationColumns(ClientContext &context, const string &function_name,
                                              const string &table_name, const vector<string> &columns = {});

// The type of `column_name` in `table_name` as the caller's context sees it, or
// LogicalTypeId::INVALID when it cannot be determined at bind time.
LogicalType LookupColumnType(ClientContext &context, const string &table_name, const string &column_name);

// SQL expression for `column_sql` as a plain TIMESTAMP in UTC. A TIMESTAMP WITH TIME ZONE
// column is converted through its epoch, so the result is the same with and without the ICU
// extension and whatever the session TimeZone (a plain CAST converts to the session time
// zone when ICU is loaded). A VARCHAR value with a UTC offset ('2026-10-29 04:00:00+02') is
// the instant it names; text without an offset, and the other types, are read as UTC as they
// are. `try_parse`: text that does not parse is NULL instead of a conversion error.
string UtcTimestampSQL(const string &column_sql, const LogicalType &type, bool try_parse = false);
// SQL expression for the UTC calendar date of `column_sql` (see UtcTimestampSQL). A VARCHAR
// column is parsed with TRY semantics: a value that is not a date yields NULL.
string UtcDateSQL(const string &column_sql, const LogicalType &type);
// SQL expression for `column_sql` as text that does not depend on the session TimeZone: a
// TIMESTAMP WITH TIME ZONE as its UTC instant with a '+00' suffix (what DuckDB prints under
// TimeZone 'UTC'), other types CAST AS VARCHAR.
string UtcTextSQL(const string &column_sql, const LogicalType &type);
// The current time as a plain UTC TIMESTAMP, independent of ICU and the session TimeZone.
string UtcNowSQL();

// Exact centring for spread statistics. A BIGINT or DECIMAL value above 2^53 loses its low
// digits when cast to DOUBLE (9007199254740992 and 9007199254740993 become equal), so a mean,
// standard deviation, quantile or correlation computed on the DOUBLE cast can report a
// constant column. Subtracting a reference value in the column's exact type first keeps the
// differences exact; the statistics are then computed on the DOUBLE differences (spread,
// z-scores and correlations do not change under a shift) and a location is reference + result.
//
// The exact type to centre a column of type `type_name` in: "HUGEINT" for the signed and
// unsigned integer types up to HUGEINT, "DECIMAL(38,<scale>)" for a DECIMAL, "" when the column
// is cast to DOUBLE as it is (FLOAT, DOUBLE, UHUGEINT, unknown types).
string ExactCenteringType(const string &type_name);
// Aggregate SQL for the reference value of `column_sql` in `centering_type`: the midpoint of
// MIN and MAX for HUGEINT (min // 2 + max // 2, which cannot overflow, so the differences fit),
// MIN for DECIMAL.
string CenteringReferenceSQL(const string &column_sql, const string &centering_type);
// SQL for the difference of `column_sql` to `reference_sql`, computed in `centering_type` and
// cast to DOUBLE.
string CenteredDoubleSQL(const string &column_sql, const string &centering_type, const string &reference_sql);

// Threshold literals of the check functions with a fixed output type: the value (or
// `null_default` when the value is NULL) as CAST(... AS DOUBLE) / CAST(... AS BIGINT), so a
// threshold column has the same type whatever literal the caller passed (0.015 would
// otherwise be a DECIMAL(4,3), 1 an INTEGER).
string DoubleThresholdSQL(const Value &value, const string &null_default = "NULL");
string BigintThresholdSQL(const Value &value, const string &null_default = "NULL");

} // namespace anofox
} // namespace duckdb

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
// through such a connection it cannot see TEMP tables of the calling connection.
vector<ResolvedColumn> ResolveColumns(ClientContext &context, const string &function_name, const string &table_name,
                                      const vector<string> &requested);

} // namespace anofox
} // namespace duckdb

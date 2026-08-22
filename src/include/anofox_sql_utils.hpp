#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

namespace duckdb {
namespace anofox {

string EscapeSqlStringLiteral(const string &value);
string QuoteSqlIdentifier(const string &identifier);
string BuildQueryTableRef(const string &table_name);

// Parse a generated SQL query into a SubqueryRef for bind_replace table functions.
// Rejects anything that is not exactly one SELECT statement.
unique_ptr<SubqueryRef> ParseSubquery(const string &query, const ParserOptions &options, const string &err_msg);

// Reject NaN/Inf double parameters at bind time with a clear error.
// NULL values are left alone (callers substitute their documented defaults).
void ValidateFiniteDouble(const Value &value, const string &function_name, const string &parameter_name);

// Parse a comma-separated column list, trimming whitespace and surrounding quotes.
vector<string> ParseCommaSeparatedColumns(const string &columns_str);

} // namespace anofox
} // namespace duckdb

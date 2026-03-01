#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

string EscapeSqlStringLiteral(const string &value);
string QuoteSqlIdentifier(const string &identifier);
string BuildQueryTableRef(const string &table_name);

} // namespace anofox
} // namespace duckdb

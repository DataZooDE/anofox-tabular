#include "anofox_sql_utils.hpp"

namespace duckdb {
namespace anofox {

string EscapeSqlStringLiteral(const string &value) {
	string escaped;
	escaped.reserve(value.size());
	for (char c : value) {
		if (c == '\'') {
			escaped += "''";
		} else {
			escaped += c;
		}
	}
	return escaped;
}

string QuoteSqlIdentifier(const string &identifier) {
	string escaped;
	escaped.reserve(identifier.size() + 2);
	for (char c : identifier) {
		if (c == '"') {
			escaped += "\"\"";
		} else {
			escaped += c;
		}
	}
	return "\"" + escaped + "\"";
}

string BuildQueryTableRef(const string &table_name) {
	return "query_table('" + EscapeSqlStringLiteral(table_name) + "')";
}

} // namespace anofox
} // namespace duckdb

#include "anofox_sql_utils.hpp"

#include "duckdb/common/exception.hpp"

#include <cmath>

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

namespace {
// 2.0: Parser(ClientContext &)
template <class P = Parser, class CONTEXT>
auto ParseWith(CONTEXT &context, const string &sql, int) -> decltype(P(context), vector<unique_ptr<SQLStatement>>()) {
	P parser(context);
	parser.ParseQuery(sql);
	return std::move(parser.statements);
}
// 1.5.6: Parser(ParserOptions), from the context's settings
template <class P = Parser, class CONTEXT>
vector<unique_ptr<SQLStatement>> ParseWith(CONTEXT &context, const string &sql, long) {
	P parser(context.GetParserOptions());
	parser.ParseQuery(sql);
	return std::move(parser.statements);
}
// 2.0: Connection::Submit
template <class CONNECTION>
auto StreamWith(CONNECTION &con, const string &sql, int) -> decltype(con.Submit(sql)) {
	return con.Submit(sql);
}
// before 2.0: Connection::SendQuery
template <class CONNECTION>
auto StreamWith(CONNECTION &con, const string &sql, long) -> decltype(con.SendQuery(sql)) {
	return con.SendQuery(sql);
}
} // namespace

unique_ptr<QueryResult> StreamQuery(Connection &con, const string &sql) {
	return StreamWith(con, sql, 0);
}

vector<unique_ptr<SQLStatement>> ParseWithContextSettings(ClientContext &context, const string &sql) {
	return ParseWith(context, sql, 0);
}

unique_ptr<SubqueryRef> ParseSubquery(const string &query, ClientContext &context, const string &err_msg) {
	auto statements = ParseWithContextSettings(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto select_stmt = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(statements[0]));
	return make_uniq<SubqueryRef>(std::move(select_stmt));
}

void ValidateFiniteDouble(const Value &value, const string &function_name, const string &parameter_name) {
	if (value.IsNull()) {
		return;
	}
	double val = value.GetValue<double>();
	if (!std::isfinite(val)) {
		throw BinderException(function_name + ": " + parameter_name + " must be a finite number");
	}
}

vector<string> ParseCommaSeparatedColumns(const string &columns_str) {
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

} // namespace anofox
} // namespace duckdb

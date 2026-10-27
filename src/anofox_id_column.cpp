#include "anofox_id_column.hpp"

#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

std::string IdColumn::SelectSuffix() const {
	return Enabled() ? ", " + QuoteSqlIdentifier(name) : std::string();
}

void AddIdColumnParameter(TableFunction &function) {
	duckdb_compat::AddNamedParameter(function, ID_COLUMN_PARAMETER, LogicalType(LogicalTypeId::VARCHAR));
}

IdColumn BindIdColumn(ClientContext &context, TableFunctionBindInput &input, const std::string &function_name,
                      const std::string &table_name, bool row_level, const std::string &row_level_mode) {
	IdColumn id_column;
	auto param = duckdb_compat::FindNamedParameter(input.named_parameters, ID_COLUMN_PARAMETER);
	if (!param || param->IsNull()) {
		return id_column;
	}
	auto requested = param->ToString();
	if (requested.empty()) {
		throw BinderException("%s: id_column must be a non-empty column name", function_name);
	}
	if (!row_level) {
		throw BinderException("%s: id_column requires output_mode '%s'", function_name, row_level_mode);
	}

	// Resolve the column against the source table with a sibling connection
	// (LIMIT 0 only binds the query); the match is case-insensitive like
	// DuckDB's own column resolution, the reported name is the table's
	auto con = OpenSiblingConnection(context);
	auto result = con.Query("SELECT * FROM " + BuildQueryTableRef(table_name) + " LIMIT 0");
	if (result->HasError()) {
		throw BinderException("%s: failed to resolve id_column '%s' in table '%s': %s", function_name, requested,
		                      table_name, result->GetError());
	}
	auto result_names = duckdb_compat::ResultNames(*result);
	auto &result_types = duckdb_compat::ResultTypes(*result);
	for (idx_t i = 0; i < result_names.size(); i++) {
		if (StringUtil::CIEquals(result_names[i], requested)) {
			id_column.name = result_names[i];
			id_column.type = result_types[i];
			break;
		}
	}
	if (!id_column.Enabled()) {
		throw BinderException("%s: id_column '%s' not found in table '%s'", function_name, requested, table_name);
	}

	AnofoxTrace(AnofoxLogLevel::Debug, function_name + ": id_column '" + id_column.name + "' (" +
	                                       id_column.type.ToString() + ") passed through from '" + table_name + "'");
	return id_column;
}

void InsertIdColumn(const IdColumn &id_column, const std::string &function_name, vector<LogicalType> &return_types,
                    duckdb_compat::BindNames &names) {
	if (!id_column.Enabled()) {
		return;
	}
	D_ASSERT(!names.empty() && duckdb_compat::Str(names[0]) == "row_id");
	for (auto &name : names) {
		if (StringUtil::CIEquals(duckdb_compat::Str(name), id_column.name)) {
			throw BinderException("%s: id_column '%s' collides with output column '%s'; rename or alias it first",
			                      function_name, id_column.name, duckdb_compat::Str(name));
		}
	}
	names.emplace(names.begin() + 1, id_column.name);
	return_types.insert(return_types.begin() + 1, id_column.type);
}

} // namespace anofox
} // namespace duckdb

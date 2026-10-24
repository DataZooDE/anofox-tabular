#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb_compat.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

//===--------------------------------------------------------------------===//
// id_column: pass an identifier column through row-level outputs (#108)
//===--------------------------------------------------------------------===//
//
// Row-level table functions (isolation_forest / isolation_forest_mv 'scores',
// dbscan / dbscan_mv 'clusters', outlier_tree 'outliers', pii_audit_table)
// identify rows by a positional row_id. With the optional named parameter
// `id_column := '<col>'` the value of that column is added to every output
// row, directly after row_id, with the column's own name and type.
//
// Alignment contract: the id must be read in the SAME internal query as the
// analyzed values (append IdColumn::SelectSuffix() to the select list) and
// captured for every row that is analyzed, so rows skipped for NULL features
// cannot shift the ids. NULL ids are passed through; they never filter rows.

//! Named parameter name, as written in SQL.
constexpr const char *ID_COLUMN_PARAMETER = "id_column";

struct IdColumn {
	//! Column name as resolved against the source table ("" = not requested)
	std::string name;
	//! The column's type in the source table
	LogicalType type;

	bool Enabled() const {
		return !name.empty();
	}

	//! ", \"<name>\"" to append to an internal select list, "" when disabled
	std::string SelectSuffix() const;
};

//! Declare the optional `id_column` named parameter on a table function.
void AddIdColumnParameter(TableFunction &function);

//! Resolve the `id_column` named parameter at bind time.
//! row_level tells whether the selected output mode emits one row per input
//! row; row_level_mode names that mode for the error message. Throws a
//! BinderException for a non-row-level mode or a column the table lacks.
IdColumn BindIdColumn(ClientContext &context, TableFunctionBindInput &input, const std::string &function_name,
                      const std::string &table_name, bool row_level, const std::string &row_level_mode);

//! Insert the id column into the bound schema directly after row_id (which
//! must be the first output column). Throws a BinderException when the id
//! column's name collides with an output column. No-op when disabled.
void InsertIdColumn(const IdColumn &id_column, const std::string &function_name, vector<LogicalType> &return_types,
                    duckdb_compat::BindNames &names);

} // namespace anofox
} // namespace duckdb

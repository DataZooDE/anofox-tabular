#pragma once

// DuckDB 2.0 moved the nested-vector accessors (ListVector, StructVector) into
// their own headers under duckdb/common/vector/; v1.5.6 declares them in
// vector.hpp. Include this instead of naming either location.
// generic_executor.hpp exists on both lines and includes whatever declares
// ListVector/StructVector there (as duckdb-compat's list_vector.hpp does). Not
// __has_include: a 1.5.6 build would also find a system DuckDB 2.0's headers.
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"

namespace duckdb {
namespace anofox {

// A child of StructVector::GetEntries(vector): the entries are unique_ptr<Vector>
// before 2.0 and Vector on 2.0. VectorRef(children[i]) is the child on both.
inline Vector &VectorRef(Vector &vector) {
	return vector;
}
inline Vector &VectorRef(unique_ptr<Vector> &vector) {
	return *vector;
}

} // namespace anofox
} // namespace duckdb

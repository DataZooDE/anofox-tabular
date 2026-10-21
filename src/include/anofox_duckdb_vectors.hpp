#pragma once

// DuckDB 2.0 moved the nested-vector accessors (ListVector, StructVector) into
// their own headers under duckdb/common/vector/; v1.5.6 declares them in
// vector.hpp. Include this instead of naming either location.
#include "duckdb/common/types/vector.hpp"
#if __has_include("duckdb/common/vector/list_vector.hpp")
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#endif

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

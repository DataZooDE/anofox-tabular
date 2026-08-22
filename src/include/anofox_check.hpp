#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

void RegisterCheckFunctions(ExtensionLoader &loader);

// Shared with the check-suite runner (anofox_check_suite.cpp):

// Validate that a caller-supplied string is a single SQL boolean expression.
// Rejects multi-statement payloads; the expression is still executed with the
// caller's privileges (same trust model as running SQL directly).
void ValidateBooleanExpression(ClientContext &context, const string &expr, const string &function_name);

// Build a ['a', 'b', ...]::VARCHAR[] literal from a LIST value.
string BuildVarcharArrayLiteral(const Value &list_value);

} // namespace anofox
} // namespace duckdb

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include "duckdb_compat.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

// Alias registration helpers (#51).
//
// Aliases are full copies of their primary function with only the name changed
// (copy-then-rename). Reconstructing the function from a subset of fields would
// silently drop metadata such as bind_replace, statistics propagation, pushdown
// flags, serialization hooks, null handling and error modes — the alias must be
// behaviorally identical to the primary.
//
// The optional descriptions parameter attaches the same FunctionDescription
// metadata to both the primary and the alias catalog entries.
//
// DuckDB 2.0 (no-ops on v1.5.6, see docs/UPDATING.md):
// - every scalar function is marked fallible: 2.0 turns an exception thrown by
//   a non-fallible scalar function into an INTERNAL error, and most of ours
//   throw on invalid input;
// - parameter names come from the descriptions (descriptions[i] names overload
//   i), or 2.0 shows col0, col1, ... in duckdb_functions().

namespace detail {
// descriptions[i] names overload i; a single description names every overload
// (the prefix-arity sets: each overload takes a prefix of the same parameters)
template <class INFO>
void NameParameters(INFO &info) {
	auto &descriptions = info.descriptions;
	idx_t count = 0;
	duckdb_compat::ForEachFunction(info.functions, [&](auto &) { count++; });
	if (descriptions.empty() || (descriptions.size() != 1 && descriptions.size() != count)) {
		return;
	}
	idx_t index = 0;
	duckdb_compat::ForEachFunction(info.functions, [&](auto &fn) {
		auto &description = descriptions.size() == 1 ? descriptions[0] : descriptions[index];
		duckdb_compat::NameParameters(fn, description.parameter_names);
		index++;
	});
}

// A scalar function whose arguments are all constant must return a constant vector:
// DuckDB folds such calls at bind time and reads the result as a constant (debug builds
// assert it). Our functions fill a flat result row by row, so mark it constant here, the
// way DuckDB's own row-by-row functions do. SetVectorType keeps row 0 and converts the
// children of STRUCT results as well. A volatile function (postal_load_data) can return
// a different value per row and is left as it is; a non-volatile one without arguments
// (anofox_contract_version) is folded like any other.
inline void ConstantResultForConstantArguments(ScalarFunction &func) {
	if (!func.HasFunctionCallback() || func.GetStability() == FunctionStability::VOLATILE) {
		return;
	}
	auto inner = func.GetFunctionCallback();
	func.SetFunctionCallback([inner](DataChunk &args, ExpressionState &state, Vector &result) {
		inner(args, state, result);
		if (args.AllConstant() && result.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	});
}

template <class INFO>
void RegisterWithAlias(ExtensionLoader &loader, INFO primary_info, INFO alias_info,
                       const vector<FunctionDescription> &descriptions) {
	primary_info.descriptions = descriptions;
	NameParameters(primary_info);
	loader.RegisterFunction(primary_info);
	alias_info.descriptions = descriptions;
	NameParameters(alias_info);
	loader.RegisterFunction(alias_info);
}
} // namespace detail

// A scalar-function bind that only needs the client context, as every bind in
// this extension does (telemetry, settings): unique_ptr<FunctionData> F(ClientContext &).
// Bind has the signature of each DuckDB line - 1.5.6 passes (context, function,
// arguments), 2.0 a BindScalarFunctionInput - and ScalarFunction::SetBindCallback
// (present on both) picks the matching overload:
//   func.SetBindCallback(&ScalarContextBind<MyBind>::Bind);
template <unique_ptr<FunctionData> (*F)(ClientContext &)>
struct ScalarContextBind {
	static unique_ptr<FunctionData> Bind(ClientContext &context, ScalarFunction &, vector<unique_ptr<Expression>> &) {
		return F(context);
	}
	template <class INPUT>
	static unique_ptr<FunctionData> Bind(INPUT &input) {
		return F(input.GetClientContext());
	}
};

// Helper to register a scalar function with an alias
inline void RegisterScalarFunctionWithAlias(ExtensionLoader &loader, ScalarFunction func,
                                            const std::string &alias_name,
                                            vector<FunctionDescription> descriptions = {}) {
	duckdb_compat::SetFallible(func);
	detail::ConstantResultForConstantArguments(func);
	CreateScalarFunctionInfo primary_info(func);

	// Alias: copy-then-rename so all metadata survives
	auto primary_name = func.name;
	auto alias_func = std::move(func);
	alias_func.name = duckdb_compat::FunctionName(alias_name);
	CreateScalarFunctionInfo alias_info(std::move(alias_func));
	alias_info.alias_of = primary_name;
	detail::RegisterWithAlias(loader, std::move(primary_info), std::move(alias_info), descriptions);
}

// Helper to register a scalar function set with an alias
inline void RegisterScalarFunctionSetWithAlias(ExtensionLoader &loader, ScalarFunctionSet func_set,
                                               const std::string &alias_name,
                                               vector<FunctionDescription> descriptions = {}) {
	duckdb_compat::ForEachFunction(func_set, [](ScalarFunction &func) {
		duckdb_compat::SetFallible(func);
		detail::ConstantResultForConstantArguments(func);
	});
	CreateScalarFunctionInfo primary_info(func_set);

	// Alias: copy-then-rename every overload so all metadata survives
	ScalarFunctionSet alias_set(duckdb_compat::FunctionName(alias_name));
	duckdb_compat::ForEachFunction(func_set, [&](ScalarFunction &func) {
		auto alias_func = func;
		alias_func.name = duckdb_compat::FunctionName(alias_name);
		alias_set.AddFunction(std::move(alias_func));
	});
	CreateScalarFunctionInfo alias_info(std::move(alias_set));
	alias_info.alias_of = func_set.name;
	detail::RegisterWithAlias(loader, std::move(primary_info), std::move(alias_info), descriptions);
}

// Helper to register a table function with an alias
inline void RegisterTableFunctionWithAlias(ExtensionLoader &loader, TableFunction func,
                                           const std::string &alias_name,
                                           vector<FunctionDescription> descriptions = {}) {
	CreateTableFunctionInfo primary_info(func);

	// Alias: copy-then-rename so all metadata survives
	auto primary_name = func.name;
	auto alias_func = std::move(func);
	alias_func.name = duckdb_compat::FunctionName(alias_name);
	CreateTableFunctionInfo alias_info(std::move(alias_func));
	alias_info.alias_of = primary_name;
	detail::RegisterWithAlias(loader, std::move(primary_info), std::move(alias_info), descriptions);
}

// Helper to register a table function set with an alias
inline void RegisterTableFunctionSetWithAlias(ExtensionLoader &loader, TableFunctionSet func_set,
                                              const std::string &alias_name,
                                              vector<FunctionDescription> descriptions = {}) {
	CreateTableFunctionInfo primary_info(func_set);

	// Alias: copy-then-rename every overload so all metadata survives
	TableFunctionSet alias_set(duckdb_compat::FunctionName(alias_name));
	duckdb_compat::ForEachFunction(func_set, [&](TableFunction &func) {
		auto alias_func = func;
		alias_func.name = duckdb_compat::FunctionName(alias_name);
		alias_set.AddFunction(std::move(alias_func));
	});
	CreateTableFunctionInfo alias_info(std::move(alias_set));
	alias_info.alias_of = func_set.name;
	detail::RegisterWithAlias(loader, std::move(primary_info), std::move(alias_info), descriptions);
}

// Helper to register a scalar function that has no alias (e.g. the anofox-wide
// anofox_contract_version()), with the same 2.0 handling as the alias helpers.
inline void RegisterScalarFunction(ExtensionLoader &loader, ScalarFunction func,
                                   vector<FunctionDescription> descriptions = {}) {
	duckdb_compat::SetFallible(func);
	CreateScalarFunctionInfo info(std::move(func));
	info.descriptions = std::move(descriptions);
	detail::NameParameters(info);
	loader.RegisterFunction(std::move(info));
}

// Set duckdb_functions().tags on a registered table function (all overloads share
// one catalog entry). CreateInfo::tags is not copied into function catalog entries
// on either DuckDB line, so the entry is tagged after registration. The lookup works
// on both lines: TryGetTableFunction takes a string on 1.5.6 and an Identifier on
// 2.0, and duckdb_compat::FunctionName builds whichever the line takes.
inline void SetTableFunctionTags(ExtensionLoader &loader, const std::string &name,
                                 const vector<std::pair<std::string, std::string>> &tags) {
	auto entry = loader.TryGetTableFunction(duckdb_compat::FunctionName(name));
	if (!entry) {
		throw InternalException("SetTableFunctionTags: table function '%s' is not registered", name);
	}
	for (auto &tag : tags) {
		entry->tags[tag.first] = tag.second;
	}
}

} // namespace anofox
} // namespace duckdb

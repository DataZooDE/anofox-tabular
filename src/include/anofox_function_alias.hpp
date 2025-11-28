#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {
namespace anofox {

// Helper to register a scalar function with an alias
inline void RegisterScalarFunctionWithAlias(ExtensionLoader &loader, ScalarFunction func, const std::string &alias_name) {
	// Register primary function
	loader.RegisterFunction(func);
	
	// Register alias
	ScalarFunction alias_func(alias_name, func.arguments, func.return_type, func.function);
	alias_func.null_handling = func.null_handling;
	alias_func.stability = func.stability;
	alias_func.varargs = func.varargs;
	alias_func.side_effects = func.side_effects;
	
	CreateScalarFunctionInfo alias_info(alias_func);
	alias_info.alias_of = func.name;
	loader.RegisterFunction(alias_info);
}

// Helper to register a scalar function set with an alias
inline void RegisterScalarFunctionSetWithAlias(ExtensionLoader &loader, ScalarFunctionSet func_set, const std::string &alias_name) {
	// Register primary function set
	loader.RegisterFunction(func_set);
	
	// Register alias - create a new function set with alias name
	ScalarFunctionSet alias_set(alias_name);
	for (auto &func : func_set.functions) {
		ScalarFunction alias_func(alias_name, func.arguments, func.return_type, func.function);
		alias_func.null_handling = func.null_handling;
		alias_func.stability = func.stability;
		alias_func.varargs = func.varargs;
		alias_func.side_effects = func.side_effects;
		alias_set.AddFunction(alias_func);
	}
	
	CreateScalarFunctionInfo alias_info(alias_set);
	alias_info.alias_of = func_set.name;
	loader.RegisterFunction(alias_info);
}

// Helper to register a table function with an alias
inline void RegisterTableFunctionWithAlias(ExtensionLoader &loader, TableFunction func, const std::string &alias_name) {
	// Register primary function
	loader.RegisterFunction(func);
	
	// Register alias
	TableFunction alias_func(alias_name, func.arguments, func.function, func.bind, func.init_global, func.init_local);
	alias_func.null_handling = func.null_handling;
	alias_func.stability = func.stability;
	alias_func.init_global = func.init_global;
	alias_func.init_local = func.init_local;
	alias_func.bind_replace = func.bind_replace;
	alias_func.named_parameters = func.named_parameters;
	
	CreateTableFunctionInfo alias_info(alias_func);
	alias_info.alias_of = func.name;
	loader.RegisterFunction(alias_info);
}

} // namespace anofox
} // namespace duckdb


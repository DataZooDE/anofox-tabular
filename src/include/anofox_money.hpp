#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

// Registration functions
void RegisterMoneyOptions(ExtensionLoader &loader);
void RegisterMoneyFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

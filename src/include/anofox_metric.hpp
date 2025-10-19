#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

void RegisterMetricFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

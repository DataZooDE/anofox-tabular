#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

void RegisterEmailOptions(ExtensionLoader &loader);
void RegisterEmailFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb


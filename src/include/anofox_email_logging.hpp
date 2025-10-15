#pragma once

#include "anofox_trace.hpp"

#include <string>

namespace duckdb {
namespace anofox {

void EmailTrace(AnofoxLogLevel level, const std::string &message);

} // namespace anofox
} // namespace duckdb

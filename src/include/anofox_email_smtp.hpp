#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace anofox {
namespace email {

struct SmtpDebugEntry {
	std::string message;
};

struct SmtpResult {
	bool success = false;
	std::string reason;
	std::vector<SmtpDebugEntry> transcript;
};

class SmtpClient {
public:
	SmtpResult Verify(const std::string &email, const std::vector<std::string> &mx_hosts);
};

} // namespace email
} // namespace anofox
} // namespace duckdb


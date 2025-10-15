#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace anofox {
namespace email {

struct SmtpOptions {
	static constexpr uint32_t MIN_TIMEOUT_MS = 1;
	static constexpr uint32_t MAX_CONNECT_TIMEOUT_MS = 5000;
	static constexpr uint32_t MAX_READ_TIMEOUT_MS = 5000;
	static constexpr uint32_t MAX_TOTAL_TIMEOUT_MS = 10000;
	uint16_t port = 25;
	uint32_t connect_timeout_ms = 5000;
	uint32_t read_timeout_ms = 5000;
	std::string helo_domain = "duckdb.local";
	std::string mail_from = "validator@duckdb.local";
};

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
	explicit SmtpClient(const SmtpOptions &options);
	SmtpResult Verify(const std::string &email, const std::vector<std::string> &mx_hosts);

private:
	SmtpOptions options;
};

} // namespace email
} // namespace anofox
} // namespace duckdb

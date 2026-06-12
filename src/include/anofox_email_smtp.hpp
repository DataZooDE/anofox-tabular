#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <chrono>
#include <cstddef>
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

//! Protocol limits applied to SMTP server responses so a hostile peer cannot
//! grow memory without bound.
struct SmtpLimits {
	static constexpr size_t MAX_LINE_BYTES = 8 * 1024;
	static constexpr size_t MAX_RESPONSE_BYTES = 64 * 1024;
	static constexpr size_t MAX_RESPONSE_LINES = 64;
};

//! Tracks byte/line consumption of a single (possibly multi-line) SMTP
//! response. Exposed so the limit enforcement is unit-testable without sockets.
class SmtpResponseBudget {
public:
	//! Account for one complete received line (including the CRLF terminator).
	//! Returns false once the line length, total response size, or line count
	//! exceeds the protocol limits.
	bool AcceptLine(size_t line_bytes);
	//! Check an unterminated partial line accumulated in the receive buffer.
	//! Returns false when it already exceeds the maximum line length.
	bool AcceptPartialLine(size_t buffered_bytes) const;

private:
	size_t total_bytes = 0;
	size_t line_count = 0;
};

//! Returns the effective wait duration for a single socket operation:
//! min(op_timeout, time remaining until the absolute deadline), never
//! negative. Exposed so the deadline math is unit-testable without sockets.
std::chrono::milliseconds ClampToDeadline(std::chrono::milliseconds op_timeout,
                                          std::chrono::steady_clock::time_point now,
                                          std::chrono::steady_clock::time_point deadline);

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

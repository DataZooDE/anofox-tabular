#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace anofox {
namespace email {

struct DnsOptions {
	static constexpr uint32_t MIN_TIMEOUT_MS = 1;
	static constexpr uint32_t MAX_TIMEOUT_MS = 5000;
	uint32_t timeout_ms = 1000;
	uint32_t tries = 1;
};

struct DnsResult {
	bool success = false;
	std::string reason;
	std::vector<std::string> mx_hosts;
};

class DnsResolver {
public:
	explicit DnsResolver(const DnsOptions &options = DnsOptions());

	DnsResult Resolve(const std::string &domain);

private:
	DnsOptions options;
};

std::string DnsStatusToReason(int status);
bool EnsureAresInitialized(std::string &error_reason);

} // namespace email
} // namespace anofox
} // namespace duckdb

#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace anofox {
namespace email {

struct DnsResult {
	bool success = false;
	std::string reason;
	std::vector<std::string> mx_hosts;
};

class DnsResolver {
public:
	DnsResolver();
	DnsResolver(uint32_t timeout_ms, uint32_t tries);

	DnsResult Resolve(const std::string &domain);

private:
	uint32_t timeout_ms;
	uint32_t tries;
};

} // namespace email
} // namespace anofox
} // namespace duckdb

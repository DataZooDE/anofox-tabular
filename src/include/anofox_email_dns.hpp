#pragma once

#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/string_type.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace anofox {
namespace email {

struct DnsOptions {
	static constexpr uint32_t MIN_TIMEOUT_MS = 1;
	static constexpr uint32_t MAX_TIMEOUT_MS = 5000;
	static constexpr uint32_t MIN_TRIES = 1;
	static constexpr uint32_t MAX_TRIES = 10;
	uint32_t timeout_ms = 1000;
	uint32_t tries = 1;
};

struct DnsResult {
	bool success = false;
	std::string reason;
	std::vector<std::string> mx_hosts;
};

//! A single MX record as returned by the DNS lookup.
struct MxRecord {
	uint16_t preference;
	std::string exchange;
};

//! Result of filtering/ordering MX records. `null_mx` is set when an RFC 7505
//! Null MX record (exchange ".") was present, signalling the domain does not
//! accept mail.
struct MxSelection {
	bool null_mx = false;
	std::vector<std::string> hosts;
};

//! Orders MX records by preference and filters out RFC 7505 Null MX entries.
//! Exposed as a free function so the logic is unit-testable without sockets.
MxSelection SelectMxHosts(std::vector<MxRecord> records);

//! Process-wide DNS result cache bounds. Results are cached per domain
//! (case-insensitive) so a column with many addresses of the same domain does
//! one lookup instead of one per row.
static constexpr uint32_t DNS_CACHE_TTL_SECONDS = 300;
static constexpr size_t DNS_CACHE_MAX_ENTRIES = 10000;

//! True for results that are definitive answers and may be cached: successful
//! lookups and authoritative negatives (domain not found, no records, Null MX).
//! Timeouts and other transient or environment errors are never cached.
bool IsCacheableDnsResult(const DnsResult &result);

class DnsResolver {
public:
	explicit DnsResolver(const DnsOptions &options = DnsOptions());

	//! Resolves the mail hosts of `domain`, answering from the process-wide
	//! cache when a fresh entry exists.
	DnsResult Resolve(const std::string &domain);

private:
	DnsResult ResolveUncached(const std::string &domain);

	DnsOptions options;
};

std::string DnsStatusToReason(int status);
bool EnsureAresInitialized(std::string &error_reason);

} // namespace email
} // namespace anofox
} // namespace duckdb

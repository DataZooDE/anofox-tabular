#include "anofox_email_dns.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <ares.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <string>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "anofox_email_logging.hpp"
#include "anofox_raii.hpp"
#include "duckdb/common/string_util.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// Undefine Windows min/max macros that conflict with std::min/max and std::numeric_limits
#undef min
#undef max
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace duckdb {
namespace anofox {
namespace email {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t DEFAULT_TIMEOUT_MS = 1000;
constexpr uint32_t DEFAULT_TRIES = 1;

struct MxQueryState {
	int *pending = nullptr;
	ares_status_t status = ARES_EDESTRUCTION;
	std::string error;
	std::vector<MxRecord> records;
};

struct AddrInfoState {
	int *pending = nullptr;
	int status = ARES_EDESTRUCTION;
	std::string error;
	std::vector<std::string> addresses;
};

struct SocketState {
	bool readable = false;
	bool writable = false;
};

struct SocketTracker {
	std::unordered_map<ares_socket_t, SocketState> sockets;
};

std::string MapAresStatus(int status) {
	switch (status) {
	case ARES_ENOTFOUND:
		return "dns_domain_not_found";
	case ARES_ENODATA:
		return "dns_no_records";
	case ARES_ETIMEOUT:
		return "dns_timeout";
	case ARES_ECONNREFUSED:
		return "dns_server_unreachable";
	case ARES_ENOMEM:
		return "dns_out_of_memory";
	default:
		return "dns_error_" + std::to_string(status);
	}
}

//! poll()/WSAPoll() wrapper: unlike select(), it has no FD_SETSIZE limit, so
//! arbitrary descriptor values are safe (issue #47).
int PollSockets(struct pollfd *fds, size_t count, int timeout_ms) {
#ifdef _WIN32
	return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
#else
	return poll(fds, static_cast<nfds_t>(count), timeout_ms);
#endif
}

void OnSocketState(void *arg, ares_socket_t socket_fd, int readable, int writable) {
	if (!arg) {
		return;
	}
	auto &tracker = *reinterpret_cast<SocketTracker *>(arg);
	if (socket_fd == ARES_SOCKET_BAD) {
		return;
	}
	if (!readable && !writable) {
		tracker.sockets.erase(socket_fd);
		return;
	}
	auto &state = tracker.sockets[socket_fd];
	state.readable = readable != 0;
	state.writable = writable != 0;
}

void OnMxQuery(void *arg, ares_status_t status, size_t timeouts, const ares_dns_record_t *dnsrec) {
	(void)timeouts;
	auto &state = *reinterpret_cast<MxQueryState *>(arg);
	state.status = status;
	if (status == ARES_SUCCESS) {
		if (!dnsrec) {
			state.status = ARES_ENODATA;
			state.error = "mx_record_absent";
		} else {
			size_t answer_count = ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ANSWER);
			for (size_t i = 0; i < answer_count; i++) {
				auto rr = ares_dns_record_rr_get_const(dnsrec, ARES_SECTION_ANSWER, i);
				if (!rr) {
					continue;
				}
				if (ares_dns_rr_get_type(rr) != ARES_REC_TYPE_MX) {
					continue;
				}
				auto preference = ares_dns_rr_get_u16(rr, ARES_RR_MX_PREFERENCE);
				auto exchange = ares_dns_rr_get_str(rr, ARES_RR_MX_EXCHANGE);
				if (exchange) {
					state.records.push_back(MxRecord {preference, exchange});
				}
			}
		}
	} else {
		state.error = ares_strerror(static_cast<int>(status));
	}
	if (state.pending) {
		(*state.pending)--;
	}
}

void OnAddrInfo(void *arg, int status, int timeouts, struct ares_addrinfo *result) {
	(void)timeouts;
	auto &state = *reinterpret_cast<AddrInfoState *>(arg);
	state.status = status;
	if (status == ARES_SUCCESS && result) {
		for (auto node = result->nodes; node; node = node->ai_next) {
			char buffer[INET6_ADDRSTRLEN] = {0};
			if (node->ai_family == AF_INET) {
				auto *addr_in = reinterpret_cast<struct sockaddr_in *>(node->ai_addr);
				if (addr_in && inet_ntop(AF_INET, &addr_in->sin_addr, buffer, sizeof(buffer))) {
					state.addresses.emplace_back(buffer);
				}
			} else if (node->ai_family == AF_INET6) {
				auto *addr_in6 = reinterpret_cast<struct sockaddr_in6 *>(node->ai_addr);
				if (addr_in6 && inet_ntop(AF_INET6, &addr_in6->sin6_addr, buffer, sizeof(buffer))) {
					state.addresses.emplace_back(buffer);
				}
			}
		}
	} else if (status != ARES_SUCCESS) {
		state.error = ares_strerror(status);
	}
	if (result) {
		ares_freeaddrinfo(result);
	}
	if (state.pending) {
		(*state.pending)--;
	}
}

bool RunEventLoop(ares_channel_t *channel, SocketTracker &tracker, int &pending, std::string &error_reason,
                  uint32_t timeout_ms) {
	if (timeout_ms == 0) {
		timeout_ms = DnsOptions::MIN_TIMEOUT_MS;
	}
	auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
	while (pending > 0) {
		auto now = Clock::now();
		if (now >= deadline) {
			error_reason = "dns_timeout";
			return false;
		}
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
		if (remaining.count() <= 0) {
			remaining = std::chrono::milliseconds(DnsOptions::MIN_TIMEOUT_MS);
		}

		std::vector<struct pollfd> poll_fds;
		poll_fds.reserve(tracker.sockets.size());
		for (auto &entry : tracker.sockets) {
			const auto &state = entry.second;
			short poll_events = 0;
			if (state.readable) {
				poll_events |= POLLIN;
			}
			if (state.writable) {
				poll_events |= POLLOUT;
			}
			if (poll_events != 0) {
				struct pollfd pfd;
				pfd.fd = entry.first;
				pfd.events = poll_events;
				pfd.revents = 0;
				poll_fds.push_back(pfd);
			}
		}

		struct timeval capped_timeout;
		capped_timeout.tv_sec = static_cast<long>(remaining.count() / 1000);
		capped_timeout.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);

		struct timeval tv;
		struct timeval *tv_ptr = ares_timeout(channel, &capped_timeout, &tv);
		if (!tv_ptr) {
			tv_ptr = &capped_timeout;
		}
		// Round up so a sub-millisecond budget does not busy-spin.
		auto wait_ms = static_cast<uint32_t>(tv_ptr->tv_sec * 1000 + (tv_ptr->tv_usec + 999) / 1000);
		auto capped_ms = static_cast<uint32_t>(remaining.count());
		if (wait_ms > capped_ms) {
			wait_ms = capped_ms;
		}
		if (wait_ms == 0) {
			wait_ms = DnsOptions::MIN_TIMEOUT_MS;
		}

		int poll_result = 0;
		if (!poll_fds.empty()) {
			poll_result = PollSockets(poll_fds.data(), poll_fds.size(), static_cast<int>(wait_ms));
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
		}
		if (poll_result < 0) {
#ifdef _WIN32
			int wsa_error = WSAGetLastError();
			if (wsa_error == WSAEINTR) {
				continue;
			}
			error_reason = "dns_poll_failed_" + std::to_string(wsa_error);
#else
			if (errno == EINTR) {
				continue;
			}
			error_reason = "dns_poll_failed_" + std::to_string(errno);
#endif
			return false;
		}

		std::vector<ares_fd_events_t> events;
		if (poll_result > 0) {
			for (auto &pfd : poll_fds) {
				if (pfd.revents == 0) {
					continue;
				}
				unsigned int event_mask = ARES_FD_EVENT_NONE;
				if (pfd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
					event_mask |= ARES_FD_EVENT_READ;
				}
				if (pfd.revents & POLLOUT) {
					event_mask |= ARES_FD_EVENT_WRITE;
				}
				if (event_mask != ARES_FD_EVENT_NONE) {
					events.push_back({pfd.fd, event_mask});
				}
			}
		}

		auto process_status = ares_process_fds(channel, events.empty() ? nullptr : events.data(), events.size(),
		                                       ARES_PROCESS_FLAG_NONE);
		if (process_status != ARES_SUCCESS && process_status != ARES_EDESTRUCTION) {
			error_reason = MapAresStatus(process_status);
			return false;
		}
	}
	return true;
}

bool EnsureLibraryInitializedInternal(std::string &error_reason) {
	static std::once_flag init_flag;
	static int init_status = std::numeric_limits<int>::min();
	static std::string init_error;

	std::call_once(init_flag, [&]() {
#ifdef _WIN32
		WSADATA data;
		int wsa = WSAStartup(MAKEWORD(2, 2), &data);
		if (wsa != 0) {
			init_status = -wsa;
			init_error = "winsock_startup_failed_" + std::to_string(wsa);
			return;
		}
#endif
		int rc = ares_library_init(ARES_LIB_INIT_ALL);
		if (rc != ARES_SUCCESS) {
			init_status = rc;
			init_error = ares_strerror(rc);
			return;
		}
		init_status = ARES_SUCCESS;
	});

	if (init_status != ARES_SUCCESS) {
		if (!init_error.empty()) {
			error_reason = init_error;
		} else {
			error_reason = "dns_init_failed";
		}
		return false;
	}
	return true;
}

} // namespace

MxSelection SelectMxHosts(std::vector<MxRecord> records) {
	MxSelection selection;
	std::sort(records.begin(), records.end(), [](const MxRecord &lhs, const MxRecord &rhs) {
		if (lhs.preference == rhs.preference) {
			return lhs.exchange < rhs.exchange;
		}
		return lhs.preference < rhs.preference;
	});
	for (auto &record : records) {
		if (record.exchange.empty() || record.exchange == ".") {
			// RFC 7505 Null MX: the domain explicitly does not accept mail.
			selection.null_mx = true;
			continue;
		}
		selection.hosts.emplace_back(std::move(record.exchange));
	}
	return selection;
}

std::string DnsStatusToReason(int status) {
	return MapAresStatus(status);
}

bool EnsureAresInitialized(std::string &error_reason) {
	return EnsureLibraryInitializedInternal(error_reason);
}

DnsResolver::DnsResolver(const DnsOptions &options_p) : options(options_p) {
	if (options.timeout_ms == 0) {
		options.timeout_ms = DEFAULT_TIMEOUT_MS;
	}
	if (options.timeout_ms < DnsOptions::MIN_TIMEOUT_MS) {
		options.timeout_ms = DnsOptions::MIN_TIMEOUT_MS;
	}
	if (options.timeout_ms > DnsOptions::MAX_TIMEOUT_MS) {
		options.timeout_ms = DnsOptions::MAX_TIMEOUT_MS;
	}
	if (options.tries == 0) {
		options.tries = DEFAULT_TRIES;
	}
	options.tries = std::min(std::max(options.tries, DnsOptions::MIN_TRIES), DnsOptions::MAX_TRIES);
	options.timeout_ms = std::min(std::max(options.timeout_ms, DnsOptions::MIN_TIMEOUT_MS), DnsOptions::MAX_TIMEOUT_MS);
}

DnsResult DnsResolver::Resolve(const std::string &domain) {
	DnsResult result;
	EmailTrace(AnofoxLogLevel::Info,
	           "DNS Resolve begin domain=" + domain + " timeout_ms=" + std::to_string(options.timeout_ms) +
	               " tries=" + std::to_string(options.tries));
	if (domain.empty()) {
		result.reason = "dns_empty_domain";
		EmailTrace(AnofoxLogLevel::Warn, "DNS early exit: empty domain");
		return result;
	}

	std::string init_error;
	if (!EnsureAresInitialized(init_error)) {
		result.reason = init_error.empty() ? "dns_init_failed" : init_error;
		EmailTrace(AnofoxLogLevel::Warn, "DNS init failure reason=" + result.reason);
		return result;
	}

	SocketTracker tracker;

	ares_options resolver_options;
	std::memset(&resolver_options, 0, sizeof(resolver_options));
	uint32_t per_try_timeout = options.timeout_ms < DnsOptions::MIN_TIMEOUT_MS
	                               ? DnsOptions::MIN_TIMEOUT_MS
	                               : std::min(options.timeout_ms, DnsOptions::MAX_TIMEOUT_MS);
	resolver_options.timeout = static_cast<int>(per_try_timeout);
	options.timeout_ms = per_try_timeout;
	resolver_options.tries = static_cast<int>(options.tries);
	resolver_options.sock_state_cb = OnSocketState;
	resolver_options.sock_state_cb_data = &tracker;

	// Declared before the channel guard: destroying the channel can still fire
	// callbacks that touch these states (and the socket tracker above), so the
	// guard must be torn down first on every exit path.
	int pending = 0;
	MxQueryState mx_state;
	mx_state.pending = &pending;
	AddrInfoState addrinfo_state;
	addrinfo_state.pending = &pending;

	ares_channel_t *raw_channel = nullptr;
	int init_rc = ares_init_options(&raw_channel, &resolver_options,
	                                ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES | ARES_OPT_SOCK_STATE_CB);
	if (init_rc != ARES_SUCCESS) {
		result.reason = MapAresStatus(init_rc);
		return result;
	}
	// RAII channel ownership (issue #60): released on every exit path.
	struct AresChannelDestroyer {
		void operator()(ares_channel_t *channel) const {
			ares_destroy(channel);
		}
	};
	UniqueHandle<ares_channel_t *, AresChannelDestroyer, nullptr> channel(raw_channel);

	auto mx_rc = ares_query_dnsrec(channel.Get(), domain.c_str(), ARES_CLASS_IN, ARES_REC_TYPE_MX, OnMxQuery,
	                               &mx_state, nullptr);
	if (mx_rc == ARES_SUCCESS) {
		pending++;
	} else {
		mx_state.status = mx_rc;
		mx_state.error = ares_strerror(static_cast<int>(mx_rc));
	}

	ares_addrinfo_hints hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	pending++;
	ares_getaddrinfo(channel.Get(), domain.c_str(), nullptr, &hints, OnAddrInfo, &addrinfo_state);

	std::string loop_error;
	uint32_t loop_timeout_ms = DnsOptions::MAX_TIMEOUT_MS;
	auto tries = std::max<uint32_t>(options.tries, 1);
	auto total_timeout =
	    static_cast<uint64_t>(per_try_timeout) * static_cast<uint64_t>(tries);
	if (total_timeout < static_cast<uint64_t>(DnsOptions::MIN_TIMEOUT_MS)) {
		total_timeout = DnsOptions::MIN_TIMEOUT_MS;
	}
	if (total_timeout < loop_timeout_ms) {
		loop_timeout_ms = static_cast<uint32_t>(total_timeout);
	}
	bool loop_ok = RunEventLoop(channel.Get(), tracker, pending, loop_error, loop_timeout_ms);
	// Cancel outstanding queries now so the states above are settled before use.
	channel.Reset();

	if (!loop_ok) {
		result.reason = loop_error.empty() ? "dns_loop_failure" : loop_error;
		EmailTrace(AnofoxLogLevel::Warn, "DNS loop failure reason=" + result.reason);
		return result;
	}

	if (pending != 0) {
		result.reason = "dns_pending_incomplete";
		EmailTrace(AnofoxLogLevel::Warn, "DNS incomplete pending=" + std::to_string(pending));
		return result;
	}

	if (mx_state.status == ARES_SUCCESS && !mx_state.records.empty()) {
		auto selection = SelectMxHosts(std::move(mx_state.records));
		if (!selection.hosts.empty()) {
			result.success = true;
			result.mx_hosts = std::move(selection.hosts);
			EmailTrace(AnofoxLogLevel::Info,
			           "DNS MX success records=" + std::to_string(result.mx_hosts.size()));
			return result;
		}
		if (selection.null_mx) {
			// RFC 7505 Null MX: the domain explicitly refuses mail; do not fall
			// back to direct host resolution.
			result.reason = "dns_null_mx";
			EmailTrace(AnofoxLogLevel::Warn, "DNS Null MX (RFC 7505) for domain=" + domain);
			return result;
		}
	}

	// MX lookup failed, attempt to fall back to direct host resolution.
	bool addrinfo_success = addrinfo_state.status == ARES_SUCCESS && !addrinfo_state.addresses.empty();

	if (addrinfo_success) {
		result.success = true;
		for (auto &address : addrinfo_state.addresses) {
			result.mx_hosts.emplace_back(address);
		}
		if (result.mx_hosts.empty()) {
			result.mx_hosts.emplace_back(domain);
		}
		EmailTrace(AnofoxLogLevel::Info,
		           "DNS addrinfo fallback success count=" + std::to_string(result.mx_hosts.size()));
		return result;
	}

	// Capture the most relevant failure reason.
	if (mx_state.status != ARES_SUCCESS) {
		result.reason = MapAresStatus(mx_state.status);
	} else if (addrinfo_state.status != ARES_SUCCESS && addrinfo_state.status != ARES_EDESTRUCTION) {
		result.reason = MapAresStatus(addrinfo_state.status);
	} else {
		if (!mx_state.error.empty()) {
			result.reason = mx_state.error;
		} else if (!addrinfo_state.error.empty()) {
			result.reason = addrinfo_state.error;
		} else {
			result.reason = "dns_no_records";
		}
	}
	EmailTrace(AnofoxLogLevel::Warn, "DNS failure reason=" + result.reason);
	return result;
}

} // namespace email
} // namespace anofox
} // namespace duckdb

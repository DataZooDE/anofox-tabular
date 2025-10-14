#include "anofox_email_dns.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <ares.h>
#include <ares_nameser.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifndef ARES_GETSOCK_MAX
#define ARES_GETSOCK_MAX ARES_GETSOCK_MAXNUM
#endif

namespace duckdb {
namespace anofox {
namespace email {

namespace {

constexpr uint32_t DEFAULT_TIMEOUT_MS = 1000;
constexpr uint32_t DEFAULT_TRIES = 1;

struct MxQueryState {
	int *pending = nullptr;
	int status = ARES_EDESTRUCTION;
	std::string error;
	std::vector<std::pair<int, std::string>> records;
};

struct HostQueryState {
	int *pending = nullptr;
	int status = ARES_EDESTRUCTION;
	std::string error;
	std::vector<std::string> addresses;
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

int SocketToNfds(ares_socket_t sock) {
#ifdef _WIN32
	return static_cast<int>(sock) + 1;
#else
	return static_cast<int>(sock) + 1;
#endif
}

void OnMxQuery(void *arg, int status, int, unsigned char *abuf, int alen) {
	auto &state = *reinterpret_cast<MxQueryState *>(arg);
	state.status = status;
	if (status == ARES_SUCCESS) {
		struct ares_mx_reply *mx_out = nullptr;
		int parse_status = ares_parse_mx_reply(abuf, alen, &mx_out);
		if (parse_status == ARES_SUCCESS && mx_out) {
			for (auto current = mx_out; current; current = current->next) {
				if (current->host) {
					state.records.emplace_back(current->priority, current->host);
				}
			}
			ares_free_data(mx_out);
		} else {
			state.status = parse_status;
			if (parse_status != ARES_SUCCESS) {
				state.error = ares_strerror(parse_status);
			}
			if (mx_out) {
				ares_free_data(mx_out);
			}
		}
	} else {
		state.error = ares_strerror(status);
	}
	if (state.pending) {
		(*state.pending)--;
	}
}

void OnHostQuery(void *arg, int status, int, struct hostent *hostent) {
	auto &state = *reinterpret_cast<HostQueryState *>(arg);
	state.status = status;
	if (status == ARES_SUCCESS && hostent && hostent->h_addr_list) {
		char buffer[INET6_ADDRSTRLEN];
		for (int i = 0; hostent->h_addr_list[i]; i++) {
			std::memset(buffer, 0, sizeof(buffer));
			const void *addr = hostent->h_addr_list[i];
			if (hostent->h_addrtype == AF_INET) {
				if (inet_ntop(AF_INET, addr, buffer, sizeof(buffer))) {
					state.addresses.emplace_back(buffer);
				}
			} else if (hostent->h_addrtype == AF_INET6) {
				if (inet_ntop(AF_INET6, addr, buffer, sizeof(buffer))) {
					state.addresses.emplace_back(buffer);
				}
			}
		}
	} else if (status != ARES_SUCCESS) {
		state.error = ares_strerror(status);
	}
	if (state.pending) {
		(*state.pending)--;
	}
}

bool RunEventLoop(ares_channel channel, int &pending, std::string &error_reason, uint32_t timeout_ms) {
	while (pending > 0) {
		fd_set read_fds;
		fd_set write_fds;
		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);

		ares_socket_t sockets[ARES_GETSOCK_MAX];
		int bitmask = ares_getsock(channel, sockets, ARES_GETSOCK_MAX);
		if (bitmask == 0) {
			ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
			std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
			continue;
		}

		int nfds = 0;
		for (int i = 0; i < ARES_GETSOCK_MAX; i++) {
			ares_socket_t sock = sockets[i];
			if (sock == ARES_SOCKET_BAD) {
				continue;
			}
			if (ARES_GETSOCK_READABLE(bitmask, i)) {
				FD_SET(sock, &read_fds);
				nfds = std::max(nfds, SocketToNfds(sock));
			}
			if (ARES_GETSOCK_WRITABLE(bitmask, i)) {
				FD_SET(sock, &write_fds);
				nfds = std::max(nfds, SocketToNfds(sock));
			}
		}

		struct timeval tv;
		struct timeval *tv_ptr = ares_timeout(channel, nullptr, &tv);
		int select_result = 0;
		if (nfds > 0) {
			select_result = select(nfds, &read_fds, &write_fds, nullptr, tv_ptr);
		} else {
			// No file descriptors, wait briefly before processing timeouts.
			std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
			continue;
		}
		if (select_result < 0) {
#ifdef _WIN32
			int wsa_error = WSAGetLastError();
			if (wsa_error == WSAEINTR) {
				continue;
			}
			error_reason = "dns_select_failed_" + std::to_string(wsa_error);
#else
			if (errno == EINTR) {
				continue;
			}
			error_reason = "dns_select_failed_" + std::to_string(errno);
#endif
			return false;
		}

		if (select_result == 0) {
			ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
			continue;
		}

		for (int i = 0; i < ARES_GETSOCK_MAX; i++) {
			ares_socket_t sock = sockets[i];
			if (sock == ARES_SOCKET_BAD) {
				continue;
			}
			ares_socket_t read_fd = ARES_GETSOCK_READABLE(bitmask, i) ? sock : ARES_SOCKET_BAD;
			ares_socket_t write_fd = ARES_GETSOCK_WRITABLE(bitmask, i) ? sock : ARES_SOCKET_BAD;
			if (read_fd == ARES_SOCKET_BAD && write_fd == ARES_SOCKET_BAD) {
				continue;
			}
			ares_process_fd(channel, read_fd, write_fd);
		}
	}
	return true;
}

bool EnsureLibraryInitialized(std::string &error_reason) {
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

DnsResolver::DnsResolver() : timeout_ms(DEFAULT_TIMEOUT_MS), tries(DEFAULT_TRIES) {
}

DnsResolver::DnsResolver(uint32_t timeout_ms_p, uint32_t tries_p)
    : timeout_ms(timeout_ms_p ? timeout_ms_p : DEFAULT_TIMEOUT_MS), tries(tries_p ? tries_p : DEFAULT_TRIES) {
}

DnsResult DnsResolver::Resolve(const std::string &domain) {
	DnsResult result;
	if (domain.empty()) {
		result.reason = "dns_empty_domain";
		return result;
	}

	std::string init_error;
	if (!EnsureLibraryInitialized(init_error)) {
		result.reason = init_error.empty() ? "dns_init_failed" : init_error;
		return result;
	}

	ares_options options;
	std::memset(&options, 0, sizeof(options));
	options.timeout = static_cast<int>(timeout_ms);
	options.tries = static_cast<int>(tries);

	ares_channel channel;
	int init_rc = ares_init_options(&channel, &options, ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES);
	if (init_rc != ARES_SUCCESS) {
		result.reason = MapAresStatus(init_rc);
		return result;
	}

	int pending = 0;
	MxQueryState mx_state;
	mx_state.pending = &pending;

	pending++;
	ares_query(channel, domain.c_str(), ns_c_in, ns_t_mx, OnMxQuery, &mx_state);

	HostQueryState ipv4_state;
	ipv4_state.pending = &pending;
	pending++;
	ares_gethostbyname(channel, domain.c_str(), AF_INET, OnHostQuery, &ipv4_state);

	HostQueryState ipv6_state;
	ipv6_state.pending = &pending;
	pending++;
	ares_gethostbyname(channel, domain.c_str(), AF_INET6, OnHostQuery, &ipv6_state);

	std::string loop_error;
	bool loop_ok = RunEventLoop(channel, pending, loop_error, timeout_ms);
	ares_destroy(channel);

	if (!loop_ok) {
		result.reason = loop_error.empty() ? "dns_loop_failure" : loop_error;
		return result;
	}

	if (pending != 0) {
		result.reason = "dns_pending_incomplete";
		return result;
	}

	if (mx_state.status == ARES_SUCCESS && !mx_state.records.empty()) {
		std::sort(mx_state.records.begin(), mx_state.records.end(),
		          [](const std::pair<int, std::string> &lhs, const std::pair<int, std::string> &rhs) {
			          if (lhs.first == rhs.first) {
				          return lhs.second < rhs.second;
			          }
			          return lhs.first < rhs.first;
		          });
		result.success = true;
		result.mx_hosts.reserve(mx_state.records.size());
		for (auto &entry : mx_state.records) {
			result.mx_hosts.emplace_back(entry.second);
		}
		return result;
	}

	// MX lookup failed, attempt to fall back to direct host resolution.
	bool ipv4_success = ipv4_state.status == ARES_SUCCESS && !ipv4_state.addresses.empty();
	bool ipv6_success = ipv6_state.status == ARES_SUCCESS && !ipv6_state.addresses.empty();

	if (ipv4_success || ipv6_success) {
		result.success = true;
		result.mx_hosts.emplace_back(domain);
		if (ipv4_success) {
			result.mx_hosts.insert(result.mx_hosts.end(), ipv4_state.addresses.begin(), ipv4_state.addresses.end());
		}
		if (ipv6_success) {
			result.mx_hosts.insert(result.mx_hosts.end(), ipv6_state.addresses.begin(), ipv6_state.addresses.end());
		}
		return result;
	}

	// Capture the most relevant failure reason.
	if (mx_state.status != ARES_SUCCESS) {
		result.reason = MapAresStatus(mx_state.status);
	} else if (ipv4_state.status != ARES_SUCCESS && ipv4_state.status != ARES_EDESTRUCTION) {
		result.reason = MapAresStatus(ipv4_state.status);
	} else if (ipv6_state.status != ARES_SUCCESS && ipv6_state.status != ARES_EDESTRUCTION) {
		result.reason = MapAresStatus(ipv6_state.status);
	} else {
		result.reason = mx_state.error.empty() ? "dns_no_records" : mx_state.error;
	}
	return result;
}

} // namespace email
} // namespace anofox
} // namespace duckdb

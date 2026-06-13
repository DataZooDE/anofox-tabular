#include "anofox_email_smtp.hpp"

#include "anofox_email_dns.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ares.h>

#include "anofox_email_logging.hpp"
#include "anofox_raii.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// Undefine Windows min/max macros that conflict with std::min/std::max and
// std::numeric_limits (e.g. ClampToDeadline's std::min call below).
#undef min
#undef max
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace duckdb {
namespace anofox {
namespace email {

namespace {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

#ifdef _WIN32
using socket_handle = SOCKET;
constexpr socket_handle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
using socket_handle = int;
constexpr socket_handle INVALID_SOCKET_HANDLE = -1;
#endif

enum class WaitStatus { Success, Timeout, Error };

//! poll()/WSAPoll() wrapper: unlike select(), it has no FD_SETSIZE limit, so
//! arbitrary descriptor values are safe (issue #47).
int PollSockets(struct pollfd *fds, size_t count, int timeout_ms) {
#ifdef _WIN32
	return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
#else
	return poll(fds, static_cast<nfds_t>(count), timeout_ms);
#endif
}

void DebugTrace(const std::string &message) {
    EmailTrace(AnofoxLogLevel::Debug, std::string("[SMTP] ") + message);
}

bool EnsureSocketSubsystem(SmtpResult &result) {
#ifdef _WIN32
	static std::once_flag init_flag;
	static int init_status = 0;
	static std::string init_error;
	std::call_once(init_flag, [&]() {
		WSADATA data;
		init_status = WSAStartup(MAKEWORD(2, 2), &data);
		if (init_status != 0) {
			init_error = std::to_string(init_status);
		}
	});
	if (init_status != 0) {
		result.reason = "smtp_socket_init_failed";
		result.transcript.push_back({"WSAStartup failed: " + init_error});
		return false;
	}
#else
	(void)result;
#endif
	return true;
}

int LastSocketError() {
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool IsWouldBlock(int err) {
#ifdef _WIN32
	return err == WSAEWOULDBLOCK;
#else
	return err == EWOULDBLOCK || err == EAGAIN;
#endif
}

std::string SocketErrorMessage(int err) {
#ifdef _WIN32
	return std::to_string(err);
#else
	return std::string(strerror(err));
#endif
}

//! Stateless closers for the RAII guards below (issue #60): socket handles and
//! c-ares channels are owned by UniqueHandle so every exit path cleans up.
struct SocketHandleCloser {
	void operator()(socket_handle sock) const {
#ifdef _WIN32
		closesocket(sock);
#else
		close(sock);
#endif
	}
};
using SocketGuard = UniqueHandle<socket_handle, SocketHandleCloser, INVALID_SOCKET_HANDLE>;

struct AresChannelDestroyer {
	void operator()(ares_channel_t *channel) const {
		ares_destroy(channel);
	}
};
using AresChannelGuard = UniqueHandle<ares_channel_t *, AresChannelDestroyer, nullptr>;

bool SetNonBlocking(socket_handle sock, bool enable) {
#ifdef _WIN32
	u_long mode = enable ? 1UL : 0UL;
	return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	if (enable) {
		flags |= O_NONBLOCK;
	} else {
		flags &= ~O_NONBLOCK;
	}
	return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

milliseconds ToDuration(uint32_t value_ms, uint32_t max_ms) {
	uint32_t clamped = value_ms;
	if (clamped < SmtpOptions::MIN_TIMEOUT_MS) {
		clamped = SmtpOptions::MIN_TIMEOUT_MS;
	}
	if (clamped > max_ms) {
		clamped = max_ms;
	}
	return milliseconds(clamped);
}

//! Waits for the socket to become ready. The per-operation timeout is capped
//! by the absolute overall deadline so a single host cannot exceed the total
//! SMTP budget (issue #47).
WaitStatus WaitForSocket(socket_handle sock, bool want_read, bool want_write, milliseconds timeout,
                         Clock::time_point overall_deadline, SmtpResult *result, const char *context) {
	auto start = Clock::now();
	auto wait_deadline = start + ClampToDeadline(timeout, start, overall_deadline);
	if (context) {
		DebugTrace(std::string(context) + " wait start (" + std::to_string(timeout.count()) + "ms)");
		if (result) {
			result->transcript.push_back({std::string("wait:start:") + context});
		}
	}
	while (true) {
		auto now = Clock::now();
		auto remaining = (now < wait_deadline)
		                    ? std::chrono::duration_cast<std::chrono::microseconds>(wait_deadline - now)
		                    : std::chrono::microseconds(0);
		// Round up so a sub-millisecond budget still polls once with a short wait.
		auto remaining_ms = static_cast<int>((remaining.count() + 999) / 1000);

		struct pollfd pfd;
		pfd.fd = sock;
		pfd.events = 0;
		pfd.revents = 0;
		if (want_read) {
			pfd.events |= POLLIN;
		}
		if (want_write) {
			pfd.events |= POLLOUT;
		}

		int ret = PollSockets(&pfd, 1, remaining_ms);
		if (ret == 0) {
			if (context) {
				DebugTrace(std::string(context) + " wait timeout");
				if (result) {
					result->transcript.push_back({std::string("wait:timeout:") + context});
				}
			}
			return WaitStatus::Timeout;
		}
		if (ret < 0) {
			int err = LastSocketError();
#ifdef _WIN32
			if (err == WSAEINTR) {
				continue;
			}
#else
			if (err == EINTR) {
				continue;
			}
#endif
			if (context) {
				DebugTrace(std::string(context) + " wait error " + SocketErrorMessage(err));
				if (result) {
					result->transcript.push_back({std::string("wait:error:") + context + ":" + SocketErrorMessage(err)});
				}
			}
			return WaitStatus::Error;
		}
		if (pfd.revents & (POLLERR | POLLNVAL)) {
			if (context) {
				DebugTrace(std::string(context) + " wait exception");
			}
			return WaitStatus::Error;
		}
		if (context) {
			DebugTrace(std::string(context) + " wait success");
			if (result) {
				result->transcript.push_back({std::string("wait:success:") + context});
			}
		}
		return WaitStatus::Success;
	}
}

std::string EndpointToString(const sockaddr *addr, socklen_t length) {
	char host[NI_MAXHOST];
	char service[NI_MAXSERV];
	if (getnameinfo(addr, length, host, sizeof(host), service, sizeof(service),
	                NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
		return std::string(host) + ":" + service;
	}
	return "unknown";
}

struct ResolvedEndpoint {
	sockaddr_storage address;
	socklen_t length = 0;
	std::string description;
};

bool EndsWith(const std::string &value, const std::string &suffix) {
	if (value.size() < suffix.size()) {
		return false;
	}
	return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

std::string ToLowerCopy(const std::string &value) {
	std::string lower = value;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower;
}

bool IsLoopbackHost(const std::string &lower) {
	return lower == "localhost" || EndsWith(lower, ".localhost");
}

bool HasReservedSuffix(const std::string &lower) {
	return lower == "example" || lower == "invalid" || lower == "test" || lower == "local" || EndsWith(lower, ".example") ||
	       EndsWith(lower, ".invalid") || EndsWith(lower, ".test") || EndsWith(lower, ".local");
}

struct ResolverSocketState {
	bool readable = false;
	bool writable = false;
};

struct ResolverSocketTracker {
	std::unordered_map<ares_socket_t, ResolverSocketState> sockets;
};

void ResolverOnSocketState(void *arg, ares_socket_t socket_fd, int readable, int writable) {
	if (!arg || socket_fd == ARES_SOCKET_BAD) {
		return;
	}
	auto &tracker = *reinterpret_cast<ResolverSocketTracker *>(arg);
	if (!readable && !writable) {
		tracker.sockets.erase(socket_fd);
		return;
	}
	auto &state = tracker.sockets[socket_fd];
	state.readable = readable != 0;
	state.writable = writable != 0;
}

struct AddressResolveState {
	int *pending = nullptr;
	int status = ARES_EDESTRUCTION;
	std::string error;
	std::vector<ResolvedEndpoint> endpoints;
};

void OnAddressResolved(void *arg, int status, int timeouts, struct ares_addrinfo *result) {
	(void)timeouts;
	auto &state = *reinterpret_cast<AddressResolveState *>(arg);
	state.status = status;
	if (status == ARES_SUCCESS && result) {
		for (auto node = result->nodes; node; node = node->ai_next) {
			if (!node->ai_addr || node->ai_addrlen == 0) {
				continue;
			}
			ResolvedEndpoint endpoint;
			std::memset(&endpoint.address, 0, sizeof(endpoint.address));
			auto copy_length = static_cast<size_t>(node->ai_addrlen);
			if (copy_length > sizeof(endpoint.address)) {
				copy_length = sizeof(endpoint.address);
			}
			std::memcpy(&endpoint.address, node->ai_addr, copy_length);
			endpoint.length = static_cast<socklen_t>(copy_length);
			endpoint.description = EndpointToString(node->ai_addr, static_cast<socklen_t>(node->ai_addrlen));
			state.endpoints.push_back(std::move(endpoint));
		}
	} else if (status != ARES_SUCCESS) {
		state.error = DnsStatusToReason(status);
	}
	if (result) {
		ares_freeaddrinfo(result);
	}
	if (state.pending) {
		(*state.pending)--;
	}
}

bool RunResolverLoop(ares_channel_t *channel, ResolverSocketTracker &tracker, int &pending, std::string &error_reason,
                     std::chrono::milliseconds timeout) {
	auto min_timeout = std::chrono::milliseconds(SmtpOptions::MIN_TIMEOUT_MS);
	if (timeout <= std::chrono::milliseconds::zero()) {
		timeout = min_timeout;
	}
	auto deadline = Clock::now() + timeout;
	while (pending > 0) {
		auto now = Clock::now();
		if (now >= deadline) {
			error_reason = "smtp_dns_timeout";
			return false;
		}
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
		if (remaining <= std::chrono::milliseconds::zero()) {
			remaining = min_timeout;
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
			wait_ms = static_cast<uint32_t>(min_timeout.count());
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
			error_reason = "smtp_dns_poll_failed_" + std::to_string(wsa_error);
#else
			if (errno == EINTR) {
				continue;
			}
			error_reason = "smtp_dns_poll_failed_" + std::to_string(errno);
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
			error_reason = "smtp_" + DnsStatusToReason(process_status);
			return false;
		}
	}
	return true;
}

bool TryParseIpLiteral(const std::string &host, uint16_t port, ResolvedEndpoint &endpoint) {
	sockaddr_in addr4;
	std::memset(&addr4, 0, sizeof(addr4));
	addr4.sin_family = AF_INET;
	addr4.sin_port = htons(port);
	if (inet_pton(AF_INET, host.c_str(), &addr4.sin_addr) == 1) {
		std::memset(&endpoint.address, 0, sizeof(endpoint.address));
		std::memcpy(&endpoint.address, &addr4, sizeof(addr4));
		endpoint.length = sizeof(addr4);
		endpoint.description = EndpointToString(reinterpret_cast<const sockaddr *>(&addr4), sizeof(addr4));
		return true;
	}

	sockaddr_in6 addr6;
	std::memset(&addr6, 0, sizeof(addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_port = htons(port);
	if (inet_pton(AF_INET6, host.c_str(), &addr6.sin6_addr) == 1) {
		std::memset(&endpoint.address, 0, sizeof(endpoint.address));
		std::memcpy(&endpoint.address, &addr6, sizeof(addr6));
		endpoint.length = sizeof(addr6);
		endpoint.description = EndpointToString(reinterpret_cast<const sockaddr *>(&addr6), sizeof(addr6));
		return true;
	}
	return false;
}

void AddLoopbackEndpoints(uint16_t port, std::vector<ResolvedEndpoint> &endpoints) {
	ResolvedEndpoint endpoint;
	if (TryParseIpLiteral("127.0.0.1", port, endpoint)) {
		endpoints.push_back(endpoint);
	}
	if (TryParseIpLiteral("::1", port, endpoint)) {
		bool duplicate = false;
		for (auto &existing : endpoints) {
			if (existing.length == endpoint.length &&
			    std::memcmp(&existing.address, &endpoint.address, endpoint.length) == 0) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			endpoints.push_back(endpoint);
		}
	}
}

bool ResolveHostEndpoints(const std::string &host, uint16_t port, std::chrono::milliseconds timeout,
                          std::vector<ResolvedEndpoint> &endpoints, SmtpResult &result) {
    EmailTrace(AnofoxLogLevel::Info, "ResolveHostEndpoints host=" + host + " port=" +
                                         std::to_string(port) + " timeout_ms=" +
                                         std::to_string(timeout.count()));
	std::string init_error;
	if (!EnsureAresInitialized(init_error)) {
		result.reason = init_error.empty() ? "smtp_dns_init_failed" : init_error;
        EmailTrace(AnofoxLogLevel::Warn, "ResolveHostEndpoints init failed reason=" + result.reason);
		return false;
	}

	ResolverSocketTracker tracker;
	ares_options resolver_options;
	std::memset(&resolver_options, 0, sizeof(resolver_options));
	auto timeout_ms = static_cast<uint32_t>(std::max<long long>(timeout.count(),
	                                                        static_cast<long long>(SmtpOptions::MIN_TIMEOUT_MS)));
	if (timeout_ms > SmtpOptions::MAX_CONNECT_TIMEOUT_MS) {
		timeout_ms = SmtpOptions::MAX_CONNECT_TIMEOUT_MS;
	}
	resolver_options.timeout = static_cast<int>(timeout_ms);
	resolver_options.tries = 1;
	resolver_options.sock_state_cb = ResolverOnSocketState;
	resolver_options.sock_state_cb_data = &tracker;

	// Declared before the channel guard: destroying the channel can still fire
	// callbacks that touch `state` (and the socket tracker above), so the guard
	// must be torn down first on every exit path.
	int pending = 0;
	AddressResolveState state;
	state.pending = &pending;

	ares_channel_t *raw_channel = nullptr;
	int init_rc = ares_init_options(&raw_channel, &resolver_options,
	                                ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES | ARES_OPT_SOCK_STATE_CB);
	if (init_rc != ARES_SUCCESS) {
		result.reason = "smtp_" + DnsStatusToReason(init_rc);
        EmailTrace(AnofoxLogLevel::Warn, "ResolveHostEndpoints init options failed reason=" + result.reason);
		return false;
	}
	AresChannelGuard channel(raw_channel);

	std::string service = std::to_string(port);
	ares_addrinfo_hints hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	pending++;
	ares_getaddrinfo(channel.Get(), host.c_str(), service.c_str(), &hints, OnAddressResolved, &state);

	std::string loop_error;
	bool loop_ok = RunResolverLoop(channel.Get(), tracker, pending, loop_error, timeout);
	// Cancel outstanding queries before `state`/`tracker` leave scope; the
	// guard would otherwise destroy the channel only at function exit.
	channel.Reset();

	if (!loop_ok) {
		result.reason = loop_error.empty() ? "smtp_dns_timeout" : loop_error;
        EmailTrace(AnofoxLogLevel::Warn, "ResolveHostEndpoints loop failed reason=" + result.reason);
		return false;
	}

	if (pending != 0) {
		result.reason = "smtp_dns_incomplete";
        EmailTrace(AnofoxLogLevel::Warn, "ResolveHostEndpoints incomplete pending=" + std::to_string(pending));
		return false;
	}

	if (state.status != ARES_SUCCESS || state.endpoints.empty()) {
		if (!state.error.empty()) {
			result.reason = "smtp_" + state.error;
		} else {
			result.reason = "smtp_dns_no_addresses";
		}
        EmailTrace(AnofoxLogLevel::Warn, "ResolveHostEndpoints failure reason=" + result.reason);
		return false;
	}

	endpoints = std::move(state.endpoints);
    EmailTrace(AnofoxLogLevel::Info,
               "ResolveHostEndpoints success count=" + std::to_string(endpoints.size()));
	return true;
}

bool SendBuffer(socket_handle sock, const std::string &data, milliseconds timeout, Clock::time_point deadline,
                SmtpResult &result) {
	size_t sent = 0;
	while (sent < data.size()) {
		auto status = WaitForSocket(sock, false, true, timeout, deadline, &result, "send");
		if (status == WaitStatus::Timeout) {
			result.reason = Clock::now() >= deadline ? "smtp_overall_timeout" : "smtp_write_timeout";
			return false;
		}
		if (status == WaitStatus::Error) {
			result.reason = "smtp_write_error";
			return false;
		}
		auto remaining = data.size() - sent;
#ifdef _WIN32
		int chunk = static_cast<int>(remaining > INT_MAX ? INT_MAX : remaining);
		int rc = send(sock, data.data() + sent, chunk, 0);
#else
		ssize_t rc = send(sock, data.data() + sent, remaining, 0);
#endif
		if (rc < 0) {
			int err = LastSocketError();
			if (IsWouldBlock(err)) {
				continue;
			}
			result.reason = "smtp_write_error";
			result.transcript.push_back({"Write error: " + SocketErrorMessage(err)});
			return false;
		}
		if (rc == 0) {
			result.reason = "smtp_write_error";
			result.transcript.push_back({"Write error: connection closed"});
			return false;
		}
		sent += static_cast<size_t>(rc);
	}
	return true;
}

bool SendCommand(socket_handle sock, const std::string &command, milliseconds timeout, Clock::time_point deadline,
                 SmtpResult &result) {
	result.transcript.push_back({"C: " + command});
	std::string wire = command;
	if (wire.size() < 2 || wire.substr(wire.size() - 2) != "\r\n") {
		wire += "\r\n";
	}
	return SendBuffer(sock, wire, timeout, deadline, result);
}

bool ReadLine(socket_handle sock, std::string &buffer, std::string &line, milliseconds timeout,
              Clock::time_point deadline, SmtpResponseBudget &budget, SmtpResult &result,
              const std::string &timeout_reason, const std::string &error_reason) {
	char temp[512];
	while (true) {
		auto pos = buffer.find("\r\n");
		if (pos != std::string::npos) {
			line = buffer.substr(0, pos);
			buffer.erase(0, pos + 2);
			if (!budget.AcceptLine(line.size() + 2)) {
				result.reason = "smtp_response_too_large";
				result.transcript.push_back({"Response exceeds SMTP protocol limits"});
				return false;
			}
			result.transcript.push_back({"S: " + line});
			return true;
		}
		if (!budget.AcceptPartialLine(buffer.size())) {
			result.reason = "smtp_response_too_large";
			result.transcript.push_back({"Response line exceeds maximum length"});
			return false;
		}
		auto status = WaitForSocket(sock, true, false, timeout, deadline, &result, "read");
		if (status == WaitStatus::Timeout) {
			result.reason = Clock::now() >= deadline ? "smtp_overall_timeout" : timeout_reason;
			return false;
		}
		if (status == WaitStatus::Error) {
			result.reason = error_reason;
			return false;
		}
#ifdef _WIN32
		int rc = recv(sock, temp, static_cast<int>(sizeof(temp)), 0);
#else
		ssize_t rc = recv(sock, temp, sizeof(temp), 0);
#endif
		if (rc < 0) {
			int err = LastSocketError();
			if (IsWouldBlock(err)) {
				continue;
			}
			result.reason = error_reason;
			result.transcript.push_back({"Read error: " + SocketErrorMessage(err)});
			return false;
		}
		if (rc == 0) {
			result.reason = error_reason;
			result.transcript.push_back({"Read error: connection closed"});
			return false;
		}
		buffer.append(temp, static_cast<size_t>(rc));
	}
}

int ParseSmtpCode(const std::string &response) {
	if (response.size() < 3 || !std::isdigit(static_cast<unsigned char>(response[0])) ||
	    !std::isdigit(static_cast<unsigned char>(response[1])) ||
	    !std::isdigit(static_cast<unsigned char>(response[2]))) {
		return -1;
	}
	return (response[0] - '0') * 100 + (response[1] - '0') * 10 + (response[2] - '0');
}

bool ReadResponse(socket_handle sock, std::string &buffer, milliseconds timeout, Clock::time_point deadline,
                  SmtpResult &result, int &code, const std::string &timeout_reason,
                  const std::string &error_reason) {
	SmtpResponseBudget budget;
	std::string line;
	if (!ReadLine(sock, buffer, line, timeout, deadline, budget, result, timeout_reason, error_reason)) {
		return false;
	}
	code = ParseSmtpCode(line);
	if (code < 0) {
		result.reason = "smtp_invalid_response";
		return false;
	}
	while (line.size() > 3 && line[3] == '-') {
		if (!ReadLine(sock, buffer, line, timeout, deadline, budget, result, timeout_reason, error_reason)) {
			return false;
		}
		int line_code = ParseSmtpCode(line);
		if (line_code != code) {
			result.reason = "smtp_invalid_response";
			return false;
		}
	}
	return true;
}

bool ConnectSocket(socket_handle sock, const sockaddr *addr, socklen_t length, milliseconds timeout,
                   Clock::time_point deadline, std::string &endpoint, SmtpResult &result) {
	int rc = connect(sock, addr, length);
	if (rc == 0) {
		endpoint = EndpointToString(addr, length);
		return true;
	}
	int err = LastSocketError();
	if (!IsWouldBlock(err)
#ifdef _WIN32
	    && err != WSAEINPROGRESS
#else
	    && err != EINPROGRESS
#endif
	) {
		result.reason = "smtp_connect_error";
		result.transcript.push_back({"Connect error: " + SocketErrorMessage(err)});
		return false;
	}
	auto status = WaitForSocket(sock, false, true, timeout, deadline, &result, "connect");
	if (status == WaitStatus::Timeout) {
		result.reason = Clock::now() >= deadline ? "smtp_overall_timeout" : "smtp_connect_timeout";
		return false;
	}
	if (status == WaitStatus::Error) {
		result.reason = "smtp_connect_error";
		return false;
	}
	int so_error = 0;
#ifdef _WIN32
	int optlen = sizeof(so_error);
#else
	socklen_t optlen = sizeof(so_error);
#endif
	if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &optlen) != 0) {
		result.reason = "smtp_connect_error";
		return false;
	}
	if (so_error != 0) {
#ifdef _WIN32
		result.reason = so_error == WSAETIMEDOUT ? "smtp_connect_timeout" : "smtp_connect_error";
#else
		result.reason = so_error == ETIMEDOUT ? "smtp_connect_timeout" : "smtp_connect_error";
#endif
		result.transcript.push_back({"Connect error: " + SocketErrorMessage(so_error)});
		return false;
	}
	endpoint = EndpointToString(addr, length);
	return true;
}

SmtpResult AttemptHost(const std::string &email, const std::string &host, const SmtpOptions &options,
                       Clock::time_point deadline) {
	SmtpResult attempt;
	attempt.transcript.push_back({"Attempt host: " + host});
    EmailTrace(AnofoxLogLevel::Info,
               "SMTP attempt host=" + host + " port=" + std::to_string(options.port));
	if (!EnsureSocketSubsystem(attempt)) {
        EmailTrace(AnofoxLogLevel::Error, "SMTP socket subsystem unavailable reason=" + attempt.reason);
		return attempt;
	}

	auto connect_timeout = ToDuration(options.connect_timeout_ms, SmtpOptions::MAX_CONNECT_TIMEOUT_MS);
	auto io_timeout = ToDuration(options.read_timeout_ms, SmtpOptions::MAX_READ_TIMEOUT_MS);
	std::string read_buffer;

	std::vector<ResolvedEndpoint> endpoints;
	ResolvedEndpoint literal_endpoint;
	if (TryParseIpLiteral(host, options.port, literal_endpoint)) {
		endpoints.push_back(literal_endpoint);
	} else {
		auto lower_host = ToLowerCopy(host);
		if (IsLoopbackHost(lower_host)) {
            EmailTrace(AnofoxLogLevel::Debug,
                       "SMTP host recognized as loopback: " + host);
			AddLoopbackEndpoints(options.port, endpoints);
		} else if (HasReservedSuffix(lower_host)) {
			attempt.reason = "smtp_dns_reserved_domain";
			attempt.transcript.push_back({"Skip reserved domain: " + host});
            EmailTrace(AnofoxLogLevel::Info,
                       "SMTP reserved domain skipped host=" + host);
			return attempt;
		} else {
            EmailTrace(AnofoxLogLevel::Info, "SMTP resolving host via DNS: " + host);
			auto resolve_timeout = ClampToDeadline(connect_timeout, Clock::now(), deadline);
			if (resolve_timeout <= milliseconds::zero()) {
				attempt.reason = "smtp_overall_timeout";
				attempt.transcript.push_back({"Overall SMTP timeout reached before resolving host: " + host});
				return attempt;
			}
			if (!ResolveHostEndpoints(host, options.port, resolve_timeout, endpoints, attempt)) {
                EmailTrace(AnofoxLogLevel::Warn,
                           "SMTP DNS resolution failed reason=" + attempt.reason);
				return attempt;
			}
		}
	}

	if (endpoints.empty()) {
		attempt.reason = "smtp_dns_no_addresses";
        EmailTrace(AnofoxLogLevel::Warn, "SMTP no endpoints resolved host=" + host);
		return attempt;
	}

    EmailTrace(AnofoxLogLevel::Info,
               "SMTP attempting " + std::to_string(endpoints.size()) + " endpoints for host=" + host);
	for (auto &endpoint : endpoints) {
		// RAII socket ownership (issue #60): the guard closes the descriptor on
		// every continue/return below, so no manual close calls are needed.
		SocketGuard sock(socket(endpoint.address.ss_family, SOCK_STREAM, 0));
		if (!sock.IsValid()) {
			attempt.transcript.push_back({"Socket creation failed: " + SocketErrorMessage(LastSocketError())});
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP socket creation failed endpoint=" + endpoint.description);
			continue;
		}
		if (!SetNonBlocking(sock.Get(), true)) {
			attempt.transcript.push_back({"Failed to set non-blocking mode: " + SocketErrorMessage(LastSocketError())});
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP failed to set non-blocking endpoint=" + endpoint.description);
			continue;
		}

		if (!endpoint.description.empty()) {
			attempt.transcript.push_back({"Resolved endpoint: " + endpoint.description});
		}

		std::string endpoint_str;
		if (!ConnectSocket(sock.Get(), reinterpret_cast<const sockaddr *>(&endpoint.address), endpoint.length,
		                   connect_timeout, deadline, endpoint_str, attempt)) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP connect failed endpoint=" + endpoint.description + " reason=" + attempt.reason);
			if (!attempt.reason.empty()) {
				return attempt;
			}
			continue;
		}

		attempt.transcript.push_back({"Connected to " + endpoint_str});
        EmailTrace(AnofoxLogLevel::Info, "SMTP connected to " + endpoint_str);
		read_buffer.clear();

		int code = 0;
		if (!ReadResponse(sock.Get(), read_buffer, io_timeout, deadline, attempt, code, "smtp_greeting_timeout",
		                  "smtp_greeting_error")) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP greeting read failed reason=" + attempt.reason);
			return attempt;
		}
		if (code / 100 != 2) {
			attempt.reason = "smtp_invalid_greeting";
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP greeting invalid code=" + std::to_string(code));
			return attempt;
		}

		if (!SendCommand(sock.Get(), "EHLO " + options.helo_domain, io_timeout, deadline, attempt)) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP EHLO send failed reason=" + attempt.reason);
			return attempt;
		}
		if (!ReadResponse(sock.Get(), read_buffer, io_timeout, deadline, attempt, code, "smtp_read_timeout",
		                  "smtp_read_error")) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP EHLO response failed reason=" + attempt.reason);
			return attempt;
		}
		if (code / 100 != 2) {
			attempt.reason = "smtp_helo_rejected";
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP EHLO rejected code=" + std::to_string(code));
			return attempt;
		}

		if (!SendCommand(sock.Get(), "MAIL FROM:<" + options.mail_from + ">", io_timeout, deadline, attempt)) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP MAIL FROM send failed reason=" + attempt.reason);
			return attempt;
		}
		if (!ReadResponse(sock.Get(), read_buffer, io_timeout, deadline, attempt, code, "smtp_read_timeout",
		                  "smtp_read_error")) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP MAIL FROM response failed reason=" + attempt.reason);
			return attempt;
		}
		if (code / 100 != 2) {
			attempt.reason = "smtp_mail_from_rejected";
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP MAIL FROM rejected code=" + std::to_string(code));
			return attempt;
		}

		if (!SendCommand(sock.Get(), "RCPT TO:<" + email + ">", io_timeout, deadline, attempt)) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP RCPT TO send failed reason=" + attempt.reason);
			return attempt;
		}
		if (!ReadResponse(sock.Get(), read_buffer, io_timeout, deadline, attempt, code, "smtp_read_timeout",
		                  "smtp_read_error")) {
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP RCPT TO response failed reason=" + attempt.reason);
			return attempt;
		}
		if (code / 100 != 2) {
			attempt.reason = "smtp_recipient_rejected";
            EmailTrace(AnofoxLogLevel::Warn,
                       "SMTP RCPT TO rejected code=" + std::to_string(code));
			return attempt;
		}

		attempt.success = true;
		attempt.reason.clear();
		SendCommand(sock.Get(), "QUIT", io_timeout, deadline, attempt);
        EmailTrace(AnofoxLogLevel::Info, "SMTP verification success host=" + host);
		return attempt;
	}

    EmailTrace(AnofoxLogLevel::Info,
               "SMTP exhausted endpoints host=" + host + " last_reason=" + attempt.reason);
	return attempt;
}

} // namespace

bool SmtpResponseBudget::AcceptLine(size_t line_bytes) {
	if (line_bytes > SmtpLimits::MAX_LINE_BYTES) {
		return false;
	}
	line_count++;
	if (line_count > SmtpLimits::MAX_RESPONSE_LINES) {
		return false;
	}
	total_bytes += line_bytes;
	return total_bytes <= SmtpLimits::MAX_RESPONSE_BYTES;
}

bool SmtpResponseBudget::AcceptPartialLine(size_t buffered_bytes) const {
	return buffered_bytes <= SmtpLimits::MAX_LINE_BYTES;
}

std::chrono::milliseconds ClampToDeadline(std::chrono::milliseconds op_timeout,
                                          std::chrono::steady_clock::time_point now,
                                          std::chrono::steady_clock::time_point deadline) {
	if (op_timeout < std::chrono::milliseconds::zero()) {
		op_timeout = std::chrono::milliseconds::zero();
	}
	if (now >= deadline) {
		return std::chrono::milliseconds::zero();
	}
	auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
	return std::min(op_timeout, remaining);
}

SmtpClient::SmtpClient(const SmtpOptions &options_p) : options(options_p) {
	if (options.port == 0) {
		options.port = 25;
	}
	if (options.connect_timeout_ms < SmtpOptions::MIN_TIMEOUT_MS) {
		options.connect_timeout_ms = SmtpOptions::MIN_TIMEOUT_MS;
	}
	if (options.read_timeout_ms < SmtpOptions::MIN_TIMEOUT_MS) {
		options.read_timeout_ms = SmtpOptions::MIN_TIMEOUT_MS;
	}
	if (options.connect_timeout_ms > SmtpOptions::MAX_CONNECT_TIMEOUT_MS) {
		options.connect_timeout_ms = SmtpOptions::MAX_CONNECT_TIMEOUT_MS;
	}
	if (options.read_timeout_ms > SmtpOptions::MAX_READ_TIMEOUT_MS) {
		options.read_timeout_ms = SmtpOptions::MAX_READ_TIMEOUT_MS;
	}
	if (options.helo_domain.empty()) {
		options.helo_domain = "duckdb.local";
	}
	if (options.mail_from.empty()) {
		options.mail_from = "validator@duckdb.local";
	}
}

SmtpResult SmtpClient::Verify(const std::string &email, const std::vector<std::string> &mx_hosts) {
	SmtpResult aggregated;
	if (mx_hosts.empty()) {
		aggregated.success = false;
		aggregated.reason = "smtp_no_hosts";
		aggregated.transcript.push_back({"No MX hosts provided for SMTP verification"});
		return aggregated;
	}
	auto overall_budget = ToDuration(options.connect_timeout_ms + options.read_timeout_ms,
	                                 SmtpOptions::MAX_TOTAL_TIMEOUT_MS);
	auto overall_deadline = Clock::now() + overall_budget;
	std::string last_reason = "smtp_verification_failed";
	for (const auto &host : mx_hosts) {
		if (Clock::now() >= overall_deadline) {
			aggregated.success = false;
			aggregated.reason = "smtp_overall_timeout";
			aggregated.transcript.push_back({"Overall SMTP timeout reached before trying host: " + host});
			return aggregated;
		}
		SmtpResult attempt = AttemptHost(email, host, options, overall_deadline);
		aggregated.transcript.insert(aggregated.transcript.end(), attempt.transcript.begin(), attempt.transcript.end());
		if (attempt.success) {
			aggregated.success = true;
			aggregated.reason.clear();
			return aggregated;
		}
		if (!attempt.reason.empty()) {
			last_reason = attempt.reason;
		}
	}
	aggregated.success = false;
	aggregated.reason = last_reason;
	return aggregated;
}

} // namespace email
} // namespace anofox
} // namespace duckdb

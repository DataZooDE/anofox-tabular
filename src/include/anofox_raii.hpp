#pragma once

//===----------------------------------------------------------------------===//
// Generic RAII ownership for C-style resources (issue #60).
//
// Modules wrapping C libraries (sockets, c-ares channels, CURL*, FILE*)
// declare a small stateless closer and alias UniqueHandle instead of pairing
// every acquisition with manual cleanup calls on each exit path.
//===----------------------------------------------------------------------===//

namespace duckdb {
namespace anofox {

/**
 * Move-only RAII owner for a C-style handle.
 *
 * @tparam HandleT        Handle type (e.g. int fd, SOCKET, ares_channel_t*).
 * @tparam Closer         Stateless function object; `Closer{}(handle)` releases
 *                        the resource. Invoked exactly once per owned handle.
 * @tparam INVALID_HANDLE Sentinel marking the empty state; never passed to the
 *                        closer, so double closes are impossible by design.
 */
template <typename HandleT, typename Closer, HandleT INVALID_HANDLE>
class UniqueHandle {
public:
	UniqueHandle() noexcept : handle_(INVALID_HANDLE) {
	}
	explicit UniqueHandle(HandleT handle) noexcept : handle_(handle) {
	}

	~UniqueHandle() {
		Reset();
	}

	UniqueHandle(const UniqueHandle &) = delete;
	UniqueHandle &operator=(const UniqueHandle &) = delete;

	UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.Release()) {
	}

	UniqueHandle &operator=(UniqueHandle &&other) noexcept {
		if (this != &other) {
			Reset(other.Release());
		}
		return *this;
	}

	//! Current handle value; ownership stays with the guard.
	HandleT Get() const noexcept {
		return handle_;
	}

	bool IsValid() const noexcept {
		return handle_ != INVALID_HANDLE;
	}

	explicit operator bool() const noexcept {
		return IsValid();
	}

	//! Gives up ownership without closing and returns the handle.
	HandleT Release() noexcept {
		HandleT handle = handle_;
		handle_ = INVALID_HANDLE;
		return handle;
	}

	//! Closes the currently owned handle (if any) and adopts `handle`.
	void Reset(HandleT handle = INVALID_HANDLE) noexcept {
		if (handle_ != INVALID_HANDLE) {
			Closer {}(handle_);
		}
		handle_ = handle;
	}

private:
	HandleT handle_;
};

} // namespace anofox
} // namespace duckdb

//===----------------------------------------------------------------------===//
// Catch2 unit tests for the UniqueHandle RAII wrapper (issue #60).
//
//  - Owned handles are closed exactly once (double-close safety).
//  - Move construction/assignment transfers ownership without double closes.
//  - Handles are closed during stack unwinding (cleanup on exception).
//
// Built as part of the standalone test binary (anofox_tabular_cpp_tests).
// Run: ./build/release/extension/anofox_tabular/anofox_tabular_cpp_tests
//===----------------------------------------------------------------------===//

#include "catch.hpp"

#include "anofox_raii.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

using duckdb::anofox::UniqueHandle;

namespace {

constexpr int INVALID_FD = -1;

//! Stateless closer that records every close in a static log so the tests can
//! assert exactly which handles were closed, and how often.
struct RecordingCloser {
	static std::vector<int> closed;

	void operator()(int handle) const {
		closed.push_back(handle);
	}

	static void Clear() {
		closed.clear();
	}

	static size_t CountFor(int handle) {
		size_t count = 0;
		for (int value : closed) {
			if (value == handle) {
				count++;
			}
		}
		return count;
	}
};

std::vector<int> RecordingCloser::closed;

using TestHandle = UniqueHandle<int, RecordingCloser, INVALID_FD>;

} // namespace

TEST_CASE("UniqueHandle default state is invalid and never closes", "[raii]") {
	RecordingCloser::Clear();
	{
		TestHandle guard;
		REQUIRE(!guard.IsValid());
		REQUIRE(!guard);
		REQUIRE(guard.Get() == INVALID_FD);
	}
	REQUIRE(RecordingCloser::closed.empty());
}

TEST_CASE("UniqueHandle closes the owned handle exactly once on scope exit", "[raii]") {
	RecordingCloser::Clear();
	{
		TestHandle guard(7);
		REQUIRE(guard.IsValid());
		REQUIRE(guard.Get() == 7);
	}
	REQUIRE(RecordingCloser::closed == std::vector<int> {7});
}

TEST_CASE("UniqueHandle Reset is double-close safe", "[raii]") {
	RecordingCloser::Clear();
	TestHandle guard(11);

	guard.Reset();
	REQUIRE(!guard.IsValid());
	REQUIRE(RecordingCloser::CountFor(11) == 1);

	// Second Reset and destruction must not close again
	guard.Reset();
	REQUIRE(RecordingCloser::CountFor(11) == 1);
}

TEST_CASE("UniqueHandle Reset with a new handle closes the old one", "[raii]") {
	RecordingCloser::Clear();
	{
		TestHandle guard(3);
		guard.Reset(4);
		REQUIRE(guard.Get() == 4);
		REQUIRE(RecordingCloser::closed == std::vector<int> {3});
	}
	REQUIRE(RecordingCloser::closed == (std::vector<int> {3, 4}));
}

TEST_CASE("UniqueHandle Release transfers ownership to the caller", "[raii]") {
	RecordingCloser::Clear();
	int released = INVALID_FD;
	{
		TestHandle guard(21);
		released = guard.Release();
		REQUIRE(!guard.IsValid());
	}
	REQUIRE(released == 21);
	REQUIRE(RecordingCloser::closed.empty());
}

TEST_CASE("UniqueHandle move construction transfers ownership", "[raii]") {
	RecordingCloser::Clear();
	{
		TestHandle source(5);
		TestHandle target(std::move(source));
		REQUIRE(!source.IsValid());
		REQUIRE(target.Get() == 5);
		REQUIRE(RecordingCloser::closed.empty());
	}
	// Only the surviving owner closes; exactly once
	REQUIRE(RecordingCloser::closed == std::vector<int> {5});
}

TEST_CASE("UniqueHandle move assignment closes the replaced handle", "[raii]") {
	RecordingCloser::Clear();
	{
		TestHandle source(8);
		TestHandle target(9);
		target = std::move(source);
		REQUIRE(!source.IsValid());
		REQUIRE(target.Get() == 8);
		// The previously owned handle was closed by the assignment
		REQUIRE(RecordingCloser::closed == std::vector<int> {9});
	}
	REQUIRE(RecordingCloser::closed == (std::vector<int> {9, 8}));
}

TEST_CASE("UniqueHandle self move assignment keeps the handle open", "[raii]") {
	RecordingCloser::Clear();
	{
		TestHandle guard(13);
		TestHandle &alias = guard;
		guard = std::move(alias);
		REQUIRE(guard.IsValid());
		REQUIRE(guard.Get() == 13);
		REQUIRE(RecordingCloser::closed.empty());
	}
	REQUIRE(RecordingCloser::closed == std::vector<int> {13});
}

TEST_CASE("UniqueHandle closes the handle during stack unwinding", "[raii]") {
	RecordingCloser::Clear();
	REQUIRE_THROWS_AS(
	    [] {
		    TestHandle guard(17);
		    throw std::runtime_error("boom");
	    }(),
	    std::runtime_error);
	REQUIRE(RecordingCloser::closed == std::vector<int> {17});
}

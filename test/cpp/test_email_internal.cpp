// Unit tests for the network-free internals of the email validation module
// (issue #47: network safety and protocol limits).
//
// These tests are linked into DuckDB's Catch2 `unittest` runner; test names
// start with "test/cpp/" so the default "test/*" filter picks them up.

#include "catch.hpp"

#include "anofox_email_dns.hpp"
#include "anofox_email_smtp.hpp"

#include <string>
#include <vector>

using namespace duckdb::anofox::email;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

TEST_CASE("test/cpp/anofox_email_mx_selection", "[anofox_email]") {
	SECTION("regular records are ordered by preference") {
		auto selection = SelectMxHosts({{20, "mx2.example.com"}, {10, "mx1.example.com"}});
		REQUIRE_FALSE(selection.null_mx);
		REQUIRE(selection.hosts == std::vector<std::string>({"mx1.example.com", "mx2.example.com"}));
	}

	SECTION("RFC 7505 Null MX yields no deliverable hosts") {
		auto selection = SelectMxHosts({{0, "."}});
		REQUIRE(selection.null_mx);
		REQUIRE(selection.hosts.empty());
	}

	SECTION("empty exchange is treated like Null MX") {
		auto selection = SelectMxHosts({{0, ""}});
		REQUIRE(selection.null_mx);
		REQUIRE(selection.hosts.empty());
	}

	SECTION("Null MX mixed with real records keeps only real hosts") {
		auto selection = SelectMxHosts({{0, "."}, {10, "mx.example.com"}});
		REQUIRE(selection.null_mx);
		REQUIRE(selection.hosts == std::vector<std::string>({"mx.example.com"}));
	}

	SECTION("no records yields no hosts and no Null MX") {
		auto selection = SelectMxHosts({});
		REQUIRE_FALSE(selection.null_mx);
		REQUIRE(selection.hosts.empty());
	}
}

TEST_CASE("test/cpp/anofox_email_smtp_response_budget", "[anofox_email]") {
	SECTION("lines within limits are accepted") {
		SmtpResponseBudget budget;
		REQUIRE(budget.AcceptLine(512));
		REQUIRE(budget.AcceptLine(SmtpLimits::MAX_LINE_BYTES));
	}

	SECTION("a single line above the maximum length is rejected") {
		SmtpResponseBudget budget;
		REQUIRE_FALSE(budget.AcceptLine(SmtpLimits::MAX_LINE_BYTES + 1));
	}

	SECTION("an unterminated partial line above the maximum length is rejected") {
		SmtpResponseBudget budget;
		REQUIRE(budget.AcceptPartialLine(SmtpLimits::MAX_LINE_BYTES));
		REQUIRE_FALSE(budget.AcceptPartialLine(SmtpLimits::MAX_LINE_BYTES + 1));
	}

	SECTION("total response size is bounded") {
		SmtpResponseBudget budget;
		// 4 KiB lines: the first 16 fill the 64 KiB response budget exactly.
		bool accepted = true;
		for (size_t i = 0; i < SmtpLimits::MAX_RESPONSE_BYTES / 4096; i++) {
			accepted = budget.AcceptLine(4096);
			REQUIRE(accepted);
		}
		REQUIRE_FALSE(budget.AcceptLine(4096));
	}

	SECTION("multi-line count is bounded") {
		SmtpResponseBudget budget;
		for (size_t i = 0; i < SmtpLimits::MAX_RESPONSE_LINES; i++) {
			REQUIRE(budget.AcceptLine(16));
		}
		REQUIRE_FALSE(budget.AcceptLine(16));
	}
}

TEST_CASE("test/cpp/anofox_email_smtp_deadline_clamp", "[anofox_email]") {
	auto now = steady_clock::now();

	SECTION("op timeout within the remaining budget is unchanged") {
		REQUIRE(ClampToDeadline(milliseconds(100), now, now + seconds(10)) == milliseconds(100));
	}

	SECTION("op timeout is capped by the remaining budget") {
		auto clamped = ClampToDeadline(seconds(5), now, now + milliseconds(200));
		REQUIRE(clamped <= milliseconds(200));
	}

	SECTION("an expired deadline yields a zero wait") {
		REQUIRE(ClampToDeadline(milliseconds(100), now, now - milliseconds(1)) == milliseconds(0));
		REQUIRE(ClampToDeadline(milliseconds(100), now, now) == milliseconds(0));
	}

	SECTION("negative op timeouts are floored at zero") {
		REQUIRE(ClampToDeadline(milliseconds(-5), now, now + seconds(1)) == milliseconds(0));
	}
}

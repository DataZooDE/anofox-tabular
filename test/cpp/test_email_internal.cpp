// Unit tests for the network-free internals of the email validation module
// (issue #47: network safety and protocol limits).
//
// These tests are linked into DuckDB's Catch2 `unittest` runner; test names
// start with "test/cpp/" so the default "test/*" filter picks them up.

#include "catch.hpp"

#include "anofox_email.hpp"
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

TEST_CASE("test/cpp/anofox_email_reserved_domains", "[anofox_email]") {
	SECTION("loopback and reserved TLDs are reserved, case-insensitively") {
		for (const char *domain : {"localhost", "LocalHost", "mail.localhost", "example", "shop.example",
		                           "nonexistent.invalid", "INVALID", "ci.test", "printer.local"}) {
			INFO(domain);
			REQUIRE(IsReservedMailDomain(domain));
		}
	}

	SECTION("ordinary domains, including RFC 2606 second-level names, are not") {
		for (const char *domain : {"web.de", "example.com", "example.org", "testing.com", "localhost.de",
		                           "mytest", "locale.fr", "invalid-domain.com"}) {
			INFO(domain);
			REQUIRE_FALSE(IsReservedMailDomain(domain));
		}
	}
}

TEST_CASE("test/cpp/anofox_email_dns_cacheability", "[anofox_email]") {
	DnsResult result;
	result.success = true;
	REQUIRE(IsCacheableDnsResult(result));
	result.success = false;
	for (const char *reason : {"dns_domain_not_found", "dns_no_records", "dns_null_mx"}) {
		result.reason = reason;
		REQUIRE(IsCacheableDnsResult(result));
	}
	for (const char *reason : {"dns_timeout", "dns_server_unreachable", "dns_init_failed"}) {
		result.reason = reason;
		REQUIRE_FALSE(IsCacheableDnsResult(result));
	}
}

// A failed check is either a definite answer about the address (valid = false)
// or an unknown outcome (valid = NULL): timeouts and unreachable servers say
// nothing about the address.
TEST_CASE("test/cpp/anofox_email_failure_classification", "[anofox_email]") {
	auto is_definite = [](const std::string &reason) {
		return ClassifyEmailFailure(reason) == EmailFailureKind::DEFINITE;
	};

	SECTION("syntax and definite DNS/SMTP answers are definite") {
		for (const char *reason :
		     {"invalid_format", "invalid_length", "dns_resolution_failed",
		      "dns_empty_domain", "dns_reserved_domain", "dns_domain_not_found", "dns_no_records", "dns_null_mx",
		      "smtp_no_hosts", "smtp_dns_reserved_domain", "smtp_dns_domain_not_found", "smtp_dns_no_records",
		      "smtp_dns_no_addresses", "smtp_recipient_rejected"}) {
			INFO(reason);
			REQUIRE(is_definite(reason));
		}
	}

	SECTION("timeouts, unreachable servers and local errors are unknown") {
		for (const char *reason :
		     {"dns_timeout", "dns_server_unreachable", "dns_out_of_memory", "dns_init_failed", "dns_loop_failure",
		      "dns_pending_incomplete", "dns_poll_failed_4", "dns_error_11", "smtp_dns_timeout",
		      "smtp_dns_incomplete", "smtp_dns_init_failed", "smtp_dns_server_unreachable",
		      "smtp_socket_init_failed", "smtp_connect_error", "smtp_connect_timeout", "smtp_greeting_error",
		      "smtp_greeting_timeout", "smtp_invalid_greeting", "smtp_helo_rejected", "smtp_mail_from_rejected",
		      "smtp_recipient_deferred", "smtp_read_error", "smtp_read_timeout", "smtp_write_error",
		      "smtp_write_timeout", "smtp_invalid_response", "smtp_response_too_large", "smtp_overall_timeout",
		      "smtp_verification_failed"}) {
			INFO(reason);
			REQUIRE_FALSE(is_definite(reason));
		}
	}

	SECTION("an unrecognised code is unknown, never a false 'invalid'") {
		REQUIRE_FALSE(is_definite(""));
		REQUIRE_FALSE(is_definite("dns_something_new"));
	}

	SECTION("the definite DNS answers are exactly the cacheable failures") {
		for (const char *reason : {"dns_domain_not_found", "dns_no_records", "dns_null_mx", "dns_timeout",
		                           "dns_server_unreachable", "dns_error_11"}) {
			DnsResult result;
			result.reason = reason;
			INFO(reason);
			REQUIRE(IsCacheableDnsResult(result) == is_definite(reason));
		}
	}
}

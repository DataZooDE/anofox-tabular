// Unit tests for the CA bundle selection used by the NER-model and libpostal
// downloads (anofox_curl_ca.hpp). The released Linux binary carries the RHEL
// bundle path; these tests pin which bundle is chosen on other systems without
// touching the real environment or file system.
//
// Linked into DuckDB's Catch2 `unittest` runner; names start with "test/cpp/".

#include "catch.hpp"

#include "anofox_curl_ca.hpp"

#include <map>
#include <set>
#include <string>

using duckdb::anofox::CaBundleSelection;
using duckdb::anofox::DefaultCaBundleCandidates;
using duckdb::anofox::SelectCaBundle;

namespace {

struct FakeSystem {
	std::map<std::string, std::string> env;
	std::set<std::string> files;

	CaBundleSelection Select(const std::string &curl_default = "/etc/pki/tls/certs/ca-bundle.crt") const {
		return SelectCaBundle(
		    [this](const char *name) -> const char * {
			    auto entry = env.find(name);
			    return entry == env.end() ? nullptr : entry->second.c_str();
		    },
		    [this](const std::string &path) { return files.count(path) != 0; }, curl_default,
		    DefaultCaBundleCandidates());
	}
};

} // namespace

TEST_CASE("test/cpp/anofox_curl_ca_selection", "[anofox_curl_ca]") {
	SECTION("Debian/Ubuntu/Arch: the RHEL default is missing, the system bundle is probed") {
		FakeSystem system;
		system.files = {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem"};
		auto selection = system.Select();
		REQUIRE(selection.ca_file == "/etc/ssl/certs/ca-certificates.crt");
		REQUIRE(selection.source == "probe");
		REQUIRE(selection.ca_path.empty());
	}

	SECTION("RHEL/Fedora: curl's existing default is kept untouched") {
		FakeSystem system;
		system.files = {"/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt"};
		auto selection = system.Select();
		REQUIRE(selection.ca_file.empty());
		REQUIRE(selection.source == "curl default");
	}

	SECTION("openSUSE and macOS bundles are found") {
		FakeSystem suse;
		suse.files = {"/etc/ssl/ca-bundle.pem"};
		REQUIRE(suse.Select().ca_file == "/etc/ssl/ca-bundle.pem");

		FakeSystem mac;
		mac.files = {"/etc/ssl/cert.pem"};
		REQUIRE(mac.Select("").ca_file == "/etc/ssl/cert.pem");

		FakeSystem homebrew;
		homebrew.files = {"/opt/homebrew/etc/openssl@3/cert.pem"};
		REQUIRE(homebrew.Select("").ca_file == "/opt/homebrew/etc/openssl@3/cert.pem");
	}

	SECTION("SSL_CERT_FILE overrides the default and the probe, even when it does not exist") {
		FakeSystem system;
		system.files = {"/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt"};
		system.env = {{"SSL_CERT_FILE", "/opt/corp/ca.pem"}};
		auto selection = system.Select();
		REQUIRE(selection.ca_file == "/opt/corp/ca.pem");
		REQUIRE(selection.source == "SSL_CERT_FILE");
	}

	SECTION("CURL_CA_BUNDLE takes precedence over SSL_CERT_FILE") {
		FakeSystem system;
		system.env = {{"SSL_CERT_FILE", "/a.pem"}, {"CURL_CA_BUNDLE", "/b.pem"}};
		auto selection = system.Select();
		REQUIRE(selection.ca_file == "/b.pem");
		REQUIRE(selection.source == "CURL_CA_BUNDLE");
	}

	SECTION("empty variables count as unset") {
		FakeSystem system;
		system.env = {{"CURL_CA_BUNDLE", ""}, {"SSL_CERT_FILE", ""}, {"SSL_CERT_DIR", ""}};
		system.files = {"/etc/ssl/certs/ca-certificates.crt"};
		auto selection = system.Select();
		REQUIRE(selection.ca_file == "/etc/ssl/certs/ca-certificates.crt");
		REQUIRE(selection.ca_path.empty());
	}

	SECTION("SSL_CERT_DIR sets the CA directory alongside the file") {
		FakeSystem system;
		system.env = {{"SSL_CERT_DIR", "/etc/ssl/certs"}};
		system.files = {"/etc/ssl/certs/ca-certificates.crt"};
		auto selection = system.Select();
		REQUIRE(selection.ca_path == "/etc/ssl/certs");
		REQUIRE(selection.ca_file == "/etc/ssl/certs/ca-certificates.crt");
	}

	SECTION("no bundle anywhere (e.g. Windows): curl's default stays, nothing is set") {
		FakeSystem system;
		auto selection = system.Select("");
		REQUIRE(selection.ca_file.empty());
		REQUIRE(selection.ca_path.empty());
		REQUIRE(selection.source.empty());
	}
}

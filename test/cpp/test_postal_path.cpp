// Unit tests for the libpostal data directory resolution
// (anofox_postal_path.hpp): the home-based default, the fallback to a
// complete cwd-relative copy left by older versions, and explicit paths.
//
// Linked into DuckDB's Catch2 `unittest` runner; names start with "test/cpp/".

#include "catch.hpp"

#include "anofox_postal_path.hpp"

#include <set>
#include <string>

using duckdb::anofox::postal::DEFAULT_POSTAL_DIR;
using duckdb::anofox::postal::LEGACY_POSTAL_DIR;
using duckdb::anofox::postal::PostalDataDirectory;
using duckdb::anofox::postal::ResolvePostalDataDirectory;

namespace {

const std::string HOME = "/home/analyst";
const std::string HOME_DIR = HOME + "/.duckdb/extensions/libpostal";

struct FakeFileSystem {
	std::set<std::string> complete_dirs;
	bool home_known = true;

	PostalDataDirectory Resolve(const std::string &configured) const {
		return ResolvePostalDataDirectory(
		    configured,
		    [](const std::string &path) { return path[0] == '~' ? HOME + path.substr(1) : path; }, home_known,
		    [this](const std::string &dir) { return complete_dirs.count(dir) != 0; });
	}
};

} // namespace

TEST_CASE("test/cpp/anofox_postal_data_directory", "[anofox_postal]") {
	FakeFileSystem fs;

	SECTION("the default is home-based") {
		REQUIRE(std::string(DEFAULT_POSTAL_DIR) == "~/.duckdb/extensions/libpostal");
		auto resolved = fs.Resolve(DEFAULT_POSTAL_DIR);
		REQUIRE(resolved.path == HOME_DIR);
		REQUIRE_FALSE(resolved.legacy_fallback);
	}

	SECTION("an empty setting means the default") {
		REQUIRE(fs.Resolve("").path == HOME_DIR);
	}

	SECTION("a complete cwd-relative copy is used while the home copy is missing") {
		fs.complete_dirs = {LEGACY_POSTAL_DIR};
		auto resolved = fs.Resolve(DEFAULT_POSTAL_DIR);
		REQUIRE(resolved.path == LEGACY_POSTAL_DIR);
		REQUIRE(resolved.legacy_fallback);
	}

	SECTION("a complete home copy wins over the cwd-relative copy") {
		fs.complete_dirs = {LEGACY_POSTAL_DIR, HOME_DIR};
		auto resolved = fs.Resolve(DEFAULT_POSTAL_DIR);
		REQUIRE(resolved.path == HOME_DIR);
		REQUIRE_FALSE(resolved.legacy_fallback);
	}

	SECTION("an explicit path never falls back") {
		fs.complete_dirs = {LEGACY_POSTAL_DIR};
		auto resolved = fs.Resolve("/srv/libpostal");
		REQUIRE(resolved.path == "/srv/libpostal");
		REQUIRE_FALSE(resolved.legacy_fallback);
	}

	SECTION("an explicit path with ~ is expanded") {
		REQUIRE(fs.Resolve("~/data/libpostal").path == HOME + "/data/libpostal");
	}

	SECTION("explicitly setting the old relative default uses it") {
		auto resolved = fs.Resolve(LEGACY_POSTAL_DIR);
		REQUIRE(resolved.path == LEGACY_POSTAL_DIR);
		REQUIRE_FALSE(resolved.legacy_fallback);
	}

	SECTION("without a home directory the cwd-relative directory is used") {
		fs.home_known = false;
		auto resolved = fs.Resolve(DEFAULT_POSTAL_DIR);
		REQUIRE(resolved.path == LEGACY_POSTAL_DIR);
		REQUIRE(resolved.no_home_directory);
	}
}

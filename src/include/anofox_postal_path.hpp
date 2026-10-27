#pragma once

// Resolution of the libpostal data directory (anofox_tab_postal_data_path).
//
// Kept free of libpostal and DuckDB file-system dependencies so the rule can
// be unit tested on every platform (test/cpp/test_postal_path.cpp), even where
// the postal module itself is not compiled.

#include <functional>
#include <string>

namespace duckdb {
namespace anofox {
namespace postal {

//! Default data directory: home-based like the NER model cache, so every
//! working directory shares one ~500 MB copy. `~` is expanded by DuckDB's file
//! system (the `home_directory` setting, else HOME, or USERPROFILE on Windows).
constexpr const char *DEFAULT_POSTAL_DIR = "~/.duckdb/extensions/libpostal";

//! Default of versions before the home-based default: relative to the
//! working directory. A complete copy there is still used while the setting is
//! at its default, so existing installations do not download the data again.
constexpr const char *LEGACY_POSTAL_DIR = ".duckdb/extensions/libpostal";

//! Files whose presence marks a data directory as complete (the same check
//! postal_status() reports as data_present).
constexpr const char *POSTAL_CLASSIFIER_FILE = "language_classifier/language_classifier.dat";
constexpr const char *POSTAL_PARSER_FILE = "address_parser/address_parser_phrases.dat";

struct PostalDataDirectory {
	//! Expanded directory to read from and download into.
	std::string path;
	//! True when the cwd-relative copy of an older version is used.
	bool legacy_fallback = false;
	//! True when the default could not be expanded (no home directory known)
	//! and the cwd-relative directory is used instead.
	bool no_home_directory = false;
};

//! Resolves the configured data directory.
//! - `configured`: the setting value (empty means the default).
//! - `expand`: expands a leading `~` to the home directory.
//! - `home_known`: whether a home directory is available (without one, `~`
//!   cannot be expanded and the cwd-relative directory is used).
//! - `is_complete`: whether an expanded directory holds a complete data set.
//! An explicitly configured path is always used as is. At the default, a
//! complete home copy wins; otherwise a complete cwd-relative legacy copy is
//! used; otherwise the home path is returned (the download target).
inline PostalDataDirectory ResolvePostalDataDirectory(const std::string &configured,
                                                      const std::function<std::string(const std::string &)> &expand,
                                                      bool home_known,
                                                      const std::function<bool(const std::string &)> &is_complete) {
	PostalDataDirectory result;
	const std::string setting = configured.empty() ? std::string(DEFAULT_POSTAL_DIR) : configured;
	if (setting != DEFAULT_POSTAL_DIR) {
		result.path = expand(setting);
		return result;
	}
	if (!home_known) {
		result.path = LEGACY_POSTAL_DIR;
		result.no_home_directory = true;
		return result;
	}
	result.path = expand(setting);
	if (is_complete(result.path)) {
		return result;
	}
	if (is_complete(LEGACY_POSTAL_DIR)) {
		result.path = LEGACY_POSTAL_DIR;
		result.legacy_fallback = true;
	}
	return result;
}

} // namespace postal
} // namespace anofox
} // namespace duckdb

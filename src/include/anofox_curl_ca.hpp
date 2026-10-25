#pragma once

// CA certificate selection for the extension's libcurl downloads (NER model,
// libpostal data).
//
// The released Linux binary is built in a RHEL-based manylinux container, so
// libcurl's compiled-in CA bundle is /etc/pki/tls/certs/ca-bundle.crt. That
// file does not exist on Debian, Ubuntu or Arch, and libcurl then fails every
// HTTPS download with "Problem with the SSL CA cert (path? access rights?)";
// libcurl itself reads no environment variable to recover. The helpers below
// pick a CA bundle that exists on the running system.
//
// The header is deliberately free of <curl/curl.h> so the selection logic can
// be unit-tested without curl include paths.

#include <functional>
#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

//! The CA settings for one download. Empty fields leave libcurl's default.
struct CaBundleSelection {
	//! Value for CURLOPT_CAINFO
	std::string ca_file;
	//! Value for CURLOPT_CAPATH
	std::string ca_path;
	//! Where ca_file came from: "CURL_CA_BUNDLE", "SSL_CERT_FILE", "curl default" or "probe";
	//! empty when no bundle was found and curl's default is kept
	std::string source;
};

//! Bundle files probed when neither environment variable is set and curl's
//! compiled-in bundle is missing, in order: Debian/Ubuntu/Arch/Gentoo,
//! RHEL/Fedora, openSUSE, older RHEL, macOS/BSD/Alpine, Homebrew (Intel, Apple
//! silicon).
const std::vector<std::string> &DefaultCaBundleCandidates();

//! Chooses the CA settings.
//! 1. CURL_CA_BUNDLE, else SSL_CERT_FILE (the curl tool's and OpenSSL's
//!    variables) set the bundle file; an explicit file is used as given, even
//!    when it does not exist, so a wrong setting fails visibly.
//! 2. Otherwise curl's compiled-in bundle (`curl_default`) is kept when it exists.
//! 3. Otherwise the first existing file of `candidates` is used.
//! 4. Otherwise nothing is set and curl's default applies (on Windows and macOS
//!    builds the TLS backend may use the native certificate store).
//! SSL_CERT_DIR, when set, becomes the CA directory independently of the file.
//! `get_env` returns nullptr for an unset variable; empty values count as unset.
CaBundleSelection SelectCaBundle(const std::function<const char *(const char *)> &get_env,
                                 const std::function<bool(const std::string &)> &file_exists,
                                 const std::string &curl_default, const std::vector<std::string> &candidates);

//! Applies SelectCaBundle (real environment and file system, the candidates
//! above) to a libcurl easy handle (`CURL *`, passed as void * to keep curl out
//! of this header) and traces the choice under `subsystem`.
void ConfigureCurlCaBundle(void *curl_handle, const std::string &subsystem);

//! A hint appended to download errors caused by CA certificates (`curl_code`
//! is a CURLcode): names the environment variables that select a bundle.
//! Empty for every other error.
std::string CurlCaErrorHint(int curl_code);

} // namespace anofox
} // namespace duckdb

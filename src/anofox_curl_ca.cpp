#include "anofox_curl_ca.hpp"
#include "anofox_trace.hpp"

#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <system_error>

namespace duckdb {
namespace anofox {

const std::vector<std::string> &DefaultCaBundleCandidates() {
	static const std::vector<std::string> candidates = {
	    "/etc/ssl/certs/ca-certificates.crt",          // Debian, Ubuntu, Arch, Gentoo
	    "/etc/pki/tls/certs/ca-bundle.crt",            // RHEL, Fedora, CentOS
	    "/etc/ssl/ca-bundle.pem",                      // openSUSE
	    "/etc/pki/tls/cacert.pem",                     // older RHEL / OpenELEC
	    "/etc/ssl/cert.pem",                           // macOS, Alpine, BSD
	    "/usr/local/etc/openssl/cert.pem",             // Homebrew (Intel)
	    "/opt/homebrew/etc/openssl@3/cert.pem",        // Homebrew (Apple silicon)
	};
	return candidates;
}

CaBundleSelection SelectCaBundle(const std::function<const char *(const char *)> &get_env,
                                 const std::function<bool(const std::string &)> &file_exists,
                                 const std::string &curl_default, const std::vector<std::string> &candidates) {
	auto env_value = [&](const char *name) -> std::string {
		const char *value = get_env(name);
		return value ? std::string(value) : std::string();
	};

	CaBundleSelection selection;
	selection.ca_path = env_value("SSL_CERT_DIR");

	for (const char *name : {"CURL_CA_BUNDLE", "SSL_CERT_FILE"}) {
		auto value = env_value(name);
		if (!value.empty()) {
			selection.ca_file = value;
			selection.source = name;
			return selection;
		}
	}
	if (!curl_default.empty() && file_exists(curl_default)) {
		// Leave curl's own setting alone; ca_file stays empty
		selection.source = "curl default";
		return selection;
	}
	for (const auto &candidate : candidates) {
		if (file_exists(candidate)) {
			selection.ca_file = candidate;
			selection.source = "probe";
			return selection;
		}
	}
	return selection;
}

void ConfigureCurlCaBundle(void *curl_handle, const std::string &subsystem) {
	auto curl = static_cast<CURL *>(curl_handle);

	// curl's compiled-in bundle (CURLINFO_CAINFO exists since curl 7.84)
	std::string curl_default;
#if LIBCURL_VERSION_NUM >= 0x075400
	char *default_cainfo = nullptr;
	if (curl_easy_getinfo(curl, CURLINFO_CAINFO, &default_cainfo) == CURLE_OK && default_cainfo) {
		curl_default = default_cainfo;
	}
#endif

	auto selection = SelectCaBundle([](const char *name) { return std::getenv(name); },
	                                [](const std::string &path) {
		                                std::error_code ec;
		                                return std::filesystem::is_regular_file(path, ec);
	                                },
	                                curl_default, DefaultCaBundleCandidates());

	if (!selection.ca_file.empty()) {
		curl_easy_setopt(curl, CURLOPT_CAINFO, selection.ca_file.c_str());
	}
	if (!selection.ca_path.empty()) {
		curl_easy_setopt(curl, CURLOPT_CAPATH, selection.ca_path.c_str());
	}

	if (AnofoxTraceConfig::Get().ShouldLog(AnofoxLogLevel::Debug)) {
		std::string message = subsystem + ": TLS CA bundle ";
		if (!selection.ca_file.empty()) {
			message += selection.ca_file + " (" + selection.source + ")";
		} else if (selection.source == "curl default") {
			message += curl_default + " (curl default)";
		} else {
			message += "not found, using curl's default" +
			           (curl_default.empty() ? std::string() : " (" + curl_default + ", missing)");
		}
		if (!selection.ca_path.empty()) {
			message += ", CA directory " + selection.ca_path + " (SSL_CERT_DIR)";
		}
		AnofoxTrace(AnofoxLogLevel::Debug, message);
	}
}

std::string CurlCaErrorHint(int curl_code) {
	if (curl_code == CURLE_SSL_CACERT_BADFILE || curl_code == CURLE_PEER_FAILED_VERIFICATION) {
		return "; point SSL_CERT_FILE (or CURL_CA_BUNDLE) at the system CA bundle, e.g. "
		       "SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt";
	}
	return std::string();
}

} // namespace anofox
} // namespace duckdb

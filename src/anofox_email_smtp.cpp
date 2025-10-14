#include "anofox_email_smtp.hpp"

namespace duckdb {
namespace anofox {
namespace email {

SmtpResult SmtpClient::Verify(const std::string &email, const std::vector<std::string> &mx_hosts) {
    SmtpResult result;
    result.success = false;
	if (mx_hosts.empty()) {
		result.reason = "smtp_no_hosts";
		result.transcript.push_back({"No MX hosts provided for SMTP verification"});
		return result;
	}
	result.reason = "smtp_verification_not_implemented";
	for (auto &host : mx_hosts) {
		result.transcript.push_back({"Skip SMTP check for " + host});
	}
    (void)email;
    return result;
}

} // namespace email
} // namespace anofox
} // namespace duckdb

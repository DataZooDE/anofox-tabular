#include "anofox_email.hpp"
#include "anofox_email_dns.hpp"
#include "anofox_email_smtp.hpp"
#include "anofox_email_logging.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <algorithm>
#include <string>
#include <limits>
#include <mutex>
#include <regex>

namespace duckdb {
namespace anofox {

namespace {

constexpr const char *EMAIL_STAGE_REGEX = "regex";
constexpr const char *EMAIL_STAGE_DNS = "dns";
constexpr const char *EMAIL_STAGE_SMTP = "smtp";
constexpr const char *EMAIL_STAGE_UNSUPPORTED = "unsupported";
constexpr const char *EMAIL_REASON_REGEX_FAIL = "invalid_format";
constexpr const char *EMAIL_REASON_DNS_FAIL = "dns_resolution_failed";
constexpr const char *EMAIL_REASON_SMTP_FAIL = "smtp_verification_failed";
constexpr const char *EMAIL_REASON_UNSUPPORTED = "validation_not_implemented";
constexpr const char *DEFAULT_VALIDATION = "regex";
constexpr const char *DEFAULT_REGEX_PATTERN =
    R"(^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*$)";

struct EmailValidationResult {
	bool valid = false;
	std::string stage;
	std::string reason;
	std::vector<std::string> mx_hosts;
	std::vector<std::string> smtp_transcript;
};

bool IsSupportedValidationMode(const std::string &mode) {
	return mode == EMAIL_STAGE_REGEX || mode == EMAIL_STAGE_DNS || mode == EMAIL_STAGE_SMTP;
}

std::string NormalizeValidationMode(const std::string &value) {
	auto cleaned = value;
	StringUtil::Trim(cleaned);
	auto normalized = StringUtil::Lower(cleaned);
	if (normalized == "full") {
		return EMAIL_STAGE_SMTP;
	}
	if (!IsSupportedValidationMode(normalized) && normalized != EMAIL_STAGE_UNSUPPORTED) {
		return EMAIL_STAGE_UNSUPPORTED;
	}
	return normalized;
}

std::string ExtractDomain(const std::string &email) {
	auto at_pos = email.rfind('@');
	if (at_pos == std::string::npos) {
		return {};
	}
	auto domain_offset = at_pos + 1;
	if (domain_offset >= email.size()) {
		return {};
	}
	return email.substr(domain_offset);
}

class EmailConfig {
public:
	static EmailConfig &Get() {
		static EmailConfig config;
		return config;
	}

	void SetDefaultValidation(const std::string &value) {
		auto normalized = NormalizeValidationMode(value);
		if (!IsSupportedValidationMode(normalized)) {
			throw InvalidInputException("Unsupported email validation type: %s", value);
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		default_validation = normalized;
	}

	void SetRegexPattern(const std::string &pattern) {
		std::lock_guard<std::mutex> lock(config_mutex);
		try {
			auto compiled = std::make_shared<std::regex>(pattern, std::regex_constants::ECMAScript);
			regex_pattern = pattern;
			regex_compiled = std::move(compiled);
		} catch (const std::regex_error &ex) {
			throw InvalidInputException("Invalid email regex pattern: %s", ex.what());
		}
	}

	std::string GetDefaultValidation() {
		std::lock_guard<std::mutex> lock(config_mutex);
		return default_validation;
	}

	std::shared_ptr<const std::regex> GetCompiledRegex() {
		std::lock_guard<std::mutex> lock(config_mutex);
		return regex_compiled;
	}

	std::string GetRegexPattern() {
		std::lock_guard<std::mutex> lock(config_mutex);
		return regex_pattern;
	}

	void SetDnsTimeout(uint32_t timeout_ms) {
		if (timeout_ms < email::DnsOptions::MIN_TIMEOUT_MS || timeout_ms > email::DnsOptions::MAX_TIMEOUT_MS) {
			throw InvalidInputException(
			    "anofox_email_dns_timeout_ms must be between " +
			    std::to_string(email::DnsOptions::MIN_TIMEOUT_MS) + " and " +
			    std::to_string(email::DnsOptions::MAX_TIMEOUT_MS));
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		dns_options.timeout_ms = std::min(std::max(timeout_ms, email::DnsOptions::MIN_TIMEOUT_MS),
		                                 email::DnsOptions::MAX_TIMEOUT_MS);
	}

	void SetDnsTries(uint32_t tries) {
		if (tries == 0) {
			throw InvalidInputException("anofox_email_dns_tries must be greater than 0");
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		dns_options.tries = tries;
	}

	email::DnsOptions GetDnsOptions() {
		std::lock_guard<std::mutex> lock(config_mutex);
		return dns_options;
	}

	void SetSmtpPort(uint16_t port) {
		if (port == 0) {
			throw InvalidInputException("anofox_email_smtp_port must be between 1 and 65535");
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		smtp_options.port = port;
	}

	void SetSmtpConnectTimeout(uint32_t timeout_ms) {
		if (timeout_ms < email::SmtpOptions::MIN_TIMEOUT_MS ||
		    timeout_ms > email::SmtpOptions::MAX_CONNECT_TIMEOUT_MS) {
			throw InvalidInputException(
			    "anofox_email_smtp_connect_timeout_ms must be between " +
			    std::to_string(email::SmtpOptions::MIN_TIMEOUT_MS) + " and " +
			    std::to_string(email::SmtpOptions::MAX_CONNECT_TIMEOUT_MS));
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		smtp_options.connect_timeout_ms =
		    std::min(std::max(timeout_ms, email::SmtpOptions::MIN_TIMEOUT_MS),
		             email::SmtpOptions::MAX_CONNECT_TIMEOUT_MS);
	}

	void SetSmtpReadTimeout(uint32_t timeout_ms) {
		if (timeout_ms < email::SmtpOptions::MIN_TIMEOUT_MS ||
		    timeout_ms > email::SmtpOptions::MAX_READ_TIMEOUT_MS) {
			throw InvalidInputException(
			    "anofox_email_smtp_read_timeout_ms must be between " +
			    std::to_string(email::SmtpOptions::MIN_TIMEOUT_MS) + " and " +
			    std::to_string(email::SmtpOptions::MAX_READ_TIMEOUT_MS));
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		smtp_options.read_timeout_ms =
		    std::min(std::max(timeout_ms, email::SmtpOptions::MIN_TIMEOUT_MS),
		             email::SmtpOptions::MAX_READ_TIMEOUT_MS);
	}

	void SetSmtpHeloDomain(const std::string &value) {
		auto cleaned = value;
		StringUtil::Trim(cleaned);
		if (cleaned.empty()) {
			throw InvalidInputException("anofox_email_smtp_helo_domain cannot be empty");
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		smtp_options.helo_domain = cleaned;
	}

	void SetSmtpMailFrom(const std::string &value) {
		auto cleaned = value;
		StringUtil::Trim(cleaned);
		if (cleaned.empty()) {
			throw InvalidInputException("anofox_email_smtp_mail_from cannot be empty");
		}
		std::lock_guard<std::mutex> lock(config_mutex);
		smtp_options.mail_from = cleaned;
	}

	email::SmtpOptions GetSmtpOptions() {
		std::lock_guard<std::mutex> lock(config_mutex);
		return smtp_options;
	}

	std::vector<std::pair<std::string, std::string>> List() {
		std::lock_guard<std::mutex> lock(config_mutex);
		std::vector<std::pair<std::string, std::string>> entries;
		entries.emplace_back("default_validation", default_validation);
		entries.emplace_back("regex_pattern", regex_pattern);
		entries.emplace_back("dns_timeout_ms", std::to_string(dns_options.timeout_ms));
		entries.emplace_back("dns_tries", std::to_string(dns_options.tries));
		entries.emplace_back("smtp_port", std::to_string(smtp_options.port));
		entries.emplace_back("smtp_connect_timeout_ms", std::to_string(smtp_options.connect_timeout_ms));
		entries.emplace_back("smtp_read_timeout_ms", std::to_string(smtp_options.read_timeout_ms));
		entries.emplace_back("smtp_helo_domain", smtp_options.helo_domain);
		entries.emplace_back("smtp_mail_from", smtp_options.mail_from);
		auto &trace = AnofoxTraceConfig::Get();
		entries.emplace_back("trace_enabled", trace.GetEnabled() ? "true" : "false");
		entries.emplace_back("trace_level", trace.GetLevelString());
		return entries;
	}

private:
	EmailConfig() {
		SetDefaultValidation(DEFAULT_VALIDATION);
		SetRegexPattern(DEFAULT_REGEX_PATTERN);
	}

	std::mutex config_mutex;
	std::string default_validation = DEFAULT_VALIDATION;
	std::string regex_pattern = DEFAULT_REGEX_PATTERN;
	std::shared_ptr<std::regex> regex_compiled;
	email::DnsOptions dns_options;
	email::SmtpOptions smtp_options;
};

EmailValidationResult ValidateRegex(const std::string &email, const std::regex &regex) {
	EmailValidationResult result;
	result.stage = EMAIL_STAGE_REGEX;
	if (std::regex_match(email, regex)) {
		result.valid = true;
	} else {
		result.valid = false;
		result.reason = EMAIL_REASON_REGEX_FAIL;
	}
	return result;
}

EmailValidationResult ValidateEmailAddress(const std::string &email, const std::string &mode_input) {
    EmailTrace(AnofoxLogLevel::Info,
               "ValidateEmailAddress start email=" + email + " mode=" + mode_input);
	auto normalized_mode = NormalizeValidationMode(mode_input);
	auto regex_ptr = EmailConfig::Get().GetCompiledRegex();
	if (!regex_ptr) {
		throw InternalException("Email regex pattern is not initialized");
	}
	auto regex_result = ValidateRegex(email, *regex_ptr);
	if (!regex_result.valid || normalized_mode == EMAIL_STAGE_REGEX) {
        EmailTrace(AnofoxLogLevel::Info,
                   std::string("Regex stage result valid=") + (regex_result.valid ? "true" : "false") +
                       " reason=" +
                       (regex_result.reason.empty() ? std::string("<none>") : regex_result.reason));
		return regex_result;
	}

	if (normalized_mode != EMAIL_STAGE_DNS && normalized_mode != EMAIL_STAGE_SMTP) {
		EmailValidationResult result;
		result.stage = EMAIL_STAGE_UNSUPPORTED;
		result.reason = EMAIL_REASON_UNSUPPORTED;
		return result;
	}

	auto domain = ExtractDomain(email);
	if (domain.empty()) {
	EmailTrace(AnofoxLogLevel::Warn, "No domain extracted from email");
		EmailValidationResult result;
		result.stage = EMAIL_STAGE_DNS;
		result.reason = EMAIL_REASON_DNS_FAIL;
		result.valid = false;
		return result;
	}

    EmailTrace(AnofoxLogLevel::Info, "Performing DNS lookup for domain=" + domain);
	auto dns_options = EmailConfig::Get().GetDnsOptions();
	email::DnsResolver resolver(dns_options);
	auto dns_lookup = resolver.Resolve(domain);

	EmailValidationResult dns_stage;
	dns_stage.stage = EMAIL_STAGE_DNS;
	dns_stage.mx_hosts = dns_lookup.mx_hosts;
	dns_stage.smtp_transcript.clear();

    EmailTrace(AnofoxLogLevel::Info,
               std::string("DNS result success=") + (dns_lookup.success ? "true" : "false") + " reason=" +
                   (dns_lookup.reason.empty() ? std::string("<none>") : dns_lookup.reason) + " hosts=" +
                   std::to_string(dns_lookup.mx_hosts.size()));
	if (!dns_lookup.success) {
		dns_stage.valid = false;
		dns_stage.reason = dns_lookup.reason.empty() ? EMAIL_REASON_DNS_FAIL : dns_lookup.reason;
		return dns_stage;
	}

	dns_stage.valid = true;
	if (normalized_mode == EMAIL_STAGE_DNS) {
		return dns_stage;
	}

	auto smtp_options = EmailConfig::Get().GetSmtpOptions();
    EmailTrace(AnofoxLogLevel::Info,
               "Starting SMTP verification with " + std::to_string(dns_lookup.mx_hosts.size()) +
                   " host candidates");
	email::SmtpClient smtp_client(smtp_options);
	auto smtp_result = smtp_client.Verify(email, dns_lookup.mx_hosts);

	EmailValidationResult smtp_stage;
	smtp_stage.stage = EMAIL_STAGE_SMTP;
	smtp_stage.mx_hosts = dns_lookup.mx_hosts;
	for (auto &entry : smtp_result.transcript) {
		smtp_stage.smtp_transcript.emplace_back(entry.message);
	}

	if (!smtp_result.success) {
        EmailTrace(AnofoxLogLevel::Info,
                   std::string("SMTP verification failed reason=") +
                       (smtp_result.reason.empty() ? std::string("<none>") : smtp_result.reason));
		smtp_stage.valid = false;
		smtp_stage.reason = smtp_result.reason.empty() ? EMAIL_REASON_SMTP_FAIL : smtp_result.reason;
		return smtp_stage;
	}

    EmailTrace(AnofoxLogLevel::Info, "SMTP verification succeeded");
	smtp_stage.valid = true;
	return smtp_stage;
}

std::string ExtractValidationMode(optional_ptr<Vector> vector_ptr, idx_t index, idx_t count) {
	if (!vector_ptr) {
		return EmailConfig::Get().GetDefaultValidation();
	}
	auto &vector = *vector_ptr;
	if (vector.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (ConstantVector::IsNull(vector)) {
			return EmailConfig::Get().GetDefaultValidation();
		}
		auto value = ConstantVector::GetData<string_t>(vector)[0].GetString();
		return NormalizeValidationMode(value);
	}
	auto unified = UnifiedVectorFormat();
	vector.ToUnifiedFormat(count, unified);
	auto idx = unified.sel->get_index(index);
	if (!unified.validity.RowIsValid(idx)) {
		return EmailConfig::Get().GetDefaultValidation();
	}
	auto value = reinterpret_cast<string_t *>(unified.data)[idx].GetString();
	return NormalizeValidationMode(value);
}

LogicalType GetEmailValidateReturnType() {
	child_list_t<LogicalType> smtp_children;
	smtp_children.emplace_back("transcript", LogicalType::LIST(LogicalType::VARCHAR));

	child_list_t<LogicalType> result_children;
	result_children.emplace_back("valid", LogicalType::BOOLEAN);
	result_children.emplace_back("stage", LogicalType::VARCHAR);
	result_children.emplace_back("reason", LogicalType::VARCHAR);
	result_children.emplace_back("mx_hosts", LogicalType::LIST(LogicalType::VARCHAR));
	result_children.emplace_back("smtp_debug", LogicalType::STRUCT(smtp_children));

	return LogicalType::STRUCT(result_children);
}

void EmailValidateFunction(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &children = StructVector::GetEntries(result);
	auto &valid_vec = *children[0];
	auto &stage_vec = *children[1];
	auto &reason_vec = *children[2];
	auto &mx_vec = *children[3];
	auto &smtp_vec = *children[4];
	auto &smtp_children = StructVector::GetEntries(smtp_vec);
	auto &smtp_transcript_vec = *smtp_children[0];

	auto valid_data = FlatVector::GetData<bool>(valid_vec);
	auto mx_entries = FlatVector::GetData<list_entry_t>(mx_vec);
	auto transcript_entries = FlatVector::GetData<list_entry_t>(smtp_transcript_vec);

	ListVector::SetListSize(mx_vec, 0);
	ListVector::SetListSize(smtp_transcript_vec, 0);

	for (idx_t row = 0; row < args.size(); row++) {
		auto email_value = args.GetValue(0, row);
		if (email_value.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		FlatVector::SetNull(result, row, false);

		optional_ptr<Vector> mode_vector;
		if (args.ColumnCount() > 1) {
			mode_vector = &args.data[1];
		}
		std::string mode = ExtractValidationMode(mode_vector, row, args.size());

		auto validation = ValidateEmailAddress(email_value.ToString(), mode);

		FlatVector::SetNull(valid_vec, row, false);
		valid_data[row] = validation.valid;

		FlatVector::SetNull(stage_vec, row, false);
		FlatVector::GetData<string_t>(stage_vec)[row] =
		    StringVector::AddString(stage_vec, validation.stage);

		if (validation.reason.empty()) {
			FlatVector::SetNull(reason_vec, row, true);
		} else {
			FlatVector::SetNull(reason_vec, row, false);
			FlatVector::GetData<string_t>(reason_vec)[row] =
			    StringVector::AddString(reason_vec, validation.reason);
		}

		mx_entries[row].offset = ListVector::GetListSize(mx_vec);
		mx_entries[row].length = validation.mx_hosts.size();
		for (auto &host : validation.mx_hosts) {
			ListVector::PushBack(mx_vec, Value(host));
		}

		transcript_entries[row].offset = ListVector::GetListSize(smtp_transcript_vec);
		transcript_entries[row].length = validation.smtp_transcript.size();
		for (auto &entry : validation.smtp_transcript) {
			ListVector::PushBack(smtp_transcript_vec, Value(entry));
		}
	}
}

void EmailIsValidFunction(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto bool_data = FlatVector::GetData<bool>(result);

	for (idx_t row = 0; row < args.size(); row++) {
		auto email_value = args.GetValue(0, row);
		if (email_value.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		FlatVector::SetNull(result, row, false);

		optional_ptr<Vector> mode_vector;
		if (args.ColumnCount() > 1) {
			mode_vector = &args.data[1];
		}
		std::string mode = ExtractValidationMode(mode_vector, row, args.size());

		auto validation = ValidateEmailAddress(email_value.ToString(), mode);
		bool_data[row] = validation.valid;
	}
}

struct EmailConfigBindData : public TableFunctionData {
    vector<pair<string, string>> entries;
};

struct EmailConfigState : public GlobalTableFunctionState {
    idx_t offset = 0;
};

unique_ptr<FunctionData> EmailConfigBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                         vector<string> &names) {
    auto bind_data = make_uniq<EmailConfigBindData>();
    bind_data->entries = EmailConfig::Get().List();
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
    names = {"key", "value"};
    return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> EmailConfigInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<EmailConfigState>();
}

void EmailConfigFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &entries = input.bind_data->Cast<EmailConfigBindData>().entries;
    auto &state = input.global_state->Cast<EmailConfigState>();
    if (state.offset >= entries.size()) {
        output.SetCardinality(0);
        return;
    }

    idx_t remaining = entries.size() - state.offset;
    idx_t to_write = remaining;
    output.SetCardinality(to_write);

    auto key_data = FlatVector::GetData<string_t>(output.data[0]);
    auto value_data = FlatVector::GetData<string_t>(output.data[1]);

    for (idx_t i = 0; i < to_write; i++) {
        auto &entry = entries[state.offset + i];
        key_data[i] = StringVector::AddString(output.data[0], entry.first);
        value_data[i] = StringVector::AddString(output.data[1], entry.second);
    }
    state.offset += to_write;
}

void SetDefaultValidationOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_default_validation cannot be NULL");
	}
	EmailConfig::Get().SetDefaultValidation(parameter.ToString());
	parameter = Value(EmailConfig::Get().GetDefaultValidation());
}

void SetRegexPatternOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_regex_pattern cannot be NULL");
	}
	EmailConfig::Get().SetRegexPattern(parameter.ToString());
	parameter = Value(EmailConfig::Get().GetRegexPattern());
}

void SetDnsTimeoutOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_dns_timeout_ms cannot be NULL");
	}
	auto value = parameter.GetValue<int64_t>();
	if (value < email::DnsOptions::MIN_TIMEOUT_MS || value > email::DnsOptions::MAX_TIMEOUT_MS) {
		throw InvalidInputException("anofox_email_dns_timeout_ms must be between " +
		                            std::to_string(email::DnsOptions::MIN_TIMEOUT_MS) + " and " +
		                            std::to_string(email::DnsOptions::MAX_TIMEOUT_MS));
	}
	EmailConfig::Get().SetDnsTimeout(static_cast<uint32_t>(value));
	parameter = Value::BIGINT(static_cast<int64_t>(EmailConfig::Get().GetDnsOptions().timeout_ms));
}

void SetDnsTriesOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_dns_tries cannot be NULL");
	}
	auto value = parameter.GetValue<int64_t>();
	if (value <= 0 || value > std::numeric_limits<uint32_t>::max()) {
		throw InvalidInputException("anofox_email_dns_tries must be between 1 and " +
		                            std::to_string(std::numeric_limits<uint32_t>::max()));
	}
	EmailConfig::Get().SetDnsTries(static_cast<uint32_t>(value));
	parameter = Value::INTEGER(static_cast<int32_t>(EmailConfig::Get().GetDnsOptions().tries));
}

void SetSmtpPortOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_smtp_port cannot be NULL");
	}
	auto value = parameter.GetValue<int64_t>();
	if (value <= 0 || value > std::numeric_limits<uint16_t>::max()) {
		throw InvalidInputException("anofox_email_smtp_port must be between 1 and 65535");
	}
	EmailConfig::Get().SetSmtpPort(static_cast<uint16_t>(value));
	parameter = Value::INTEGER(static_cast<int32_t>(EmailConfig::Get().GetSmtpOptions().port));
}

void SetSmtpConnectTimeoutOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_smtp_connect_timeout_ms cannot be NULL");
	}
	auto value = parameter.GetValue<int64_t>();
	if (value < email::SmtpOptions::MIN_TIMEOUT_MS || value > email::SmtpOptions::MAX_CONNECT_TIMEOUT_MS) {
		throw InvalidInputException("anofox_email_smtp_connect_timeout_ms must be between " +
		                            std::to_string(email::SmtpOptions::MIN_TIMEOUT_MS) + " and " +
		                            std::to_string(email::SmtpOptions::MAX_CONNECT_TIMEOUT_MS));
	}
	EmailConfig::Get().SetSmtpConnectTimeout(static_cast<uint32_t>(value));
	parameter = Value::BIGINT(static_cast<int64_t>(EmailConfig::Get().GetSmtpOptions().connect_timeout_ms));
}

void SetSmtpReadTimeoutOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_smtp_read_timeout_ms cannot be NULL");
	}
	auto value = parameter.GetValue<int64_t>();
	if (value < email::SmtpOptions::MIN_TIMEOUT_MS || value > email::SmtpOptions::MAX_READ_TIMEOUT_MS) {
		throw InvalidInputException("anofox_email_smtp_read_timeout_ms must be between " +
		                            std::to_string(email::SmtpOptions::MIN_TIMEOUT_MS) + " and " +
		                            std::to_string(email::SmtpOptions::MAX_READ_TIMEOUT_MS));
	}
	EmailConfig::Get().SetSmtpReadTimeout(static_cast<uint32_t>(value));
	parameter = Value::BIGINT(static_cast<int64_t>(EmailConfig::Get().GetSmtpOptions().read_timeout_ms));
}

void SetSmtpHeloDomainOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_smtp_helo_domain cannot be NULL");
	}
	EmailConfig::Get().SetSmtpHeloDomain(parameter.ToString());
	parameter = Value(EmailConfig::Get().GetSmtpOptions().helo_domain);
}

void SetSmtpMailFromOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_email_smtp_mail_from cannot be NULL");
	}
	EmailConfig::Get().SetSmtpMailFrom(parameter.ToString());
	parameter = Value(EmailConfig::Get().GetSmtpOptions().mail_from);
}

void SetTraceEnabledOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_trace_enabled cannot be NULL");
	}
	bool enabled = parameter.GetValue<bool>();
	AnofoxTraceConfig::Get().SetEnabled(enabled);
	parameter = Value::BOOLEAN(AnofoxTraceConfig::Get().GetEnabled());
}

void SetTraceLevelOption(ClientContext &, SetScope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_trace_level cannot be NULL");
	}
	auto value = StringUtil::Lower(parameter.ToString());
	AnofoxLogLevel parsed;
	if (value == "trace") {
		parsed = AnofoxLogLevel::Trace;
	} else if (value == "debug") {
		parsed = AnofoxLogLevel::Debug;
	} else if (value == "info") {
		parsed = AnofoxLogLevel::Info;
	} else if (value == "warn" || value == "warning") {
		parsed = AnofoxLogLevel::Warn;
	} else if (value == "error") {
		parsed = AnofoxLogLevel::Error;
	} else if (value == "critical") {
		parsed = AnofoxLogLevel::Critical;
	} else if (value == "off" || value == "none") {
		parsed = AnofoxLogLevel::Off;
	} else {
		throw InvalidInputException("Unsupported anofox_trace_level value: %s", value);
	}
	AnofoxTraceConfig::Get().SetLevel(parsed);
	parameter = Value(AnofoxTraceConfig::Get().GetLevelString());
}

} // namespace

void EmailTrace(AnofoxLogLevel level, const std::string &message) {
	AnofoxTrace(level, std::string("email: ") + message);
}

void RegisterEmailOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("anofox_email_default_validation",
	                          "Default validation mode for anofox_email_is_valid (regex, dns, smtp)",
	                          LogicalType::VARCHAR, Value(DEFAULT_VALIDATION), SetDefaultValidationOption);
	config.AddExtensionOption("anofox_email_regex_pattern",
	                          "Regular expression used during email regex validation",
	                          LogicalType::VARCHAR, Value(DEFAULT_REGEX_PATTERN), SetRegexPatternOption);
	auto dns_options = EmailConfig::Get().GetDnsOptions();
	auto smtp_options = EmailConfig::Get().GetSmtpOptions();
	config.AddExtensionOption("anofox_email_dns_timeout_ms",
	                          "DNS resolver timeout in milliseconds",
	                          LogicalType::BIGINT,
	                          Value::BIGINT(static_cast<int64_t>(dns_options.timeout_ms)), SetDnsTimeoutOption);
	config.AddExtensionOption("anofox_email_dns_tries",
	                          "Number of DNS queries to attempt before failing",
	                          LogicalType::INTEGER,
	                          Value::INTEGER(static_cast<int32_t>(dns_options.tries)), SetDnsTriesOption);
	config.AddExtensionOption("anofox_email_smtp_port",
	                          "SMTP port used when connecting to MX hosts",
	                          LogicalType::INTEGER,
	                          Value::INTEGER(static_cast<int32_t>(smtp_options.port)), SetSmtpPortOption);
	config.AddExtensionOption("anofox_email_smtp_connect_timeout_ms",
	                          "SMTP connect timeout in milliseconds",
	                          LogicalType::BIGINT,
	                          Value::BIGINT(static_cast<int64_t>(smtp_options.connect_timeout_ms)),
	                          SetSmtpConnectTimeoutOption);
	config.AddExtensionOption("anofox_email_smtp_read_timeout_ms",
	                          "SMTP read/write timeout in milliseconds",
	                          LogicalType::BIGINT,
	                          Value::BIGINT(static_cast<int64_t>(smtp_options.read_timeout_ms)),
	                          SetSmtpReadTimeoutOption);
	config.AddExtensionOption("anofox_email_smtp_helo_domain",
	                          "Domain value used during SMTP EHLO negotiation",
	                          LogicalType::VARCHAR, Value(smtp_options.helo_domain), SetSmtpHeloDomainOption);
	config.AddExtensionOption("anofox_email_smtp_mail_from",
	                          "MAIL FROM address presented during SMTP verification",
	                          LogicalType::VARCHAR, Value(smtp_options.mail_from), SetSmtpMailFromOption);
	config.AddExtensionOption("anofox_trace_enabled",
	                          "Enable anofox tracing output",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(AnofoxTraceConfig::Get().GetEnabled()),
	                          SetTraceEnabledOption);
	config.AddExtensionOption("anofox_trace_level",
	                          "Minimum tracing level (trace/debug/info/warn/error/critical/off)",
	                          LogicalType::VARCHAR, Value(AnofoxTraceConfig::Get().GetLevelString()),
	                          SetTraceLevelOption);
}

void RegisterEmailFunctions(ExtensionLoader &loader) {
	ScalarFunction validate_fun("anofox_email_validate",
	                            {LogicalType::VARCHAR, LogicalType::VARCHAR}, GetEmailValidateReturnType(),
	                            EmailValidateFunction);
	validate_fun.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	validate_fun.stability = FunctionStability::CONSISTENT;

	ScalarFunction validate_single("anofox_email_validate", {LogicalType::VARCHAR}, GetEmailValidateReturnType(),
	                               EmailValidateFunction);
	validate_single.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	validate_single.stability = FunctionStability::CONSISTENT;

	ScalarFunction is_valid_fun("anofox_email_is_valid", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                            LogicalType::BOOLEAN, EmailIsValidFunction);
	is_valid_fun.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	is_valid_fun.stability = FunctionStability::CONSISTENT;

	ScalarFunction is_valid_single("anofox_email_is_valid", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                               EmailIsValidFunction);
	is_valid_single.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	is_valid_single.stability = FunctionStability::CONSISTENT;

	loader.RegisterFunction(validate_fun);
	loader.RegisterFunction(validate_single);
	loader.RegisterFunction(is_valid_fun);
	loader.RegisterFunction(is_valid_single);

	TableFunction config_fun("anofox_email_config", {}, EmailConfigFunction, EmailConfigBind);
	config_fun.init_global = EmailConfigInit;
	loader.RegisterFunction(config_fun);
}

} // namespace anofox
} // namespace duckdb

#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

#include <string>

namespace duckdb {
namespace anofox {

namespace email {

//! How a failed email check is reported.
//! - DEFINITE: the check got an answer that the address is not usable (bad
//!   syntax, the domain does not exist or accepts no mail, the mail server
//!   rejected the mailbox). email_validate/email_is_valid report valid = false.
//! - UNKNOWN: the check could not finish (a timeout, an unreachable DNS or mail
//!   server, a temporary 4xx SMTP reply, a local resolver or socket problem).
//!   It says nothing about the address, so valid is NULL; the reason is kept.
enum class EmailFailureKind { DEFINITE, UNKNOWN };

//! Classifies the reason code of a failed check. Codes not in the definite
//! list (including future and parameterised codes such as dns_error_<status>)
//! are UNKNOWN, so a new failure mode never turns into a false "invalid".
EmailFailureKind ClassifyEmailFailure(const std::string &reason);

} // namespace email

void RegisterEmailOptions(ExtensionLoader &loader);
void RegisterEmailFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb


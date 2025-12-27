#include "anofox_pii.hpp"
#include "anofox_trace.hpp"
#include "anofox_function_alias.hpp"
#include "telemetry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <openssl/sha.h>

namespace duckdb {
namespace anofox {

// ============================================================================
// Type Conversion Functions
// ============================================================================

std::string PIITypeToString(PIIType type) {
    switch (type) {
        case PIIType::EMAIL: return "EMAIL";
        case PIIType::PHONE: return "PHONE";
        case PIIType::CREDIT_CARD: return "CREDIT_CARD";
        case PIIType::US_SSN: return "US_SSN";
        case PIIType::IP_ADDRESS: return "IP_ADDRESS";
        case PIIType::IBAN: return "IBAN";
        case PIIType::DE_TAX_ID: return "DE_TAX_ID";
        case PIIType::URL: return "URL";
        case PIIType::US_PASSPORT: return "US_PASSPORT";
        case PIIType::CRYPTO_ADDRESS: return "CRYPTO_ADDRESS";
        case PIIType::UK_NINO: return "UK_NINO";
        case PIIType::MAC_ADDRESS: return "MAC_ADDRESS";
        case PIIType::API_KEY: return "API_KEY";
        default: return "UNKNOWN";
    }
}

PIIType StringToPIIType(const std::string &str) {
    auto upper = StringUtil::Upper(str);
    StringUtil::Trim(upper);

    if (upper == "EMAIL") return PIIType::EMAIL;
    if (upper == "PHONE") return PIIType::PHONE;
    if (upper == "CREDIT_CARD" || upper == "CREDITCARD" || upper == "CC") return PIIType::CREDIT_CARD;
    if (upper == "US_SSN" || upper == "SSN") return PIIType::US_SSN;
    if (upper == "IP_ADDRESS" || upper == "IP") return PIIType::IP_ADDRESS;
    if (upper == "IBAN") return PIIType::IBAN;
    if (upper == "DE_TAX_ID" || upper == "STEUER_ID" || upper == "GERMAN_TAX_ID") return PIIType::DE_TAX_ID;
    if (upper == "URL") return PIIType::URL;
    if (upper == "US_PASSPORT" || upper == "PASSPORT") return PIIType::US_PASSPORT;
    if (upper == "CRYPTO_ADDRESS" || upper == "CRYPTO") return PIIType::CRYPTO_ADDRESS;
    if (upper == "UK_NINO" || upper == "NINO") return PIIType::UK_NINO;
    if (upper == "MAC_ADDRESS" || upper == "MAC") return PIIType::MAC_ADDRESS;
    if (upper == "API_KEY" || upper == "APIKEY" || upper == "KEY") return PIIType::API_KEY;
    return PIIType::UNKNOWN;
}

std::string MaskStrategyToString(MaskStrategy strategy) {
    switch (strategy) {
        case MaskStrategy::REDACT: return "REDACT";
        case MaskStrategy::HASH: return "HASH";
        case MaskStrategy::PARTIAL: return "PARTIAL";
        case MaskStrategy::ASTERISK: return "ASTERISK";
        default: return "NONE";
    }
}

MaskStrategy StringToMaskStrategy(const std::string &str) {
    auto upper = StringUtil::Upper(str);
    StringUtil::Trim(upper);

    if (upper == "REDACT" || upper == "REDACTED") return MaskStrategy::REDACT;
    if (upper == "HASH" || upper == "SHA256") return MaskStrategy::HASH;
    if (upper == "PARTIAL") return MaskStrategy::PARTIAL;
    if (upper == "ASTERISK" || upper == "ASTERISKS" || upper == "STAR" || upper == "STARS") return MaskStrategy::ASTERISK;
    return MaskStrategy::NONE;
}

// ============================================================================
// PIIMatch Implementation
// ============================================================================

std::string PIIMatch::ToJSON() const {
    std::ostringstream oss;
    oss << "{\"type\":\"" << PIITypeToString(type) << "\","
        << "\"start\":" << start_pos << ","
        << "\"end\":" << end_pos << ","
        << "\"confidence\":" << std::fixed << std::setprecision(2) << confidence << "}";
    return oss.str();
}

// ============================================================================
// RegexRecognizer Base Implementation
// ============================================================================

RegexRecognizer::RegexRecognizer(PIIType type, const std::string &name, const std::string &pattern)
    : type_(type), name_(name) {
    try {
        pattern_ = std::regex(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
    } catch (const std::regex_error &e) {
        throw InvalidInputException("Invalid regex pattern for %s: %s", name, e.what());
    }
}

RegexRecognizer::~RegexRecognizer() {}

PIIType RegexRecognizer::GetType() const {
    return type_;
}

std::string RegexRecognizer::GetName() const {
    return name_;
}

bool RegexRecognizer::Validate(const std::string &text) const {
    return true;  // Default: no checksum validation
}

std::vector<PIIMatch> RegexRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;
    std::sregex_iterator iter(text.begin(), text.end(), pattern_);
    std::sregex_iterator end;

    while (iter != end) {
        std::smatch match = *iter;
        std::string matched_text = match.str();

        // Validate the match (e.g., checksum for credit cards)
        if (Validate(matched_text)) {
            PIIMatch pii_match(
                type_,
                matched_text,
                static_cast<size_t>(match.position()),
                static_cast<size_t>(match.position() + match.length()),
                1.0  // Full confidence for valid matches
            );
            matches.push_back(pii_match);
        }
        ++iter;
    }
    return matches;
}

std::string RegexRecognizer::GetPartialMask(const std::string &text) const {
    // Default: show first and last 2 characters
    if (text.length() <= 4) {
        return std::string(text.length(), '*');
    }
    return text.substr(0, 2) + std::string(text.length() - 4, '*') + text.substr(text.length() - 2);
}

// ============================================================================
// Credit Card Recognizer
// ============================================================================

CreditCardRecognizer::CreditCardRecognizer()
    : RegexRecognizer(
        PIIType::CREDIT_CARD,
        "Credit Card",
        // Matches major card formats: Visa, Mastercard, Amex, Discover with optional separators
        R"(\b(?:4[0-9]{3}[\s\-]?[0-9]{4}[\s\-]?[0-9]{4}[\s\-]?[0-9]{4}|5[1-5][0-9]{2}[\s\-]?[0-9]{4}[\s\-]?[0-9]{4}[\s\-]?[0-9]{4}|3[47][0-9]{2}[\s\-]?[0-9]{6}[\s\-]?[0-9]{5}|6(?:011|5[0-9]{2})[\s\-]?[0-9]{4}[\s\-]?[0-9]{4}[\s\-]?[0-9]{4})\b)"
    ) {}

CreditCardRecognizer::~CreditCardRecognizer() {
    // Force non-inline destructor to emit vtable in this translation unit
    (void)type_;
}

bool CreditCardRecognizer::Validate(const std::string &text) const {
    return LuhnCheck(text);
}

bool CreditCardRecognizer::LuhnCheck(const std::string &digits) {
    // Extract only digits
    std::string clean;
    for (char c : digits) {
        if (std::isdigit(c)) {
            clean += c;
        }
    }

    if (clean.length() < 13 || clean.length() > 19) {
        return false;
    }

    // Luhn algorithm
    int sum = 0;
    bool alternate = false;
    for (int i = static_cast<int>(clean.length()) - 1; i >= 0; --i) {
        int digit = clean[i] - '0';
        if (alternate) {
            digit *= 2;
            if (digit > 9) {
                digit -= 9;
            }
        }
        sum += digit;
        alternate = !alternate;
    }
    return (sum % 10 == 0);
}

std::string CreditCardRecognizer::GetPartialMask(const std::string &text) const {
    // Extract digits only
    std::string clean;
    for (char c : text) {
        if (std::isdigit(c)) {
            clean += c;
        }
    }

    if (clean.length() < 8) {
        return std::string(text.length(), '*');
    }

    // Show first 4 and last 4 digits: 4111********1111
    std::string first4 = clean.substr(0, 4);
    std::string last4 = clean.substr(clean.length() - 4);
    return first4 + std::string(clean.length() - 8, '*') + last4;
}

// ============================================================================
// US SSN Recognizer
// ============================================================================

USSSNRecognizer::USSSNRecognizer()
    : RegexRecognizer(
        PIIType::US_SSN,
        "US SSN",
        // Matches XXX-XX-XXXX or XXXXXXXXX
        R"(\b(?!000|666|9\d{2})([0-8]\d{2}|7([0-6]\d|7[012]))([\-\s]?)(?!00)\d{2}\3(?!0000)\d{4}\b)"
    ) {}

USSSNRecognizer::~USSSNRecognizer() {}

bool USSSNRecognizer::Validate(const std::string &text) const {
    // Extract digits
    std::string digits;
    for (char c : text) {
        if (std::isdigit(c)) {
            digits += c;
        }
    }

    if (digits.length() != 9) {
        return false;
    }

    // Area number (first 3 digits) cannot be 000, 666, or 900-999
    int area = std::stoi(digits.substr(0, 3));
    if (area == 0 || area == 666 || area >= 900) {
        return false;
    }

    // Group number (middle 2 digits) cannot be 00
    int group = std::stoi(digits.substr(3, 2));
    if (group == 0) {
        return false;
    }

    // Serial number (last 4 digits) cannot be 0000
    int serial = std::stoi(digits.substr(5, 4));
    if (serial == 0) {
        return false;
    }

    return true;
}

std::string USSSNRecognizer::GetPartialMask(const std::string &text) const {
    // Show as ***-**-XXXX (last 4 digits)
    std::string digits;
    for (char c : text) {
        if (std::isdigit(c)) {
            digits += c;
        }
    }

    if (digits.length() < 4) {
        return "***-**-****";
    }

    return "***-**-" + digits.substr(digits.length() - 4);
}

// ============================================================================
// IBAN Recognizer
// ============================================================================

IBANRecognizer::IBANRecognizer()
    : RegexRecognizer(
        PIIType::IBAN,
        "IBAN",
        // Matches IBAN format: 2 letters + 2 check digits + up to 30 alphanumeric
        R"(\b[A-Z]{2}[0-9]{2}[\s]?[A-Z0-9]{4}[\s]?[A-Z0-9]{4}[\s]?[A-Z0-9]{4}[\s]?[A-Z0-9]{0,14}\b)"
    ) {}

IBANRecognizer::~IBANRecognizer() {}

bool IBANRecognizer::Validate(const std::string &text) const {
    return Mod97Check(text);
}

bool IBANRecognizer::Mod97Check(const std::string &iban) {
    // Remove spaces and convert to uppercase
    std::string clean;
    for (char c : iban) {
        if (c != ' ' && c != '-') {
            clean += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    if (clean.length() < 15 || clean.length() > 34) {
        return false;
    }

    // Move first 4 characters to end
    std::string rearranged = clean.substr(4) + clean.substr(0, 4);

    // Convert letters to numbers (A=10, B=11, ..., Z=35)
    std::string numeric;
    for (char c : rearranged) {
        if (std::isdigit(c)) {
            numeric += c;
        } else if (std::isalpha(c)) {
            int value = c - 'A' + 10;
            numeric += std::to_string(value);
        }
    }

    // Calculate MOD 97 in chunks to avoid overflow
    int remainder = 0;
    for (size_t i = 0; i < numeric.length(); i += 7) {
        std::string chunk = std::to_string(remainder) + numeric.substr(i, 7);
        remainder = std::stoll(chunk) % 97;
    }

    return remainder == 1;
}

std::string IBANRecognizer::GetPartialMask(const std::string &text) const {
    // Show country code + check digits + masked: DE89***********
    std::string clean;
    for (char c : text) {
        if (c != ' ' && c != '-') {
            clean += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    if (clean.length() < 4) {
        return std::string(text.length(), '*');
    }

    // Show first 4 (country + check) and last 4
    return clean.substr(0, 4) + std::string(clean.length() - 8, '*') + clean.substr(clean.length() - 4);
}

// ============================================================================
// German Tax ID Recognizer
// ============================================================================

DETaxIDRecognizer::DETaxIDRecognizer()
    : RegexRecognizer(
        PIIType::DE_TAX_ID,
        "German Tax ID",
        // 11 digits, optionally with spaces
        R"(\b[1-9]\d[\s]?\d{3}[\s]?\d{3}[\s]?\d{3}\b)"
    ) {}

DETaxIDRecognizer::~DETaxIDRecognizer() {}

bool DETaxIDRecognizer::Validate(const std::string &text) const {
    return ChecksumValidate(text);
}

bool DETaxIDRecognizer::ChecksumValidate(const std::string &digits) {
    // Extract only digits
    std::string clean;
    for (char c : digits) {
        if (std::isdigit(c)) {
            clean += c;
        }
    }

    if (clean.length() != 11) {
        return false;
    }

    // German Tax ID checksum validation
    // One digit must appear exactly 2 or 3 times, others exactly once
    std::vector<int> counts(10, 0);
    for (size_t i = 0; i < 10; ++i) {
        counts[clean[i] - '0']++;
    }

    int duplicates = 0;
    int triplicates = 0;
    for (int count : counts) {
        if (count == 2) duplicates++;
        if (count == 3) triplicates++;
    }

    // Must have either one duplicate or one triplicate (with one missing digit)
    if (!((duplicates == 1 && triplicates == 0) ||
          (duplicates == 0 && triplicates == 1))) {
        return false;
    }

    // Check digit calculation (simplified - full algorithm is more complex)
    // The 11th digit is a check digit
    int product = 10;
    for (size_t i = 0; i < 10; ++i) {
        int sum = ((clean[i] - '0') + product) % 10;
        if (sum == 0) sum = 10;
        product = (sum * 2) % 11;
    }

    int check_digit = (11 - product) % 10;
    return (clean[10] - '0') == check_digit;
}

std::string DETaxIDRecognizer::GetPartialMask(const std::string &text) const {
    // Show as **-***-***-XXX (last 3 digits)
    std::string digits;
    for (char c : text) {
        if (std::isdigit(c)) {
            digits += c;
        }
    }

    if (digits.length() < 3) {
        return "**-***-***-***";
    }

    return "**-***-***-" + digits.substr(digits.length() - 3);
}

// ============================================================================
// IP Address Recognizer
// ============================================================================

IPAddressRecognizer::IPAddressRecognizer()
    : RegexRecognizer(
        PIIType::IP_ADDRESS,
        "IP Address",
        // IPv4 and simplified IPv6
        R"(\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\b|)"  // IPv4
        R"(\b(?:[0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}\b)"  // Full IPv6
    ) {}

IPAddressRecognizer::~IPAddressRecognizer() {}

std::string IPAddressRecognizer::GetPartialMask(const std::string &text) const {
    // For IPv4: show first octet, mask rest: 192.***.***.**
    auto dot_pos = text.find('.');
    if (dot_pos != std::string::npos && dot_pos < text.length()) {
        return text.substr(0, dot_pos + 1) + "***.***.**";
    }
    // IPv6: show first group
    auto colon_pos = text.find(':');
    if (colon_pos != std::string::npos) {
        return text.substr(0, colon_pos + 1) + "****:****:****:****:****:****:****";
    }
    return std::string(text.length(), '*');
}

// ============================================================================
// URL Recognizer
// ============================================================================

URLRecognizer::URLRecognizer()
    : RegexRecognizer(
        PIIType::URL,
        "URL",
        R"(\bhttps?://[^\s<>\"\{\}\|\\\^\[\]`]+\b)"
    ) {}

URLRecognizer::~URLRecognizer() {}

std::string URLRecognizer::GetPartialMask(const std::string &text) const {
    // Show protocol + domain, mask path: https://example.com/***
    size_t protocol_end = text.find("://");
    if (protocol_end == std::string::npos) {
        return "[URL REDACTED]";
    }

    size_t domain_start = protocol_end + 3;
    size_t path_start = text.find('/', domain_start);
    if (path_start == std::string::npos) {
        return text;  // No path, show full URL
    }

    return text.substr(0, path_start) + "/***";
}

// ============================================================================
// Email Recognizer
// ============================================================================

EmailRecognizer::EmailRecognizer()
    : RegexRecognizer(
        PIIType::EMAIL,
        "Email",
        R"(\b[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\b)"
    ) {}

EmailRecognizer::~EmailRecognizer() {}

std::string EmailRecognizer::GetPartialMask(const std::string &text) const {
    // Show first 2 chars of local part + domain: jo***@example.com
    auto at_pos = text.find('@');
    if (at_pos == std::string::npos || at_pos < 2) {
        return "***@***.***";
    }

    std::string local = text.substr(0, at_pos);
    std::string domain = text.substr(at_pos);

    if (local.length() <= 2) {
        return local + "***" + domain;
    }

    return local.substr(0, 2) + std::string(local.length() - 2, '*') + domain;
}

// ============================================================================
// MAC Address Recognizer
// ============================================================================

MACAddressRecognizer::MACAddressRecognizer()
    : RegexRecognizer(
        PIIType::MAC_ADDRESS,
        "MAC Address",
        // Matches: XX:XX:XX:XX:XX:XX, XX-XX-XX-XX-XX-XX, XXXX.XXXX.XXXX, XXXXXXXXXXXX
        R"(\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b|\b(?:[0-9A-Fa-f]{4}\.){2}[0-9A-Fa-f]{4}\b|\b[0-9A-Fa-f]{12}\b)"
    ) {}

MACAddressRecognizer::~MACAddressRecognizer() {
    (void)type_;
}

std::string MACAddressRecognizer::GetPartialMask(const std::string &text) const {
    // Show first octet (manufacturer OUI), mask rest
    // Detect separator style
    if (text.find(':') != std::string::npos) {
        // XX:XX:XX:XX:XX:XX -> XX:XX:**:**:**:**
        if (text.length() >= 8) {
            return text.substr(0, 5) + ":**:**:**:**";
        }
    } else if (text.find('-') != std::string::npos) {
        // XX-XX-XX-XX-XX-XX -> XX-XX-**-**-**-**
        if (text.length() >= 8) {
            return text.substr(0, 5) + "-**-**-**-**";
        }
    } else if (text.find('.') != std::string::npos) {
        // XXXX.XXXX.XXXX -> XXXX.****.****
        if (text.length() >= 4) {
            return text.substr(0, 4) + ".****.****.";
        }
    } else {
        // XXXXXXXXXXXX -> XXXX********
        if (text.length() >= 4) {
            return text.substr(0, 4) + std::string(text.length() - 4, '*');
        }
    }
    return std::string(text.length(), '*');
}

// ============================================================================
// UK NINO Recognizer
// ============================================================================

UKNINORecognizer::UKNINORecognizer()
    : RegexRecognizer(
        PIIType::UK_NINO,
        "UK National Insurance Number",
        // Matches: AB123456C or AB 12 34 56 C (with optional spaces)
        R"(\b[A-CEGHJ-PR-TW-Z][A-CEGHJ-NPR-TW-Z]\s?\d{2}\s?\d{2}\s?\d{2}\s?[A-D]\b)"
    ) {}

UKNINORecognizer::~UKNINORecognizer() {
    (void)type_;
}

bool UKNINORecognizer::Validate(const std::string &text) const {
    // Remove spaces and uppercase
    std::string clean;
    for (char c : text) {
        if (c != ' ') {
            clean += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    if (clean.length() != 9) return false;

    // Extract prefix (first 2 chars)
    std::string prefix = clean.substr(0, 2);

    // Forbidden prefixes
    static const std::vector<std::string> forbidden = {
        "BG", "GB", "NK", "KN", "TN", "NT", "ZZ"
    };
    for (const auto &fb : forbidden) {
        if (prefix == fb) return false;
    }

    // Suffix must be A, B, C, or D
    char suffix = clean[8];
    if (suffix != 'A' && suffix != 'B' && suffix != 'C' && suffix != 'D') {
        return false;
    }

    return true;
}

std::string UKNINORecognizer::GetPartialMask(const std::string &text) const {
    // Remove spaces and uppercase
    std::string clean;
    for (char c : text) {
        if (c != ' ') {
            clean += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }

    if (clean.length() < 9) {
        return "** ** ** ** *";
    }

    // Show first 2 and last 1: AB ** ** ** C
    return clean.substr(0, 2) + " ** ** ** " + clean.substr(8, 1);
}

// ============================================================================
// US Passport Recognizer
// ============================================================================

USPassportRecognizer::USPassportRecognizer()
    : RegexRecognizer(
        PIIType::US_PASSPORT,
        "US Passport",
        // Matches: 9 digits (current) or 1 letter + 8 digits (legacy)
        R"(\b(?:[A-Z]\d{8}|\d{9})\b)"
    ) {}

USPassportRecognizer::~USPassportRecognizer() {
    (void)type_;
}

std::string USPassportRecognizer::GetPartialMask(const std::string &text) const {
    // Show first character, mask rest
    if (text.length() < 2) {
        return std::string(text.length(), '*');
    }
    return text.substr(0, 1) + std::string(text.length() - 1, '*');
}

// ============================================================================
// Phone Recognizer (pattern-based, lightweight)
// ============================================================================

PhoneRecognizer::PhoneRecognizer()
    : RegexRecognizer(
        PIIType::PHONE,
        "Phone Number",
        // Matches phone formats requiring distinctive features to avoid SSN overlap:
        // - International prefix (+1, +44, etc.) with 7+ digits following
        // - US with parentheses: (555) 123-4567
        // Note: No leading \b for international since + is not a word char
        R"((?:^|[\s])(\+\d{1,4}[\s\-\.]?\(?\d{1,4}\)?[\s\-\.]?\d{1,4}[\s\-\.]?\d{2,4}[\s\-\.]?\d{2,4})(?:$|[\s])|(\(\d{3}\)[\s\-\.]?\d{3}[\s\-\.]?\d{4}))"
    ) {}

PhoneRecognizer::~PhoneRecognizer() {
    (void)type_;
}

std::string PhoneRecognizer::GetPartialMask(const std::string &text) const {
    // Extract digits only
    std::string digits;
    bool has_plus = (!text.empty() && text[0] == '+');
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits += c;
        }
    }

    if (digits.length() < 4) {
        return std::string(text.length(), '*');
    }

    // Show last 4 digits
    std::string prefix = has_plus ? "+***-***-" : "***-***-";
    return prefix + digits.substr(digits.length() - 4);
}

// ============================================================================
// API Key Recognizer
// ============================================================================

APIKeyRecognizer::APIKeyRecognizer()
    : RegexRecognizer(
        PIIType::API_KEY,
        "API Key",
        // Matches: AWS keys (AKIA...), GitHub tokens (ghp_, gho_, ghs_, ghr_), generic high-entropy
        R"(\bAKIA[0-9A-Z]{16}\b|\bgh[pors]_[A-Za-z0-9]{36,255}\b|\b[A-Za-z0-9_\-]{32,64}\b)"
    ) {}

APIKeyRecognizer::~APIKeyRecognizer() {
    (void)type_;
}

double APIKeyRecognizer::CalculateShannonEntropy(const std::string &text) {
    if (text.empty()) return 0.0;

    std::unordered_map<char, int> freq;
    for (char c : text) {
        freq[c]++;
    }

    double entropy = 0.0;
    double len = static_cast<double>(text.length());
    for (const auto &pair : freq) {
        double p = static_cast<double>(pair.second) / len;
        entropy -= p * std::log2(p);
    }
    return entropy;
}

bool APIKeyRecognizer::Validate(const std::string &text) const {
    // AWS keys - specific prefix, always valid
    if (text.length() >= 4 && text.substr(0, 4) == "AKIA") {
        return true;
    }

    // GitHub tokens - specific prefix patterns
    if (text.length() >= 4 && text.substr(0, 2) == "gh") {
        char type_char = text[2];
        if ((type_char == 'p' || type_char == 'o' || type_char == 's' || type_char == 'r') &&
            text[3] == '_') {
            return true;
        }
    }

    // Generic pattern - require high entropy to reduce false positives
    // Minimum 32 chars and entropy >= 3.5 bits/char
    if (text.length() >= 32) {
        double entropy = CalculateShannonEntropy(text);
        return entropy >= 3.5;
    }

    return false;
}

std::string APIKeyRecognizer::GetPartialMask(const std::string &text) const {
    // AWS keys: show AKIA prefix
    if (text.length() >= 4 && text.substr(0, 4) == "AKIA") {
        return "AKIA" + std::string(text.length() - 4, '*');
    }

    // GitHub tokens: show gh*_ prefix
    if (text.length() >= 4 && text.substr(0, 2) == "gh" && text[3] == '_') {
        return text.substr(0, 4) + std::string(text.length() - 4, '*');
    }

    // Generic: show first 4 chars
    if (text.length() > 8) {
        return text.substr(0, 4) + std::string(text.length() - 4, '*');
    }

    return std::string(text.length(), '*');
}

// ============================================================================
// Cryptocurrency Address Recognizer (Bitcoin, Ethereum)
// ============================================================================

CryptoAddressRecognizer::CryptoAddressRecognizer()
    : RegexRecognizer(
        PIIType::CRYPTO_ADDRESS,
        "Crypto Address",
        // Three patterns combined:
        // Bitcoin legacy/P2SH (starts with 1 or 3)
        // Bitcoin SegWit (starts with bc1)
        // Ethereum (starts with 0x)
        R"(\b([13][a-km-zA-HJ-NP-Z1-9]{25,34})\b|\b(bc1[a-z0-9]{39,87})\b|\b(0x[0-9a-fA-F]{40})\b)"
    ) {}

CryptoAddressRecognizer::~CryptoAddressRecognizer() {
    (void)type_;  // Force vtable emission
}

bool CryptoAddressRecognizer::Validate(const std::string &text) const {
    if (text.empty()) return false;

    // Bitcoin legacy/P2SH (starts with 1 or 3)
    if (text[0] == '1' || text[0] == '3') {
        return ValidateBitcoinAddress(text);
    }

    // Bitcoin SegWit (starts with bc1)
    if (text.length() >= 3 && text.substr(0, 3) == "bc1") {
        // Bech32 checksum validation is complex - accept format match
        return text.length() >= 42 && text.length() <= 90;
    }

    // Ethereum (starts with 0x)
    if (text.length() >= 2 && text.substr(0, 2) == "0x") {
        return ValidateEthereumAddress(text);
    }

    return false;
}

bool CryptoAddressRecognizer::ValidateBitcoinAddress(const std::string &address) {
    try {
        std::vector<uint8_t> decoded = DecodeBase58(address);

        // Need at least 25 bytes: 1 version + 20 hash + 4 checksum
        if (decoded.size() < 25) return false;

        // Split into payload and checksum
        size_t payload_size = decoded.size() - 4;
        std::vector<uint8_t> payload(decoded.begin(), decoded.begin() + payload_size);
        std::vector<uint8_t> expected_checksum(decoded.end() - 4, decoded.end());

        // Double SHA-256 hash
        unsigned char hash1[SHA256_DIGEST_LENGTH];
        SHA256(payload.data(), payload.size(), hash1);

        unsigned char hash2[SHA256_DIGEST_LENGTH];
        SHA256(hash1, SHA256_DIGEST_LENGTH, hash2);

        // Compare first 4 bytes of hash2 with checksum
        return std::equal(expected_checksum.begin(), expected_checksum.end(), hash2);

    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> CryptoAddressRecognizer::DecodeBase58(const std::string &input) {
    static const char* BASE58_ALPHABET =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    // Build reverse lookup (lazy init)
    static std::unordered_map<char, int> base58_map;
    if (base58_map.empty()) {
        for (int i = 0; i < 58; ++i) {
            base58_map[BASE58_ALPHABET[i]] = i;
        }
    }

    // Count leading '1's (represent leading zeros in output)
    size_t leading_zeros = 0;
    for (char c : input) {
        if (c == '1') leading_zeros++;
        else break;
    }

    // Convert Base58 to big integer then to bytes
    std::vector<uint8_t> result;
    result.reserve(input.size() * 733 / 1000 + 1);  // log(58) / log(256)

    for (char c : input) {
        auto it = base58_map.find(c);
        if (it == base58_map.end()) {
            throw std::invalid_argument("Invalid Base58 character");
        }

        int carry = it->second;
        for (size_t i = 0; i < result.size(); ++i) {
            carry += 58 * result[i];
            result[i] = carry % 256;
            carry /= 256;
        }
        while (carry > 0) {
            result.push_back(carry % 256);
            carry /= 256;
        }
    }

    // Reverse to get big-endian
    std::reverse(result.begin(), result.end());

    // Add leading zeros
    result.insert(result.begin(), leading_zeros, 0);

    return result;
}

bool CryptoAddressRecognizer::ValidateEthereumAddress(const std::string &address) {
    // Must be exactly 42 chars: 0x + 40 hex
    if (address.length() != 42 || address.substr(0, 2) != "0x") {
        return false;
    }

    // Validate hex characters
    for (size_t i = 2; i < address.length(); ++i) {
        char c = address[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }

    return true;
}

std::string CryptoAddressRecognizer::GetPartialMask(const std::string &text) const {
    // Show first 4 and last 4 characters: bc1q****...****5mdq
    if (text.length() <= 8) {
        return std::string(text.length(), '*');
    }

    return text.substr(0, 4) + std::string(text.length() - 8, '*') +
           text.substr(text.length() - 4);
}

// ============================================================================
// PIIEngine Implementation
// ============================================================================

PIIEngine::PIIEngine() {
    InitializeDefaultRecognizers();
}

PIIEngine& PIIEngine::Instance() {
    static PIIEngine instance;
    return instance;
}

void PIIEngine::InitializeDefaultRecognizers() {
    // Original 7 recognizers
    recognizers_.push_back(std::make_unique<EmailRecognizer>());
    recognizers_.push_back(std::make_unique<CreditCardRecognizer>());
    recognizers_.push_back(std::make_unique<USSSNRecognizer>());
    recognizers_.push_back(std::make_unique<IBANRecognizer>());
    recognizers_.push_back(std::make_unique<DETaxIDRecognizer>());
    recognizers_.push_back(std::make_unique<IPAddressRecognizer>());
    recognizers_.push_back(std::make_unique<URLRecognizer>());

    // New recognizers (Phase 2 expansion)
    recognizers_.push_back(std::make_unique<MACAddressRecognizer>());
    recognizers_.push_back(std::make_unique<UKNINORecognizer>());
    recognizers_.push_back(std::make_unique<USPassportRecognizer>());
    recognizers_.push_back(std::make_unique<PhoneRecognizer>());
    // CryptoAddress must be before APIKey - crypto addresses are high-entropy
    // strings that would otherwise match the generic API_KEY pattern
    recognizers_.push_back(std::make_unique<CryptoAddressRecognizer>());
    recognizers_.push_back(std::make_unique<APIKeyRecognizer>());
}

std::vector<PIIType> PIIEngine::GetSupportedTypes() const {
    std::vector<PIIType> types;
    for (const auto &recognizer : recognizers_) {
        types.push_back(recognizer->GetType());
    }
    return types;
}

void PIIEngine::RegisterRecognizer(std::unique_ptr<PIIRecognizer> recognizer) {
    recognizers_.push_back(std::move(recognizer));
}

const PIIRecognizer* PIIEngine::GetRecognizer(PIIType type) const {
    for (const auto &recognizer : recognizers_) {
        if (recognizer->GetType() == type) {
            return recognizer.get();
        }
    }
    return nullptr;
}

std::vector<PIIMatch> PIIEngine::Detect(
    const std::string &text,
    const std::vector<PIIType> &types
) const {
    std::vector<PIIMatch> all_matches;

    for (const auto &recognizer : recognizers_) {
        // Filter by types if specified
        if (!types.empty()) {
            bool found = false;
            for (const auto &t : types) {
                if (t == recognizer->GetType()) {
                    found = true;
                    break;
                }
            }
            if (!found) continue;
        }

        auto matches = recognizer->FindMatches(text);
        all_matches.insert(all_matches.end(), matches.begin(), matches.end());
    }

    // Sort by position
    std::sort(all_matches.begin(), all_matches.end(),
              [](const PIIMatch &a, const PIIMatch &b) {
                  return a.start_pos < b.start_pos;
              });

    return all_matches;
}

std::string PIIEngine::ApplyMask(const PIIMatch &match, MaskStrategy strategy) const {
    switch (strategy) {
        case MaskStrategy::REDACT:
            return "[" + PIITypeToString(match.type) + "]";

        case MaskStrategy::HASH: {
            // SHA-256 hash, truncated to first 8 chars
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256(reinterpret_cast<const unsigned char*>(match.matched_text.c_str()),
                   match.matched_text.length(), hash);
            std::ostringstream oss;
            for (int i = 0; i < 4; ++i) {
                oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
            }
            return oss.str();
        }

        case MaskStrategy::PARTIAL: {
            // Use type-specific partial masking
            const auto *recognizer = GetRecognizer(match.type);
            if (recognizer) {
                return recognizer->GetPartialMask(match.matched_text);
            }
            return std::string(match.matched_text.length(), '*');
        }

        case MaskStrategy::ASTERISK:
            return std::string(match.matched_text.length(), '*');

        default:
            return match.matched_text;
    }
}

std::string PIIEngine::Mask(
    const std::string &text,
    MaskStrategy strategy,
    const std::vector<PIIType> &types
) const {
    auto matches = Detect(text, types);

    if (matches.empty()) {
        return text;
    }

    // Build result by replacing matches (process in reverse to maintain positions)
    std::string result = text;
    for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
        std::string replacement = ApplyMask(*it, strategy);
        result.replace(it->start_pos, it->end_pos - it->start_pos, replacement);
    }

    return result;
}

// ============================================================================
// DuckDB Scalar Functions
// ============================================================================

namespace {

// anofox_tab_pii_detect(text) -> JSON array of matches (alias: pii_detect)
struct PIIDetectFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();

        UnaryExecutor::Execute<string_t, string_t>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                auto matches = engine.Detect(text);

                // Build JSON array
                std::ostringstream oss;
                oss << "[";
                for (size_t i = 0; i < matches.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << matches[i].ToJSON();
                }
                oss << "]";

                return StringVector::AddString(result, oss.str());
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect", {LogicalType::VARCHAR},
                              LogicalType::VARCHAR, Execute);
    }
};

// anofox_tab_pii_mask(text, strategy) -> masked text (alias: pii_mask)
struct PIIMaskFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();

        BinaryExecutor::Execute<string_t, string_t, string_t>(
            args.data[0], args.data[1], result, args.size(),
            [&](string_t input, string_t strategy_str) {
                std::string text = input.GetString();
                MaskStrategy strategy = StringToMaskStrategy(strategy_str.GetString());

                std::string masked = engine.Mask(text, strategy);
                return StringVector::AddString(result, masked);
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_mask", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                              LogicalType::VARCHAR, Execute);
    }
};

// anofox_tab_pii_mask with default strategy (redact)
struct PIIMaskDefaultFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();

        UnaryExecutor::Execute<string_t, string_t>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                std::string masked = engine.Mask(text, MaskStrategy::REDACT);
                return StringVector::AddString(result, masked);
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_mask", {LogicalType::VARCHAR},
                              LogicalType::VARCHAR, Execute);
    }
};

// anofox_tab_pii_contains(text) -> boolean (true if any PII found) (alias: pii_contains)
struct PIIContainsFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();

        UnaryExecutor::Execute<string_t, bool>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                auto matches = engine.Detect(text);
                return !matches.empty();
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_contains", {LogicalType::VARCHAR},
                              LogicalType::BOOLEAN, Execute);
    }
};

// anofox_tab_pii_count(text) -> integer (count of PII matches) (alias: pii_count)
struct PIICountFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();

        UnaryExecutor::Execute<string_t, int64_t>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                auto matches = engine.Detect(text);
                return static_cast<int64_t>(matches.size());
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_count", {LogicalType::VARCHAR},
                              LogicalType::BIGINT, Execute);
    }
};

// ============================================================================
// pii_status() Table Function
// ============================================================================

struct PIIStatusState : public GlobalTableFunctionState {
    idx_t current_index = 0;
    std::vector<PIIType> types;

    PIIStatusState() {
        types = PIIEngine::Instance().GetSupportedTypes();
    }
};

unique_ptr<GlobalTableFunctionState> PIIStatusInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<PIIStatusState>();
}

unique_ptr<FunctionData> PIIStatusBind(ClientContext &, TableFunctionBindInput &,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("pii_status");

    names.emplace_back("pii_type");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("recognizer_name");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("enabled");
    return_types.emplace_back(LogicalTypeId::BOOLEAN);

    names.emplace_back("pattern_info");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    return nullptr;
}

void PIIStatusFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<PIIStatusState>();

    if (state.current_index >= state.types.size()) {
        output.SetCardinality(0);
        return;
    }

    // Static maps for recognizer metadata
    static const std::unordered_map<PIIType, std::string> recognizer_names = {
        {PIIType::EMAIL, "Email"},
        {PIIType::PHONE, "Phone"},
        {PIIType::CREDIT_CARD, "Credit Card"},
        {PIIType::US_SSN, "US SSN"},
        {PIIType::IP_ADDRESS, "IP Address"},
        {PIIType::IBAN, "IBAN"},
        {PIIType::DE_TAX_ID, "German Tax ID"},
        {PIIType::URL, "URL"},
        {PIIType::US_PASSPORT, "US Passport"},
        {PIIType::CRYPTO_ADDRESS, "Crypto Address"},
        {PIIType::UK_NINO, "UK NINO"},
        {PIIType::MAC_ADDRESS, "MAC Address"},
        {PIIType::API_KEY, "API Key"},
        {PIIType::UNKNOWN, "Unknown"}
    };

    static const std::unordered_map<PIIType, std::string> pattern_info = {
        {PIIType::EMAIL, "RFC 5322 email pattern"},
        {PIIType::PHONE, "International phone formats (+XX, parentheses, dashes)"},
        {PIIType::CREDIT_CARD, "Visa/MC/Amex/Discover with Luhn validation"},
        {PIIType::US_SSN, "XXX-XX-XXXX format with area validation"},
        {PIIType::IP_ADDRESS, "IPv4 and IPv6 formats"},
        {PIIType::IBAN, "ISO 13616 with MOD-97 checksum"},
        {PIIType::DE_TAX_ID, "11-digit Steueridentifikationsnummer"},
        {PIIType::URL, "HTTP/HTTPS URLs"},
        {PIIType::US_PASSPORT, "9 digits or letter + 8 digits"},
        {PIIType::CRYPTO_ADDRESS, "Bitcoin (P2PKH/P2SH/SegWit) and Ethereum"},
        {PIIType::UK_NINO, "UK National Insurance Number format"},
        {PIIType::MAC_ADDRESS, "Colon/hyphen/dot/no-separator formats"},
        {PIIType::API_KEY, "AWS/GitHub tokens and high-entropy strings"},
        {PIIType::UNKNOWN, "Unknown pattern"}
    };

    // Calculate how many rows to output in this batch
    idx_t remaining = state.types.size() - state.current_index;
    idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);

    output.SetCardinality(count);

    for (idx_t i = 0; i < count; i++) {
        PIIType type = state.types[state.current_index + i];

        // Column 0: pii_type
        output.SetValue(0, i, Value(PIITypeToString(type)));

        // Column 1: recognizer_name
        auto name_it = recognizer_names.find(type);
        std::string name = (name_it != recognizer_names.end()) ? name_it->second : "Unknown";
        output.SetValue(1, i, Value(name));

        // Column 2: enabled (always true - no disable mechanism)
        output.SetValue(2, i, Value::BOOLEAN(true));

        // Column 3: pattern_info
        auto info_it = pattern_info.find(type);
        std::string info = (info_it != pattern_info.end()) ? info_it->second : "No description";
        output.SetValue(3, i, Value(info));
    }

    state.current_index += count;
}

TableFunction CreatePIIStatusFunction() {
    return TableFunction("anofox_tab_pii_status", {}, PIIStatusFunction, PIIStatusBind, PIIStatusInit);
}

// ============================================================================
// pii_scan_table() Table Function
// ============================================================================

struct PIIScanTableBindData : public TableFunctionData {
    std::string table_name;
    std::vector<std::string> column_filter;  // Empty = all VARCHAR columns

    PIIScanTableBindData(const std::string &table, const std::vector<std::string> &cols)
        : table_name(table), column_filter(cols) {}
};

struct PIIScanTableResult {
    std::string column_name;
    PIIType pii_type;
    int64_t match_count;
    std::vector<std::string> sample_values;
    double confidence;

    PIIScanTableResult() : pii_type(PIIType::UNKNOWN), match_count(0), confidence(1.0) {}
};

struct PIIScanTableState : public GlobalTableFunctionState {
    idx_t current_index = 0;
    std::vector<PIIScanTableResult> results;
    bool scanned = false;
};

unique_ptr<GlobalTableFunctionState> PIIScanTableInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<PIIScanTableState>();
}

unique_ptr<FunctionData> PIIScanTableBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("pii_scan_table");

    // Get table name (required)
    if (input.inputs.empty()) {
        throw BinderException("pii_scan_table requires a table name");
    }
    auto table_name = input.inputs[0].GetValue<std::string>();

    // Get optional column filter
    std::vector<std::string> column_filter;
    if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
        auto cols_str = input.inputs[1].GetValue<std::string>();
        // Parse comma-separated column names
        std::stringstream ss(cols_str);
        std::string col;
        while (std::getline(ss, col, ',')) {
            // Trim whitespace
            col.erase(0, col.find_first_not_of(" \t"));
            col.erase(col.find_last_not_of(" \t") + 1);
            if (!col.empty()) {
                column_filter.push_back(col);
            }
        }
    }

    // Define output columns
    names.emplace_back("column_name");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("pii_type");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("match_count");
    return_types.emplace_back(LogicalTypeId::BIGINT);

    names.emplace_back("sample_values");
    return_types.emplace_back(LogicalType::LIST(LogicalTypeId::VARCHAR));

    names.emplace_back("confidence");
    return_types.emplace_back(LogicalTypeId::DOUBLE);

    return make_uniq<PIIScanTableBindData>(table_name, column_filter);
}

void PIIScanTableFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<PIIScanTableState>();
    auto &bind_data = input.bind_data->Cast<PIIScanTableBindData>();

    // On first call, scan the table
    if (!state.scanned) {
        state.scanned = true;

        try {
            // Get table metadata from catalog
            auto qname = QualifiedName::Parse(bind_data.table_name);
            auto &catalog = Catalog::GetCatalog(context, qname.catalog);
            auto &entry = catalog.GetEntry<TableCatalogEntry>(context, qname.schema, qname.name);

            // Get list of VARCHAR columns to scan
            std::vector<std::string> columns_to_scan;
            for (auto &col : entry.GetColumns().Logical()) {
                if (col.Type().id() == LogicalTypeId::VARCHAR) {
                    // If column filter is set, check if this column is in the filter
                    if (!bind_data.column_filter.empty()) {
                        bool found = false;
                        for (const auto &filter_col : bind_data.column_filter) {
                            if (filter_col == col.Name()) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) continue;
                    }
                    columns_to_scan.push_back(col.Name());
                }
            }

            // For each column, query values and detect PII
            auto &engine = PIIEngine::Instance();

            for (const auto &col_name : columns_to_scan) {
                // Build query to get column values
                std::string query = "SELECT \"" + col_name + "\" FROM " + bind_data.table_name +
                                   " WHERE \"" + col_name + "\" IS NOT NULL";

                // Execute query using a new connection
                Connection con(*context.db);
                auto result = con.Query(query);

                if (result->HasError()) {
                    AnofoxTrace(AnofoxLogLevel::Warn, "pii_scan_table: Error querying column " + col_name);
                    continue;
                }

                // Aggregate PII detections by type
                std::unordered_map<PIIType, PIIScanTableResult> col_results;

                while (true) {
                    auto chunk = result->Fetch();
                    if (!chunk || chunk->size() == 0) break;

                    for (idx_t row = 0; row < chunk->size(); row++) {
                        auto val = chunk->GetValue(0, row);
                        if (val.IsNull()) continue;

                        auto text = val.GetValue<std::string>();
                        auto matches = engine.Detect(text);

                        for (const auto &match : matches) {
                            auto &res = col_results[match.type];
                            res.column_name = col_name;
                            res.pii_type = match.type;
                            res.match_count++;
                            // Keep up to 5 samples
                            if (res.sample_values.size() < 5) {
                                res.sample_values.push_back(match.matched_text);
                            }
                            res.confidence = 1.0;  // Regex matches have full confidence
                        }
                    }
                }

                // Add results to output
                for (auto &[type, res] : col_results) {
                    state.results.push_back(std::move(res));
                }
            }

        } catch (const std::exception &e) {
            throw InvalidInputException("pii_scan_table: Failed to scan table - %s", e.what());
        }
    }

    // Output results
    if (state.current_index >= state.results.size()) {
        output.SetCardinality(0);
        return;
    }

    idx_t remaining = state.results.size() - state.current_index;
    idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);

    output.SetCardinality(count);

    auto &sample_vec = output.data[3];

    for (idx_t i = 0; i < count; i++) {
        const auto &res = state.results[state.current_index + i];

        // Column 0: column_name
        output.SetValue(0, i, Value(res.column_name));

        // Column 1: pii_type
        output.SetValue(1, i, Value(PIITypeToString(res.pii_type)));

        // Column 2: match_count
        output.SetValue(2, i, Value::BIGINT(res.match_count));

        // Column 3: sample_values (LIST)
        std::vector<Value> sample_list;
        for (const auto &sample : res.sample_values) {
            sample_list.push_back(Value(sample));
        }
        output.SetValue(3, i, Value::LIST(LogicalTypeId::VARCHAR, std::move(sample_list)));

        // Column 4: confidence
        output.SetValue(4, i, Value::DOUBLE(res.confidence));
    }

    state.current_index += count;
}

TableFunctionSet CreatePIIScanTableFunctionSet() {
    TableFunctionSet set("anofox_tab_pii_scan_table");

    // Version 1: table_name only
    TableFunction func1("anofox_tab_pii_scan_table",
                        {LogicalType::VARCHAR},
                        PIIScanTableFunction, PIIScanTableBind, PIIScanTableInit);
    set.AddFunction(func1);

    // Version 2: table_name + column filter
    TableFunction func2("anofox_tab_pii_scan_table",
                        {LogicalType::VARCHAR, LogicalType::VARCHAR},
                        PIIScanTableFunction, PIIScanTableBind, PIIScanTableInit);
    set.AddFunction(func2);

    return set;
}

} // anonymous namespace

// ============================================================================
// Registration
// ============================================================================

void RegisterPIIFunctions(ExtensionLoader &loader) {
    // anofox_tab_pii_detect (alias: pii_detect)
    RegisterScalarFunctionWithAlias(loader, PIIDetectFunction::GetFunction(), "pii_detect");

    // anofox_tab_pii_mask (alias: pii_mask) - with function set for overloads
    ScalarFunctionSet mask_set("anofox_tab_pii_mask");
    mask_set.AddFunction(PIIMaskFunction::GetFunction());
    mask_set.AddFunction(PIIMaskDefaultFunction::GetFunction());
    RegisterScalarFunctionSetWithAlias(loader, mask_set, "pii_mask");

    // anofox_tab_pii_contains (alias: pii_contains)
    RegisterScalarFunctionWithAlias(loader, PIIContainsFunction::GetFunction(), "pii_contains");

    // anofox_tab_pii_count (alias: pii_count)
    RegisterScalarFunctionWithAlias(loader, PIICountFunction::GetFunction(), "pii_count");

    // anofox_tab_pii_status (alias: pii_status) - table function
    auto pii_status_func = CreatePIIStatusFunction();
    RegisterTableFunctionWithAlias(loader, pii_status_func, "pii_status");

    // anofox_tab_pii_scan_table (alias: pii_scan_table) - table function set
    auto pii_scan_set = CreatePIIScanTableFunctionSet();
    RegisterTableFunctionSetWithAlias(loader, pii_scan_set, "pii_scan_table");

    AnofoxTrace(AnofoxLogLevel::Info, "[anofox] PII detection functions registered");
}

} // namespace anofox
} // namespace duckdb

#include "anofox_pii.hpp"
#include "anofox_ner.hpp"
#include "anofox_sql_utils.hpp"
#include "anofox_trace.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_id_column.hpp"
#include "anofox_phonenumber.hpp"
#include "anofox_phonenumber_metadata.hpp"
#include "anofox_money_currency.hpp"
#include "telemetry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "anofox_duckdb_vectors.hpp"
#include "duckdb_compat.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <iterator>
#include <cstring>
#include <cmath>
#include <string_view>
#include <openssl/sha.h>
#include "anofox_tabular_banner.hpp"

namespace duckdb {
namespace anofox {

namespace {
// Intrinsic noise floor for NER-based recognizers (PER/ORG/LOC/MISC).
// Entities below this confidence are model noise and are never reported.
// The user-configured anofox_pii_min_confidence threshold is applied
// centrally in PIIEngine::Detect() on top of this floor.
constexpr double NER_MIN_ENTITY_CONFIDENCE = 0.7;

bool IsNERType(PIIType type) {
    return type == PIIType::NAME || type == PIIType::ORGANIZATION || type == PIIType::LOCATION ||
           type == PIIType::MISC;
}

// A byte that belongs to a word: ASCII letters and digits, any byte of a
// multi-byte UTF-8 character, and the joiners '-' and '\'' ("Klaus-Peter").
bool IsWordByte(unsigned char c) {
    return std::isalnum(c) || c >= 0x80 || c == '-' || c == '\'';
}

// NER spans come from subword tokens and can stop inside a word ("w[NAME]",
// "[LOCATION]ße"). Widen the span to whole words so masking never leaves part
// of a word in clear text.
void WidenToWords(const std::string &text, PIIMatch &match) {
    size_t start = match.start_pos;
    size_t end = match.end_pos;
    while (start > 0 && IsWordByte(static_cast<unsigned char>(text[start - 1]))) {
        start--;
    }
    while (end < text.size() && IsWordByte(static_cast<unsigned char>(text[end]))) {
        end++;
    }
    // Do not swallow a trailing joiner ("Wagner-" at the end of a span)
    while (end > start && (text[end - 1] == '-' || text[end - 1] == '\'')) {
        end--;
    }
    if (start != match.start_pos || end != match.end_pos) {
        match.start_pos = start;
        match.end_pos = end;
        match.matched_text = text.substr(start, end - start);
    }
}

// Words that label a PII field ("IBAN DE89...", "SSN: 123-...") rather than
// name an entity. The NER model tags some of them as ORG or LOC (#118).
bool IsFieldLabel(const std::string &text) {
    static const std::unordered_set<std::string> labels = {
        "IBAN", "BIC", "SWIFT", "SSN", "VAT", "VAT ID", "USTID", "UST-IDNR", "TIN", "NINO", "EIN",
        "ITIN", "PIN", "CVV", "CVC", "DOB", "ID", "EMAIL", "E-MAIL", "PHONE", "TEL", "FAX", "MOBILE",
        "URL", "IP", "MAC", "PASSPORT", "API KEY"};
    std::string upper;
    for (char c : text) {
        if (c == ':' || c == '.' || c == '#') {
            continue;
        }
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    while (!upper.empty() && std::isspace(static_cast<unsigned char>(upper.back()))) {
        upper.pop_back();
    }
    return labels.count(upper) > 0;
}

// An ISO 3166-1 alpha-2 country code ("DE", "GB") or ISO 4217 currency code
// ("EUR", "USD"), written in capitals. The NER model tags such codes in
// master data as ORG or LOC; on their own they identify nobody.
bool IsCountryOrCurrencyCode(const std::string &text) {
    const bool all_upper = !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return c >= 'A' && c <= 'Z';
    });
    if (!all_upper) {
        return false;
    }
    if (text.size() == 2) {
        // ISO 3166-1 alpha-2, officially assigned codes
        static const std::unordered_set<std::string> country_codes = {
            "AD", "AE", "AF", "AG", "AI", "AL", "AM", "AO", "AQ", "AR", "AS", "AT", "AU", "AW", "AX", "AZ", "BA",
            "BB", "BD", "BE", "BF", "BG", "BH", "BI", "BJ", "BL", "BM", "BN", "BO", "BQ", "BR", "BS", "BT", "BV",
            "BW", "BY", "BZ", "CA", "CC", "CD", "CF", "CG", "CH", "CI", "CK", "CL", "CM", "CN", "CO", "CR", "CU",
            "CV", "CW", "CX", "CY", "CZ", "DE", "DJ", "DK", "DM", "DO", "DZ", "EC", "EE", "EG", "EH", "ER", "ES",
            "ET", "FI", "FJ", "FK", "FM", "FO", "FR", "GA", "GB", "GD", "GE", "GF", "GG", "GH", "GI", "GL", "GM",
            "GN", "GP", "GQ", "GR", "GS", "GT", "GU", "GW", "GY", "HK", "HM", "HN", "HR", "HT", "HU", "ID", "IE",
            "IL", "IM", "IN", "IO", "IQ", "IR", "IS", "IT", "JE", "JM", "JO", "JP", "KE", "KG", "KH", "KI", "KM",
            "KN", "KP", "KR", "KW", "KY", "KZ", "LA", "LB", "LC", "LI", "LK", "LR", "LS", "LT", "LU", "LV", "LY",
            "MA", "MC", "MD", "ME", "MF", "MG", "MH", "MK", "ML", "MM", "MN", "MO", "MP", "MQ", "MR", "MS", "MT",
            "MU", "MV", "MW", "MX", "MY", "MZ", "NA", "NC", "NE", "NF", "NG", "NI", "NL", "NO", "NP", "NR", "NU",
            "NZ", "OM", "PA", "PE", "PF", "PG", "PH", "PK", "PL", "PM", "PN", "PR", "PS", "PT", "PW", "PY", "QA",
            "RE", "RO", "RS", "RU", "RW", "SA", "SB", "SC", "SD", "SE", "SG", "SH", "SI", "SJ", "SK", "SL", "SM",
            "SN", "SO", "SR", "SS", "ST", "SV", "SX", "SY", "SZ", "TC", "TD", "TF", "TG", "TH", "TJ", "TK", "TL",
            "TM", "TN", "TO", "TR", "TT", "TV", "TW", "TZ", "UA", "UG", "UM", "US", "UY", "UZ", "VA", "VC", "VE",
            "VG", "VI", "VN", "VU", "WF", "WS", "YE", "YT", "ZA", "ZM", "ZW"};
        return country_codes.count(text) > 0;
    }
    if (text.size() == 3) {
        return CurrencyRegistry::GetInstance().CurrencyExists(text);
    }
    return false;
}

// ---------------------------------------------------------------------------
// UTF-8 code point helpers for masking. Masks keep or replace whole
// characters: a byte-based substr can split a multi-byte sequence and yield
// invalid UTF-8 that DuckDB rejects ("Invalid unicode"). Input is valid UTF-8
// (DuckDB guarantees that for VARCHAR); a code point starts at every byte
// that is not a continuation byte (10xxxxxx).
// ---------------------------------------------------------------------------
bool IsUtf8Continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Number of code points in text
size_t Utf8Length(std::string_view text) {
    return static_cast<size_t>(std::count_if(text.begin(), text.end(),
                                             [](char c) { return !IsUtf8Continuation(c); }));
}

// Byte offset at which code point number n starts (text.size() if n >= length)
size_t Utf8ByteOffset(std::string_view text, size_t n) {
    size_t seen = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!IsUtf8Continuation(text[i])) {
            if (seen == n) {
                return i;
            }
            ++seen;
        }
    }
    return text.size();
}

// First n code points of text
std::string Utf8Prefix(std::string_view text, size_t n) {
    return std::string(text.substr(0, Utf8ByteOffset(text, n)));
}

// Last n code points of text
std::string Utf8Suffix(std::string_view text, size_t n) {
    const size_t length = Utf8Length(text);
    if (n >= length) {
        return std::string(text);
    }
    return std::string(text.substr(Utf8ByteOffset(text, length - n)));
}

// One '*' per character of text
std::string Utf8Stars(std::string_view text) {
    return std::string(Utf8Length(text), '*');
}

// ---------------------------------------------------------------------------
// Partial masks keep at most half of the letters and digits of a value and
// mask short values completely. Each recognizer has its own rule; ApplyMask()
// checks every result against the half rule and falls back to one '*' per
// character, so no type (also one added later) can reveal more.
// ---------------------------------------------------------------------------

// Words of at most this many characters are masked completely by partial
constexpr size_t PARTIAL_SHORT_WORD = 4;

// Letters and digits of text: ASCII letters and digits plus every non-ASCII
// code point. Counts what a partial mask reveals ('*' and separators are not
// counted).
size_t CountLettersAndDigits(std::string_view text) {
    size_t count = 0;
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || (byte >= 0xC0)) {
            ++count;
        }
    }
    return count;
}

// True if text is well-formed UTF-8 (a byte-based partial mask of arbitrary
// pii_mask_value input could cut a character)
bool IsValidUtf8(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k) {
            if (!IsUtf8Continuation(text[i + k])) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

// Partial mask of one word of an entity: words of at most
// PARTIAL_SHORT_WORD characters become stars, longer words keep their first
// `keep` characters but never more than half of them
std::string MaskWordKeepingPrefix(const std::string &word, size_t keep) {
    const size_t length = Utf8Length(word);
    if (length <= PARTIAL_SHORT_WORD) {
        return std::string(length, '*');
    }
    keep = std::min(keep, length / 2);
    return Utf8Prefix(word, keep) + std::string(length - keep, '*');
}

// Canonical form of a matched value for the HASH strategy, so differently
// formatted copies of one value (4111 1111 1111 1111 / 4111-1111-1111-1111)
// hash alike: separators (space, dash, dot, slash, parentheses) are removed
// from numeric identifiers, IBAN and UK NINO are upper-cased, e-mail
// addresses are lower-cased. Other types are hashed as matched.
std::string NormalizeForHash(PIIType type, const std::string &text) {
    switch (type) {
    case PIIType::PHONE:
    case PIIType::CREDIT_CARD:
    case PIIType::US_SSN:
    case PIIType::DE_TAX_ID:
    case PIIType::IBAN:
    case PIIType::UK_NINO:
    case PIIType::DE_SVNR:
    case PIIType::DE_ID_CARD: {
        const bool upper = type == PIIType::IBAN || type == PIIType::UK_NINO || type == PIIType::DE_SVNR ||
                           type == PIIType::DE_ID_CARD;
        std::string normalized;
        normalized.reserve(text.size());
        for (char c : text) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '/' ||
                c == '(' || c == ')') {
                continue;
            }
            normalized += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        }
        return normalized;
    }
    case PIIType::EMAIL: {
        std::string normalized = StringUtil::Lower(text);
        StringUtil::Trim(normalized);
        return normalized;
    }
    default:
        return text;
    }
}
} // anonymous namespace

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
        case PIIType::NAME: return "NAME";
        case PIIType::ORGANIZATION: return "ORGANIZATION";
        case PIIType::LOCATION: return "LOCATION";
        case PIIType::MISC: return "MISC";
        case PIIType::US_PASSPORT: return "US_PASSPORT";
        case PIIType::CRYPTO_ADDRESS: return "CRYPTO_ADDRESS";
        case PIIType::UK_NINO: return "UK_NINO";
        case PIIType::MAC_ADDRESS: return "MAC_ADDRESS";
        case PIIType::API_KEY: return "API_KEY";
        case PIIType::DATE_OF_BIRTH: return "DATE_OF_BIRTH";
        case PIIType::DE_SVNR: return "DE_SVNR";
        case PIIType::DE_ID_CARD: return "DE_ID_CARD";
        case PIIType::POSTAL_CODE: return "POSTAL_CODE";
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
    if (upper == "NAME" || upper == "PERSON" || upper == "PERSON_NAME") return PIIType::NAME;
    if (upper == "ORGANIZATION" || upper == "ORG" || upper == "COMPANY") return PIIType::ORGANIZATION;
    if (upper == "LOCATION" || upper == "LOC" || upper == "PLACE" || upper == "GEO") return PIIType::LOCATION;
    if (upper == "MISC" || upper == "MISCELLANEOUS" || upper == "OTHER") return PIIType::MISC;
    if (upper == "US_PASSPORT" || upper == "PASSPORT") return PIIType::US_PASSPORT;
    if (upper == "CRYPTO_ADDRESS" || upper == "CRYPTO") return PIIType::CRYPTO_ADDRESS;
    if (upper == "UK_NINO" || upper == "NINO") return PIIType::UK_NINO;
    if (upper == "MAC_ADDRESS" || upper == "MAC") return PIIType::MAC_ADDRESS;
    if (upper == "API_KEY" || upper == "APIKEY" || upper == "KEY") return PIIType::API_KEY;
    if (upper == "DATE_OF_BIRTH" || upper == "DOB" || upper == "BIRTH_DATE") return PIIType::DATE_OF_BIRTH;
    if (upper == "DE_SVNR" || upper == "SVNR" || upper == "RVNR") return PIIType::DE_SVNR;
    if (upper == "DE_ID_CARD" || upper == "PERSONALAUSWEIS") return PIIType::DE_ID_CARD;
    if (upper == "POSTAL_CODE" || upper == "POSTCODE" || upper == "ZIP" || upper == "PLZ") return PIIType::POSTAL_CODE;
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

MaskStrategy ParseMaskStrategy(const std::string &str) {
    auto parsed = StringToMaskStrategy(str);
    auto upper = StringUtil::Upper(str);
    StringUtil::Trim(upper);
    if (parsed == MaskStrategy::NONE && upper != "NONE") {
        throw InvalidInputException("Invalid mask strategy: " + str +
                                    ". Valid values: redact, hash, partial, asterisk, none (case-insensitive)");
    }
    return parsed;
}

// ============================================================================
// PII Configuration Singleton
// ============================================================================

bool IsOptInPIIType(PIIType type) {
    // POSTAL_CODE: too many plain 4-5 digit numbers look like one
    return type == PIIType::POSTAL_CODE;
}

bool PIIConfigSnapshot::IsTypeEnabled(PIIType type) const {
    if (enabled_types.empty()) {
        return !IsOptInPIIType(type);  // Default set: all types except opt-in ones
    }
    return std::find(enabled_types.begin(), enabled_types.end(), type) != enabled_types.end();
}

PIIConfig& PIIConfig::Get() {
    static PIIConfig instance;
    return instance;
}

PIIConfig::PIIConfig()
    : min_confidence_(DEFAULT_MIN_CONFIDENCE),
      default_mask_strategy_(MaskStrategy::REDACT),
      enabled_types_() {
}

PIIConfigSnapshot PIIConfig::Snapshot() const {
    std::lock_guard<std::mutex> guard(mutex_);
    PIIConfigSnapshot snapshot;
    snapshot.min_confidence = min_confidence_;
    snapshot.default_mask_strategy = default_mask_strategy_;
    snapshot.enabled_types = enabled_types_;
    snapshot.deep_validation = deep_validation_;
    snapshot.require_checksum = require_checksum_;
    return snapshot;
}

void PIIConfig::SetMinConfidence(double value) {
    if (value < MIN_CONFIDENCE || value > MAX_CONFIDENCE) {
        throw InvalidInputException("anofox_pii_min_confidence must be between " +
                                    std::to_string(MIN_CONFIDENCE) + " and " +
                                    std::to_string(MAX_CONFIDENCE));
    }
    std::lock_guard<std::mutex> guard(mutex_);
    min_confidence_ = value;
}

std::string PIIConfig::GetDefaultMaskStrategyString() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return MaskStrategyToString(default_mask_strategy_);
}

void PIIConfig::SetDefaultMaskStrategy(const std::string &strategy) {
    auto parsed = ParseMaskStrategy(strategy);
    std::lock_guard<std::mutex> guard(mutex_);
    default_mask_strategy_ = parsed;
}

std::string PIIConfig::GetEnabledTypesString() const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (enabled_types_.empty()) {
        return "";  // Empty means all types enabled
    }
    std::ostringstream oss;
    for (size_t i = 0; i < enabled_types_.size(); i++) {
        if (i > 0) oss << ",";
        oss << PIITypeToString(enabled_types_[i]);
    }
    return oss.str();
}

void PIIConfig::SetEnabledTypes(const std::string &types_csv) {
    std::vector<PIIType> parsed_types;

    // Parse comma-separated list (empty = all types enabled)
    std::istringstream iss(types_csv);
    std::string token;
    while (std::getline(iss, token, ',')) {
        StringUtil::Trim(token);
        if (token.empty()) continue;

        // DEFAULT expands to the default set (every type except opt-in ones),
        // so 'DEFAULT,POSTAL_CODE' adds an opt-in type to it
        if (StringUtil::Upper(token) == "DEFAULT") {
            for (int t = 0; t < static_cast<int>(PIIType::UNKNOWN); t++) {
                const auto type = static_cast<PIIType>(t);
                if (!IsOptInPIIType(type) &&
                    std::find(parsed_types.begin(), parsed_types.end(), type) == parsed_types.end()) {
                    parsed_types.push_back(type);
                }
            }
            continue;
        }
        auto pii_type = StringToPIIType(token);
        if (pii_type == PIIType::UNKNOWN) {
            throw InvalidInputException("Unknown PII type: " + token);
        }
        if (std::find(parsed_types.begin(), parsed_types.end(), pii_type) == parsed_types.end()) {
            parsed_types.push_back(pii_type);
        }
    }

    std::lock_guard<std::mutex> guard(mutex_);
    enabled_types_ = std::move(parsed_types);
}

bool PIIConfig::IsDeepValidationEnabled() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return deep_validation_;
}

void PIIConfig::SetDeepValidation(bool enabled) {
    std::lock_guard<std::mutex> guard(mutex_);
    deep_validation_ = enabled;
}

void PIIConfig::SetRequireChecksum(bool enabled) {
    std::lock_guard<std::mutex> guard(mutex_);
    require_checksum_ = enabled;
}

// ============================================================================
// PII Match Return Types (for idiomatic DuckDB API)
// ============================================================================

/**
 * Returns the STRUCT type for a single PII match:
 * STRUCT(type VARCHAR, text VARCHAR, start_pos BIGINT, end_pos BIGINT, confidence DOUBLE)
 */
LogicalType GetPIIMatchStructType() {
    child_list_t<LogicalType> struct_children;
    struct_children.emplace_back("type", LogicalType(LogicalTypeId::VARCHAR));
    struct_children.emplace_back("text", LogicalType(LogicalTypeId::VARCHAR));
    struct_children.emplace_back("start_pos", LogicalType(LogicalTypeId::BIGINT));
    struct_children.emplace_back("end_pos", LogicalType(LogicalTypeId::BIGINT));
    struct_children.emplace_back("confidence", LogicalType(LogicalTypeId::DOUBLE));
    return LogicalType::STRUCT(struct_children);
}

/**
 * Returns the LIST(STRUCT(...)) type for pii_detect return value
 */
LogicalType GetPIIMatchListType() {
    return LogicalType::LIST(GetPIIMatchStructType());
}

// ============================================================================
// PIIMatch Implementation
// ============================================================================

std::string PIIMatch::ToJSON() const {
    std::ostringstream oss;
    // Escape any special JSON characters in matched_text
    std::string escaped_text;
    for (char c : matched_text) {
        switch (c) {
            case '"': escaped_text += "\\\""; break;
            case '\\': escaped_text += "\\\\"; break;
            case '\n': escaped_text += "\\n"; break;
            case '\r': escaped_text += "\\r"; break;
            case '\t': escaped_text += "\\t"; break;
            default: escaped_text += c;
        }
    }
    oss << "{\"type\":\"" << PIITypeToString(type) << "\","
        << "\"text\":\"" << escaped_text << "\","
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
        // Patterns that anchor on whitespace (ECMAScript has no lookbehind)
        // consume it; no PII value starts or ends with whitespace, so it is
        // not part of the match and must survive masking.
        auto start = static_cast<size_t>(match.position());
        auto stop = start + static_cast<size_t>(match.length());
        while (start < stop && std::isspace(static_cast<unsigned char>(text[start]))) {
            start++;
        }
        while (stop > start && std::isspace(static_cast<unsigned char>(text[stop - 1]))) {
            stop--;
        }
        std::string matched_text = text.substr(start, stop - start);

        // Validate the match (e.g., checksum for credit cards)
        if (!matched_text.empty() && Validate(matched_text)) {
            PIIMatch pii_match(
                type_,
                matched_text,
                start,
                stop,
                1.0  // Full confidence for valid matches
            );
            matches.push_back(pii_match);
        }
        ++iter;
    }
    return matches;
}

std::string RegexRecognizer::GetPartialMask(const std::string &text) const {
    // Default: keep the first and last character for 5 to 7 characters, the
    // first and last 2 from 8 characters on (at most a quarter at each end);
    // short values are masked completely
    const size_t length = Utf8Length(text);
    if (length <= PARTIAL_SHORT_WORD) {
        return std::string(length, '*');
    }
    const size_t keep = std::min<size_t>(2, length / 4);
    return Utf8Prefix(text, keep) + std::string(length - 2 * keep, '*') + Utf8Suffix(text, keep);
}

// ============================================================================
// Credit Card Recognizer
// ============================================================================

CreditCardRecognizer::CreditCardRecognizer()
    : RegexRecognizer(
        PIIType::CREDIT_CARD,
        "Credit Card",
        // FindMatches scans digit groups directly; the pattern only describes
        // the accepted shape: 13-19 digits, optionally in groups joined by one
        // space, dash or dot.
        R"(\b\d{4}(?:[ .\-]?\d{1,6}){1,6}\b)"
    ) {}

CreditCardRecognizer::~CreditCardRecognizer() {
    // Force non-inline destructor to emit vtable in this translation unit
    (void)type_;
}

namespace {

bool IsAsciiDigitAt(const std::string &text, size_t pos) {
    return pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]));
}

bool IsAsciiWordAt(const std::string &text, size_t pos) {
    if (pos >= text.size()) {
        return false;
    }
    const auto c = static_cast<unsigned char>(text[pos]);
    return std::isalnum(c) || c == '_';
}

// Card numbers are at most 19 digits; longer chains of digit groups (tables
// of figures, long reference numbers) are not searched for card numbers.
constexpr size_t CARD_MAX_CHAIN_GROUPS = 32;

} // namespace

std::vector<PIIMatch> CreditCardRecognizer::FindMatches(const std::string &text) const {
    return ScanCardChains(text, false, 1.0);
}

std::vector<PIIMatch> CreditCardRecognizer::ScanCardChains(const std::string &text, bool checksum_failures,
                                                           double confidence) const {
    // A candidate is a chain of digit groups joined by one separator (space,
    // dash or dot) that is the same throughout: 4111111111111111,
    // 4111 1111 1111 1111, 3782-822463-10005, 4111.1111.1111.1111.
    // A chain that continues a longer token (DE89 3704 ..., a decimal
    // 1.4111..., a phone number +4111...) is skipped. Inside a chain of
    // space-separated groups the first run of groups with 13-19 digits that
    // passes the Luhn checksum and carries a brand prefix is a card number
    // ("Ref 12 4111 1111 1111 1111").
    std::vector<PIIMatch> matches;
    const size_t n = text.size();
    size_t pos = 0;
    while (pos < n) {
        if (!IsAsciiDigitAt(text, pos)) {
            pos++;
            continue;
        }
        // Collect the chain of groups as [begin, end) byte ranges
        std::vector<std::pair<size_t, size_t>> groups;
        char separator = 0;
        size_t end = pos;
        while (true) {
            const size_t group_begin = end;
            while (IsAsciiDigitAt(text, end)) {
                end++;
            }
            groups.emplace_back(group_begin, end);
            if (end < n && (text[end] == ' ' || text[end] == '-' || text[end] == '.') &&
                (separator == 0 || text[end] == separator) && IsAsciiDigitAt(text, end + 1)) {
                separator = text[end];
                end++;
                continue;
            }
            break;
        }
        const size_t chain_begin = pos;
        pos = end;

        // The chain must stand on its own
        if (chain_begin > 0) {
            const char before = text[chain_begin - 1];
            if (IsAsciiWordAt(text, chain_begin - 1) || before == '+') {
                continue;
            }
            if ((before == '.' || before == ',' || before == '-' || before == '/') && chain_begin >= 2 &&
                IsAsciiDigitAt(text, chain_begin - 2)) {
                continue;
            }
        }
        if (IsAsciiWordAt(text, end)) {
            continue;
        }
        if (end < n && (text[end] == '.' || text[end] == ',' || text[end] == '-' || text[end] == '/') &&
            IsAsciiDigitAt(text, end + 1)) {
            continue;
        }
        if (groups.size() > CARD_MAX_CHAIN_GROUPS) {
            AnofoxTrace(AnofoxLogLevel::Trace, "pii: digit chain too long for a card number at byte " +
                                                   std::to_string(chain_begin));
            continue;
        }

        // Search the runs of groups [first, last], longest first. Groups
        // joined by dashes or dots form one token ("4111111111111111-01"),
        // which is a card number only as a whole; space-separated groups can
        // hold a card number next to other figures.
        const bool whole_chain_only = separator == '-' || separator == '.';
        size_t first = 0;
        while (first < groups.size()) {
            bool found = false;
            for (size_t last = groups.size(); last-- > first;) {
                if (whole_chain_only && (first != 0 || last + 1 != groups.size())) {
                    continue;
                }
                std::string digits;
                bool shape_ok = true;
                for (size_t g = first; g <= last; ++g) {
                    const size_t len = groups[g].second - groups[g].first;
                    // Separated: a 4-digit first group, 3-6 digits in the
                    // middle groups, 1-6 in the last (4-4-4-4, 4-6-5, 4-4-4-4-3)
                    if (last > first && ((g == first && len != 4) || (g > first && g < last && (len < 3 || len > 6)) ||
                                         (g == last && len > 6))) {
                        shape_ok = false;
                        break;
                    }
                    digits.append(text, groups[g].first, len);
                }
                if (!shape_ok || digits.size() < 13 || digits.size() > 19) {
                    continue;
                }
                // checksum_failures (anofox_pii_require_checksum = false) looks
                // for the same branded chains whose Luhn checksum fails
                if (!HasCardBrandPrefix(digits) || LuhnCheck(digits) == checksum_failures) {
                    AnofoxTrace(AnofoxLogLevel::Trace, "pii: card candidate failed Luhn or brand check: " + digits);
                    continue;
                }
                const size_t start = groups[first].first;
                const size_t stop = groups[last].second;
                matches.emplace_back(type_, text.substr(start, stop - start), start, stop, confidence);
                first = last + 1;
                found = true;
                break;
            }
            if (!found) {
                first++;
            }
        }
    }
    return matches;
}

bool CreditCardRecognizer::HasCardBrandPrefix(const std::string &digits) {
    // IIN ranges and issued lengths of the card brands:
    // Visa 4 (13, 16, 19); Mastercard 51-55, 2221-2720 (16); Amex 34, 37 (15);
    // Discover 6011, 644-649, 65 (16-19); Maestro 5018, 5020, 5038, 5893,
    // 6304, 6759, 6761-6763 (12-19); JCB 3528-3589 (16-19); Diners Club
    // 300-305, 36, 38, 39 (14-19); UnionPay 62 (16-19)
    const size_t len = digits.size();
    if (len < 4) {
        return false;
    }
    const int p2 = std::stoi(digits.substr(0, 2));
    const int p3 = std::stoi(digits.substr(0, 3));
    const int p4 = std::stoi(digits.substr(0, 4));
    auto in = [](int value, int low, int high) { return value >= low && value <= high; };

    if (digits[0] == '4') {
        return len == 13 || len == 16 || len == 19;
    }
    if (in(p2, 51, 55) || in(p4, 2221, 2720)) {
        return len == 16;
    }
    if (p2 == 34 || p2 == 37) {
        return len == 15;
    }
    if (p4 == 6011 || in(p3, 644, 649) || p2 == 65) {
        return in(static_cast<int>(len), 16, 19);
    }
    if (p4 == 5018 || p4 == 5020 || p4 == 5038 || p4 == 5893 || p4 == 6304 || p4 == 6759 || in(p4, 6761, 6763)) {
        return in(static_cast<int>(len), 12, 19);
    }
    if (in(p4, 3528, 3589)) {
        return in(static_cast<int>(len), 16, 19);
    }
    if (in(p3, 300, 305) || p2 == 36 || p2 == 38 || p2 == 39) {
        return in(static_cast<int>(len), 14, 19);
    }
    if (p2 == 62) {
        return in(static_cast<int>(len), 16, 19);
    }
    return false;
}

bool CreditCardRecognizer::Validate(const std::string &text) const {
    return LuhnCheck(text);
}

bool CreditCardRecognizer::LuhnCheck(const std::string &digits) {
    // Extract only digits
    std::string clean;
    for (char c : digits) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
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
        if (std::isdigit(static_cast<unsigned char>(c))) {
            clean += c;
        }
    }

    if (clean.length() < 8) {
        return Utf8Stars(text);
    }

    // Last 4 digits, plus the first 4 (issuer) from 16 digits on, so at most
    // half of the digits: 4111********1111, Amex ***********0005
    const size_t first = clean.length() >= 16 ? 4 : 0;
    return clean.substr(0, first) + std::string(clean.length() - first - 4, '*') +
           clean.substr(clean.length() - 4);
}

// ============================================================================
// US SSN Recognizer
// ============================================================================

USSSNRecognizer::USSSNRecognizer()
    : RegexRecognizer(
        PIIType::US_SSN,
        "US SSN",
        // Matches XXX-XX-XXXX or XXX XX XXXX. Bare 9-digit numbers are left
        // out: contract, order and claim numbers look the same.
        R"(\b(?!000|666|9\d{2})([0-8]\d{2}|7([0-6]\d|7[012]))([\- ])(?!00)\d{2}\3(?!0000)\d{4}\b)"
    ) {}

USSSNRecognizer::~USSSNRecognizer() {}

bool USSSNRecognizer::Validate(const std::string &text) const {
    // Extract digits
    std::string digits;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
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
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits += c;
        }
    }

    // Last 4 of 9 digits; fewer than 8 digits are no SSN, mask them all
    if (digits.length() < 8) {
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
        // 2 letters + 2 check digits, then groups of four alphanumerics and a
        // shorter last group. The groups are either all compact or all
        // separated by the same single space or dash (DE89 3704 0044 ...,
        // DE89-3704-0044-..., DE89370400440532013000).
        R"(\b[A-Z]{2}[0-9]{2}([ \-]?)[A-Z0-9]{4}(?:\1[A-Z0-9]{4}){1,6}(?:\1[A-Z0-9]{1,3})?\b)"
    ) {}

IBANRecognizer::~IBANRecognizer() {}

std::vector<PIIMatch> IBANRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;
    std::sregex_iterator end;
    for (std::sregex_iterator iter(text.begin(), text.end(), pattern_); iter != end; ++iter) {
        const auto &match = *iter;
        auto start = static_cast<size_t>(match.position());
        std::string candidate = match.str();
        const std::string separator = match[1].str();

        // A grouped IBAN can be followed by a word that looks like one more
        // group ("AT61 1904 3002 3457 3201 und"). When the checksum fails,
        // drop trailing groups until it holds or too little is left.
        bool valid = Validate(candidate);
        while (!valid && !separator.empty()) {
            auto cut = candidate.rfind(separator[0]);
            if (cut == std::string::npos || cut < 4) {
                break;
            }
            candidate.resize(cut);
            valid = Validate(candidate);
        }
        if (valid) {
            matches.emplace_back(type_, candidate, start, start + candidate.size(), 1.0);
        } else {
            AnofoxTrace(AnofoxLogLevel::Trace, "pii: IBAN candidate failed validation: " + match.str());
        }
    }
    return matches;
}

bool IBANRecognizer::Validate(const std::string &text) const {
    return IsValidIban(text);
}

bool IBANRecognizer::IsValidIban(const std::string &iban) {
    // MOD-97 alone accepts about 1 in 97 hex ids that start with two letters
    // (md5 digests, object ids); a real IBAN also has a registered country
    // code and exactly that country's length.
    if (!Mod97Check(iban)) {
        return false;
    }
    std::string country;
    size_t length = 0;
    for (char c : iban) {
        if (c == ' ' || c == '-') {
            continue;
        }
        if (country.size() < 2) {
            country += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        length++;
    }
    const size_t expected = RegisteredLength(country);
    if (expected == 0 || expected != length) {
        AnofoxTrace(AnofoxLogLevel::Trace, "pii: IBAN candidate passes MOD-97 but has no registered country/length: " +
                                               country + "/" + std::to_string(length));
        return false;
    }
    return true;
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
        if (std::isdigit(static_cast<unsigned char>(c))) {
            numeric += c;
        } else if (std::isalpha(static_cast<unsigned char>(c))) {
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

    if (clean.length() < 10) {
        return Utf8Stars(clean);
    }

    // Country code and check digits, plus up to 4 trailing characters while
    // at most half of the IBAN stays readable: DE89**************3000
    const size_t last = std::min<size_t>(4, clean.length() / 2 - 4);
    return clean.substr(0, 4) + std::string(clean.length() - 4 - last, '*') +
           clean.substr(clean.length() - last);
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
        if (std::isdigit(static_cast<unsigned char>(c))) {
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
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits += c;
        }
    }

    // Last 3 of 11 digits; fewer than 9 digits are masked completely
    if (digits.length() < 9) {
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
        // IPv4; IPv6 is found by FindMatches
        R"(\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\b)"
    ) {}

IPAddressRecognizer::~IPAddressRecognizer() {}

namespace {

bool IsHexDigitChar(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool IsValidIPv4(const std::string &text) {
    size_t octets = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t dot = std::min(text.find('.', pos), text.size());
        const std::string octet = text.substr(pos, dot - pos);
        if (octet.empty() || octet.size() > 3 ||
            !std::all_of(octet.begin(), octet.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) ||
            std::stoi(octet) > 255) {
            return false;
        }
        octets++;
        pos = dot + 1;
    }
    return octets == 4;
}

// RFC 4291 text forms: eight groups of 1-4 hex digits, one "::" for a run of
// zero groups, and an IPv4 address in place of the last two groups
// (::ffff:192.0.2.1). Addresses without any decimal digit (cafe::beef, and
// C++ or Rust paths such as a::b) are not reported.
bool IsValidIPv6(const std::string &text) {
    if (text.size() < 3 || text.find(":::") != std::string::npos ||
        std::none_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        return false;
    }
    const size_t compressed = text.find("::");
    if (compressed != std::string::npos && text.find("::", compressed + 1) != std::string::npos) {
        return false;
    }
    std::vector<std::string> parts;
    auto split = [&parts](const std::string &side) {
        if (side.empty()) {
            return;
        }
        size_t pos = 0;
        while (pos <= side.size()) {
            const size_t colon = std::min(side.find(':', pos), side.size());
            parts.push_back(side.substr(pos, colon - pos));
            pos = colon + 1;
        }
    };
    if (compressed == std::string::npos) {
        split(text);
    } else {
        split(text.substr(0, compressed));
        split(text.substr(compressed + 2));
    }
    size_t group_count = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        if (part.find('.') != std::string::npos) {
            if (i + 1 != parts.size() || !IsValidIPv4(part)) {
                return false;
            }
            group_count += 2;
            continue;
        }
        if (part.empty() || part.size() > 4 || !std::all_of(part.begin(), part.end(), IsHexDigitChar)) {
            return false;
        }
        group_count++;
    }
    return compressed == std::string::npos ? group_count == 8 : (group_count >= 1 && group_count <= 7);
}

} // namespace

std::vector<PIIMatch> IPAddressRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> ipv6_matches;
    const size_t n = text.size();
    auto is_ipv6_char = [&text](size_t pos) {
        return IsHexDigitChar(text[pos]) || text[pos] == ':' || text[pos] == '.';
    };
    size_t pos = 0;
    while (pos < n) {
        if (!is_ipv6_char(pos)) {
            pos++;
            continue;
        }
        size_t begin = pos;
        size_t end = pos;
        while (end < n && is_ipv6_char(end)) {
            end++;
        }
        pos = end;
        if (std::count(text.begin() + static_cast<std::ptrdiff_t>(begin), text.begin() + static_cast<std::ptrdiff_t>(end), ':') < 2) {
            continue;
        }
        // A label colon or sentence punctuation around the address is not
        // part of it ("IP:2001:db8::1.", "2001:db8::1: unreachable")
        if (text[begin] == ':' && (begin + 1 >= end || text[begin + 1] != ':')) {
            begin++;
        } else if (begin > 0 && IsAsciiWordAt(text, begin - 1)) {
            continue;
        }
        while (end > begin && text[end - 1] == '.') {
            end--;
        }
        if (end - begin >= 2 && text[end - 1] == ':' && text[end - 2] != ':') {
            end--;
        }
        if (IsAsciiWordAt(text, pos) && text[pos] != '%') {
            continue;
        }
        std::string candidate = text.substr(begin, end - begin);
        if (!IsValidIPv6(candidate)) {
            AnofoxTrace(AnofoxLogLevel::Trace, "pii: IPv6 candidate rejected: " + candidate);
            continue;
        }
        // Zone index of a link-local address: fe80::1%eth0
        if (end == pos && end < n && text[end] == '%') {
            size_t zone_end = end + 1;
            while (zone_end < n && (IsAsciiWordAt(text, zone_end) || text[zone_end] == '-' || text[zone_end] == '.')) {
                zone_end++;
            }
            if (zone_end > end + 1) {
                end = zone_end;
                pos = zone_end;
            }
        }
        ipv6_matches.emplace_back(type_, text.substr(begin, end - begin), begin, end, 1.0);
    }

    // IPv4, except the IPv4 tail of an IPv6 address (::ffff:192.0.2.1)
    std::vector<PIIMatch> matches;
    for (auto &match : RegexRecognizer::FindMatches(text)) {
        const bool inside_ipv6 = std::any_of(ipv6_matches.begin(), ipv6_matches.end(), [&match](const PIIMatch &v6) {
            return match.start_pos < v6.end_pos && v6.start_pos < match.end_pos;
        });
        if (!inside_ipv6) {
            matches.push_back(std::move(match));
        }
    }
    matches.insert(matches.end(), ipv6_matches.begin(), ipv6_matches.end());
    std::sort(matches.begin(), matches.end(),
              [](const PIIMatch &a, const PIIMatch &b) { return a.start_pos < b.start_pos; });
    return matches;
}

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
    // Keep the scheme and the top-level domain; mask every other host label
    // and replace path, query and fragment with "/***":
    // https://crm.example.com/customers/4711 -> https://***.*******.com/***
    // (a host such as anna-schmidt.blog can name a person)
    size_t protocol_end = text.find("://");
    if (protocol_end == std::string::npos) {
        return Utf8Stars(text);
    }

    const size_t host_start = protocol_end + 3;
    size_t host_end = text.find_first_of("/?#", host_start);
    const bool has_path = host_end != std::string::npos;
    if (!has_path) {
        host_end = text.size();
    }
    const std::string host = text.substr(host_start, host_end - host_start);

    // The TLD stays readable when it is letters only (no port, no IP octet)
    size_t tld_start = host.rfind('.');
    if (tld_start != std::string::npos) {
        const std::string tld = host.substr(tld_start + 1);
        const bool letters_only = !tld.empty() && std::all_of(tld.begin(), tld.end(), [](char c) {
            return std::isalpha(static_cast<unsigned char>(c));
        });
        if (!letters_only) {
            tld_start = std::string::npos;
        }
    }

    std::string masked_host;
    const std::string_view masked_part = std::string_view(host).substr(0, tld_start == std::string::npos ? host.size() : tld_start);
    size_t label_start = 0;
    while (label_start <= masked_part.size()) {
        size_t dot = masked_part.find('.', label_start);
        if (dot == std::string_view::npos) {
            dot = masked_part.size();
        }
        masked_host += Utf8Stars(masked_part.substr(label_start, dot - label_start));
        if (dot < masked_part.size()) {
            masked_host += '.';
        }
        label_start = dot + 1;
    }
    if (tld_start != std::string::npos) {
        masked_host += host.substr(tld_start);
    }

    return text.substr(0, host_start) + masked_host + (has_path ? "/***" : "");
}

// ============================================================================
// Email Recognizer
// ============================================================================

namespace {

// Email pattern with non-ASCII letters (jörg, müller, weiß, пётр).
// std::regex matches bytes, so such a letter is a UTF-8 sequence: a lead byte
// C3-DF, E1 or E3-EF and one or two continuation bytes. C2 and E2 are left out
// so NBSP, quotes and dashes (« » „ “ –) around an address stay outside it.
// The local part and each domain label start with a letter or digit, labels
// also end with one. The local part has at most 64 characters and a label at
// most 63 (RFC 5321); the bounds keep every match attempt short.
std::string BuildEmailPattern() {
    const std::string utf8_letter = R"([\xC3-\xDF\xE1\xE3-\xEF][\x80-\xBF]{1,2})";
    const std::string alnum = "(?:[A-Za-z0-9]|" + utf8_letter + ")";
    const std::string label = alnum + "(?:(?:[A-Za-z0-9-]|" + utf8_letter + "){0,61}" + alnum + ")?";
    const std::string local = alnum + "(?:[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]|" + utf8_letter + "){0,63}";
    return local + "@" + label + R"((?:\.)" + label + ")*";
}

// Bytes that can be part of a local part: the ASCII characters of the pattern
// and the bytes of the non-ASCII letters it accepts.
bool IsEmailLocalByte(unsigned char c) {
    if (c >= 0x80) {
        return c <= 0xBF || (c >= 0xC3 && c <= 0xDF) || c == 0xE1 || (c >= 0xE3 && c <= 0xEF);
    }
    return std::isalnum(c) || std::strchr(".!#$%&'*+/=?^_`{|}~-", c) != nullptr;
}

// A byte that can start the local part: an ASCII letter or digit, or the lead
// byte of an accepted non-ASCII letter.
bool IsEmailAlnumStart(unsigned char c) {
    return std::isalnum(c) || (c >= 0xC3 && c <= 0xDF) || c == 0xE1 || (c >= 0xE3 && c <= 0xEF);
}

// Upper bound of the bytes an address can span after its '@': 253 characters
// of domain, of which non-ASCII letters take up to three bytes.
constexpr size_t EMAIL_DOMAIN_MAX_BYTES = 3 * 253;

} // namespace

EmailRecognizer::EmailRecognizer()
    : RegexRecognizer(
        PIIType::EMAIL,
        "Email",
        BuildEmailPattern()
    ) {}

std::vector<PIIMatch> EmailRecognizer::FindMatches(const std::string &text) const {
    // Every address contains an '@', so the pattern only runs where there is
    // one. Searching the whole text instead let std::regex try each byte as a
    // start and rescan the run of word characters after it: quadratic in the
    // length of long tokens such as base64 attachments.
    // The local part is the run of local-part bytes before the '@', starting
    // at its first letter or digit (the start the leftmost match would pick).
    // Runs of different '@' do not overlap, so the backward scans are linear
    // in total; the match itself is bounded by the pattern.
    std::vector<PIIMatch> matches;
    size_t consumed = 0;  // Bytes before this belong to an earlier match or '@'
    for (size_t at = text.find('@'); at != std::string::npos; at = text.find('@', at + 1)) {
        size_t start = at;
        while (start > consumed && IsEmailLocalByte(static_cast<unsigned char>(text[start - 1]))) {
            start--;
        }
        while (start < at && !IsEmailAlnumStart(static_cast<unsigned char>(text[start]))) {
            start++;
        }
        if (start == at) {
            consumed = at + 1;
            continue;
        }
        const auto window_end = text.begin() + static_cast<std::ptrdiff_t>(std::min(text.size(), at + 1 + EMAIL_DOMAIN_MAX_BYTES));
        std::smatch match;
        if (!std::regex_search(text.begin() + static_cast<std::ptrdiff_t>(start), window_end, match, pattern_,
                               std::regex_constants::match_continuous)) {
            // E.g. a local part over 64 characters or no domain after the '@'
            AnofoxTrace(AnofoxLogLevel::Trace, "pii: no email address around '@' at byte " + std::to_string(at));
            consumed = at + 1;
            continue;
        }
        const size_t stop = start + static_cast<size_t>(match.length());
        matches.emplace_back(type_, text.substr(start, stop - start), start, stop, 1.0);
        consumed = stop;
        at = stop - 1;
    }
    return matches;
}

EmailRecognizer::~EmailRecognizer() {}

std::string EmailRecognizer::GetPartialMask(const std::string &text) const {
    // Keep the first character of the local part and the top-level domain;
    // every other character becomes '*' (dots between domain labels stay):
    // jane.doe@acme-corp.com -> j*******@*********.com, ab@cd.de -> a*@**.de.
    // The domain is masked too: a company or personal domain identifies the
    // person as well as the local part does. Characters are code points.
    auto at_pos = text.rfind('@');
    if (at_pos == std::string::npos) {
        return Utf8Stars(text);
    }

    const std::string local = text.substr(0, at_pos);
    const std::string domain = text.substr(at_pos + 1);
    const size_t local_length = Utf8Length(local);
    std::string masked = local_length >= 2 ? Utf8Prefix(local, 1) + std::string(local_length - 1, '*')
                                           : std::string(local_length, '*');
    masked += '@';

    const size_t tld_start = domain.rfind('.');
    size_t label_start = 0;
    while (true) {
        size_t dot = domain.find('.', label_start);
        if (dot == std::string::npos) {
            break;
        }
        masked += Utf8Stars(std::string_view(domain).substr(label_start, dot - label_start)) + ".";
        label_start = dot + 1;
    }
    if (tld_start == std::string::npos) {
        masked += Utf8Stars(domain);
    } else {
        masked += domain.substr(tld_start + 1);
    }
    return masked;
}

// ============================================================================
// MAC Address Recognizer
// ============================================================================

MACAddressRecognizer::MACAddressRecognizer()
    : RegexRecognizer(
        PIIType::MAC_ADDRESS,
        "MAC Address",
        // Matches: XX:XX:XX:XX:XX:XX, XX-XX-XX-XX-XX-XX, XXXX.XXXX.XXXX, XXXXXXXXXXXX.
        // The bare 12-character form is a whole token with at least one
        // letter A-F and one digit, so plain 12-digit numbers (e.g. the
        // digits of +491711234567) are not MACs; a Dutch VAT number body
        // (9 digits, B, 2 digits as in NL 004495445B01) is excluded as well.
        // The dotted form is exactly three groups with at least one letter
        // A-F: digit-only groups are card, account or reference numbers
        // (4111.1111.1111.1111), not MACs.
        R"(\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b|)"
        R"(\b(?=[0-9.]{0,13}[A-Fa-f])(?:[0-9A-Fa-f]{4}\.){2}[0-9A-Fa-f]{4}\b(?!\.[0-9A-Fa-f])|)"
        R"(\b(?!\d{9}B\d{2}\b)(?=[0-9A-F]*[A-F])(?=[A-F]*\d)[0-9A-F]{12}\b)"
    ) {}

MACAddressRecognizer::~MACAddressRecognizer() {
    (void)type_;
}

std::vector<PIIMatch> MACAddressRecognizer::FindMatches(const std::string &text) const {
    // The pattern cannot look behind: drop a dotted match that continues
    // earlier groups ("aabb.ccdd.eeff.0011" has four groups, no MAC has)
    auto matches = RegexRecognizer::FindMatches(text);
    matches.erase(std::remove_if(matches.begin(), matches.end(),
                                 [&text](const PIIMatch &match) {
                                     return match.matched_text.size() == 14 && match.matched_text[4] == '.' &&
                                            match.start_pos >= 2 && text[match.start_pos - 1] == '.' &&
                                            std::isxdigit(static_cast<unsigned char>(text[match.start_pos - 2]));
                                 }),
                  matches.end());
    return matches;
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
            return text.substr(0, 4) + ".****.****";
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
        // Matches 1 letter + 8 digits. Bare 9-digit passport numbers are not
        // matched: without a checksum they are indistinguishable from any
        // other 9-digit reference number.
        R"(\b[A-Z]\d{8}\b)"
    ) {}

USPassportRecognizer::~USPassportRecognizer() {
    (void)type_;
}

std::string USPassportRecognizer::GetPartialMask(const std::string &text) const {
    // Show first character (the letter), mask the digits
    const size_t length = Utf8Length(text);
    if (length <= PARTIAL_SHORT_WORD) {
        return std::string(length, '*');
    }
    return Utf8Prefix(text, 1) + std::string(length - 1, '*');
}

// ============================================================================
// Phone Recognizer (pattern-based, lightweight)
// ============================================================================

PhoneRecognizer::PhoneRecognizer()
    : RegexRecognizer(
        PIIType::PHONE,
        "Phone Number",
        // Group 1: a number that starts at the beginning of the text or after
        // any character that is no letter, digit or '+' (whitespace, quotes,
        // '=', ',', '>', '|', '/', ...; ECMAScript has no lookbehind, so that
        // character is consumed and FindMatches reports group 1 only). This
        // covers JSON values, CSV fields, HTML cells and "tel=+49...":
        // - international: +CC or 00CC (00 form needs a separator after the
        //   country code), then up to six digit groups or a parenthesised
        //   group such as "(0)", "(415)": +49 (0) 221 4711-0, +491711234567
        // - national: an area code 0XX..0XXXXX, compact numbers are not
        //   matched; "(089) 123 456 78", "0171 1234567", "089/12345678",
        //   "089-123456-78"
        // The number must end at the end of text or at a character that is no
        // letter, digit or '+' ("</td>", '"', '&', ...).
        // Group 2: US style (555) 123-4567.
        // Dots are no separator in national numbers so dates (25.10.2026),
        // amounts and version strings stay unmatched; Validate() checks the
        // digit count.
        R"((?:^|[^A-Za-z0-9+]))"
        R"(((?:\+\d{1,3}|00[1-9]\d{0,2}(?=[ \-./]))(?:[ \-./]?\(\d{1,4}\)|[ \-./]?\d{1,5}){1,6})"
        R"(|(?:\(0[1-9]\d{1,4}\) ?|0[1-9]\d{1,4}[ \-/])\d{2,8}(?:[ \-/]\d{1,8}){0,3}))"
        R"((?=$|[^A-Za-z0-9+]))"
        R"(|(\(\d{3}\)[\s\-\.]?\d{3}[\s\-\.]?\d{4}))"
    ) {}

PhoneRecognizer::~PhoneRecognizer() {
    (void)type_;
}

std::vector<PIIMatch> PhoneRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;
    std::sregex_iterator end;
    for (std::sregex_iterator iter(text.begin(), text.end(), pattern_); iter != end; ++iter) {
        const auto &match = *iter;
        const size_t group = match[1].matched ? 1 : (match[2].matched ? 2 : 0);
        if (group == 0) {
            continue;
        }
        auto start = static_cast<size_t>(match.position(group));
        std::string number = match[group].str();

        // A number without "+" right after a digit and a separator (space,
        // '/', '.') continues a longer digit sequence, e.g. the
        // "0044 0532 0130 00" groups of an IBAN. A dash joins it to any
        // letter or digit before ("20261027-0001", the groups of a UUID).
        if (group == 1 && number[0] != '+' && start >= 2) {
            const auto before = static_cast<unsigned char>(text[start - 1]);
            const auto previous = static_cast<unsigned char>(text[start - 2]);
            const bool continues = (before == '-' && std::isalnum(previous)) ||
                                   ((std::isspace(before) || before == '/' || before == '.') && std::isdigit(previous));
            if (continues) {
                AnofoxTrace(AnofoxLogLevel::Trace, "pii: phone candidate continues a digit sequence: " + number);
                continue;
            }
        }
        // Likewise a dash right after the number that leads on to letters or
        // digits ("0123-4567-8901-abcdef...") makes it part of a longer token
        const size_t stop = start + number.size();
        if (group == 1 && stop + 1 < text.size() && text[stop] == '-' &&
            std::isalnum(static_cast<unsigned char>(text[stop + 1]))) {
            AnofoxTrace(AnofoxLogLevel::Trace, "pii: phone candidate continues into a longer token: " + number);
            continue;
        }
        if (Validate(number)) {
            matches.emplace_back(type_, number, start, start + number.size(), 1.0);
        }
    }
    return matches;
}

bool PhoneRecognizer::Validate(const std::string &text) const {
    // E.164 numbers have at most 15 digits; fewer than 7 is a short code or
    // a fragment rather than a phone number.
    size_t digit_count = 0;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digit_count++;
        }
    }
    if (digit_count < 7 || digit_count > 15) {
        return false;
    }
    // The 00 of an international prefix is no part of the number itself;
    // without this, SSN-like "001-45-6789" would pass as a phone number.
    if (text.compare(0, 2, "00") == 0 && digit_count - 2 < 8) {
        return false;
    }

    // Without deep validation, the pattern and the digit count are enough
    if (!PIIConfig::Get().IsDeepValidationEnabled()) {
        return true;
    }
    try {
        return PassesDeepValidation(text);
    } catch (const std::exception &e) {
        AnofoxTrace(AnofoxLogLevel::Debug, "pii: phone deep validation failed, keeping the match: " + std::string(e.what()));
        return true;
    }
}

bool PhoneRecognizer::PassesDeepValidation(const std::string &text) {
    // The phone module judges a number written with +CC (or the default
    // region's international prefix) for that country and a national number
    // for anofox_tab_phonenumber_default_region. A match is rejected only
    // when the number has an impossible length there. A national number in
    // another country's format (0171 1234567 with the default region US,
    // which has no trunk prefix 0) cannot be judged and is kept, so deep
    // validation never unmasks a number just because the region differs.
    auto &manager = phonenumber::PhoneNumberManager::Instance();
    const auto parts = manager.Parse(text, "");
    if (parts.valid) {
        return true;
    }
    if (parts.reason != phonenumber::PhoneNumberInvalidReason::INVALID_LENGTH) {
        // Unknown country code or unsupported region: no judgement possible
        AnofoxTrace(AnofoxLogLevel::Trace, "pii: phone deep validation cannot judge " + text + " (" + parts.reason + ")");
        return true;
    }
    std::string digits;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits += c;
        }
    }
    const bool international = !text.empty() && text[0] == '+';
    if (!international) {
        const auto region = manager.EffectiveDefaultRegion("");
        const auto *metadata = phonenumber::GetMetadataForRegion(region);
        const bool region_idd = metadata && !metadata->international_prefix.empty() &&
                                digits.compare(0, metadata->international_prefix.size(), metadata->international_prefix) == 0;
        const bool region_trunk = metadata && !metadata->national_prefix.empty() &&
                                  digits.compare(0, metadata->national_prefix.size(), metadata->national_prefix) == 0;
        if (!region_idd && !region_trunk && !digits.empty() && digits[0] == '0') {
            AnofoxTrace(AnofoxLogLevel::Trace,
                        "pii: phone deep validation keeps " + text + ": not in the national format of " + region);
            return true;
        }
    }
    AnofoxTrace(AnofoxLogLevel::Trace, "pii: phone deep validation rejects " + text + " (invalid length)");
    return false;
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

    if (digits.length() <= PARTIAL_SHORT_WORD) {
        return Utf8Stars(text);
    }

    // Show the last 4 digits, never more than half of them
    const size_t keep = std::min<size_t>(4, digits.length() / 2);
    std::string prefix = has_plus ? "+***-***-" : "***-***-";
    return prefix + digits.substr(digits.length() - keep);
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

std::vector<PIIMatch> APIKeyRecognizer::FindMatches(const std::string &text) const {
    // A generic high-entropy run next to '+' or '/' is a piece of a longer
    // base64 or path token (an inline attachment, a data URI), not a key on
    // its own; AWS and GitHub keys are recognised by their prefix anyway.
    auto matches = RegexRecognizer::FindMatches(text);
    auto is_base64_joint = [&text](size_t pos) { return pos < text.size() && (text[pos] == '+' || text[pos] == '/'); };
    matches.erase(std::remove_if(matches.begin(), matches.end(),
                                 [&](const PIIMatch &match) {
                                     const auto &value = match.matched_text;
                                     const bool prefixed = value.compare(0, 4, "AKIA") == 0 ||
                                                           (value.size() > 4 && value.compare(0, 2, "gh") == 0 && value[3] == '_');
                                     return !prefixed && ((match.start_pos > 0 && is_base64_joint(match.start_pos - 1)) ||
                                                          is_base64_joint(match.end_pos));
                                 }),
                  matches.end());
    return matches;
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
    // Immutable reverse lookup table. Initialization of the function-local
    // static is thread-safe (C++11 magic statics) and the table is never
    // mutated afterwards, so concurrent lookups are race-free.
    static const std::array<int, 256> BASE58_LOOKUP = [] {
        std::array<int, 256> table{};
        table.fill(-1);
        const char *alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
        for (int i = 0; i < 58; ++i) {
            table[static_cast<unsigned char>(alphabet[i])] = i;
        }
        return table;
    }();

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
        int carry = BASE58_LOOKUP[static_cast<unsigned char>(c)];
        if (carry < 0) {
            throw std::invalid_argument("Invalid Base58 character");
        }
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
// Name Recognizer (Dictionary-based)
// ============================================================================

namespace {

// Common first names dictionary (top names from US, UK, EU census data)
// Stored in lowercase for case-insensitive matching
const std::unordered_set<std::string>& GetCommonNames() {
    static std::unordered_set<std::string> names = {
        // Male names (US/UK/EU common)
        "james", "john", "robert", "michael", "david", "william", "richard", "joseph",
        "thomas", "charles", "christopher", "daniel", "matthew", "anthony", "mark",
        "donald", "steven", "paul", "andrew", "joshua", "kenneth", "kevin", "brian",
        "george", "timothy", "ronald", "edward", "jason", "jeffrey", "ryan", "jacob",
        "gary", "nicholas", "eric", "jonathan", "stephen", "larry", "justin", "scott",
        "brandon", "benjamin", "samuel", "raymond", "gregory", "frank", "alexander",
        "patrick", "jack", "dennis", "jerry", "tyler", "aaron", "jose", "adam", "nathan",
        "henry", "douglas", "zachary", "peter", "kyle", "noah", "ethan", "jeremy",
        "walter", "christian", "keith", "roger", "terry", "austin", "sean", "gerald",
        "carl", "harold", "dylan", "arthur", "lawrence", "jordan", "jesse", "bryan",
        "billy", "bruce", "gabriel", "joe", "logan", "albert", "willie", "alan", "eugene",
        "ralph", "roy", "louis", "russell", "philip", "harry", "vincent", "bobby", "johnny", "bob",
        "martin", "oliver", "charlie", "lucas", "mason", "liam", "aiden", "jackson",
        // Female names (US/UK/EU common)
        "mary", "patricia", "jennifer", "linda", "barbara", "elizabeth", "susan",
        "jessica", "sarah", "karen", "lisa", "nancy", "betty", "margaret", "sandra",
        "ashley", "kimberly", "emily", "donna", "michelle", "dorothy", "carol", "amanda",
        "melissa", "deborah", "stephanie", "rebecca", "sharon", "laura", "cynthia",
        "kathleen", "amy", "angela", "shirley", "anna", "brenda", "pamela", "emma",
        "nicole", "helen", "samantha", "katherine", "christine", "debra", "rachel",
        "carolyn", "janet", "catherine", "maria", "heather", "diane", "ruth", "julie",
        "olivia", "joyce", "virginia", "victoria", "kelly", "lauren", "christina", "joan",
        "evelyn", "judith", "megan", "andrea", "cheryl", "hannah", "jacqueline", "martha",
        "gloria", "teresa", "ann", "sara", "madison", "frances", "kathryn", "janice",
        "jean", "abigail", "alice", "judy", "sophia", "grace", "denise", "amber", "doris",
        "marilyn", "danielle", "beverly", "isabella", "theresa", "diana", "natalie",
        "brittany", "charlotte", "marie", "kayla", "alexis", "lori", "jane", "claire",
        "julia", "lucy", "ella", "chloe", "mia", "ava", "lily", "zoe", "molly", "ruby",
        // Surnames commonly used as first names
        "taylor", "morgan", "jordan", "cameron", "bailey", "parker", "hunter", "carter",
        "riley", "mason", "tyler", "logan", "dylan", "blake", "ryan", "austin", "evan",
        // International common names (EU)
        "hans", "franz", "klaus", "andreas", "stefan", "peter", "martin", "thomas",
        "marie", "anna", "sophie", "laura", "julia", "sarah", "emma", "max", "paul",
        "pierre", "jean", "michel", "marie", "sophie", "camille", "lea", "chloe",
        "marco", "luca", "matteo", "giulia", "francesca", "elena", "maria", "giuseppe",
        "pablo", "carlos", "juan", "jose", "antonio", "maria", "carmen", "lucia",
        // Additional UK common names
        "alfie", "archie", "freddie", "oscar", "george", "harry", "leo", "teddy",
        "poppy", "isla", "jessica", "emily", "daisy", "freya", "florence", "elsie"
    };
    return names;
}

// Helper: convert string to lowercase
std::string ToLowerCase(const std::string &str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

// Helper: extract first word from a string
std::string ExtractFirstWord(const std::string &str) {
    size_t end = str.find(' ');
    if (end == std::string::npos) {
        return str;
    }
    return str.substr(0, end);
}

// Helper: split string into words
std::vector<std::string> SplitIntoWords(const std::string &str) {
    std::vector<std::string> words;
    std::istringstream iss(str);
    std::string word;
    while (iss >> word) {
        words.push_back(word);
    }
    return words;
}

} // anonymous namespace

NameRecognizer::NameRecognizer() {
    try {
        // Match capitalized words (potential names): 1-3 consecutive capitalized words
        // Pattern: Starts with uppercase, followed by lowercase letters (2-20 chars per word)
        pattern_ = std::regex(R"(\b([A-Z][a-z]{1,19})(?:\s+([A-Z][a-z]{1,19})){0,2}\b)",
                              std::regex_constants::ECMAScript);
    } catch (const std::regex_error &e) {
        throw InvalidInputException("Invalid regex pattern for NameRecognizer: %s", e.what());
    }
}

NameRecognizer::~NameRecognizer() = default;

PIIType NameRecognizer::GetType() const {
    return PIIType::NAME;
}

std::string NameRecognizer::GetName() const {
    return "Person Name";
}

std::vector<PIIMatch> NameRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;

#if HAVE_OPENVINO
    // Try NER-based detection first if OpenVINO is available
    auto &ner = NERModelManager::Instance();
    if (ner.GetStatus() == NERStatus::NOT_LOADED) {
        // Lazy initialize the NER model
        ner.EnsureInitialized();
    }

    if (ner.IsAvailable()) {
        try {
            auto entities = ner.ExtractEntities(text);
            for (const auto &entity : entities) {
                // Only include PER (person) entities above the intrinsic
                // NER noise floor. The user-configured threshold
                // (anofox_pii_min_confidence) is applied centrally in
                // PIIEngine::Detect() on top of this floor.
                if (entity.label == "PER" && entity.confidence >= NER_MIN_ENTITY_CONFIDENCE) {
                    matches.emplace_back(
                        PIIType::NAME,
                        entity.text,
                        entity.start_pos,
                        entity.end_pos,
                        entity.confidence
                    );
                }
            }
            // If NER found matches, return them
            if (!matches.empty()) {
                return matches;
            }
        } catch (const std::exception &e) {
            AnofoxTrace(AnofoxLogLevel::Warn,
                "pii: NER extraction failed, falling back to dictionary: " + std::string(e.what()));
        }
    }
#endif

    // Fallback to dictionary-based detection
    return ExtractWithDictionary(text);
}

std::vector<PIIMatch> NameRecognizer::ExtractWithDictionary(const std::string &text) const {
    std::vector<PIIMatch> matches;

    // First, find all individual capitalized words with their positions
    std::regex word_pattern(R"(\b([A-Z][a-z]{1,19})\b)", std::regex_constants::ECMAScript);
    std::vector<std::tuple<std::string, size_t, size_t>> words; // (word, start, end)

    std::sregex_iterator iter(text.begin(), text.end(), word_pattern);
    std::sregex_iterator end;

    while (iter != end) {
        std::smatch match = *iter;
        words.emplace_back(
            match.str(),
            static_cast<size_t>(match.position()),
            static_cast<size_t>(match.position() + match.length())
        );
        ++iter;
    }

    // Now scan through words looking for dictionary matches
    const auto& name_dict = GetCommonNames();
    std::unordered_set<size_t> used_positions; // Track positions we've already included in a match

    for (size_t i = 0; i < words.size(); ++i) {
        const auto& [word, start, word_end] = words[i];

        // Skip if this word is already part of a previous match
        if (used_positions.count(start) > 0) {
            continue;
        }

        std::string lower_word = ToLowerCase(word);

        // Check if this word is a name in our dictionary
        if (name_dict.count(lower_word) > 0) {
            // Found a name! Now extend to include consecutive capitalized words
            std::string full_name = word;
            size_t name_end = word_end;
            used_positions.insert(start);

            // Look ahead for consecutive capitalized words (with single space between)
            for (size_t j = i + 1; j < words.size() && j < i + 3; ++j) { // Max 3 words total
                const auto& [next_word, next_start, next_end] = words[j];

                // Check if next word immediately follows (single space between)
                // The space should be at name_end, and next_start should be name_end + 1
                if (next_start == name_end + 1) {
                    full_name += " " + next_word;
                    name_end = next_end;
                    used_positions.insert(next_start);
                } else {
                    break; // Not consecutive
                }
            }

            PIIMatch pii_match(
                PIIType::NAME,
                full_name,
                start,
                name_end,
                0.5  // Lower confidence for dictionary-based detection
            );
            matches.push_back(pii_match);
        }
    }

    return matches;
}

bool NameRecognizer::Validate(const std::string &text) const {
    // Name validation is probabilistic - dictionary match already done in FindMatches
    return true;
}

std::string NameRecognizer::GetPartialMask(const std::string &text) const {
    // Mask: "Anna Schmidt" -> "**** S******" (short words are masked completely)
    std::vector<std::string> words = SplitIntoWords(text);
    std::string masked;

    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) masked += " ";

        masked += MaskWordKeepingPrefix(words[i], 1);
    }

    return masked;
}

// ============================================================================
// OrganizationRecognizer Implementation (NER-based ORG entities)
// ============================================================================

OrganizationRecognizer::OrganizationRecognizer() : type_(PIIType::ORGANIZATION) {}

OrganizationRecognizer::~OrganizationRecognizer() = default;

PIIType OrganizationRecognizer::GetType() const {
    return type_;
}

std::string OrganizationRecognizer::GetName() const {
    return "Organization Name";
}

std::vector<PIIMatch> OrganizationRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;

#if HAVE_OPENVINO
    auto &ner = NERModelManager::Instance();
    if (ner.GetStatus() == NERStatus::NOT_LOADED) {
        ner.EnsureInitialized();
    }

    if (ner.IsAvailable()) {
        try {
            auto entities = ner.ExtractEntities(text);
            for (const auto &entity : entities) {
                // Only include ORG entities above the intrinsic NER noise
                // floor; the user-configured threshold is applied centrally
                // in PIIEngine::Detect() on top of this floor.
                if (entity.label == "ORG" && entity.confidence >= NER_MIN_ENTITY_CONFIDENCE) {
                    matches.emplace_back(
                        PIIType::ORGANIZATION,
                        entity.text,
                        entity.start_pos,
                        entity.end_pos,
                        entity.confidence
                    );
                    AnofoxTrace(AnofoxLogLevel::Debug,
                        "pii: Detected ORGANIZATION '" + entity.text +
                        "' (confidence=" + std::to_string(entity.confidence) + ")");
                }
            }
        } catch (const std::exception &e) {
            AnofoxTrace(AnofoxLogLevel::Warn,
                "pii: NER extraction failed for organizations: " + std::string(e.what()));
        }
    }
#endif

    return matches;
}

bool OrganizationRecognizer::Validate(const std::string &text) const {
    return true;
}

std::string OrganizationRecognizer::GetPartialMask(const std::string &text) const {
    // Mask: "Microsoft Corp" -> "Mic****** ****"
    std::vector<std::string> words = SplitIntoWords(text);
    std::string masked;

    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) masked += " ";

        masked += MaskWordKeepingPrefix(words[i], 3);
    }

    return masked;
}

// ============================================================================
// LocationRecognizer Implementation (NER-based LOC entities)
// ============================================================================

LocationRecognizer::LocationRecognizer() : type_(PIIType::LOCATION) {}

LocationRecognizer::~LocationRecognizer() = default;

PIIType LocationRecognizer::GetType() const {
    return type_;
}

std::string LocationRecognizer::GetName() const {
    return "Location";
}

std::vector<PIIMatch> LocationRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;

#if HAVE_OPENVINO
    auto &ner = NERModelManager::Instance();
    if (ner.GetStatus() == NERStatus::NOT_LOADED) {
        ner.EnsureInitialized();
    }

    if (ner.IsAvailable()) {
        try {
            auto entities = ner.ExtractEntities(text);
            for (const auto &entity : entities) {
                // Only include LOC entities above the intrinsic NER noise
                // floor; the user-configured threshold is applied centrally
                // in PIIEngine::Detect() on top of this floor.
                if (entity.label == "LOC" && entity.confidence >= NER_MIN_ENTITY_CONFIDENCE) {
                    matches.emplace_back(
                        PIIType::LOCATION,
                        entity.text,
                        entity.start_pos,
                        entity.end_pos,
                        entity.confidence
                    );
                    AnofoxTrace(AnofoxLogLevel::Debug,
                        "pii: Detected LOCATION '" + entity.text +
                        "' (confidence=" + std::to_string(entity.confidence) + ")");
                }
            }
        } catch (const std::exception &e) {
            AnofoxTrace(AnofoxLogLevel::Warn,
                "pii: NER extraction failed for locations: " + std::string(e.what()));
        }
    }
#endif

    return matches;
}

bool LocationRecognizer::Validate(const std::string &text) const {
    return true;
}

std::string LocationRecognizer::GetPartialMask(const std::string &text) const {
    // Mask: "Frankfurt am Main" -> "Fra****** ** ****"; words of up to 4
    // characters are masked completely ("Rome" -> "****", "New York" -> "*** ****")
    std::vector<std::string> words = SplitIntoWords(text);
    std::string masked;

    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) masked += " ";

        if (i == 0) {
            masked += MaskWordKeepingPrefix(words[i], 3);
        } else {
            masked += Utf8Stars(words[i]);
        }
    }

    return masked;
}

// ============================================================================
// MiscRecognizer Implementation (NER-based MISC entities)
// ============================================================================

MiscRecognizer::MiscRecognizer() : type_(PIIType::MISC) {}

MiscRecognizer::~MiscRecognizer() = default;

PIIType MiscRecognizer::GetType() const {
    return type_;
}

std::string MiscRecognizer::GetName() const {
    return "Miscellaneous Entity";
}

std::vector<PIIMatch> MiscRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;

#if HAVE_OPENVINO
    auto &ner = NERModelManager::Instance();
    if (ner.GetStatus() == NERStatus::NOT_LOADED) {
        ner.EnsureInitialized();
    }

    if (ner.IsAvailable()) {
        try {
            auto entities = ner.ExtractEntities(text);
            for (const auto &entity : entities) {
                // Only include MISC entities above the intrinsic NER noise
                // floor; the user-configured threshold is applied centrally
                // in PIIEngine::Detect() on top of this floor.
                if (entity.label == "MISC" && entity.confidence >= NER_MIN_ENTITY_CONFIDENCE) {
                    matches.emplace_back(
                        PIIType::MISC,
                        entity.text,
                        entity.start_pos,
                        entity.end_pos,
                        entity.confidence
                    );
                    AnofoxTrace(AnofoxLogLevel::Debug,
                        "pii: Detected MISC entity '" + entity.text +
                        "' (confidence=" + std::to_string(entity.confidence) + ")");
                }
            }
        } catch (const std::exception &e) {
            AnofoxTrace(AnofoxLogLevel::Warn,
                "pii: NER extraction failed for misc entities: " + std::string(e.what()));
        }
    }
#endif

    return matches;
}

bool MiscRecognizer::Validate(const std::string &text) const {
    return true;
}

std::string MiscRecognizer::GetPartialMask(const std::string &text) const {
    // Mask: "Nobel Prize" -> "No*** Pr***"
    std::vector<std::string> words = SplitIntoWords(text);
    std::string masked;

    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) masked += " ";

        masked += MaskWordKeepingPrefix(words[i], 3);
    }

    return masked;
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

    // German insurance data (F24): context-gated or checksum-validated
    recognizers_.push_back(std::make_unique<DateOfBirthRecognizer>());
    recognizers_.push_back(std::make_unique<DESVNRRecognizer>());
    recognizers_.push_back(std::make_unique<DEIDCardRecognizer>());
    recognizers_.push_back(std::make_unique<PostalCodeRecognizer>());  // opt-in

    // NER-based recognizers (Person, Organization, Location, Misc entities)
    recognizers_.push_back(std::make_unique<NameRecognizer>());          // PER entities
    recognizers_.push_back(std::make_unique<OrganizationRecognizer>());  // ORG entities
    recognizers_.push_back(std::make_unique<LocationRecognizer>());      // LOC entities
    recognizers_.push_back(std::make_unique<MiscRecognizer>());          // MISC entities
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

bool PIIEngine::ValidateType(const std::string &text, PIIType type) const {
    auto recognizer = GetRecognizer(type);
    if (!recognizer) {
        return false;
    }

    // First check if text matches the pattern
    auto matches = recognizer->FindMatches(text);

    // Look for an exact match (the entire text should match)
    for (const auto &match : matches) {
        if (match.matched_text == text) {
            // Then validate checksum/format
            return recognizer->Validate(text);
        }
    }

    // Also try validating directly - some validators work on the raw input
    // This handles cases where the text IS the value (not wrapped in context)
    return recognizer->Validate(text);
}

std::vector<PIIMatch> PIIEngine::Detect(
    const std::string &text,
    const std::vector<PIIType> &types
) const {
    return Detect(text, types, PIIConfig::Get().Snapshot());
}

std::vector<PIIMatch> PIIEngine::Detect(
    const std::string &text,
    const std::vector<PIIType> &types,
    const PIIConfigSnapshot &config
) const {
    // An explicit non-empty types argument overrides the configured filter
    const std::vector<PIIType> &effective_types = !types.empty() ? types : config.enabled_types;

    std::vector<PIIMatch> all_matches;

    for (const auto &recognizer : recognizers_) {
        // Filter by types if specified; without a filter, opt-in types stay off
        const auto type = recognizer->GetType();
        if (effective_types.empty() ? IsOptInPIIType(type)
                                    : std::find(effective_types.begin(), effective_types.end(), type) ==
                                          effective_types.end()) {
            continue;
        }

        auto matches = recognizer->FindMatches(text);
        if (!config.require_checksum) {
            // anofox_pii_require_checksum = false: also report values whose
            // checksum fails (mistyped IBANs, card numbers) at lower confidence
            auto failures = recognizer->FindChecksumFailures(text);
            std::move(failures.begin(), failures.end(), std::back_inserter(matches));
        }
        for (auto &match : matches) {
            // Apply the configured minimum confidence threshold centrally
            if (match.confidence < config.min_confidence) {
                continue;
            }
            if (IsNERType(match.type)) {
                WidenToWords(text, match);
                if (IsFieldLabel(match.matched_text) || IsCountryOrCurrencyCode(match.matched_text)) {
                    continue;
                }
            }
            all_matches.push_back(std::move(match));
        }
    }

    return ResolveOverlaps(std::move(all_matches));
}

std::vector<PIIMatch> PIIEngine::ResolveOverlaps(std::vector<PIIMatch> matches) {
    // Deterministic priority: earlier start wins; on ties prefer the longer
    // match, then the higher confidence, then the lower PIIType enumeration
    // value (more specific, checksum-validated types come first).
    std::sort(matches.begin(), matches.end(),
              [](const PIIMatch &a, const PIIMatch &b) {
                  if (a.start_pos != b.start_pos) {
                      return a.start_pos < b.start_pos;
                  }
                  auto a_len = a.end_pos - a.start_pos;
                  auto b_len = b.end_pos - b.start_pos;
                  if (a_len != b_len) {
                      return a_len > b_len;
                  }
                  if (a.confidence != b.confidence) {
                      return a.confidence > b.confidence;
                  }
                  return static_cast<int>(a.type) < static_cast<int>(b.type);
              });

    // Greedily keep non-overlapping matches so Mask() replaces each region
    // of the input exactly once. Pattern matches (validated formats such as
    // IBAN or card numbers) are placed first; a NER span is kept only where it
    // overlaps none of them, so a NER guess like "IBAN DE" can never leave the
    // rest of an account number unmasked.
    std::vector<PIIMatch> resolved;
    resolved.reserve(matches.size());
    auto overlaps_kept = [&resolved](const PIIMatch &match) {
        return std::any_of(resolved.begin(), resolved.end(), [&match](const PIIMatch &kept) {
            return match.start_pos < kept.end_pos && kept.start_pos < match.end_pos;
        });
    };
    for (bool ner_pass : {false, true}) {
        for (auto &match : matches) {
            if (IsNERType(match.type) != ner_pass || overlaps_kept(match)) {
                continue;
            }
            resolved.push_back(std::move(match));
        }
    }
    std::sort(resolved.begin(), resolved.end(),
              [](const PIIMatch &a, const PIIMatch &b) { return a.start_pos < b.start_pos; });
    return resolved;
}

std::vector<std::vector<PIIMatch>> PIIEngine::DetectBatch(
    const std::vector<std::string> &texts,
    const std::vector<PIIType> &types
) const {
    return DetectBatch(texts, types, PIIConfig::Get().Snapshot());
}

std::vector<std::vector<PIIMatch>> PIIEngine::DetectBatch(
    const std::vector<std::string> &texts,
    const std::vector<PIIType> &types,
    const PIIConfigSnapshot &config
) const {
    std::vector<std::vector<PIIMatch>> all_results;
    all_results.reserve(texts.size());

    if (texts.empty()) {
        return all_results;
    }

    // Pre-warm NER cache by running batch extraction
    // This ensures subsequent Detect calls hit the cache instead of re-running inference.
    // Skipped when no NER type is requested, so regex-only scans never pay for inference
    const std::vector<PIIType> &effective_types = !types.empty() ? types : config.enabled_types;
    const bool wants_ner = effective_types.empty() ||
                           std::any_of(effective_types.begin(), effective_types.end(), IsNERType);
    auto &ner = NERModelManager::Instance();
    if (wants_ner && ner.IsAvailable()) {
        AnofoxTrace(AnofoxLogLevel::Debug,
                    "pii: Pre-warming NER cache for " + std::to_string(texts.size()) + " texts");
        ner.ExtractEntitiesBatch(texts);
    }

    // Now run individual detection for each text (NER calls will hit cache)
    for (const auto &text : texts) {
        all_results.push_back(Detect(text, types, config));
    }

    return all_results;
}

std::string PIIEngine::ApplyMask(const PIIMatch &match, MaskStrategy strategy) const {
    switch (strategy) {
        case MaskStrategy::REDACT:
            return "[" + PIITypeToString(match.type) + "]";

        case MaskStrategy::HASH: {
            // Pseudonym: the first 16 hex characters (64 bits) of the unsalted
            // SHA-256 of the normalised value, so equal values join across
            // formats. Not anonymisation: small value spaces (phone numbers,
            // card numbers) can be brute-forced back to the value.
            const std::string normalized = NormalizeForHash(match.type, match.matched_text);
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256(reinterpret_cast<const unsigned char*>(normalized.data()), normalized.size(), hash);
            std::ostringstream oss;
            for (int i = 0; i < 8; ++i) {
                oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
            }
            return oss.str();
        }

        case MaskStrategy::PARTIAL: {
            // Type-specific partial mask, checked against the half rule: a
            // partial mask that reveals more than half of the letters and
            // digits of the value (or is no valid UTF-8, possible for
            // arbitrary pii_mask_value input) falls back to one '*' per
            // character
            const auto *recognizer = GetRecognizer(match.type);
            if (!recognizer) {
                return Utf8Stars(match.matched_text);
            }
            std::string masked = recognizer->GetPartialMask(match.matched_text);
            if (!IsValidUtf8(masked) ||
                CountLettersAndDigits(masked) > CountLettersAndDigits(match.matched_text) / 2) {
                AnofoxTrace(AnofoxLogLevel::Debug, "pii: partial mask of a " + PIITypeToString(match.type) +
                                                       " value reveals too much, masking it completely");
                return Utf8Stars(match.matched_text);
            }
            return masked;
        }

        case MaskStrategy::ASTERISK:
            // One '*' per character, not per byte
            return Utf8Stars(match.matched_text);

        default:
            return match.matched_text;
    }
}

std::string PIIEngine::MaskValue(const std::string &value, PIIType type, MaskStrategy strategy) const {
    // Nothing to mask in an empty value; a redact label or hash would claim one
    if (value.empty()) {
        return value;
    }
    return ApplyMask(PIIMatch(type, value, 0, value.size(), 1.0), strategy);
}

std::string PIIEngine::Mask(
    const std::string &text,
    MaskStrategy strategy,
    const std::vector<PIIType> &types
) const {
    return Mask(text, strategy, types, PIIConfig::Get().Snapshot());
}

std::string PIIEngine::Mask(
    const std::string &text,
    MaskStrategy strategy,
    const std::vector<PIIType> &types,
    const PIIConfigSnapshot &config
) const {
    // Detect() returns non-overlapping matches sorted by position, so each
    // region of the input is replaced exactly once.
    auto matches = Detect(text, types, config);

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

// Shared implementation for all detect-style scalars: runs Detect() with the
// given type filter and writes the matches directly into the LIST(STRUCT)
// result vector. The input is normalized once per chunk and match rows are
// written into the list child vectors without per-row Value materialization.
void ExecutePIIDetectToList(DataChunk &args, Vector &result, const std::vector<PIIType> &filter) {
    auto &engine = PIIEngine::Instance();
    // Snapshot the configuration once per chunk; all reads go through it
    const auto config = PIIConfig::Get().Snapshot();
    result.SetVectorType(VectorType::FLAT_VECTOR);

    // Get list entries data and child struct vector
    auto list_entries = duckdb_compat::FlatData<list_entry_t>(result);
    auto &child_struct = ListVector::GetEntry(result);
    auto &struct_children = StructVector::GetEntries(child_struct);

    // References to individual struct fields
    auto &type_vec = anofox::VectorRef(struct_children[0]);       // VARCHAR: type
    auto &text_vec = anofox::VectorRef(struct_children[1]);       // VARCHAR: text
    auto &start_pos_vec = anofox::VectorRef(struct_children[2]);  // BIGINT: start_pos
    auto &end_pos_vec = anofox::VectorRef(struct_children[3]);    // BIGINT: end_pos
    auto &confidence_vec = anofox::VectorRef(struct_children[4]); // DOUBLE: confidence

    // Reset list size
    ListVector::SetListSize(result, 0);

    UnifiedVectorFormat input_data;
    args.data[0].ToUnifiedFormat(args.size(), input_data);
    auto input_values = UnifiedVectorFormat::GetData<string_t>(input_data);

    for (idx_t row = 0; row < args.size(); row++) {
        auto input_idx = input_data.sel->get_index(row);
        if (!input_data.validity.RowIsValid(input_idx)) {
            FlatVector::SetNull(result, row, true);
            continue;
        }
        FlatVector::SetNull(result, row, false);

        std::string text = input_values[input_idx].GetString();
        auto matches = engine.Detect(text, filter, config);

        // Set list entry for this row and reserve space in the child vector
        auto offset = ListVector::GetListSize(result);
        list_entries[row].offset = offset;
        list_entries[row].length = matches.size();
        ListVector::Reserve(result, offset + matches.size());

        // Re-fetch the child data pointers after Reserve: growing the list may
        // reallocate the child vectors.
        auto type_data = duckdb_compat::FlatData<string_t>(type_vec);
        auto text_data = duckdb_compat::FlatData<string_t>(text_vec);
        auto start_data = duckdb_compat::FlatData<int64_t>(start_pos_vec);
        auto end_data = duckdb_compat::FlatData<int64_t>(end_pos_vec);
        auto confidence_data = duckdb_compat::FlatData<double>(confidence_vec);

        for (idx_t i = 0; i < matches.size(); i++) {
            auto &match = matches[i];
            auto child_idx = offset + i;
            type_data[child_idx] = StringVector::AddString(type_vec, PIITypeToString(match.type));
            text_data[child_idx] = StringVector::AddString(text_vec, match.matched_text);
            start_data[child_idx] = static_cast<int64_t>(match.start_pos);
            end_data[child_idx] = static_cast<int64_t>(match.end_pos);
            confidence_data[child_idx] = match.confidence;
        }
        ListVector::SetListSize(result, offset + matches.size());
    }

    // Fix for constant folding: Convert to CONSTANT_VECTOR when all inputs are constant
    // This prevents assertion failures in DuckDB's expression evaluator which expects
    // constant input arguments to produce constant result vectors
    if (args.AllConstant()) {
        result.SetVectorType(VectorType::CONSTANT_VECTOR);
    }
}

// anofox_tab_pii_detect(text) -> LIST(STRUCT(...)) of matches (alias: pii_detect)
// Returns idiomatic DuckDB types that can be queried with unnest, dot notation, etc.
struct PIIDetectFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        ExecutePIIDetectToList(args, result, {});
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect", {LogicalType::VARCHAR},
                              GetPIIMatchListType(), Execute);
    }
};

// anofox_tab_pii_mask(text, strategy) -> masked text (alias: pii_mask)
struct PIIMaskFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();
        const auto config = PIIConfig::Get().Snapshot();

        BinaryExecutor::Execute<string_t, string_t, string_t>(
            args.data[0], args.data[1], result, args.size(),
            [&](string_t input, string_t strategy_str) {
                std::string text = input.GetString();
                MaskStrategy strategy = ParseMaskStrategy(strategy_str.GetString());

                std::string masked = engine.Mask(text, strategy, {}, config);
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
        const auto config = PIIConfig::Get().Snapshot();

        UnaryExecutor::Execute<string_t, string_t>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                std::string masked = engine.Mask(text, MaskStrategy::REDACT, {}, config);
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
        const auto config = PIIConfig::Get().Snapshot();

        UnaryExecutor::Execute<string_t, bool>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                auto matches = engine.Detect(text, {}, config);
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
        const auto config = PIIConfig::Get().Snapshot();

        UnaryExecutor::Execute<string_t, int64_t>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                auto matches = engine.Detect(text, {}, config);
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
// Individual Validation Functions
// ============================================================================

// Template for type-specific validators
template<PIIType Type>
struct PIIIsValidFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();

        UnaryExecutor::Execute<string_t, bool>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                std::string text = input.GetString();
                return engine.ValidateType(text, Type);
            }
        );
    }
};

// pii_is_valid_ssn(text) -> boolean
struct PIIIsValidSSNFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        PIIIsValidFunction<PIIType::US_SSN>::Execute(args, state, result);
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_is_valid_ssn", {LogicalType::VARCHAR},
                              LogicalType::BOOLEAN, Execute);
    }
};

// pii_is_valid_iban(text) -> boolean
struct PIIIsValidIBANFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        PIIIsValidFunction<PIIType::IBAN>::Execute(args, state, result);
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_is_valid_iban", {LogicalType::VARCHAR},
                              LogicalType::BOOLEAN, Execute);
    }
};

// pii_is_valid_credit_card(text) -> boolean
struct PIIIsValidCreditCardFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        PIIIsValidFunction<PIIType::CREDIT_CARD>::Execute(args, state, result);
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_is_valid_credit_card", {LogicalType::VARCHAR},
                              LogicalType::BOOLEAN, Execute);
    }
};

// pii_is_valid_nino(text) -> boolean (UK National Insurance Number)
struct PIIIsValidNINOFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        PIIIsValidFunction<PIIType::UK_NINO>::Execute(args, state, result);
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_is_valid_nino", {LogicalType::VARCHAR},
                              LogicalType::BOOLEAN, Execute);
    }
};

// pii_is_valid_de_tax_id(text) -> boolean (German Tax ID)
struct PIIIsValidDETaxIDFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        PIIIsValidFunction<PIIType::DE_TAX_ID>::Execute(args, state, result);
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_is_valid_de_tax_id", {LogicalType::VARCHAR},
                              LogicalType::BOOLEAN, Execute);
    }
};

// pii_is_valid_crypto_address(text) -> boolean (Bitcoin/Ethereum)
struct PIIIsValidCryptoFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        PIIIsValidFunction<PIIType::CRYPTO_ADDRESS>::Execute(args, state, result);
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_is_valid_crypto_address", {LogicalType::VARCHAR},
                              LogicalType::BOOLEAN, Execute);
    }
};

// ============================================================================
// Type-Specific Detection Functions
// ============================================================================

// Helper to create match values as LIST(STRUCT(...))
Value CreatePIIMatchListValue(const std::vector<PIIMatch> &matches) {
    std::vector<Value> match_values;
    for (const auto &match : matches) {
        child_list_t<Value> struct_values;
        struct_values.emplace_back("type", Value(PIITypeToString(match.type)));
        struct_values.emplace_back("text", Value(match.matched_text));
        struct_values.emplace_back("start_pos", Value::BIGINT(static_cast<int64_t>(match.start_pos)));
        struct_values.emplace_back("end_pos", Value::BIGINT(static_cast<int64_t>(match.end_pos)));
        struct_values.emplace_back("confidence", Value::DOUBLE(match.confidence));
        match_values.push_back(Value::STRUCT(std::move(struct_values)));
    }
    return Value::LIST(GetPIIMatchStructType(), std::move(match_values));
}

// pii_detect_emails(text) -> LIST(STRUCT(...))
struct PIIDetectEmailsFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        ExecutePIIDetectToList(args, result, {PIIType::EMAIL});
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect_emails", {LogicalType::VARCHAR},
                              GetPIIMatchListType(), Execute);
    }
};

// pii_detect_phones(text) -> LIST(STRUCT(...))
struct PIIDetectPhonesFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        ExecutePIIDetectToList(args, result, {PIIType::PHONE});
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect_phones", {LogicalType::VARCHAR},
                              GetPIIMatchListType(), Execute);
    }
};

// pii_detect_credit_cards(text) -> LIST(STRUCT(...))
struct PIIDetectCreditCardsFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        ExecutePIIDetectToList(args, result, {PIIType::CREDIT_CARD});
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect_credit_cards", {LogicalType::VARCHAR},
                              GetPIIMatchListType(), Execute);
    }
};

// pii_detect_ssns(text) -> LIST(STRUCT(...))
struct PIIDetectSSNsFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        ExecutePIIDetectToList(args, result, {PIIType::US_SSN});
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect_ssns", {LogicalType::VARCHAR},
                              GetPIIMatchListType(), Execute);
    }
};

// pii_detect_names(text) -> LIST(STRUCT(...))
struct PIIDetectNamesFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        ExecutePIIDetectToList(args, result, {PIIType::NAME});
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect_names", {LogicalType::VARCHAR},
                              GetPIIMatchListType(), Execute);
    }
};

// pii_detect_ibans(text) -> LIST(STRUCT(...))
struct PIIDetectIBANsFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        ExecutePIIDetectToList(args, result, {PIIType::IBAN});
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_detect_ibans", {LogicalType::VARCHAR},
                              GetPIIMatchListType(), Execute);
    }
};

// ============================================================================
// Batch Detection Function
// ============================================================================

// pii_detect_batch(texts VARCHAR[]) -> LIST(LIST(STRUCT(...)))
// Returns an array of results, one for each input text
struct PIIDetectBatchFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();
        const auto config = PIIConfig::Get().Snapshot();

        // Process each row (which contains an array)
        for (idx_t i = 0; i < args.size(); i++) {
            auto val = args.data[0].GetValue(i);

            if (val.IsNull()) {
                result.SetValue(i, Value(LogicalType::SQLNULL));
                continue;
            }

            // val is a LIST of VARCHARs
            auto &list_val = ListValue::GetChildren(val);

            // Collect texts for batch processing
            std::vector<std::string> texts;
            texts.reserve(list_val.size());
            for (auto &item : list_val) {
                if (item.IsNull()) {
                    texts.push_back("");
                } else {
                    texts.push_back(item.GetValue<std::string>());
                }
            }

            // Run batch detection (pre-warms NER cache)
            auto batch_results = engine.DetectBatch(texts, {}, config);

            // Convert to LIST(LIST(STRUCT(...)))
            std::vector<Value> outer_list;
            outer_list.reserve(batch_results.size());

            for (size_t j = 0; j < batch_results.size(); j++) {
                if (list_val[j].IsNull()) {
                    // Preserve NULL for NULL inputs
                    outer_list.push_back(Value(LogicalType::SQLNULL));
                } else {
                    outer_list.push_back(CreatePIIMatchListValue(batch_results[j]));
                }
            }

            result.SetValue(i, Value::LIST(GetPIIMatchListType(), std::move(outer_list)));
        }
    }

    static ScalarFunction GetFunction() {
        // Input: LIST(VARCHAR), Output: LIST(LIST(STRUCT(...)))
        auto inner_list = GetPIIMatchListType();
        auto outer_list = LogicalType::LIST(inner_list);

        return ScalarFunction("anofox_tab_pii_detect_batch",
                              {LogicalType::LIST(LogicalType::VARCHAR)},
                              outer_list, Execute);
    }
};

// ============================================================================
// Advanced Masking Functions
// ============================================================================

// pii_mask_column(value, pii_type, strategy) -> VARCHAR
// Masks a specific PII type in the value using the given strategy
// Useful for UPDATE statements where you know the column contains a specific PII type
struct PIIMaskColumnFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();
        const auto config = PIIConfig::Get().Snapshot();

        // The type and strategy are read per row; a NULL argument yields NULL for that row.
        // Mask() returns the original text when nothing of the requested type is
        // found, so a single call per row suffices (no separate Detect pass).
        TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
            args.data[0], args.data[1], args.data[2], result, args.size(),
            [&](string_t input, string_t type_str, string_t strategy_str) {
                auto pii_type = StringToPIIType(type_str.GetString());
                if (pii_type == PIIType::UNKNOWN) {
                    throw InvalidInputException("Unknown PII type: %s", type_str.GetString());
                }
                auto strategy = ParseMaskStrategy(strategy_str.GetString());
                return StringVector::AddString(result, engine.Mask(input.GetString(), strategy, {pii_type}, config));
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_mask_column",
                              {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                              LogicalType::VARCHAR, Execute);
    }
};

// pii_mask_value(value, pii_type, strategy) -> VARCHAR
// Masks the whole non-NULL value as the given PII type, without detection: for
// columns known to hold PII where detection finds nothing ('Li', '01511234567').
// Type and strategy are read per row; a NULL argument yields NULL.
struct PIIMaskValueFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();

        TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
            args.data[0], args.data[1], args.data[2], result, args.size(),
            [&](string_t input, string_t type_str, string_t strategy_str) {
                auto pii_type = StringToPIIType(type_str.GetString());
                if (pii_type == PIIType::UNKNOWN) {
                    throw InvalidInputException("Unknown PII type: %s", type_str.GetString());
                }
                auto strategy = ParseMaskStrategy(strategy_str.GetString());
                return StringVector::AddString(result, engine.MaskValue(input.GetString(), pii_type, strategy));
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_mask_value",
                              {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                              LogicalType::VARCHAR, Execute);
    }
};

// pii_redact_column(value, strategy) -> VARCHAR
// Shorthand for masking without specifying type - masks all PII types
struct PIIRedactColumnFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();
        const auto config = PIIConfig::Get().Snapshot();

        // The strategy is read per row; a NULL argument yields NULL for that row.
        BinaryExecutor::Execute<string_t, string_t, string_t>(
            args.data[0], args.data[1], result, args.size(),
            [&](string_t input, string_t strategy_str) {
                auto strategy = ParseMaskStrategy(strategy_str.GetString());
                return StringVector::AddString(result, engine.Mask(input.GetString(), strategy, {}, config));
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_redact_column",
                              {LogicalType::VARCHAR, LogicalType::VARCHAR},
                              LogicalType::VARCHAR, Execute);
    }
};

// pii_redact_column(value) -> VARCHAR (overload with default strategy)
struct PIIRedactColumnDefaultFunction {
    static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
        auto &engine = PIIEngine::Instance();
        const auto config = PIIConfig::Get().Snapshot();
        auto strategy = config.default_mask_strategy;

        UnaryExecutor::Execute<string_t, string_t>(
            args.data[0], result, args.size(),
            [&](string_t input) {
                return StringVector::AddString(result, engine.Mask(input.GetString(), strategy, {}, config));
            }
        );
    }

    static ScalarFunction GetFunction() {
        return ScalarFunction("anofox_tab_pii_redact_column",
                              {LogicalType::VARCHAR},
                              LogicalType::VARCHAR, Execute);
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
                                        vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_status");

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
        duckdb_compat::SetChildCardinality(output, 0);
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
        {PIIType::NAME, "Person Name"},
        {PIIType::ORGANIZATION, "Organization Name"},
        {PIIType::LOCATION, "Location"},
        {PIIType::MISC, "Miscellaneous Entity"},
        {PIIType::US_PASSPORT, "US Passport"},
        {PIIType::CRYPTO_ADDRESS, "Crypto Address"},
        {PIIType::UK_NINO, "UK NINO"},
        {PIIType::MAC_ADDRESS, "MAC Address"},
        {PIIType::API_KEY, "API Key"},
        {PIIType::DATE_OF_BIRTH, "Date of Birth"},
        {PIIType::DE_SVNR, "German Social Insurance Number"},
        {PIIType::DE_ID_CARD, "German ID Card / Passport"},
        {PIIType::POSTAL_CODE, "Postal Code"},
        {PIIType::UNKNOWN, "Unknown"}
    };

    static const std::unordered_map<PIIType, std::string> pattern_info = {
        {PIIType::EMAIL, "RFC 5322 email pattern"},
        {PIIType::PHONE, "International (+XX, 00XX) and national (0XX) formats"},
        {PIIType::CREDIT_CARD, "Card brand prefixes (IIN) with Luhn validation"},
        {PIIType::US_SSN, "XXX-XX-XXXX format with area validation"},
        {PIIType::IP_ADDRESS, "IPv4 and IPv6 formats"},
        {PIIType::IBAN, "ISO 13616 with MOD-97 checksum"},
        {PIIType::DE_TAX_ID, "11-digit Steueridentifikationsnummer"},
        {PIIType::URL, "HTTP/HTTPS URLs"},
#if HAVE_OPENVINO
        {PIIType::NAME, "OpenVINO DistilBERT NER (92% F1)"},
        {PIIType::ORGANIZATION, "OpenVINO DistilBERT NER, ORG entities (companies, institutions)"},
        {PIIType::LOCATION, "OpenVINO DistilBERT NER, LOC entities (cities, countries, regions)"},
        {PIIType::MISC, "OpenVINO DistilBERT NER, MISC entities (events, nationalities, products)"},
#else
        {PIIType::NAME, "Dictionary-based name detection (200+ first names)"},
        {PIIType::ORGANIZATION, "Needs the NER model, which this build does not include"},
        {PIIType::LOCATION, "Needs the NER model, which this build does not include"},
        {PIIType::MISC, "Needs the NER model, which this build does not include"},
#endif
        {PIIType::US_PASSPORT, "Letter + 8 digits"},
        {PIIType::CRYPTO_ADDRESS, "Bitcoin (P2PKH/P2SH/SegWit) and Ethereum"},
        {PIIType::UK_NINO, "UK National Insurance Number format"},
        {PIIType::MAC_ADDRESS, "Colon/hyphen/dot formats; bare hex needs letter+digit"},
        {PIIType::API_KEY, "AWS/GitHub tokens and high-entropy strings"},
        {PIIType::DATE_OF_BIRTH, "Date after a birth keyword (geb., Geburtsdatum, DOB, born on), real calendar date"},
        {PIIType::DE_SVNR, "Rentenversicherungsnummer with check digit"},
        {PIIType::DE_ID_CARD, "9-character document number; check digit or keyword (Personalausweis, Reisepass) required"},
        {PIIType::POSTAL_CODE, "DE/AT/CH, NL and UK postcodes next to an address (off by default)"},
        {PIIType::UNKNOWN, "Unknown pattern"}
    };

    const auto config = PIIConfig::Get().Snapshot();

    // Calculate how many rows to output in this batch
    idx_t remaining = state.types.size() - state.current_index;
    idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);

    duckdb_compat::SetChildCardinality(output, count);

    for (idx_t i = 0; i < count; i++) {
        PIIType type = state.types[state.current_index + i];

        // Column 0: pii_type
        output.SetValue(0, i, Value(PIITypeToString(type)));

        // Column 1: recognizer_name
        auto name_it = recognizer_names.find(type);
        std::string name = (name_it != recognizer_names.end()) ? name_it->second : "Unknown";
        output.SetValue(1, i, Value(name));

        // Column 2: enabled under anofox_pii_enabled_types (opt-in types are off by default)
        output.SetValue(2, i, Value::BOOLEAN(config.IsTypeEnabled(type)));

        // Column 3: pattern_info
        auto info_it = pattern_info.find(type);
        std::string info = (info_it != pattern_info.end()) ? info_it->second : "No description";
        output.SetValue(3, i, Value(info));
    }

    state.current_index += count;
}

TableFunction CreatePIIStatusFunction() {
    return TableFunction("anofox_tab_pii_status", {}, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIStatusFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIStatusBind), PIIStatusInit);
}

// ============================================================================
// anofox_ner_status() Table Function
// ============================================================================

struct NERStatusState : public GlobalTableFunctionState {
    bool returned = false;
};

unique_ptr<GlobalTableFunctionState> NERStatusInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<NERStatusState>();
}

unique_ptr<FunctionData> NERStatusBind(ClientContext &, TableFunctionBindInput &,
                                       vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("anofox_ner_status");

    names.emplace_back("onnx_available");
    return_types.emplace_back(LogicalTypeId::BOOLEAN);

    names.emplace_back("model_status");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("model_path");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("model_size_mb");
    return_types.emplace_back(LogicalTypeId::DOUBLE);

    names.emplace_back("status_message");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    return nullptr;
}

void NERStatusFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<NERStatusState>();

    if (state.returned) {
        duckdb_compat::SetChildCardinality(output, 0);
        return;
    }

    duckdb_compat::SetChildCardinality(output, 1);

#if HAVE_OPENVINO
    auto &ner = NERModelManager::Instance();
    // Single consistent snapshot: status, message and paths are read under
    // one lock so a concurrently loading model cannot produce torn strings
    auto snapshot = ner.GetStatusSnapshot();

    // onnx_available (keep column name for backward compatibility)
    output.data[0].SetValue(0, Value::BOOLEAN(true));

    // model_status
    std::string status_str;
    switch (snapshot.status) {
        case NERStatus::NOT_LOADED: status_str = "NOT_LOADED"; break;
        case NERStatus::DOWNLOADING: status_str = "DOWNLOADING"; break;
        case NERStatus::LOADED: status_str = "LOADED"; break;
        case NERStatus::FAILED: status_str = "FAILED"; break;
        case NERStatus::NOT_AVAILABLE: status_str = "NOT_AVAILABLE"; break;
        case NERStatus::NOT_DOWNLOADED: status_str = "NOT_DOWNLOADED"; break;
        default: status_str = "UNKNOWN"; break;
    }
    output.data[1].SetValue(0, Value(status_str));

    // model_path
    output.data[2].SetValue(0, Value(snapshot.model_path));

    // model_size_mb
    output.data[3].SetValue(0, Value::DOUBLE(ner.GetModelSizeMB()));

    // status_message
    output.data[4].SetValue(0, Value(snapshot.message));
#else
    // OpenVINO not compiled in
    output.data[0].SetValue(0, Value::BOOLEAN(false));
    output.data[1].SetValue(0, Value("NOT_AVAILABLE"));
    output.data[2].SetValue(0, Value("N/A"));
    output.data[3].SetValue(0, Value::DOUBLE(0.0));
    output.data[4].SetValue(0, Value("OpenVINO not compiled in - using dictionary fallback for NAME detection"));
#endif

    state.returned = true;
}

TableFunction CreateNERStatusFunction() {
    return TableFunction("anofox_ner_status", {}, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, NERStatusFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, NERStatusBind), NERStatusInit);
}

// ============================================================================
// pii_audit_table() Table Function - Row-level PII audit
// ============================================================================

struct PIIAuditTableBindData : public TableFunctionData {
    std::string table_name;
    std::vector<std::string> column_filter;  // Empty = all VARCHAR columns
    IdColumn id_column;                      // Optional pass-through identifier (#108)

    PIIAuditTableBindData(const std::string &table, const std::vector<std::string> &cols)
        : table_name(table), column_filter(cols) {}
};

// Resolve the columns to scan of a table or view. Without a filter: every VARCHAR column
// (JSON included). With a filter: exactly the named columns, matched case-insensitively and of
// any type (the scan queries cast them to VARCHAR); an unknown name is an error.
// Shared by pii_audit_table and pii_scan_table. The relation is bound with a sibling
// connection (LIMIT 0 reads no rows), the same way the scan itself reads it; a catalog
// lookup would only find tables, and casting a view entry to a table entry crashed.
// One column of a table to scan: its name and type, and whether it is scanned
struct PIIScanColumn {
    std::string name;
    std::string type;
    bool scan;
};

// The columns pii_scan_table reports, in table order. Without a filter: every column, VARCHAR
// columns scanned and the others marked as skipped. With a filter: exactly the named columns,
// all scanned.
static std::vector<PIIScanColumn> ResolvePIIScanPlan(ClientContext &context, const std::string &function_name,
                                                     const std::string &table_name,
                                                     const std::vector<std::string> &column_filter) {
    Connection con(*context.db);
    auto result = con.Query("SELECT * FROM " + BuildQueryTableRef(table_name) + " LIMIT 0");
    if (result->HasError()) {
        throw InvalidInputException("%s: cannot read table '%s': %s", function_name, table_name, result->GetError());
    }
    auto result_names = duckdb_compat::ResultNames(*result);
    auto &result_types = duckdb_compat::ResultTypes(*result);
    vector<std::string> available_names(result_names.begin(), result_names.end());

    std::vector<bool> selected(available_names.size(), false);
    if (column_filter.empty()) {
        for (idx_t i = 0; i < available_names.size(); i++) {
            selected[i] = result_types[i].id() == LogicalTypeId::VARCHAR;
        }
    } else {
        for (auto idx : MatchColumnNames(function_name, table_name, available_names,
                                             vector<std::string>(column_filter.begin(), column_filter.end()))) {
            selected[idx] = true;
        }
    }

    std::vector<PIIScanColumn> plan;
    for (idx_t i = 0; i < available_names.size(); i++) {
        if (selected[i] || column_filter.empty()) {
            plan.push_back({available_names[i], result_types[i].ToString(), selected[i]});
        }
    }
    return plan;
}

// The names of the columns to scan (see ResolvePIIScanPlan)
static std::vector<std::string> ResolvePIIScanColumns(ClientContext &context, const std::string &function_name,
                                                      const std::string &table_name,
                                                      const std::vector<std::string> &column_filter) {
    std::vector<std::string> columns;
    for (auto &column : ResolvePIIScanPlan(context, function_name, table_name, column_filter)) {
        if (column.scan) {
            columns.push_back(std::move(column.name));
        }
    }
    return columns;
}

struct PIIAuditTableResult {
    int64_t row_id;
    std::string column_name;
    PIIType pii_type;
    std::string original_value;
    std::string masked_value;
    std::string matched_text;
    int64_t start_pos;
    int64_t end_pos;
    double confidence;
    Value id_value;  // id_column value of the row (unused without id_column)

    PIIAuditTableResult() : row_id(0), pii_type(PIIType::UNKNOWN),
                            start_pos(0), end_pos(0), confidence(0.0) {}
};

// Streaming audit state: the source table is scanned column by column and
// chunk by chunk; only the matches of the most recent input chunk are
// buffered, so neither the input nor the full match set is materialized.
struct PIIAuditTableState : public GlobalTableFunctionState {
    bool initialized = false;
    // Columns to audit (resolved from the catalog on first call)
    std::vector<std::string> columns_to_scan;
    idx_t column_idx = 0;
    // Active streaming scan over the current column; the connection must
    // outlive its streaming query result
    unique_ptr<Connection> connection;
    unique_ptr<QueryResult> column_result;
    int64_t source_row = 0;  // 1-based row position within the current column scan
    // Configuration snapshot taken once for the whole audit
    PIIConfigSnapshot config;
    MaskStrategy mask_strategy = MaskStrategy::REDACT;
    // Matches detected but not yet emitted (bounded: refilled only when drained)
    std::vector<PIIAuditTableResult> pending;
    idx_t pending_offset = 0;
};

unique_ptr<GlobalTableFunctionState> PIIAuditTableInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<PIIAuditTableState>();
}

unique_ptr<FunctionData> PIIAuditTableBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_audit_table");

    // Get table name (required)
    if (input.inputs.empty()) {
        throw BinderException("pii_audit_table requires a table name");
    }
    auto table_name = RequireNonNullArgument(input, 0, "pii_audit_table", "table_name");

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
    // Resolve the filter now so an unknown column is a bind error; the scan uses the
    // table's own spelling of each name
    if (!column_filter.empty()) {
        column_filter = ResolvePIIScanColumns(context, "pii_audit_table", table_name, column_filter);
    }

    // Define output columns matching PIIAuditResult struct
    names.emplace_back("row_id");
    return_types.emplace_back(LogicalTypeId::BIGINT);

    names.emplace_back("column_name");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("pii_type");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("original_value");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("masked_value");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    names.emplace_back("start_pos");
    return_types.emplace_back(LogicalTypeId::BIGINT);

    names.emplace_back("end_pos");
    return_types.emplace_back(LogicalTypeId::BIGINT);

    names.emplace_back("confidence");
    return_types.emplace_back(LogicalTypeId::DOUBLE);

    names.emplace_back("matched_text");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    // Every output row is row-level, so id_column is valid in any call
    auto id_column = BindIdColumn(context, input, "pii_audit_table", table_name, true, "");
    InsertIdColumn(id_column, "pii_audit_table", return_types, names);

    auto bind_data = make_uniq<PIIAuditTableBindData>(table_name, column_filter);
    bind_data->id_column = std::move(id_column);
    return std::move(bind_data);
}

void PIIAuditTableFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<PIIAuditTableState>();
    auto &bind_data = input.bind_data->Cast<PIIAuditTableBindData>();
    auto &engine = PIIEngine::Instance();

    try {
        if (!state.initialized) {
            state.initialized = true;
            // Resolve the VARCHAR columns to audit and snapshot the
            // configuration once for the whole scan; enabled_types and
            // min_confidence are applied centrally by PIIEngine::Detect
            state.columns_to_scan = ResolvePIIScanColumns(context, "pii_audit_table", bind_data.table_name, bind_data.column_filter);
            state.config = PIIConfig::Get().Snapshot();
            state.mask_strategy = state.config.default_mask_strategy;
        }

        // Refill the pending buffer by streaming input chunks until at least
        // one match is found or all columns are exhausted. The buffer only
        // ever holds the matches of a single input chunk.
        while (state.pending_offset >= state.pending.size()) {
            state.pending.clear();
            state.pending_offset = 0;

            if (!state.column_result) {
                if (state.column_idx >= state.columns_to_scan.size()) {
                    break;  // All columns audited
                }
                const auto &col_name = state.columns_to_scan[state.column_idx];
                // The id column (if any) is read in the same query as the
                // audited column, so each id stays with its own row
                std::string query = "SELECT CAST(" + QuoteSqlIdentifier(col_name) + " AS VARCHAR)" +
                                    bind_data.id_column.SelectSuffix() +
                                    " FROM " + BuildQueryTableRef(bind_data.table_name);

                // Execute as a streaming query on a dedicated connection
                // (the connection must outlive the streaming result)
                state.connection = make_uniq<Connection>(*context.db);
                auto result = StreamQuery(*state.connection, query);
                if (result->HasError()) {
                    AnofoxTrace(AnofoxLogLevel::Warn, "pii_audit_table: Error querying column " + col_name);
                    state.connection.reset();
                    state.column_idx++;
                    continue;
                }
                state.column_result = std::move(result);
                state.source_row = 0;
            }

            auto chunk = state.column_result->Fetch();
            if (!chunk || chunk->size() == 0) {
                if (state.column_result->HasError()) {
                    AnofoxTrace(AnofoxLogLevel::Warn, "pii_audit_table: Error querying column " +
                                                          state.columns_to_scan[state.column_idx]);
                }
                state.column_result.reset();
                state.connection.reset();
                state.column_idx++;
                continue;
            }

            const auto &col_name = state.columns_to_scan[state.column_idx];
            UnifiedVectorFormat fmt;
            chunk->data[0].ToUnifiedFormat(chunk->size(), fmt);
            auto values = UnifiedVectorFormat::GetData<string_t>(fmt);

            for (idx_t row = 0; row < chunk->size(); row++) {
                state.source_row++;  // NULL rows keep their position in the numbering
                auto idx = fmt.sel->get_index(row);
                if (!fmt.validity.RowIsValid(idx)) {
                    continue;
                }
                std::string text = values[idx].GetString();

                // Detect PII in this value (the snapshot's enabled_types and
                // min_confidence are applied)
                auto matches = engine.Detect(text, {}, state.config);
                if (matches.empty()) {
                    continue;
                }
                // Mask covers the whole value, so it is identical for every
                // match of this row; compute it once
                std::string masked = engine.Mask(text, state.mask_strategy, {}, state.config);
                Value id_value = bind_data.id_column.Enabled() ? chunk->data[1].GetValue(row) : Value();

                for (const auto &match : matches) {
                    PIIAuditTableResult res;
                    res.row_id = state.source_row;
                    res.column_name = col_name;
                    res.pii_type = match.type;
                    res.original_value = text;
                    res.masked_value = masked;
                    res.matched_text = match.matched_text;
                    res.start_pos = static_cast<int64_t>(match.start_pos);
                    res.end_pos = static_cast<int64_t>(match.end_pos);
                    res.confidence = match.confidence;
                    res.id_value = id_value;

                    state.pending.push_back(std::move(res));
                }
            }
        }
    } catch (const std::exception &e) {
        throw InvalidInputException("pii_audit_table: Failed to audit table - %s", e.what());
    }

    // Emit up to STANDARD_VECTOR_SIZE pending match rows
    idx_t remaining = state.pending.size() - state.pending_offset;
    idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
    duckdb_compat::SetChildCardinality(output, count);
    if (count == 0) {
        return;
    }

    // With id_column the id sits at index 1 and shifts the rest by one
    const idx_t id_col = bind_data.id_column.Enabled() ? 1 : 0;
    const idx_t base = 1 + id_col;
    auto &column_name_vec = output.data[base];
    auto &pii_type_vec = output.data[base + 1];
    auto &original_value_vec = output.data[base + 2];
    auto &masked_value_vec = output.data[base + 3];
    auto row_ids = duckdb_compat::FlatData<int64_t>(output.data[0]);
    auto column_names = duckdb_compat::FlatData<string_t>(column_name_vec);
    auto pii_types = duckdb_compat::FlatData<string_t>(pii_type_vec);
    auto original_values = duckdb_compat::FlatData<string_t>(original_value_vec);
    auto masked_values = duckdb_compat::FlatData<string_t>(masked_value_vec);
    auto start_positions = duckdb_compat::FlatData<int64_t>(output.data[base + 4]);
    auto end_positions = duckdb_compat::FlatData<int64_t>(output.data[base + 5]);
    auto confidences = duckdb_compat::FlatData<double>(output.data[base + 6]);
    auto &matched_text_vec = output.data[base + 7];
    auto matched_texts = duckdb_compat::FlatData<string_t>(matched_text_vec);

    for (idx_t i = 0; i < count; i++) {
        const auto &res = state.pending[state.pending_offset + i];
        row_ids[i] = res.row_id;
        if (id_col) {
            output.data[id_col].SetValue(i, res.id_value);
        }
        column_names[i] = StringVector::AddString(column_name_vec, res.column_name);
        pii_types[i] = StringVector::AddString(pii_type_vec, PIITypeToString(res.pii_type));
        original_values[i] = StringVector::AddString(original_value_vec, res.original_value);
        masked_values[i] = StringVector::AddString(masked_value_vec, res.masked_value);
        start_positions[i] = res.start_pos;
        end_positions[i] = res.end_pos;
        confidences[i] = res.confidence;
        matched_texts[i] = StringVector::AddString(matched_text_vec, res.matched_text);
    }

    state.pending_offset += count;
}

TableFunctionSet CreatePIIAuditTableFunctionSet() {
    TableFunctionSet set("anofox_tab_pii_audit_table");

    // Version 1: table_name only
    TableFunction func1("anofox_tab_pii_audit_table",
                        {LogicalType::VARCHAR},
                        DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIAuditTableFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIAuditTableBind), PIIAuditTableInit);
    AddIdColumnParameter(func1);
    set.AddFunction(func1);

    // Version 2: table_name + column filter
    TableFunction func2("anofox_tab_pii_audit_table",
                        {LogicalType::VARCHAR, LogicalType::VARCHAR},
                        DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIAuditTableFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIAuditTableBind), PIIAuditTableInit);
    AddIdColumnParameter(func2);
    set.AddFunction(func2);

    return set;
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

// One output row of pii_scan_table: a PII type found in a column (status "scanned"), or a
// column without any match ("no_match"), skipped for its type ("skipped (DATE)") or not
// readable ("error (...)"); the last three have pii_type UNKNOWN (output NULL)
struct PIIScanTableResult {
    std::string column_name;
    PIIType pii_type;
    int64_t match_count;
    std::vector<std::string> sample_values;
    double confidence;       // mean match confidence, set once the column is scanned
    double confidence_sum;   // running sum of match confidences
    int64_t values_scanned;  // non-NULL values of the column that were scanned
    std::string status;

    PIIScanTableResult()
        : pii_type(PIIType::UNKNOWN), match_count(0), confidence(1.0), confidence_sum(0.0), values_scanned(0),
          status("scanned") {}
};

PIIScanTableResult MakePIIScanStatusRow(const std::string &column_name, int64_t values_scanned,
                                        const std::string &status) {
    PIIScanTableResult row;
    row.column_name = column_name;
    row.values_scanned = values_scanned;
    row.status = status;
    return row;
}

// Streaming scan state: columns are scanned one at a time with streamed
// input chunks; only the (small) per-column aggregates of columns that have
// not been emitted yet are buffered.
struct PIIScanTableState : public GlobalTableFunctionState {
    bool initialized = false;
    // Columns to report (resolved on first call), scanned or skipped
    std::vector<PIIScanColumn> columns_to_scan;
    idx_t column_idx = 0;
    // Configuration snapshot taken once for the whole scan
    PIIConfigSnapshot config;
    // Aggregated results of scanned columns that have not been emitted yet
    std::vector<PIIScanTableResult> pending;
    idx_t pending_offset = 0;
};

unique_ptr<GlobalTableFunctionState> PIIScanTableInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<PIIScanTableState>();
}

unique_ptr<FunctionData> PIIScanTableBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_scan_table");

    // Get table name (required)
    if (input.inputs.empty()) {
        throw BinderException("pii_scan_table requires a table name");
    }
    auto table_name = RequireNonNullArgument(input, 0, "pii_scan_table", "table_name");

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
    // Resolve the filter now so an unknown column is a bind error; the scan uses the
    // table's own spelling of each name
    if (!column_filter.empty()) {
        column_filter = ResolvePIIScanColumns(context, "pii_scan_table", table_name, column_filter);
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

    names.emplace_back("values_scanned");
    return_types.emplace_back(LogicalTypeId::BIGINT);

    names.emplace_back("status");
    return_types.emplace_back(LogicalTypeId::VARCHAR);

    return make_uniq<PIIScanTableBindData>(table_name, column_filter);
}

void PIIScanTableFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<PIIScanTableState>();
    auto &bind_data = input.bind_data->Cast<PIIScanTableBindData>();
    auto &engine = PIIEngine::Instance();

    try {
        if (!state.initialized) {
            state.initialized = true;
            // Resolve the VARCHAR columns to scan and snapshot the
            // configuration once for the whole scan
            state.columns_to_scan = ResolvePIIScanPlan(context, "pii_scan_table", bind_data.table_name, bind_data.column_filter);
            state.config = PIIConfig::Get().Snapshot();
        }

        // Scan one source column at a time until at least one aggregate row
        // is available; input chunks are streamed and never fully
        // materialized. Per-type aggregation requires consuming a full
        // column before its summary rows can be emitted.
        while (state.pending_offset >= state.pending.size() && state.column_idx < state.columns_to_scan.size()) {
            state.pending.clear();
            state.pending_offset = 0;

            const auto &column = state.columns_to_scan[state.column_idx];
            const auto &col_name = column.name;
            state.column_idx++;

            // A column that is not scanned is still reported, so the result
            // accounts for every column of the table
            if (!column.scan) {
                state.pending.push_back(MakePIIScanStatusRow(col_name, 0, "skipped (" + column.type + ")"));
                continue;
            }

            // Build query to get the non-NULL column values
            std::string query = "SELECT CAST(" + QuoteSqlIdentifier(col_name) + " AS VARCHAR) FROM " +
                                BuildQueryTableRef(bind_data.table_name) + " WHERE " +
                                QuoteSqlIdentifier(col_name) + " IS NOT NULL";

            // Execute as a streaming query using a new connection
            Connection con(*context.db);
            auto result = StreamQuery(con, query);
            if (result->HasError()) {
                AnofoxTrace(AnofoxLogLevel::Warn, "pii_scan_table: Error querying column " + col_name);
                state.pending.push_back(MakePIIScanStatusRow(col_name, 0, "error (" + result->GetError() + ")"));
                continue;
            }

            // Aggregate PII detections by type
            std::unordered_map<PIIType, PIIScanTableResult> col_results;
            std::vector<std::string> batch_texts;
            int64_t values_scanned = 0;

            while (true) {
                auto chunk = result->Fetch();
                if (!chunk || chunk->size() == 0) break;

                UnifiedVectorFormat fmt;
                chunk->data[0].ToUnifiedFormat(chunk->size(), fmt);
                auto values = UnifiedVectorFormat::GetData<string_t>(fmt);

                // Collect all texts from this chunk for batch processing
                // (NULLs are filtered in the query; keep a placeholder for safety)
                batch_texts.clear();
                batch_texts.reserve(chunk->size());
                for (idx_t row = 0; row < chunk->size(); row++) {
                    auto idx = fmt.sel->get_index(row);
                    batch_texts.emplace_back(fmt.validity.RowIsValid(idx) ? values[idx].GetString()
                                                                          : std::string());
                }

                // Run batch detection (pre-warms NER cache)
                auto batch_matches = engine.DetectBatch(batch_texts, {}, state.config);
                values_scanned += static_cast<int64_t>(batch_texts.size());

                // Process results
                for (size_t i = 0; i < batch_matches.size(); i++) {
                    for (const auto &match : batch_matches[i]) {
                        auto &res = col_results[match.type];
                        res.column_name = col_name;
                        res.pii_type = match.type;
                        res.match_count++;
                        // Keep up to 5 samples
                        if (res.sample_values.size() < 5) {
                            res.sample_values.push_back(match.matched_text);
                        }
                        res.confidence_sum += match.confidence;
                    }
                }
            }
            if (result->HasError()) {
                AnofoxTrace(AnofoxLogLevel::Warn, "pii_scan_table: Error querying column " + col_name);
                state.pending.push_back(MakePIIScanStatusRow(col_name, values_scanned, "error (" + result->GetError() + ")"));
                continue;
            }
            if (col_results.empty()) {
                state.pending.push_back(MakePIIScanStatusRow(col_name, values_scanned, "no_match"));
                continue;
            }

            // Stage this column's aggregates for emission, ordered by
            // pii_type so the output order does not depend on hash order.
            // confidence is the mean of the match confidences (1.0 for
            // regex matches, the model score for NER matches)
            const size_t first_pending = state.pending.size();
            for (auto &[type, res] : col_results) {
                res.confidence = res.confidence_sum / static_cast<double>(res.match_count);
                res.values_scanned = values_scanned;
                state.pending.push_back(std::move(res));
            }
            std::sort(state.pending.begin() + static_cast<std::ptrdiff_t>(first_pending), state.pending.end(),
                      [](const PIIScanTableResult &a, const PIIScanTableResult &b) {
                          return PIITypeToString(a.pii_type) < PIITypeToString(b.pii_type);
                      });
        }
    } catch (const std::exception &e) {
        throw InvalidInputException("pii_scan_table: Failed to scan table - %s", e.what());
    }

    // Emit pending aggregate rows (at most a handful per column)
    idx_t remaining = state.pending.size() - state.pending_offset;
    idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);
    duckdb_compat::SetChildCardinality(output, count);

    for (idx_t i = 0; i < count; i++) {
        const auto &res = state.pending[state.pending_offset + i];

        // Column 0: column_name
        output.SetValue(0, i, Value(res.column_name));

        // Rows without a match (no_match, skipped, error) have no type,
        // samples or confidence
        const bool has_match = res.pii_type != PIIType::UNKNOWN;

        // Column 1: pii_type
        output.SetValue(1, i, has_match ? Value(PIITypeToString(res.pii_type)) : Value(LogicalType::VARCHAR));

        // Column 2: match_count
        output.SetValue(2, i, Value::BIGINT(res.match_count));

        // Column 3: sample_values (LIST)
        std::vector<Value> sample_list;
        for (const auto &sample : res.sample_values) {
            sample_list.push_back(Value(sample));
        }
        output.SetValue(3, i, Value::LIST(LogicalTypeId::VARCHAR, std::move(sample_list)));

        // Column 4: confidence
        output.SetValue(4, i, has_match ? Value::DOUBLE(res.confidence) : Value(LogicalType::DOUBLE));

        // Column 5: values_scanned, column 6: status
        output.SetValue(5, i, Value::BIGINT(res.values_scanned));
        output.SetValue(6, i, Value(res.status));
    }

    state.pending_offset += count;
}

TableFunctionSet CreatePIIScanTableFunctionSet() {
    TableFunctionSet set("anofox_tab_pii_scan_table");

    // Version 1: table_name only
    TableFunction func1("anofox_tab_pii_scan_table",
                        {LogicalType::VARCHAR},
                        DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIScanTableFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIScanTableBind), PIIScanTableInit);
    set.AddFunction(func1);

    // Version 2: table_name + column filter
    TableFunction func2("anofox_tab_pii_scan_table",
                        {LogicalType::VARCHAR, LogicalType::VARCHAR},
                        DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIScanTableFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIScanTableBind), PIIScanTableInit);
    set.AddFunction(func2);

    return set;
}

} // anonymous namespace

// ============================================================================
// Telemetry Bind Functions for PII Scalar Functions
// ============================================================================

unique_ptr<FunctionData> PIIDetectBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect");
    return nullptr;
}

unique_ptr<FunctionData> PIIMaskBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_mask");
    return nullptr;
}

unique_ptr<FunctionData> PIIContainsBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_contains");
    return nullptr;
}

unique_ptr<FunctionData> PIICountBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_count");
    return nullptr;
}

unique_ptr<FunctionData> PIIIsValidSSNBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_is_valid_ssn");
    return nullptr;
}

unique_ptr<FunctionData> PIIIsValidIBANBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_is_valid_iban");
    return nullptr;
}

unique_ptr<FunctionData> PIIIsValidCreditCardBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_is_valid_credit_card");
    return nullptr;
}

unique_ptr<FunctionData> PIIIsValidNINOBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_is_valid_nino");
    return nullptr;
}

unique_ptr<FunctionData> PIIIsValidDETaxIDBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_is_valid_de_tax_id");
    return nullptr;
}

unique_ptr<FunctionData> PIIIsValidCryptoBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_is_valid_crypto_address");
    return nullptr;
}

unique_ptr<FunctionData> PIIDetectEmailsBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect_emails");
    return nullptr;
}

unique_ptr<FunctionData> PIIDetectPhonesBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect_phones");
    return nullptr;
}

unique_ptr<FunctionData> PIIDetectCreditCardsBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect_credit_cards");
    return nullptr;
}

unique_ptr<FunctionData> PIIDetectSSNsBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect_ssns");
    return nullptr;
}

unique_ptr<FunctionData> PIIDetectNamesBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect_names");
    return nullptr;
}

unique_ptr<FunctionData> PIIDetectIBANsBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect_ibans");
    return nullptr;
}

unique_ptr<FunctionData> PIIDetectBatchBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_detect_batch");
    return nullptr;
}

unique_ptr<FunctionData> PIIMaskColumnBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_mask_column");
    return nullptr;
}

unique_ptr<FunctionData> PIIMaskValueBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_mask_value");
    return nullptr;
}

unique_ptr<FunctionData> PIIRedactColumnBind(ClientContext &) {
    PostHogTelemetry::Instance().RecordFunctionCall("pii_redact_column");
    return nullptr;
}

// ============================================================================
// PII Configuration Option Setters
// ============================================================================

void SetPIIMinConfidenceOption(ClientContext &, SetScope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_pii_min_confidence cannot be NULL");
    }
    auto value = parameter.GetValue<double>();
    PIIConfig::Get().SetMinConfidence(value);
    parameter = Value::DOUBLE(value);
}

void SetPIIDefaultMaskStrategyOption(ClientContext &, SetScope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_pii_default_mask_strategy cannot be NULL");
    }
    auto strategy = parameter.ToString();
    // 'none' is valid as an explicit argument, but as the default it would
    // silently turn pii_redact_column(value) and pii_audit_table.masked_value
    // into pass-throughs of raw PII. Unknown names still raise the generic
    // "Invalid mask strategy" error from ParseMaskStrategy
    if (ParseMaskStrategy(strategy) == MaskStrategy::NONE) {
        throw InvalidInputException("anofox_pii_default_mask_strategy cannot be 'none': a default that "
                                    "disables masking would expose raw PII. Use redact, hash, partial or "
                                    "asterisk (pass 'none' explicitly to pii_mask and friends instead)");
    }
    PIIConfig::Get().SetDefaultMaskStrategy(strategy);
    parameter = Value(PIIConfig::Get().GetDefaultMaskStrategyString());
}

void SetPIIEnabledTypesOption(ClientContext &, SetScope, Value &parameter) {
    if (parameter.IsNull()) {
        PIIConfig::Get().SetEnabledTypes("");  // Reset to all types
        parameter = Value("");
        return;
    }
    auto types_csv = parameter.ToString();
    PIIConfig::Get().SetEnabledTypes(types_csv);
    parameter = Value(PIIConfig::Get().GetEnabledTypesString());
}

void SetPIIRequireChecksumOption(ClientContext &, SetScope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_pii_require_checksum cannot be NULL");
    }
    PIIConfig::Get().SetRequireChecksum(BooleanValue::Get(parameter));
}

void SetPIIDeepValidationOption(ClientContext &, SetScope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_pii_deep_validation cannot be NULL");
    }
    auto enabled = BooleanValue::Get(parameter);
    PIIConfig::Get().SetDeepValidation(enabled);
}

// ============================================================================
// pii_config() Table Function
// ============================================================================

struct PIIConfigData : TableFunctionData {
    bool done = false;
};

struct PIIConfigState : GlobalTableFunctionState {
    idx_t current_row = 0;
};

unique_ptr<FunctionData> PIIConfigBind(ClientContext &, TableFunctionBindInput &,
                                       vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
    return_types.push_back(LogicalType::VARCHAR);  // option_name
    return_types.push_back(LogicalType::VARCHAR);  // option_value
    return_types.push_back(LogicalType::VARCHAR);  // description

    names.emplace_back("option_name");
    names.emplace_back("option_value");
    names.emplace_back("description");

    return make_uniq<PIIConfigData>();
}

unique_ptr<GlobalTableFunctionState> PIIConfigInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<PIIConfigState>();
}

void PIIConfigFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
    auto &state = input.global_state->Cast<PIIConfigState>();

    struct ConfigOption {
        const char* name;
        std::string value;
        const char* description;
    };

    // Single consistent snapshot of the configuration
    const auto config = PIIConfig::Get().Snapshot();
    const auto enabled_types_str = PIIConfig::Get().GetEnabledTypesString();

    std::vector<ConfigOption> options = {
        {"anofox_pii_min_confidence",
         std::to_string(config.min_confidence),
         "Minimum confidence threshold for NER-based detection (0.0 - 1.0)"},
        {"anofox_pii_default_mask_strategy",
         MaskStrategyToString(config.default_mask_strategy),
         "Default masking strategy (REDACT, HASH, PARTIAL, ASTERISK; NONE is rejected)"},
        {"anofox_pii_enabled_types",
         enabled_types_str.empty() ? "(all types except POSTAL_CODE)" : enabled_types_str,
         "Comma-separated list of PII types to detect (empty = all types except POSTAL_CODE)"},
        {"anofox_pii_deep_validation",
         config.deep_validation ? "true" : "false",
         "Enable deep validation using libphonenumber for phone numbers"},
        {"anofox_pii_require_checksum",
         config.require_checksum ? "true" : "false",
         "Drop IBAN and credit card matches that fail their checksum (false: report them at confidence 0.6)"}
    };

    if (state.current_row >= options.size()) {
        duckdb_compat::SetChildCardinality(output, 0);
        return;
    }

    idx_t remaining = options.size() - state.current_row;
    idx_t count = MinValue<idx_t>(remaining, STANDARD_VECTOR_SIZE);

    duckdb_compat::SetChildCardinality(output, count);

    for (idx_t i = 0; i < count; i++) {
        const auto &opt = options[state.current_row + i];
        output.SetValue(0, i, Value(opt.name));
        output.SetValue(1, i, Value(opt.value));
        output.SetValue(2, i, Value(opt.description));
    }

    state.current_row += count;
}

TableFunction CreatePIIConfigFunction() {
    TableFunction func("anofox_tab_pii_config", {}, DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIConfigFunction), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, PIIConfigBind), PIIConfigInit);
    return func;
}

// ============================================================================
// Registration
// ============================================================================

void RegisterPIIOptions(ExtensionLoader &loader) {
    auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());

    config.AddExtensionOption("anofox_pii_min_confidence",
                              "Minimum confidence threshold for NER-based PII detection (0.0 - 1.0)",
                              LogicalTypeId::DOUBLE,
                              Value::DOUBLE(PIIConfig::DEFAULT_MIN_CONFIDENCE),
                              SetPIIMinConfidenceOption);

    config.AddExtensionOption("anofox_pii_default_mask_strategy",
                              "Default masking strategy (REDACT, HASH, PARTIAL, ASTERISK; NONE is rejected)",
                              LogicalTypeId::VARCHAR,
                              Value(PIIConfig::DEFAULT_MASK_STRATEGY),
                              SetPIIDefaultMaskStrategyOption);

    config.AddExtensionOption("anofox_pii_enabled_types",
                              "Comma-separated list of PII types to detect (empty = all types except POSTAL_CODE)",
                              LogicalTypeId::VARCHAR,
                              Value(""),
                              SetPIIEnabledTypesOption);

    config.AddExtensionOption("anofox_pii_deep_validation",
                              "Enable deep validation using libphonenumber for phone numbers (default: false)",
                              LogicalTypeId::BOOLEAN,
                              Value::BOOLEAN(false),
                              SetPIIDeepValidationOption);

    config.AddExtensionOption("anofox_pii_require_checksum",
                              "Drop IBAN and credit card matches that fail their checksum (false: report them "
                              "at confidence 0.6 so mistyped values can still be masked)",
                              LogicalTypeId::BOOLEAN,
                              Value::BOOLEAN(true),
                              SetPIIRequireChecksumOption);

    AnofoxTrace(AnofoxLogLevel::Info, "PII configuration options registered");
}

void RegisterPIIFunctions(ExtensionLoader &loader) {
    // anofox_tab_pii_detect (alias: pii_detect)
    {
        FunctionDescription desc;
        desc.description = "Detects all PII entities in a text string and returns them as a list of structs with type, value, start, and end positions.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_detect('Contact John at john@example.com or +1-555-123-4567');"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_func = PIIDetectFunction::GetFunction();
        pii_detect_func.SetBindCallback(&ScalarContextBind<PIIDetectBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_func, "pii_detect", {std::move(desc)});
    }
    // anofox_tab_pii_mask (alias: pii_mask) - with function set for overloads
    {
        FunctionDescription desc;
        desc.description = "Masks all detected PII in a text string using the specified strategy ('redact', 'hash', 'partial'). Defaults to 'redact'.";
        desc.parameter_names = {"text", "strategy"};
        desc.examples = {"SELECT pii_mask('Call me at +1-555-123-4567');", "SELECT pii_mask('john@example.com', 'hash');"};
        desc.categories = {"pii", "masking"};
        ScalarFunctionSet mask_set("anofox_tab_pii_mask");
        auto pii_mask_func = PIIMaskFunction::GetFunction();
        pii_mask_func.SetBindCallback(&ScalarContextBind<PIIMaskBind>::Bind);
        mask_set.AddFunction(pii_mask_func);
        auto pii_mask_default_func = PIIMaskDefaultFunction::GetFunction();
        pii_mask_default_func.SetBindCallback(&ScalarContextBind<PIIMaskBind>::Bind);
        mask_set.AddFunction(pii_mask_default_func);
        RegisterScalarFunctionSetWithAlias(loader, mask_set, "pii_mask", {std::move(desc)});
    }
    // anofox_tab_pii_contains (alias: pii_contains)
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the text contains any PII of the specified type (e.g., 'email', 'phone', 'ssn').";
        desc.parameter_names = {"text", "pii_type"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_contains('Contact us at info@company.com', 'email');"};
        desc.categories = {"pii", "detection"};
        auto pii_contains_func = PIIContainsFunction::GetFunction();
        pii_contains_func.SetBindCallback(&ScalarContextBind<PIIContainsBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_contains_func, "pii_contains", {std::move(desc)});
    }
    // anofox_tab_pii_count (alias: pii_count)
    {
        FunctionDescription desc;
        desc.description = "Returns the total count of PII entities detected in the text.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_count('John at john@example.com, tel: +1-555-0100');"};
        desc.categories = {"pii", "detection"};
        auto pii_count_func = PIICountFunction::GetFunction();
        pii_count_func.SetBindCallback(&ScalarContextBind<PIICountBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_count_func, "pii_count", {std::move(desc)});
    }
    // Individual validation functions
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the string is a valid US Social Security Number (SSN).";
        desc.parameter_names = {"ssn"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_is_valid_ssn('123-45-6789');"};
        desc.categories = {"pii", "validation"};
        auto pii_valid_ssn_func = PIIIsValidSSNFunction::GetFunction();
        pii_valid_ssn_func.SetBindCallback(&ScalarContextBind<PIIIsValidSSNBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_valid_ssn_func, "pii_is_valid_ssn", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the string is a valid IBAN (International Bank Account Number).";
        desc.parameter_names = {"iban"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_is_valid_iban('DE89370400440532013000');"};
        desc.categories = {"pii", "validation"};
        auto pii_valid_iban_func = PIIIsValidIBANFunction::GetFunction();
        pii_valid_iban_func.SetBindCallback(&ScalarContextBind<PIIIsValidIBANBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_valid_iban_func, "pii_is_valid_iban", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the string is a valid credit card number (uses Luhn algorithm).";
        desc.parameter_names = {"card_number"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_is_valid_credit_card('4111111111111111');"};
        desc.categories = {"pii", "validation"};
        auto pii_valid_cc_func = PIIIsValidCreditCardFunction::GetFunction();
        pii_valid_cc_func.SetBindCallback(&ScalarContextBind<PIIIsValidCreditCardBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_valid_cc_func, "pii_is_valid_credit_card", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the string is a valid UK National Insurance Number (NINO).";
        desc.parameter_names = {"nino"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_is_valid_nino('AB123456C');"};
        desc.categories = {"pii", "validation"};
        auto pii_valid_nino_func = PIIIsValidNINOFunction::GetFunction();
        pii_valid_nino_func.SetBindCallback(&ScalarContextBind<PIIIsValidNINOBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_valid_nino_func, "pii_is_valid_nino", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the string is a valid German tax identification number (Steueridentifikationsnummer).";
        desc.parameter_names = {"tax_id"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_is_valid_de_tax_id('12345678901');"};
        desc.categories = {"pii", "validation"};
        auto pii_valid_de_tax_func = PIIIsValidDETaxIDFunction::GetFunction();
        pii_valid_de_tax_func.SetBindCallback(&ScalarContextBind<PIIIsValidDETaxIDBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_valid_de_tax_func, "pii_is_valid_de_tax_id", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the string is a valid cryptocurrency wallet address (Bitcoin, Ethereum, etc.).";
        desc.parameter_names = {"address"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_is_valid_crypto_address('1A1zP1eP5QGefi2DMPTfTL5SLmv7Divf');"};
        desc.categories = {"pii", "validation"};
        auto pii_valid_crypto_func = PIIIsValidCryptoFunction::GetFunction();
        pii_valid_crypto_func.SetBindCallback(&ScalarContextBind<PIIIsValidCryptoBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_valid_crypto_func, "pii_is_valid_crypto_address", {std::move(desc)});
    }
    // Type-specific detection functions
    {
        FunctionDescription desc;
        desc.description = "Detects all email addresses in the text and returns them as a list of structs.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_detect_emails('Contact jane@example.com or bob@company.org');"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_emails_func = PIIDetectEmailsFunction::GetFunction();
        pii_detect_emails_func.SetBindCallback(&ScalarContextBind<PIIDetectEmailsBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_emails_func, "pii_detect_emails", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Detects all phone numbers in the text and returns them as a list of structs.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_detect_phones('Call +1-555-0100 or 0800 123 456');"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_phones_func = PIIDetectPhonesFunction::GetFunction();
        pii_detect_phones_func.SetBindCallback(&ScalarContextBind<PIIDetectPhonesBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_phones_func, "pii_detect_phones", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Detects all credit card numbers in the text and returns them as a list of structs.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_detect_credit_cards('Pay with 4111111111111111');"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_cc_func = PIIDetectCreditCardsFunction::GetFunction();
        pii_detect_cc_func.SetBindCallback(&ScalarContextBind<PIIDetectCreditCardsBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_cc_func, "pii_detect_credit_cards", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Detects all Social Security Numbers (SSNs) in the text and returns them as a list of structs.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_detect_ssns('SSN: 123-45-6789');"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_ssns_func = PIIDetectSSNsFunction::GetFunction();
        pii_detect_ssns_func.SetBindCallback(&ScalarContextBind<PIIDetectSSNsBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_ssns_func, "pii_detect_ssns", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Detects all person names in the text using NLP and returns them as a list of structs.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_detect_names('Signed by John Smith and Jane Doe');"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_names_func = PIIDetectNamesFunction::GetFunction();
        pii_detect_names_func.SetBindCallback(&ScalarContextBind<PIIDetectNamesBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_names_func, "pii_detect_names", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Detects all IBAN numbers in the text and returns them as a list of structs.";
        desc.parameter_names = {"text"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_detect_ibans('Bank: DE89370400440532013000');"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_ibans_func = PIIDetectIBANsFunction::GetFunction();
        pii_detect_ibans_func.SetBindCallback(&ScalarContextBind<PIIDetectIBANsBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_ibans_func, "pii_detect_ibans", {std::move(desc)});
    }
    // Batch detection function
    {
        FunctionDescription desc;
        desc.description = "Runs all PII detectors on an array of text strings and returns a combined list of all detected entities per input.";
        desc.parameter_names = {"texts"};
        desc.parameter_types = {LogicalType::LIST(LogicalType::VARCHAR)};
        desc.examples = {"SELECT pii_detect_batch(['john@example.com', 'SSN: 123-45-6789']);"};
        desc.categories = {"pii", "detection"};
        auto pii_detect_batch_func = PIIDetectBatchFunction::GetFunction();
        pii_detect_batch_func.SetBindCallback(&ScalarContextBind<PIIDetectBatchBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_detect_batch_func, "pii_detect_batch", {std::move(desc)});
    }
    // Advanced masking functions
    {
        FunctionDescription desc;
        desc.description = "Masks a value of a specific PII type using the specified strategy ('redact', 'hash', 'partial').";
        desc.parameter_names = {"value", "pii_type", "strategy"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_mask_column('john@example.com', 'email', 'hash');"};
        desc.categories = {"pii", "masking"};
        auto pii_mask_column_func = PIIMaskColumnFunction::GetFunction();
        pii_mask_column_func.SetBindCallback(&ScalarContextBind<PIIMaskColumnBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_mask_column_func, "pii_mask_column", {std::move(desc)});
    }
    // pii_mask_value(value, pii_type, strategy) - mask a whole value of a known type
    {
        FunctionDescription desc;
        desc.description = "Masks a whole value as the given PII type, without detection: for columns known to hold "
                           "PII ('redact', 'partial', 'asterisk', 'hash', 'none').";
        desc.parameter_names = {"value", "pii_type", "strategy"};
        desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
        desc.examples = {"SELECT pii_mask_value('Li', 'NAME', 'redact');",
                         "SELECT pii_mask_value('01511234567', 'PHONE', 'partial');"};
        desc.categories = {"pii", "masking"};
        auto pii_mask_value_func = PIIMaskValueFunction::GetFunction();
        pii_mask_value_func.SetBindCallback(&ScalarContextBind<PIIMaskValueBind>::Bind);
        RegisterScalarFunctionWithAlias(loader, pii_mask_value_func, "pii_mask_value", {std::move(desc)});
    }
    // pii_redact_column(value, strategy?) - mask all PII, optional strategy
    {
        FunctionDescription desc;
        desc.description = "Detects and redacts all PII in a text value using the specified strategy. Defaults to 'redact'.";
        desc.parameter_names = {"value", "strategy"};
        desc.examples = {"SELECT pii_redact_column('john@example.com');", "SELECT pii_redact_column('john@example.com', 'hash');"};
        desc.categories = {"pii", "masking"};
        ScalarFunctionSet redact_column_set("anofox_tab_pii_redact_column");
        auto pii_redact_column_func = PIIRedactColumnFunction::GetFunction();
        pii_redact_column_func.SetBindCallback(&ScalarContextBind<PIIRedactColumnBind>::Bind);
        redact_column_set.AddFunction(pii_redact_column_func);
        auto pii_redact_column_default_func = PIIRedactColumnDefaultFunction::GetFunction();
        pii_redact_column_default_func.SetBindCallback(&ScalarContextBind<PIIRedactColumnBind>::Bind);
        redact_column_set.AddFunction(pii_redact_column_default_func);
        RegisterScalarFunctionSetWithAlias(loader, redact_column_set, "pii_redact_column", {std::move(desc)});
    }
    // anofox_tab_pii_status (alias: pii_status)
    {
        FunctionDescription desc;
        desc.description = "Returns the current configuration and status of the PII detection module.";
        desc.examples = {"SELECT * FROM pii_status();"};
        desc.categories = {"pii", "status"};
        auto pii_status_func = CreatePIIStatusFunction();
        RegisterTableFunctionWithAlias(loader, pii_status_func, "pii_status", {std::move(desc)});
    }
    // anofox_tab_pii_scan_table (alias: pii_scan_table)
    {
        FunctionDescription desc;
        desc.description = "Scans all string columns of a table and returns a summary of detected PII types per column.";
        desc.parameter_names = {"table_name", "columns"};
        desc.examples = {"SELECT * FROM pii_scan_table('customers');"};
        desc.categories = {"pii", "audit"};
        auto pii_scan_set = CreatePIIScanTableFunctionSet();
        RegisterTableFunctionSetWithAlias(loader, pii_scan_set, "pii_scan_table", {std::move(desc)});
    }
    // anofox_tab_pii_audit_table (alias: pii_audit_table)
    {
        FunctionDescription desc;
        desc.description = "Returns a row-level audit of PII detected in all string columns of a table.";
        desc.parameter_names = {"table_name", "columns"};
        desc.examples = {"SELECT * FROM pii_audit_table('customers');"};
        desc.categories = {"pii", "audit"};
        auto pii_audit_set = CreatePIIAuditTableFunctionSet();
        RegisterTableFunctionSetWithAlias(loader, pii_audit_set, "pii_audit_table", {std::move(desc)});
    }
    // anofox_ner_status - NER model status (no alias, direct name)
    {
        FunctionDescription desc;
        desc.description = "Returns the status of the NER (Named Entity Recognition) model used for name detection.";
        desc.examples = {"SELECT * FROM anofox_ner_status();"};
        desc.categories = {"pii", "status"};
        auto ner_status_func = CreateNERStatusFunction();
        CreateTableFunctionInfo ner_info(ner_status_func);
        ner_info.descriptions = {std::move(desc)};
        loader.RegisterFunction(ner_info);
    }
    // anofox_tab_pii_config (alias: pii_config)
    {
        FunctionDescription desc;
        desc.description = "Returns the current configuration settings of the PII detection module.";
        desc.examples = {"SELECT * FROM pii_config();"};
        desc.categories = {"pii", "config"};
        auto pii_config_func = CreatePIIConfigFunction();
        RegisterTableFunctionWithAlias(loader, pii_config_func, "pii_config", {std::move(desc)});
    }

    AnofoxTrace(AnofoxLogLevel::Info, "PII detection functions registered");
}

} // namespace anofox
} // namespace duckdb

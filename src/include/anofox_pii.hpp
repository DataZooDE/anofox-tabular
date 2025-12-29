#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

#include <string>
#include <vector>
#include <memory>
#include <regex>
#include <functional>
#include <unordered_map>
#include <optional>

namespace duckdb {
namespace anofox {

/**
 * Supported PII entity types
 */
enum class PIIType {
    EMAIL,           // Email addresses
    PHONE,           // Phone numbers (international)
    CREDIT_CARD,     // Credit card numbers (Visa, MC, Amex, Discover)
    US_SSN,          // US Social Security Numbers
    IP_ADDRESS,      // IPv4 and IPv6 addresses
    IBAN,            // International Bank Account Numbers
    DE_TAX_ID,       // German Tax ID (Steueridentifikationsnummer)
    URL,             // HTTP/HTTPS URLs
    NAME,            // Person names (NER-based detection, PER entities)
    ORGANIZATION,    // Organization/company names (NER-based, ORG entities)
    LOCATION,        // Geographic locations (NER-based, LOC entities)
    MISC,            // Miscellaneous entities (NER-based, MISC entities - events, languages)
    US_PASSPORT,
    CRYPTO_ADDRESS,
    UK_NINO,
    MAC_ADDRESS,     // Network hardware addresses
    API_KEY,         // API keys (AWS, GitHub, generic)
    UNKNOWN
};

/**
 * Convert PIIType to string representation
 */
std::string PIITypeToString(PIIType type);

/**
 * Convert string to PIIType (case-insensitive)
 */
PIIType StringToPIIType(const std::string &str);

/**
 * Masking strategies for PII
 */
enum class MaskStrategy {
    REDACT,    // Replace with [REDACTED] or similar
    HASH,      // Replace with SHA-256 hash (truncated)
    PARTIAL,   // Show partial value (e.g., ***-**-1234 for SSN)
    ASTERISK,  // Replace with asterisks (same length)
    NONE       // No masking (for detection only)
};

/**
 * Convert MaskStrategy to string
 */
std::string MaskStrategyToString(MaskStrategy strategy);

/**
 * Convert string to MaskStrategy (case-insensitive)
 */
MaskStrategy StringToMaskStrategy(const std::string &str);

/**
 * Result of a PII detection match
 */
struct PIIMatch {
    PIIType type;              // Type of PII detected
    std::string matched_text;  // The actual matched text
    size_t start_pos;          // Start position in original string
    size_t end_pos;            // End position in original string (exclusive)
    double confidence;         // Confidence score (0.0 - 1.0)

    PIIMatch() : type(PIIType::UNKNOWN), start_pos(0), end_pos(0), confidence(0.0) {}

    PIIMatch(PIIType t, const std::string &text, size_t start, size_t end, double conf = 1.0)
        : type(t), matched_text(text), start_pos(start), end_pos(end), confidence(conf) {}

    // Convert to JSON representation
    std::string ToJSON() const;
};

/**
 * Interface for PII pattern recognizers
 */
class PIIRecognizer {
public:
    virtual ~PIIRecognizer() = default;

    /**
     * Get the PII type this recognizer detects
     */
    virtual PIIType GetType() const = 0;

    /**
     * Get pattern name for display
     */
    virtual std::string GetName() const = 0;

    /**
     * Find all matches in the input text
     * @param text: Input text to scan
     * @return Vector of matches found
     */
    virtual std::vector<PIIMatch> FindMatches(const std::string &text) const = 0;

    /**
     * Validate a candidate match (e.g., checksum validation)
     * @param text: The matched text to validate
     * @return true if valid
     */
    virtual bool Validate(const std::string &text) const = 0;

    /**
     * Apply partial masking specific to this PII type
     * @param text: The text to partially mask
     * @return Partially masked text (e.g., ***-**-1234 for SSN)
     */
    virtual std::string GetPartialMask(const std::string &text) const = 0;
};

/**
 * Base class for regex-based recognizers
 */
class RegexRecognizer : public PIIRecognizer {
public:
    RegexRecognizer(PIIType type, const std::string &name, const std::string &pattern);
    ~RegexRecognizer() override;

    PIIType GetType() const override;
    std::string GetName() const override;
    std::vector<PIIMatch> FindMatches(const std::string &text) const override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

protected:
    PIIType type_;
    std::string name_;
    std::regex pattern_;
};

/**
 * Credit card number recognizer with Luhn checksum validation
 */
class CreditCardRecognizer : public RegexRecognizer {
public:
    CreditCardRecognizer();
    ~CreditCardRecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    static bool LuhnCheck(const std::string &digits);
};

/**
 * US Social Security Number recognizer
 */
class USSSNRecognizer : public RegexRecognizer {
public:
    USSSNRecognizer();
    ~USSSNRecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * IBAN recognizer with MOD-97 checksum validation
 */
class IBANRecognizer : public RegexRecognizer {
public:
    IBANRecognizer();
    ~IBANRecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    static bool Mod97Check(const std::string &iban);
};

/**
 * German Tax ID (Steueridentifikationsnummer) recognizer
 */
class DETaxIDRecognizer : public RegexRecognizer {
public:
    DETaxIDRecognizer();
    ~DETaxIDRecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    static bool ChecksumValidate(const std::string &digits);
};

/**
 * IP Address recognizer (IPv4 and IPv6)
 */
class IPAddressRecognizer : public RegexRecognizer {
public:
    IPAddressRecognizer();
    ~IPAddressRecognizer() override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * URL recognizer
 */
class URLRecognizer : public RegexRecognizer {
public:
    URLRecognizer();
    ~URLRecognizer() override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * Email recognizer (reuses existing email validation logic)
 */
class EmailRecognizer : public RegexRecognizer {
public:
    EmailRecognizer();
    ~EmailRecognizer() override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * MAC Address recognizer (network hardware addresses)
 */
class MACAddressRecognizer : public RegexRecognizer {
public:
    MACAddressRecognizer();
    ~MACAddressRecognizer() override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * UK National Insurance Number recognizer
 */
class UKNINORecognizer : public RegexRecognizer {
public:
    UKNINORecognizer();
    ~UKNINORecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * US Passport number recognizer
 */
class USPassportRecognizer : public RegexRecognizer {
public:
    USPassportRecognizer();
    ~USPassportRecognizer() override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * Phone number recognizer (pattern-based, lightweight)
 */
class PhoneRecognizer : public RegexRecognizer {
public:
    PhoneRecognizer();
    ~PhoneRecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;
};

/**
 * API Key recognizer (AWS, GitHub, generic high-entropy)
 */
class APIKeyRecognizer : public RegexRecognizer {
public:
    APIKeyRecognizer();
    ~APIKeyRecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    static double CalculateShannonEntropy(const std::string &text);
};

/**
 * Cryptocurrency Address recognizer (Bitcoin, Ethereum)
 */
class CryptoAddressRecognizer : public RegexRecognizer {
public:
    CryptoAddressRecognizer();
    ~CryptoAddressRecognizer() override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    static bool ValidateBitcoinAddress(const std::string &address);
    static std::vector<uint8_t> DecodeBase58(const std::string &input);
    static bool ValidateEthereumAddress(const std::string &address);
};

/**
 * Person Name recognizer (NER-based detection, PER entities)
 * Uses OpenVINO DistilBERT NER model with dictionary fallback
 */
class NameRecognizer : public PIIRecognizer {
public:
    NameRecognizer();
    ~NameRecognizer() override;

    PIIType GetType() const override;
    std::string GetName() const override;
    std::vector<PIIMatch> FindMatches(const std::string &text) const override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    std::vector<PIIMatch> ExtractWithDictionary(const std::string &text) const;
    std::regex pattern_;
};

/**
 * Organization Name recognizer (NER-based detection, ORG entities)
 * Uses OpenVINO DistilBERT NER model to detect company/organization names
 */
class OrganizationRecognizer : public PIIRecognizer {
public:
    OrganizationRecognizer();
    ~OrganizationRecognizer() override;

    PIIType GetType() const override;
    std::string GetName() const override;
    std::vector<PIIMatch> FindMatches(const std::string &text) const override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    PIIType type_;
};

/**
 * Location recognizer (NER-based detection, LOC entities)
 * Uses OpenVINO DistilBERT NER model to detect geographic locations
 */
class LocationRecognizer : public PIIRecognizer {
public:
    LocationRecognizer();
    ~LocationRecognizer() override;

    PIIType GetType() const override;
    std::string GetName() const override;
    std::vector<PIIMatch> FindMatches(const std::string &text) const override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    PIIType type_;
};

/**
 * Miscellaneous Entity recognizer (NER-based detection, MISC entities)
 * Uses OpenVINO DistilBERT NER model to detect misc entities (events, languages, etc.)
 */
class MiscRecognizer : public PIIRecognizer {
public:
    MiscRecognizer();
    ~MiscRecognizer() override;

    PIIType GetType() const override;
    std::string GetName() const override;
    std::vector<PIIMatch> FindMatches(const std::string &text) const override;
    bool Validate(const std::string &text) const override;
    std::string GetPartialMask(const std::string &text) const override;

private:
    PIIType type_;
};

/**
 * PII Detection Engine
 * Main class for detecting and masking PII in text
 */
class PIIEngine {
public:
    PIIEngine();
    ~PIIEngine() = default;

    /**
     * Get singleton instance
     */
    static PIIEngine& Instance();

    /**
     * Detect all PII in input text
     * @param text: Input text to scan
     * @param types: Optional filter for specific PII types (empty = all)
     * @return Vector of all matches found, sorted by position
     */
    std::vector<PIIMatch> Detect(
        const std::string &text,
        const std::vector<PIIType> &types = {}
    ) const;

    /**
     * Batch detection of PII in multiple texts
     * Pre-warms NER cache for efficiency when processing many texts
     * @param texts: Vector of input texts to scan
     * @param types: Optional filter for specific PII types (empty = all)
     * @return Vector of match vectors (one per input text)
     */
    std::vector<std::vector<PIIMatch>> DetectBatch(
        const std::vector<std::string> &texts,
        const std::vector<PIIType> &types = {}
    ) const;

    /**
     * Mask all PII in input text
     * @param text: Input text to process
     * @param strategy: Masking strategy to apply
     * @param types: Optional filter for specific PII types (empty = all)
     * @return Text with PII masked
     */
    std::string Mask(
        const std::string &text,
        MaskStrategy strategy,
        const std::vector<PIIType> &types = {}
    ) const;

    /**
     * Get list of supported PII types
     */
    std::vector<PIIType> GetSupportedTypes() const;

    /**
     * Register a custom recognizer
     */
    void RegisterRecognizer(std::unique_ptr<PIIRecognizer> recognizer);

    /**
     * Validate a text for a specific PII type
     * First checks if text matches the pattern, then validates checksum if applicable
     * @param text: Input text to validate
     * @param type: PII type to validate against
     * @return true if text is valid for the specified type
     */
    bool ValidateType(const std::string &text, PIIType type) const;

private:
    std::vector<std::unique_ptr<PIIRecognizer>> recognizers_;

    /**
     * Initialize default recognizers
     */
    void InitializeDefaultRecognizers();

    /**
     * Apply masking to a single match
     */
    std::string ApplyMask(const PIIMatch &match, MaskStrategy strategy) const;

    /**
     * Get recognizer for a specific type
     */
    const PIIRecognizer* GetRecognizer(PIIType type) const;
};

/**
 * Column-level PII scan result
 */
struct PIIScanResult {
    std::string column_name;
    PIIType pii_type;
    int64_t match_count;
    std::vector<std::string> sample_values;  // Up to 5 sample matches
    double confidence;  // Average confidence across matches

    PIIScanResult() : pii_type(PIIType::UNKNOWN), match_count(0), confidence(0.0) {}
};

/**
 * Row-level PII audit result
 */
struct PIIAuditResult {
    int64_t row_id;
    std::string column_name;
    PIIType pii_type;
    std::string original_value;
    std::string masked_value;
    size_t start_pos;
    size_t end_pos;
    double confidence;

    PIIAuditResult() : row_id(0), pii_type(PIIType::UNKNOWN), start_pos(0), end_pos(0), confidence(0.0) {}
};

/**
 * PII Configuration Singleton
 * Manages runtime configuration for PII detection and masking
 */
class PIIConfig {
public:
    static PIIConfig& Get();

    // Confidence threshold for NER-based detection (0.0 - 1.0)
    double GetMinConfidence() const { return min_confidence_; }
    void SetMinConfidence(double value);

    // Default masking strategy
    MaskStrategy GetDefaultMaskStrategy() const { return default_mask_strategy_; }
    std::string GetDefaultMaskStrategyString() const { return MaskStrategyToString(default_mask_strategy_); }
    void SetDefaultMaskStrategy(const std::string &strategy);

    // Enabled PII types filter (empty = all types enabled)
    const std::vector<PIIType>& GetEnabledTypes() const { return enabled_types_; }
    std::string GetEnabledTypesString() const;
    void SetEnabledTypes(const std::string &types_csv);
    bool IsTypeEnabled(PIIType type) const;

    // Deep validation for emails and phones (uses existing validators)
    bool IsDeepValidationEnabled() const { return deep_validation_; }
    void SetDeepValidation(bool enabled) { deep_validation_ = enabled; }

    // Configuration limits
    static constexpr double MIN_CONFIDENCE = 0.0;
    static constexpr double MAX_CONFIDENCE = 1.0;
    static constexpr double DEFAULT_MIN_CONFIDENCE = 0.5;
    static constexpr const char* DEFAULT_MASK_STRATEGY = "redact";

private:
    PIIConfig();
    ~PIIConfig() = default;
    PIIConfig(const PIIConfig&) = delete;
    PIIConfig& operator=(const PIIConfig&) = delete;

    double min_confidence_;
    MaskStrategy default_mask_strategy_;
    std::vector<PIIType> enabled_types_;  // Empty = all types
    bool deep_validation_ = false;        // Use email/phone validators
};

/**
 * Register PII configuration options with DuckDB
 */
void RegisterPIIOptions(ExtensionLoader &loader);

/**
 * Register PII functions with DuckDB
 */
void RegisterPIIFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

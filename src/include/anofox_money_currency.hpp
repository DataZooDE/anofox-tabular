#pragma once

#include "duckdb.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {
namespace anofox {

// Currency metadata structure
struct CurrencyInfo {
    string iso_code;
    string iso_numeric;
    int32_t priority;
    string name;
    string symbol;
    vector<string> alternate_symbols;
    string subunit;  // Empty string if no subunit
    int32_t subunit_to_unit;
    bool symbol_first;
    string decimal_mark;
    string thousands_separator;
    int32_t smallest_denomination;

    CurrencyInfo() = default;
    ~CurrencyInfo() = default;
};

// Global currency registry. Fully initialized by its constructor (thread-safe
// via C++11 magic statics) and immutable afterwards.
class CurrencyRegistry {
public:
    static CurrencyRegistry &GetInstance();

    // Look up currency by ISO code (case-insensitive)
    optional_ptr<const CurrencyInfo> GetCurrency(const string &iso_code) const;

    // Get all currencies
    const case_insensitive_map_t<unique_ptr<CurrencyInfo>> &GetAllCurrencies() const;

    // Check if currency exists
    bool CurrencyExists(const string &iso_code) const;

    // Get currency count
    idx_t GetCurrencyCount() const;

private:
    CurrencyRegistry();

    case_insensitive_map_t<unique_ptr<CurrencyInfo>> currencies;

    // Load hardcoded currencies into registry
    void LoadCurrencies();
};

} // namespace anofox
} // namespace duckdb

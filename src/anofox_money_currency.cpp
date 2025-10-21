#include "anofox_money_currency.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace anofox {

CurrencyRegistry &CurrencyRegistry::GetInstance() {
    static CurrencyRegistry instance;
    if (!instance.initialized) {
        instance.Initialize();
    }
    return instance;
}

void CurrencyRegistry::Initialize() {
    LoadCurrencies();
    initialized = true;
}

void CurrencyRegistry::LoadCurrencies() {
    // Hardcoded currencies
    auto add_currency = [this](const CurrencyInfo &info) {
        currencies[info.iso_code] = make_uniq<CurrencyInfo>(info);
    };

    add_currency({"USD", "840", 1, "United States Dollar", "$", {"US$"}, "Cent", 100, true, ".", ",", 1});
    add_currency({"EUR", "978", 2, "Euro", "€", {}, "Cent", 100, false, ",", ".", 1});
    add_currency({"GBP", "826", 3, "British Pound", "£", {}, "Penny", 100, true, ".", ",", 1});
    add_currency({"JPY", "392", 4, "Japanese Yen", "¥", {}, "", 1, true, ".", ",", 1});
    add_currency({"CAD", "124", 5, "Canadian Dollar", "$", {}, "Cent", 100, true, ".", ",", 1});
    add_currency({"AUD", "036", 6, "Australian Dollar", "$", {}, "Cent", 100, true, ".", ",", 1});
    add_currency({"CHF", "756", 7, "Swiss Franc", "CHF", {}, "Centime", 100, false, ".", "'", 5});
    add_currency({"CNY", "156", 8, "Chinese Yuan", "¥", {}, "Fen", 100, true, ".", ",", 1});
    add_currency({"INR", "356", 9, "Indian Rupee", "₹", {}, "Paisa", 100, true, ".", ",", 1});
    add_currency({"BRL", "986", 10, "Brazilian Real", "R$", {}, "Centavo", 100, true, ",", ".", 1});
}

optional_ptr<const CurrencyInfo> CurrencyRegistry::GetCurrency(const string &iso_code) const {
    auto it = currencies.find(iso_code);
    if (it != currencies.end()) {
        return it->second.get();
    }
    return nullptr;
}

const case_insensitive_map_t<unique_ptr<CurrencyInfo>> &CurrencyRegistry::GetAllCurrencies() const {
    return currencies;
}

bool CurrencyRegistry::CurrencyExists(const string &iso_code) const {
    return currencies.find(iso_code) != currencies.end();
}

idx_t CurrencyRegistry::GetCurrencyCount() const {
    return currencies.size();
}

} // namespace anofox
} // namespace duckdb

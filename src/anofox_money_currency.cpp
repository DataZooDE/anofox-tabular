#include "anofox_money_currency.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace anofox {

CurrencyRegistry::CurrencyRegistry() {
    LoadCurrencies();
}

CurrencyRegistry &CurrencyRegistry::GetInstance() {
    // Thread-safe under C++11 magic statics: the constructor fully populates the
    // registry before the reference can escape to any thread, and the registry is
    // immutable afterwards (issue #43).
    static CurrencyRegistry instance;
    return instance;
}

void CurrencyRegistry::LoadCurrencies() {
    // Hardcoded ISO 4217 currencies. subunit_to_unit follows the ISO 4217 minor
    // units (100 = 2 decimals, 1 = 0 decimals, 1000 = 3 decimals); it must divide
    // the money storage scale (10^3). Symbol position and separators follow the
    // usual local convention of each currency.
    auto add_currency = [this](const CurrencyInfo &info) {
        currencies[info.iso_code] = make_uniq<CurrencyInfo>(info);
    };

    // iso, numeric, priority, name, symbol, alternate symbols, subunit, subunit_to_unit,
    // symbol_first, decimal_mark, thousands_separator, smallest_denomination
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

    // European currencies outside the euro area
    add_currency({"PLN", "985", 11, "Polish Złoty", "zł", {}, "Grosz", 100, false, ",", " ", 1});
    add_currency({"SEK", "752", 12, "Swedish Krona", "kr", {}, "Öre", 100, false, ",", " ", 100});
    add_currency({"DKK", "208", 13, "Danish Krone", "kr.", {}, "Øre", 100, false, ",", ".", 50});
    add_currency({"NOK", "578", 14, "Norwegian Krone", "kr", {}, "Øre", 100, false, ",", " ", 100});
    add_currency({"CZK", "203", 15, "Czech Koruna", "Kč", {}, "Haléř", 100, false, ",", " ", 100});
    add_currency({"HUF", "348", 16, "Hungarian Forint", "Ft", {}, "Fillér", 100, false, ",", " ", 500});
    add_currency({"RON", "946", 17, "Romanian Leu", "lei", {}, "Ban", 100, false, ",", ".", 1});
    add_currency({"BGN", "975", 18, "Bulgarian Lev", "лв.", {}, "Stotinka", 100, false, ",", " ", 1});
    add_currency({"ISK", "352", 19, "Icelandic Króna", "kr.", {}, "", 1, false, ",", ".", 1});
    add_currency({"TRY", "949", 20, "Turkish Lira", "₺", {}, "Kuruş", 100, true, ",", ".", 1});
    add_currency({"RUB", "643", 21, "Russian Ruble", "₽", {}, "Kopeck", 100, false, ",", " ", 1});
    add_currency({"UAH", "980", 22, "Ukrainian Hryvnia", "₴", {}, "Kopiyka", 100, false, ",", " ", 1});

    // Other common currencies
    add_currency({"NZD", "554", 23, "New Zealand Dollar", "$", {"NZ$"}, "Cent", 100, true, ".", ",", 10});
    add_currency({"SGD", "702", 24, "Singapore Dollar", "$", {"S$"}, "Cent", 100, true, ".", ",", 1});
    add_currency({"HKD", "344", 25, "Hong Kong Dollar", "$", {"HK$"}, "Cent", 100, true, ".", ",", 10});
    add_currency({"KRW", "410", 26, "South Korean Won", "₩", {}, "", 1, true, ".", ",", 1});
    add_currency({"MXN", "484", 27, "Mexican Peso", "$", {"MX$"}, "Centavo", 100, true, ".", ",", 5});
    add_currency({"ZAR", "710", 28, "South African Rand", "R", {}, "Cent", 100, true, ".", ",", 10});
    add_currency({"ILS", "376", 29, "Israeli New Shekel", "₪", {}, "Agora", 100, true, ".", ",", 10});
    add_currency({"AED", "784", 30, "United Arab Emirates Dirham", "د.إ", {}, "Fils", 100, true, ".", ",", 25});
    add_currency({"SAR", "682", 31, "Saudi Riyal", "ر.س", {}, "Halala", 100, true, ".", ",", 5});
    add_currency({"THB", "764", 32, "Thai Baht", "฿", {}, "Satang", 100, true, ".", ",", 1});
    add_currency({"IDR", "360", 33, "Indonesian Rupiah", "Rp", {}, "Sen", 100, true, ",", ".", 5000});
    add_currency({"MYR", "458", 34, "Malaysian Ringgit", "RM", {}, "Sen", 100, true, ".", ",", 5});
    add_currency({"PHP", "608", 35, "Philippine Peso", "₱", {}, "Centavo", 100, true, ".", ",", 1});
    add_currency({"TWD", "901", 36, "New Taiwan Dollar", "NT$", {}, "Cent", 100, true, ".", ",", 50});
    add_currency({"KWD", "414", 37, "Kuwaiti Dinar", "د.ك", {}, "Fils", 1000, true, ".", ",", 5});
    add_currency({"BHD", "048", 38, "Bahraini Dinar", "ب.د", {}, "Fils", 1000, true, ".", ",", 5});
}

//! Removes surrounding whitespace from a currency code (' eur ' -> 'eur').
static string TrimCurrencyCode(const string &code) {
    auto trimmed = code;
    StringUtil::Trim(trimmed);
    return trimmed;
}

optional_ptr<const CurrencyInfo> CurrencyRegistry::GetCurrency(const string &iso_code) const {
    auto it = currencies.find(TrimCurrencyCode(iso_code));
    if (it != currencies.end()) {
        return it->second.get();
    }
    return nullptr;
}

const case_insensitive_map_t<unique_ptr<CurrencyInfo>> &CurrencyRegistry::GetAllCurrencies() const {
    return currencies;
}

bool CurrencyRegistry::CurrencyExists(const string &iso_code) const {
    return currencies.find(TrimCurrencyCode(iso_code)) != currencies.end();
}

idx_t CurrencyRegistry::GetCurrencyCount() const {
    return currencies.size();
}

} // namespace anofox
} // namespace duckdb

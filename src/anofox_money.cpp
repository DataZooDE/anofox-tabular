#include "anofox_money.hpp"
#include "anofox_money_currency.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/string_util.hpp"

#include <cmath>
#include <type_traits>

namespace duckdb {
namespace anofox {

//----------------------------------------------------------------------------------------------------------------------
// Version compatibility
//----------------------------------------------------------------------------------------------------------------------

// ScalarFunction::SetFallible() exists only in DuckDB v1.5+. On v1.4.4 the
// "fallible" concept does not exist (scalar functions may always throw), so
// marking a function fallible is a no-op there. Detected at compile time so a
// single source tree builds against both DuckDB versions.
template <typename T, typename = void>
struct HasSetFallible : std::false_type {};
template <typename T>
struct HasSetFallible<T, std::void_t<decltype(std::declval<T &>().SetFallible())>> : std::true_type {};

template <typename T>
static void SetFallibleCompat(T &func) {
    if constexpr (HasSetFallible<T>::value) {
        func.SetFallible();
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Type Helpers
//----------------------------------------------------------------------------------------------------------------------

static LogicalType GetMoneyType() {
    child_list_t<LogicalType> children;
    children.push_back(make_pair("amount", LogicalType::DECIMAL(MONEY_WIDTH, MONEY_SCALE)));
    children.push_back(make_pair("currency", LogicalTypeId::VARCHAR));
    return LogicalType::STRUCT(children);
}

static LogicalType GetMoneyAmountType() {
    return LogicalType::DECIMAL(MONEY_WIDTH, MONEY_SCALE);
}

//----------------------------------------------------------------------------------------------------------------------
// Exact Amount Helpers (scaled DECIMAL(18,3) arithmetic, issue #57)
//----------------------------------------------------------------------------------------------------------------------

//! Converts a DOUBLE amount in major units to the scaled DECIMAL(18,3) representation.
//! Rejects NaN/Inf and rounds half away from zero to MONEY_SCALE fractional digits.
static int64_t ScaledFromDouble(double amount) {
    if (!std::isfinite(amount)) {
        throw InvalidInputException("Money amount must be finite, got: %s", std::to_string(amount));
    }
    double scaled = std::round(amount * static_cast<double>(MONEY_SCALE_FACTOR));
    if (!(scaled >= -static_cast<double>(MONEY_MAX_SCALED) && scaled <= static_cast<double>(MONEY_MAX_SCALED))) {
        throw OutOfRangeException("Money amount %s is out of range for DECIMAL(%d,%d)", std::to_string(amount),
                                  MONEY_WIDTH, MONEY_SCALE);
    }
    return static_cast<int64_t>(scaled);
}

//! Verifies that a scaled value fits the DECIMAL(18,3) width.
static int64_t CheckScaledRange(int64_t scaled, bool in_range, const char *operation) {
    if (!in_range || scaled > MONEY_MAX_SCALED || scaled < -MONEY_MAX_SCALED) {
        throw OutOfRangeException("Money %s result is out of range for DECIMAL(%d,%d)", operation, MONEY_WIDTH,
                                  MONEY_SCALE);
    }
    return scaled;
}

static int64_t CheckedAddScaled(int64_t left, int64_t right) {
    int64_t result = 0;
    bool ok = TryAddOperator::Operation<int64_t, int64_t, int64_t>(left, right, result);
    return CheckScaledRange(result, ok, "addition");
}

static int64_t CheckedSubtractScaled(int64_t left, int64_t right) {
    int64_t result = 0;
    bool ok = TrySubtractOperator::Operation<int64_t, int64_t, int64_t>(left, right, result);
    return CheckScaledRange(result, ok, "subtraction");
}

//! Converts an amount in the smallest currency unit (e.g. cents) to the scaled
//! representation exactly: scaled = cents * (MONEY_SCALE_FACTOR / subunit_to_unit).
static int64_t ScaledFromCents(int64_t cents, const CurrencyInfo &currency) {
    const auto divisor = currency.subunit_to_unit;
    if (divisor <= 0) {
        throw InternalException("Invalid subunit_to_unit %d for currency: %s", divisor, currency.iso_code.c_str());
    }
    if (MONEY_SCALE_FACTOR % divisor != 0) {
        // No registered currency has such a subunit; reject instead of losing precision.
        throw NotImplementedException("Currency %s with subunit_to_unit %d is not representable in DECIMAL(%d,%d)",
                                      currency.iso_code.c_str(), divisor, MONEY_WIDTH, MONEY_SCALE);
    }
    const int64_t factor = MONEY_SCALE_FACTOR / divisor;
    int64_t result = 0;
    bool ok = TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(cents, factor, result);
    if (!ok || result > MONEY_MAX_SCALED || result < -MONEY_MAX_SCALED) {
        throw OutOfRangeException("Money amount of %s %s subunits is out of range for DECIMAL(%d,%d)",
                                  std::to_string(cents), currency.iso_code.c_str(), MONEY_WIDTH, MONEY_SCALE);
    }
    return result;
}

//! Multiplies a scaled amount by a DOUBLE factor with explicit overflow checks,
//! rounding half away from zero to MONEY_SCALE fractional digits.
static int64_t CheckedMultiplyFactor(int64_t scaled, double factor) {
    if (!std::isfinite(factor)) {
        throw InvalidInputException("Money multiply factor must be finite, got: %s", std::to_string(factor));
    }
    long double product = static_cast<long double>(scaled) * static_cast<long double>(factor);
    if (!(product >= -static_cast<long double>(MONEY_MAX_SCALED) &&
          product <= static_cast<long double>(MONEY_MAX_SCALED))) {
        throw OutOfRangeException("Money multiplication result is out of range for DECIMAL(%d,%d)", MONEY_WIDTH,
                                  MONEY_SCALE);
    }
    return static_cast<int64_t>(llroundl(product));
}

//! Looks up a currency in the registry (case-insensitive) and throws for unknown codes.
static const CurrencyInfo &LookupCurrencyOrThrow(const CurrencyRegistry &registry, const std::string &currency_code) {
    auto currency = registry.GetCurrency(currency_code);
    if (!currency) {
        throw InvalidInputException("Invalid currency code: %s", currency_code);
    }
    return *currency;
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money(amount, currency_code) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &amount_vec = args.data[0];
    auto &currency_vec = args.data[1];
    idx_t count = args.size();

    UnifiedVectorFormat amount_data;
    UnifiedVectorFormat currency_data;
    amount_vec.ToUnifiedFormat(count, amount_data);
    currency_vec.ToUnifiedFormat(count, currency_data);

    auto amount_values = reinterpret_cast<double *>(amount_data.data);
    auto currency_values = reinterpret_cast<string_t *>(currency_data.data);
    auto builder = PrepareMoneyResult(result);

    auto &registry = CurrencyRegistry::GetInstance();

    for (idx_t i = 0; i < count; i++) {
        auto amount_idx = amount_data.sel->get_index(i);
        auto currency_idx = currency_data.sel->get_index(i);

        if (!amount_data.validity.RowIsValid(amount_idx) || !currency_data.validity.RowIsValid(currency_idx)) {
            SetMoneyResultNull(result, i);
        } else {
            auto currency_code = currency_values[currency_idx].GetString();
            auto &currency = LookupCurrencyOrThrow(registry, currency_code);
            // Store the canonical ISO code so that downstream comparisons work regardless of input case.
            // The DOUBLE input is validated (finite) and rounded to the exact DECIMAL(18,3) representation.
            SetMoneyResult(builder, i, ScaledFromDouble(amount_values[amount_idx]), currency.iso_code);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_from_cents(cents, currency_code) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyFromCentsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &cents_vec = args.data[0];
    auto &currency_vec = args.data[1];
    idx_t count = args.size();

    UnifiedVectorFormat cents_data;
    UnifiedVectorFormat currency_data;
    cents_vec.ToUnifiedFormat(count, cents_data);
    currency_vec.ToUnifiedFormat(count, currency_data);

    auto cents_values = reinterpret_cast<int64_t *>(cents_data.data);
    auto currency_values = reinterpret_cast<string_t *>(currency_data.data);
    auto builder = PrepareMoneyResult(result);

    auto &registry = CurrencyRegistry::GetInstance();

    for (idx_t i = 0; i < count; i++) {
        auto cents_idx = cents_data.sel->get_index(i);
        auto currency_idx = currency_data.sel->get_index(i);

        if (!cents_data.validity.RowIsValid(cents_idx) || !currency_data.validity.RowIsValid(currency_idx)) {
            SetMoneyResultNull(result, i);
        } else {
            auto currency_code = currency_values[currency_idx].GetString();
            auto &currency = LookupCurrencyOrThrow(registry, currency_code);

            // Convert the smallest-unit amount to major units exactly (e.g. 10050 cents -> 100.500 USD)
            SetMoneyResult(builder, i, ScaledFromCents(cents_values[cents_idx], currency), currency.iso_code);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_amount(money) -> DECIMAL(18,3)
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyAmountFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    idx_t count = args.size();

    auto data = ExtractMoneyStruct(money_vec, count);
    auto result_data = FlatVector::GetData<int64_t>(result);

    for (idx_t i = 0; i < count; i++) {
        if (!data.RowIsValid(i)) {
            FlatVector::SetNull(result, i, true);
        } else {
            result_data[i] = data.Amount(i);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_currency(money) -> VARCHAR
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyCurrencyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    idx_t count = args.size();

    auto data = ExtractMoneyStruct(money_vec, count);
    auto result_data = FlatVector::GetData<string_t>(result);

    for (idx_t i = 0; i < count; i++) {
        if (!data.RowIsValid(i)) {
            FlatVector::SetNull(result, i, true);
        } else {
            result_data[i] = StringVector::AddString(result, data.Currency(i));
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_is_valid_currency(code) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxIsValidCurrencyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &registry = CurrencyRegistry::GetInstance();
    IterateCurrencyCode(args, result, [&](const std::string& code, idx_t i) {
        auto result_data = FlatVector::GetData<bool>(result);
        result_data[i] = registry.CurrencyExists(code);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_currency_symbol(code) -> VARCHAR
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxCurrencySymbolFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &registry = CurrencyRegistry::GetInstance();
    IterateCurrencyCode(args, result, [&](const std::string& code, idx_t i) {
        auto &currency = LookupCurrencyOrThrow(registry, code);
        auto result_data = FlatVector::GetData<string_t>(result);
        result_data[i] = StringVector::AddString(result, currency.symbol);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_format(money, format_style) -> VARCHAR
//----------------------------------------------------------------------------------------------------------------------

//! Number of fractional digits implied by a currency's subunit_to_unit
//! (100 -> 2, 1 -> 0, 1000 -> 3, 5 -> 1), capped at the storage scale.
static int CurrencyDecimalDigits(const CurrencyInfo &currency) {
    int digits = 0;
    int64_t power = 1;
    while (power < currency.subunit_to_unit && digits < MONEY_SCALE) {
        power *= 10;
        digits++;
    }
    return digits;
}

//! Renders a scaled amount with the currency's scale. When `localized` is set,
//! the currency's decimal_mark and thousands_separator are applied; otherwise
//! the output is canonical (dot decimal mark, no grouping). Shared across the
//! 'symbol', 'long' and 'code' styles so validation and rounding are uniform.
static std::string FormatMoneyAmount(int64_t scaled, const CurrencyInfo &currency, bool localized) {
    const int decimals = CurrencyDecimalDigits(currency);

    // Round half away from zero to the currency scale
    int64_t drop = 1;
    for (int d = decimals; d < MONEY_SCALE; d++) {
        drop *= 10;
    }
    const int64_t half = drop / 2;
    int64_t rounded = scaled >= 0 ? (scaled + half) / drop : -((-scaled + half) / drop);

    int64_t frac_divisor = 1;
    for (int d = 0; d < decimals; d++) {
        frac_divisor *= 10;
    }

    const bool negative = rounded < 0;
    const uint64_t abs_value = negative ? static_cast<uint64_t>(-rounded) : static_cast<uint64_t>(rounded);
    const uint64_t int_part = abs_value / static_cast<uint64_t>(frac_divisor);
    const uint64_t frac_part = abs_value % static_cast<uint64_t>(frac_divisor);

    // Integer digits with optional grouping
    std::string digits = std::to_string(int_part);
    const std::string &separator = currency.thousands_separator;
    if (localized && !separator.empty()) {
        std::string grouped;
        const idx_t length = digits.size();
        for (idx_t pos = 0; pos < length; pos++) {
            if (pos > 0 && (length - pos) % 3 == 0) {
                grouped += separator;
            }
            grouped += digits[pos];
        }
        digits = std::move(grouped);
    }

    std::string formatted = negative ? "-" : "";
    formatted += digits;
    if (decimals > 0) {
        const std::string &mark = (localized && !currency.decimal_mark.empty()) ? currency.decimal_mark : ".";
        std::string frac_digits = std::to_string(frac_part);
        formatted += mark + std::string(static_cast<idx_t>(decimals) - frac_digits.size(), '0') + frac_digits;
    }
    return formatted;
}

static void AnofoxMoneyFormatFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    auto &style_vec = args.data[1];
    idx_t count = args.size();

    UnifiedVectorFormat style_data;
    style_vec.ToUnifiedFormat(count, style_data);
    auto style_values = reinterpret_cast<string_t *>(style_data.data);

    auto data = ExtractMoneyStruct(money_vec, count);

    auto &registry = CurrencyRegistry::GetInstance();
    auto result_data = FlatVector::GetData<string_t>(result);

    for (idx_t i = 0; i < count; i++) {
        auto style_idx = style_data.sel->get_index(i);

        if (!data.RowIsValid(i) || !style_data.validity.RowIsValid(style_idx)) {
            FlatVector::SetNull(result, i, true);
        } else {
            auto scaled = data.Amount(i);
            auto currency_code = data.Currency(i).GetString();
            auto format_style = style_values[style_idx].GetString();

            auto &currency = LookupCurrencyOrThrow(registry, currency_code);

            // Format based on style; the amount rendering is shared (scale and
            // rounding from currency metadata, issue #57)
            std::string formatted;
            if (format_style == "symbol") {
                // Localized: $1,234.56 or 1.234,56 € depending on symbol_first
                auto amount_str = FormatMoneyAmount(scaled, currency, true);
                if (currency.symbol_first) {
                    formatted = currency.symbol + amount_str;
                } else {
                    formatted = amount_str + " " + currency.symbol;
                }
            } else if (format_style == "long") {
                // Localized amount with the full currency name: 1.234,56 Euro
                formatted = FormatMoneyAmount(scaled, currency, true) + " " + currency.name;
            } else {
                // 'code' and default: canonical machine-readable amount with the ISO code
                formatted = FormatMoneyAmount(scaled, currency, false) + " " + currency.iso_code;
            }

            result_data[i] = StringVector::AddString(result, formatted);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_is_positive(money) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyIsPositiveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateMoneyComparison(args, result, [](int64_t scaled) {
        return scaled > 0;
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_is_negative(money) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyIsNegativeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateMoneyComparison(args, result, [](int64_t scaled) {
        return scaled < 0;
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_is_zero(money) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyIsZeroFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateMoneyComparison(args, result, [](int64_t scaled) {
        return scaled == 0;
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_abs(money) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyAbsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    idx_t count = args.size();

    auto data = ExtractMoneyStruct(money_vec, count);
    auto builder = PrepareMoneyResult(result);

    for (idx_t i = 0; i < count; i++) {
        if (!data.RowIsValid(i)) {
            SetMoneyResultNull(result, i);
        } else {
            // The scaled amount is range-checked at construction, so negation cannot overflow
            auto scaled = data.Amount(i);
            SetMoneyResult(builder, i, scaled < 0 ? -scaled : scaled, data.Currency(i));
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_add(money1, money2) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyAddFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateBinaryMoneyOp(args, result, true, [](MoneyResultBuilder& builder, idx_t i,
                                                int64_t scaled1, int64_t scaled2,
                                                string_t currency) {
        SetMoneyResult(builder, i, CheckedAddScaled(scaled1, scaled2), currency);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_subtract(money1, money2) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneySubtractFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateBinaryMoneyOp(args, result, true, [](MoneyResultBuilder& builder, idx_t i,
                                                int64_t scaled1, int64_t scaled2,
                                                string_t currency) {
        SetMoneyResult(builder, i, CheckedSubtractScaled(scaled1, scaled2), currency);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_multiply(money, factor) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyMultiplyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    auto &factor_vec = args.data[1];
    idx_t count = args.size();

    auto data = ExtractMoneyStruct(money_vec, count);
    auto builder = PrepareMoneyResult(result);

    UnifiedVectorFormat factor_data;
    factor_vec.ToUnifiedFormat(count, factor_data);
    auto factor_values = reinterpret_cast<double *>(factor_data.data);

    for (idx_t i = 0; i < count; i++) {
        auto factor_idx = factor_data.sel->get_index(i);

        if (!data.RowIsValid(i) || !factor_data.validity.RowIsValid(factor_idx)) {
            SetMoneyResultNull(result, i);
        } else {
            SetMoneyResult(builder, i, CheckedMultiplyFactor(data.Amount(i), factor_values[factor_idx]),
                           data.Currency(i));
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_in_range(money, min_amount, max_amount) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyInRangeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    auto &min_vec = args.data[1];
    auto &max_vec = args.data[2];
    idx_t count = args.size();

    auto data = ExtractMoneyStruct(money_vec, count);

    UnifiedVectorFormat min_data, max_data;
    min_vec.ToUnifiedFormat(count, min_data);
    max_vec.ToUnifiedFormat(count, max_data);

    auto min_values = reinterpret_cast<double *>(min_data.data);
    auto max_values = reinterpret_cast<double *>(max_data.data);
    auto result_data = FlatVector::GetData<bool>(result);

    for (idx_t i = 0; i < count; i++) {
        auto min_idx = min_data.sel->get_index(i);
        auto max_idx = max_data.sel->get_index(i);

        if (!data.RowIsValid(i) || !min_data.validity.RowIsValid(min_idx) ||
            !max_data.validity.RowIsValid(max_idx)) {
            FlatVector::SetNull(result, i, true);
        } else {
            double amount = static_cast<double>(data.Amount(i)) / static_cast<double>(MONEY_SCALE_FACTOR);
            double min_val = min_values[min_idx];
            double max_val = max_values[max_idx];
            result_data[i] = (amount >= min_val && amount <= max_val);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_same_currency(money1, money2) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneySameCurrencyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money1_vec = args.data[0];
    auto &money2_vec = args.data[1];
    idx_t count = args.size();

    auto data1 = ExtractMoneyStruct(money1_vec, count);
    auto data2 = ExtractMoneyStruct(money2_vec, count);
    auto result_data = FlatVector::GetData<bool>(result);

    for (idx_t i = 0; i < count; i++) {
        if (!data1.RowIsValid(i) || !data2.RowIsValid(i)) {
            FlatVector::SetNull(result, i, true);
        } else {
            // Canonical codes are stored at construction; compare case-insensitively so that
            // manually constructed structs with mixed-case codes behave consistently
            result_data[i] = CurrencyCIEquals(data1.Currency(i), data2.Currency(i));
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_currency_name(code) -> VARCHAR
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxCurrencyNameFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &registry = CurrencyRegistry::GetInstance();
    IterateCurrencyCode(args, result, [&](const std::string& code, idx_t i) {
        auto &currency = LookupCurrencyOrThrow(registry, code);
        auto result_data = FlatVector::GetData<string_t>(result);
        result_data[i] = StringVector::AddString(result, currency.name);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Registration Functions
//----------------------------------------------------------------------------------------------------------------------

// Telemetry bind functions for scalar functions
unique_ptr<FunctionData> MoneyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money");
    return nullptr;
}

unique_ptr<FunctionData> MoneyFromCentsBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_from_cents");
    return nullptr;
}

unique_ptr<FunctionData> MoneyAmountBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_amount");
    return nullptr;
}

unique_ptr<FunctionData> MoneyCurrencyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_currency");
    return nullptr;
}

unique_ptr<FunctionData> IsValidCurrencyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("is_valid_currency");
    return nullptr;
}

unique_ptr<FunctionData> CurrencySymbolBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("currency_symbol");
    return nullptr;
}

unique_ptr<FunctionData> CurrencyNameBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("currency_name");
    return nullptr;
}

unique_ptr<FunctionData> MoneyFormatBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_format");
    return nullptr;
}

unique_ptr<FunctionData> MoneyIsPositiveBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_is_positive");
    return nullptr;
}

unique_ptr<FunctionData> MoneyIsNegativeBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_is_negative");
    return nullptr;
}

unique_ptr<FunctionData> MoneyIsZeroBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_is_zero");
    return nullptr;
}

unique_ptr<FunctionData> MoneyAbsBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_abs");
    return nullptr;
}

unique_ptr<FunctionData> MoneyAddBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_add");
    return nullptr;
}

unique_ptr<FunctionData> MoneySubtractBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_subtract");
    return nullptr;
}

unique_ptr<FunctionData> MoneyMultiplyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_multiply");
    return nullptr;
}

unique_ptr<FunctionData> MoneyInRangeBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_in_range");
    return nullptr;
}

unique_ptr<FunctionData> MoneySameCurrencyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().RecordFunctionCall("money_same_currency");
    return nullptr;
}

void RegisterMoneyOptions(ExtensionLoader &loader) {
    // Configuration options would go here
}

void RegisterMoneyFunctions(ExtensionLoader &loader) {
    {
        FunctionDescription desc;
        desc.description = "Creates a money value from a decimal amount and ISO 4217 currency code.";
        desc.parameter_names = {"amount", "currency_code"};
        desc.parameter_types = {LogicalType::DOUBLE, LogicalType::VARCHAR};
        desc.examples = {"SELECT money(19.99, 'USD');"};
        desc.categories = {"money"};
        ScalarFunction money_func("anofox_tab_money", {LogicalTypeId::DOUBLE, LogicalTypeId::VARCHAR}, GetMoneyType(), AnofoxMoneyFunction);
        money_func.bind = MoneyBind;
        SetFallibleCompat(money_func);
        RegisterScalarFunctionWithAlias(loader, money_func, "money", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Creates a money value from an integer amount in the smallest currency unit (e.g. cents) and ISO 4217 currency code.";
        desc.parameter_names = {"cents", "currency_code"};
        desc.parameter_types = {LogicalType::BIGINT, LogicalType::VARCHAR};
        desc.examples = {"SELECT money_from_cents(1999, 'USD');"};
        desc.categories = {"money"};
        ScalarFunction money_from_cents_func("anofox_tab_money_from_cents", {LogicalTypeId::BIGINT, LogicalTypeId::VARCHAR}, GetMoneyType(), AnofoxMoneyFromCentsFunction);
        money_from_cents_func.bind = MoneyFromCentsBind;
        SetFallibleCompat(money_from_cents_func);
        RegisterScalarFunctionWithAlias(loader, money_from_cents_func, "money_from_cents", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Extracts the exact numeric amount (DECIMAL(18,3)) from a money value.";
        desc.parameter_names = {"money"};
        desc.parameter_types = {GetMoneyType()};
        desc.examples = {"SELECT money_amount(money(19.99, 'USD'));"};
        desc.categories = {"money"};
        ScalarFunction money_amount_func("anofox_tab_money_amount", {GetMoneyType()}, GetMoneyAmountType(), AnofoxMoneyAmountFunction);
        money_amount_func.bind = MoneyAmountBind;
        SetFallibleCompat(money_amount_func);
        RegisterScalarFunctionWithAlias(loader, money_amount_func, "money_amount", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Extracts the ISO 4217 currency code from a money value.";
        desc.parameter_names = {"money"};
        desc.parameter_types = {GetMoneyType()};
        desc.examples = {"SELECT money_currency(money(19.99, 'USD'));"};
        desc.categories = {"money"};
        ScalarFunction money_currency_func("anofox_tab_money_currency", {GetMoneyType()}, LogicalTypeId::VARCHAR, AnofoxMoneyCurrencyFunction);
        money_currency_func.bind = MoneyCurrencyBind;
        SetFallibleCompat(money_currency_func);
        RegisterScalarFunctionWithAlias(loader, money_currency_func, "money_currency", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the given string is a valid ISO 4217 currency code.";
        desc.parameter_names = {"currency_code"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT is_valid_currency('USD');"};
        desc.categories = {"money", "validation"};
        ScalarFunction is_valid_currency_func("anofox_tab_is_valid_currency", {LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN, AnofoxIsValidCurrencyFunction);
        is_valid_currency_func.bind = IsValidCurrencyBind;
        RegisterScalarFunctionWithAlias(loader, is_valid_currency_func, "is_valid_currency", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns the currency symbol for a given ISO 4217 currency code (e.g., '$' for USD).";
        desc.parameter_names = {"currency_code"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT currency_symbol('EUR');"};
        desc.categories = {"money"};
        ScalarFunction currency_symbol_func("anofox_tab_currency_symbol", {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, AnofoxCurrencySymbolFunction);
        currency_symbol_func.bind = CurrencySymbolBind;
        SetFallibleCompat(currency_symbol_func);
        RegisterScalarFunctionWithAlias(loader, currency_symbol_func, "currency_symbol", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns the full English name of a currency given its ISO 4217 code (e.g., 'US Dollar').";
        desc.parameter_names = {"currency_code"};
        desc.parameter_types = {LogicalType::VARCHAR};
        desc.examples = {"SELECT currency_name('EUR');"};
        desc.categories = {"money"};
        ScalarFunction currency_name_func("anofox_tab_currency_name", {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, AnofoxCurrencyNameFunction);
        currency_name_func.bind = CurrencyNameBind;
        SetFallibleCompat(currency_name_func);
        RegisterScalarFunctionWithAlias(loader, currency_name_func, "currency_name", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Formats a money value as a string using the specified style ('symbol', 'code', or 'plain').";
        desc.parameter_names = {"money", "format_style"};
        desc.parameter_types = {GetMoneyType(), LogicalType::VARCHAR};
        desc.examples = {"SELECT money_format(money(19.99, 'USD'), 'symbol');"};
        desc.categories = {"money", "formatting"};
        ScalarFunction money_format_func("anofox_tab_money_format", {GetMoneyType(), LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, AnofoxMoneyFormatFunction);
        money_format_func.bind = MoneyFormatBind;
        SetFallibleCompat(money_format_func);
        RegisterScalarFunctionWithAlias(loader, money_format_func, "money_format", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the money amount is greater than zero.";
        desc.parameter_names = {"money"};
        desc.parameter_types = {GetMoneyType()};
        desc.examples = {"SELECT money_is_positive(money(19.99, 'USD'));"};
        desc.categories = {"money", "validation"};
        ScalarFunction money_is_positive_func("anofox_tab_money_is_positive", {GetMoneyType()}, LogicalTypeId::BOOLEAN, AnofoxMoneyIsPositiveFunction);
        money_is_positive_func.bind = MoneyIsPositiveBind;
        SetFallibleCompat(money_is_positive_func);
        RegisterScalarFunctionWithAlias(loader, money_is_positive_func, "money_is_positive", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the money amount is less than zero.";
        desc.parameter_names = {"money"};
        desc.parameter_types = {GetMoneyType()};
        desc.examples = {"SELECT money_is_negative(money(-5.00, 'USD'));"};
        desc.categories = {"money", "validation"};
        ScalarFunction money_is_negative_func("anofox_tab_money_is_negative", {GetMoneyType()}, LogicalTypeId::BOOLEAN, AnofoxMoneyIsNegativeFunction);
        money_is_negative_func.bind = MoneyIsNegativeBind;
        SetFallibleCompat(money_is_negative_func);
        RegisterScalarFunctionWithAlias(loader, money_is_negative_func, "money_is_negative", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the money amount is exactly zero.";
        desc.parameter_names = {"money"};
        desc.parameter_types = {GetMoneyType()};
        desc.examples = {"SELECT money_is_zero(money(0.00, 'USD'));"};
        desc.categories = {"money", "validation"};
        ScalarFunction money_is_zero_func("anofox_tab_money_is_zero", {GetMoneyType()}, LogicalTypeId::BOOLEAN, AnofoxMoneyIsZeroFunction);
        money_is_zero_func.bind = MoneyIsZeroBind;
        SetFallibleCompat(money_is_zero_func);
        RegisterScalarFunctionWithAlias(loader, money_is_zero_func, "money_is_zero", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns the absolute value of a money amount.";
        desc.parameter_names = {"money"};
        desc.parameter_types = {GetMoneyType()};
        desc.examples = {"SELECT money_abs(money(-19.99, 'USD'));"};
        desc.categories = {"money"};
        ScalarFunction money_abs_func("anofox_tab_money_abs", {GetMoneyType()}, GetMoneyType(), AnofoxMoneyAbsFunction);
        money_abs_func.bind = MoneyAbsBind;
        SetFallibleCompat(money_abs_func);
        RegisterScalarFunctionWithAlias(loader, money_abs_func, "money_abs", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Adds two money values of the same currency.";
        desc.parameter_names = {"money1", "money2"};
        desc.parameter_types = {GetMoneyType(), GetMoneyType()};
        desc.examples = {"SELECT money_add(money(10.00, 'USD'), money(5.00, 'USD'));"};
        desc.categories = {"money", "arithmetic"};
        ScalarFunction money_add_func("anofox_tab_money_add", {GetMoneyType(), GetMoneyType()}, GetMoneyType(), AnofoxMoneyAddFunction);
        money_add_func.bind = MoneyAddBind;
        SetFallibleCompat(money_add_func);
        RegisterScalarFunctionWithAlias(loader, money_add_func, "money_add", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Subtracts the second money value from the first (must be the same currency).";
        desc.parameter_names = {"money1", "money2"};
        desc.parameter_types = {GetMoneyType(), GetMoneyType()};
        desc.examples = {"SELECT money_subtract(money(10.00, 'USD'), money(3.00, 'USD'));"};
        desc.categories = {"money", "arithmetic"};
        ScalarFunction money_subtract_func("anofox_tab_money_subtract", {GetMoneyType(), GetMoneyType()}, GetMoneyType(), AnofoxMoneySubtractFunction);
        money_subtract_func.bind = MoneySubtractBind;
        SetFallibleCompat(money_subtract_func);
        RegisterScalarFunctionWithAlias(loader, money_subtract_func, "money_subtract", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Multiplies a money amount by a scalar factor.";
        desc.parameter_names = {"money", "factor"};
        desc.parameter_types = {GetMoneyType(), LogicalType::DOUBLE};
        desc.examples = {"SELECT money_multiply(money(10.00, 'USD'), 1.5);"};
        desc.categories = {"money", "arithmetic"};
        ScalarFunction money_multiply_func("anofox_tab_money_multiply", {GetMoneyType(), LogicalTypeId::DOUBLE}, GetMoneyType(), AnofoxMoneyMultiplyFunction);
        money_multiply_func.bind = MoneyMultiplyBind;
        SetFallibleCompat(money_multiply_func);
        RegisterScalarFunctionWithAlias(loader, money_multiply_func, "money_multiply", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if the money amount is within the inclusive range [min, max].";
        desc.parameter_names = {"money", "min", "max"};
        desc.parameter_types = {GetMoneyType(), LogicalType::DOUBLE, LogicalType::DOUBLE};
        desc.examples = {"SELECT money_in_range(money(15.00, 'USD'), 10.0, 20.0);"};
        desc.categories = {"money", "validation"};
        ScalarFunction money_in_range_func("anofox_tab_money_in_range", {GetMoneyType(), LogicalTypeId::DOUBLE, LogicalTypeId::DOUBLE}, LogicalTypeId::BOOLEAN, AnofoxMoneyInRangeFunction);
        money_in_range_func.bind = MoneyInRangeBind;
        SetFallibleCompat(money_in_range_func);
        RegisterScalarFunctionWithAlias(loader, money_in_range_func, "money_in_range", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Returns TRUE if two money values have the same currency code.";
        desc.parameter_names = {"money1", "money2"};
        desc.parameter_types = {GetMoneyType(), GetMoneyType()};
        desc.examples = {"SELECT money_same_currency(money(10.00, 'USD'), money(5.00, 'USD'));"};
        desc.categories = {"money", "comparison"};
        ScalarFunction money_same_currency_func("anofox_tab_money_same_currency", {GetMoneyType(), GetMoneyType()}, LogicalTypeId::BOOLEAN, AnofoxMoneySameCurrencyFunction);
        money_same_currency_func.bind = MoneySameCurrencyBind;
        SetFallibleCompat(money_same_currency_func);
        RegisterScalarFunctionWithAlias(loader, money_same_currency_func, "money_same_currency", {std::move(desc)});
    }

    AnofoxTrace(AnofoxLogLevel::Info, "Money module functions registered");
}

} // namespace anofox
} // namespace duckdb

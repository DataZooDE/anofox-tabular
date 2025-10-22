#include "anofox_money.hpp"
#include "anofox_money_currency.hpp"
#include "anofox_trace.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace anofox {

//----------------------------------------------------------------------------------------------------------------------
// Type Helpers
//----------------------------------------------------------------------------------------------------------------------

static LogicalType GetMoneyType() {
    child_list_t<LogicalType> children;
    children.push_back(make_pair("amount", LogicalTypeId::DOUBLE));
    children.push_back(make_pair("currency", LogicalTypeId::VARCHAR));
    return LogicalType::STRUCT(children);
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
            builder.amount_validity.SetInvalid(i);
            builder.currency_validity.SetInvalid(i);
        } else {
            auto currency_code = currency_values[currency_idx].GetString();

            // Validate currency code
            if (!registry.CurrencyExists(currency_code)) {
                throw InvalidInputException("Invalid currency code: %s", currency_code);
            }

            SetMoneyResult(builder, i, amount_values[amount_idx], currency_code, result);
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
            builder.amount_validity.SetInvalid(i);
            builder.currency_validity.SetInvalid(i);
        } else {
            auto currency_code = currency_values[currency_idx].GetString();

            // Validate currency code
            if (!registry.CurrencyExists(currency_code)) {
                throw InvalidInputException("Invalid currency code: %s", currency_code);
            }

            auto currency = registry.GetCurrency(currency_code);
            if (!currency) {
                throw InvalidInputException("Currency not found: %s", currency_code);
            }

            // Store amount as cents converted to decimal representation
            // For now, just store the cents value as double
            double amount = static_cast<double>(cents_values[cents_idx]);
            SetMoneyResult(builder, i, amount, currency_code, result);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_amount(money) -> DECIMAL
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyAmountFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    auto &children = StructVector::GetEntries(money_vec);

    // Extract amount field (first child)
    if (children.size() < 2) {
        throw InvalidInputException("Money struct must have amount and currency fields");
    }

    result.Reference(*children[0]);
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_currency(money) -> VARCHAR
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyCurrencyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    auto &children = StructVector::GetEntries(money_vec);

    // Extract currency field (second child)
    if (children.size() < 2) {
        throw InvalidInputException("Money struct must have amount and currency fields");
    }

    result.Reference(*children[1]);
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
        auto currency = registry.GetCurrency(code);
        if (!currency) {
            throw InvalidInputException("Currency not found: %s", code.c_str());
        }
        auto result_data = FlatVector::GetData<string_t>(result);
        result_data[i] = StringVector::AddString(result, currency->symbol);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_format(money, format_style) -> VARCHAR
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyFormatFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    auto &style_vec = args.data[1];
    idx_t count = args.size();

    UnifiedVectorFormat style_data;
    style_vec.ToUnifiedFormat(count, style_data);
    auto style_values = reinterpret_cast<string_t *>(style_data.data);

    // Get money struct entries
    auto &children = StructVector::GetEntries(money_vec);
    if (children.size() != 2) {
        throw InvalidInputException("Money struct must have exactly 2 fields");
    }

    auto &amount_vec = *children[0];
    auto &currency_vec = *children[1];

    UnifiedVectorFormat amount_data;
    UnifiedVectorFormat currency_data;
    amount_vec.ToUnifiedFormat(count, amount_data);
    currency_vec.ToUnifiedFormat(count, currency_data);

    auto amount_values = reinterpret_cast<double *>(amount_data.data);
    auto currency_values = reinterpret_cast<string_t *>(currency_data.data);

    auto &registry = CurrencyRegistry::GetInstance();
    auto result_data = FlatVector::GetData<string_t>(result);

    for (idx_t i = 0; i < count; i++) {
        auto amount_idx = amount_data.sel->get_index(i);
        auto currency_idx = currency_data.sel->get_index(i);
        auto style_idx = style_data.sel->get_index(i);

        if (!amount_data.validity.RowIsValid(amount_idx) || !currency_data.validity.RowIsValid(currency_idx) ||
            !style_data.validity.RowIsValid(style_idx)) {
            FlatVector::SetNull(result, i, true);
        } else {
            double amount = amount_values[amount_idx];
            auto currency_code = currency_values[currency_idx].GetString();
            auto format_style = style_values[style_idx].GetString();

            auto currency = registry.GetCurrency(currency_code);
            if (!currency) {
                throw InvalidInputException("Currency not found: %s", currency_code);
            }

            // Format based on style
            std::string formatted;
            if (format_style == "symbol") {
                // Format: $100.50 or 100,50 € depending on symbol_first
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "%.2f", amount);
                std::string amount_str(buffer);

                // Replace decimal mark if needed
                if (currency->decimal_mark != ".") {
                    size_t dot_pos = amount_str.find('.');
                    if (dot_pos != std::string::npos) {
                        amount_str[dot_pos] = currency->decimal_mark[0];
                    }
                }

                if (currency->symbol_first) {
                    formatted = currency->symbol + amount_str;
                } else {
                    formatted = amount_str + " " + currency->symbol;
                }
            } else if (format_style == "code") {
                // Format: 100.50 USD
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "%.2f %s", amount, currency_code.c_str());
                formatted = buffer;
            } else if (format_style == "long") {
                // Format: 100.50 United States Dollar
                char buffer[512];
                snprintf(buffer, sizeof(buffer), "%.2f %s", amount, currency->name.c_str());
                formatted = buffer;
            } else {
                // Default: ISO format with code
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "%.2f %s", amount, currency_code.c_str());
                formatted = buffer;
            }

            result_data[i] = StringVector::AddString(result, formatted);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_is_positive(money) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyIsPositiveFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateMoneyComparison(args, result, [](double amount) {
        return amount > 0.0;
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_is_negative(money) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyIsNegativeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateMoneyComparison(args, result, [](double amount) {
        return amount < 0.0;
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_is_zero(money) -> BOOLEAN
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyIsZeroFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateMoneyComparison(args, result, [](double amount) {
        return amount == 0.0;
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_abs(money) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyAbsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &money_vec = args.data[0];
    auto &children = StructVector::GetEntries(money_vec);

    if (children.size() != 2) {
        throw InvalidInputException("Money struct must have exactly 2 fields");
    }

    idx_t count = args.size();
    auto &amount_vec = *children[0];
    auto &currency_vec = *children[1];

    UnifiedVectorFormat amount_data;
    UnifiedVectorFormat currency_data;
    amount_vec.ToUnifiedFormat(count, amount_data);
    currency_vec.ToUnifiedFormat(count, currency_data);

    auto amount_values = reinterpret_cast<double *>(amount_data.data);
    auto currency_values = reinterpret_cast<string_t *>(currency_data.data);

    // Get result struct entries
    auto &result_children = StructVector::GetEntries(result);
    if (result_children.size() != 2) {
        throw InvalidInputException("Money struct must have exactly 2 fields");
    }

    auto &result_amount = *result_children[0];
    auto &result_currency = *result_children[1];

    result_amount.SetVectorType(VectorType::FLAT_VECTOR);
    result_currency.SetVectorType(VectorType::FLAT_VECTOR);

    auto result_amount_ptr = FlatVector::GetData<double>(result_amount);
    auto result_currency_ptr = FlatVector::GetData<string_t>(result_currency);
    auto &result_amount_validity = FlatVector::Validity(result_amount);
    auto &result_currency_validity = FlatVector::Validity(result_currency);

    for (idx_t i = 0; i < count; i++) {
        auto amount_idx = amount_data.sel->get_index(i);
        auto currency_idx = currency_data.sel->get_index(i);

        if (!amount_data.validity.RowIsValid(amount_idx) || !currency_data.validity.RowIsValid(currency_idx)) {
            result_amount_validity.SetInvalid(i);
            result_currency_validity.SetInvalid(i);
        } else {
            double amount = amount_values[amount_idx];
            auto currency_code = currency_values[currency_idx].GetString();

            result_amount_ptr[i] = std::fabs(amount);
            result_currency_ptr[i] = StringVector::AddString(result_currency, currency_code);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_add(money1, money2) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneyAddFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateBinaryMoneyOp(args, result, true, [](MoneyResultBuilder& builder, idx_t i,
                                                double amount1, double amount2,
                                                const std::string& currency, Vector& result) {
        builder.amount_ptr[i] = amount1 + amount2;
        builder.currency_ptr[i] = StringVector::AddString(
            *StructVector::GetEntries(result)[1], currency);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_money_subtract(money1, money2) -> STRUCT
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxMoneySubtractFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    IterateBinaryMoneyOp(args, result, true, [](MoneyResultBuilder& builder, idx_t i,
                                                double amount1, double amount2,
                                                const std::string& currency, Vector& result) {
        builder.amount_ptr[i] = amount1 - amount2;
        builder.currency_ptr[i] = StringVector::AddString(
            *StructVector::GetEntries(result)[1], currency);
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
        auto amount_idx = data.amount_data.sel->get_index(i);
        auto currency_idx = data.currency_data.sel->get_index(i);
        auto factor_idx = factor_data.sel->get_index(i);

        if (!data.amount_data.validity.RowIsValid(amount_idx) ||
            !data.currency_data.validity.RowIsValid(currency_idx) ||
            !factor_data.validity.RowIsValid(factor_idx)) {
            builder.amount_validity.SetInvalid(i);
            builder.currency_validity.SetInvalid(i);
        } else {
            double amount = data.amount_values[amount_idx];
            auto currency = data.currency_values[currency_idx].GetString();
            double factor = factor_values[factor_idx];

            builder.amount_ptr[i] = amount * factor;
            auto& currency_vec = *StructVector::GetEntries(result)[1];
            builder.currency_ptr[i] = StringVector::AddString(currency_vec, currency);
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

    auto &children = StructVector::GetEntries(money_vec);
    if (children.size() != 2) {
        throw InvalidInputException("Money struct must have exactly 2 fields");
    }

    auto &amount_vec = *children[0];

    UnifiedVectorFormat amount_data, min_data, max_data;
    amount_vec.ToUnifiedFormat(count, amount_data);
    min_vec.ToUnifiedFormat(count, min_data);
    max_vec.ToUnifiedFormat(count, max_data);

    auto amount_values = reinterpret_cast<double *>(amount_data.data);
    auto min_values = reinterpret_cast<double *>(min_data.data);
    auto max_values = reinterpret_cast<double *>(max_data.data);
    auto result_data = FlatVector::GetData<bool>(result);

    for (idx_t i = 0; i < count; i++) {
        auto amount_idx = amount_data.sel->get_index(i);
        auto min_idx = min_data.sel->get_index(i);
        auto max_idx = max_data.sel->get_index(i);

        if (!amount_data.validity.RowIsValid(amount_idx) || !min_data.validity.RowIsValid(min_idx) ||
            !max_data.validity.RowIsValid(max_idx)) {
            FlatVector::SetNull(result, i, true);
        } else {
            double amount = amount_values[amount_idx];
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

    auto &children1 = StructVector::GetEntries(money1_vec);
    auto &children2 = StructVector::GetEntries(money2_vec);

    if (children1.size() != 2 || children2.size() != 2) {
        throw InvalidInputException("Money struct must have exactly 2 fields");
    }

    auto &currency1_vec = *children1[1];
    auto &currency2_vec = *children2[1];

    UnifiedVectorFormat currency1_data, currency2_data;
    currency1_vec.ToUnifiedFormat(count, currency1_data);
    currency2_vec.ToUnifiedFormat(count, currency2_data);

    auto currency1_values = reinterpret_cast<string_t *>(currency1_data.data);
    auto currency2_values = reinterpret_cast<string_t *>(currency2_data.data);
    auto result_data = FlatVector::GetData<bool>(result);

    for (idx_t i = 0; i < count; i++) {
        auto currency1_idx = currency1_data.sel->get_index(i);
        auto currency2_idx = currency2_data.sel->get_index(i);

        if (!currency1_data.validity.RowIsValid(currency1_idx) || !currency2_data.validity.RowIsValid(currency2_idx)) {
            FlatVector::SetNull(result, i, true);
        } else {
            auto c1 = currency1_values[currency1_idx].GetString();
            auto c2 = currency2_values[currency2_idx].GetString();
            result_data[i] = (c1 == c2);
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar Function: anofox_currency_name(code) -> VARCHAR
//----------------------------------------------------------------------------------------------------------------------

static void AnofoxCurrencyNameFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &registry = CurrencyRegistry::GetInstance();
    IterateCurrencyCode(args, result, [&](const std::string& code, idx_t i) {
        auto currency = registry.GetCurrency(code);
        if (!currency) {
            throw InvalidInputException("Currency not found: %s", code.c_str());
        }
        auto result_data = FlatVector::GetData<string_t>(result);
        result_data[i] = StringVector::AddString(result, currency->name);
    });
}

//----------------------------------------------------------------------------------------------------------------------
// Registration Functions
//----------------------------------------------------------------------------------------------------------------------

void RegisterMoneyOptions(ExtensionLoader &loader) {
    // Configuration options would go here
}

void RegisterMoneyFunctions(ExtensionLoader &loader) {
    // Scalar function: anofox_money(amount, currency_code) -> STRUCT
    ScalarFunction money_func("anofox_money", {LogicalTypeId::DOUBLE, LogicalTypeId::VARCHAR}, GetMoneyType(),
                              AnofoxMoneyFunction);
    loader.RegisterFunction(money_func);

    // Scalar function: anofox_money_from_cents(cents, currency_code) -> STRUCT
    ScalarFunction money_from_cents_func("anofox_money_from_cents", {LogicalTypeId::BIGINT, LogicalTypeId::VARCHAR},
                                         GetMoneyType(), AnofoxMoneyFromCentsFunction);
    loader.RegisterFunction(money_from_cents_func);

    // Scalar function: anofox_money_amount(money) -> DOUBLE
    ScalarFunction money_amount_func("anofox_money_amount", {GetMoneyType()},
                                     LogicalTypeId::DOUBLE, AnofoxMoneyAmountFunction);
    loader.RegisterFunction(money_amount_func);

    // Scalar function: anofox_money_currency(money) -> VARCHAR
    ScalarFunction money_currency_func("anofox_money_currency", {GetMoneyType()}, LogicalTypeId::VARCHAR,
                                       AnofoxMoneyCurrencyFunction);
    loader.RegisterFunction(money_currency_func);

    // Scalar function: anofox_is_valid_currency(code) -> BOOLEAN
    ScalarFunction is_valid_currency_func("anofox_is_valid_currency", {LogicalTypeId::VARCHAR},
                                          LogicalTypeId::BOOLEAN, AnofoxIsValidCurrencyFunction);
    loader.RegisterFunction(is_valid_currency_func);

    // Scalar function: anofox_currency_symbol(code) -> VARCHAR
    ScalarFunction currency_symbol_func("anofox_currency_symbol", {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR,
                                        AnofoxCurrencySymbolFunction);
    loader.RegisterFunction(currency_symbol_func);

    // Scalar function: anofox_currency_name(code) -> VARCHAR
    ScalarFunction currency_name_func("anofox_currency_name", {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR,
                                      AnofoxCurrencyNameFunction);
    loader.RegisterFunction(currency_name_func);

    // Scalar function: anofox_money_format(money, format_style) -> VARCHAR
    ScalarFunction money_format_func("anofox_money_format", {GetMoneyType(), LogicalTypeId::VARCHAR},
                                     LogicalTypeId::VARCHAR, AnofoxMoneyFormatFunction);
    loader.RegisterFunction(money_format_func);

    // Scalar function: anofox_money_is_positive(money) -> BOOLEAN
    ScalarFunction money_is_positive_func("anofox_money_is_positive", {GetMoneyType()},
                                          LogicalTypeId::BOOLEAN, AnofoxMoneyIsPositiveFunction);
    loader.RegisterFunction(money_is_positive_func);

    // Scalar function: anofox_money_is_negative(money) -> BOOLEAN
    ScalarFunction money_is_negative_func("anofox_money_is_negative", {GetMoneyType()},
                                          LogicalTypeId::BOOLEAN, AnofoxMoneyIsNegativeFunction);
    loader.RegisterFunction(money_is_negative_func);

    // Scalar function: anofox_money_is_zero(money) -> BOOLEAN
    ScalarFunction money_is_zero_func("anofox_money_is_zero", {GetMoneyType()},
                                      LogicalTypeId::BOOLEAN, AnofoxMoneyIsZeroFunction);
    loader.RegisterFunction(money_is_zero_func);

    // Scalar function: anofox_money_abs(money) -> STRUCT
    ScalarFunction money_abs_func("anofox_money_abs", {GetMoneyType()}, GetMoneyType(),
                                  AnofoxMoneyAbsFunction);
    loader.RegisterFunction(money_abs_func);

    // Scalar function: anofox_money_add(money1, money2) -> STRUCT
    ScalarFunction money_add_func("anofox_money_add", {GetMoneyType(), GetMoneyType()},
                                  GetMoneyType(), AnofoxMoneyAddFunction);
    loader.RegisterFunction(money_add_func);

    // Scalar function: anofox_money_subtract(money1, money2) -> STRUCT
    ScalarFunction money_subtract_func("anofox_money_subtract", {GetMoneyType(), GetMoneyType()},
                                       GetMoneyType(), AnofoxMoneySubtractFunction);
    loader.RegisterFunction(money_subtract_func);

    // Scalar function: anofox_money_multiply(money, factor) -> STRUCT
    ScalarFunction money_multiply_func("anofox_money_multiply", {GetMoneyType(), LogicalTypeId::DOUBLE},
                                       GetMoneyType(), AnofoxMoneyMultiplyFunction);
    loader.RegisterFunction(money_multiply_func);

    // Scalar function: anofox_money_in_range(money, min, max) -> BOOLEAN
    ScalarFunction money_in_range_func("anofox_money_in_range",
                                       {GetMoneyType(), LogicalTypeId::DOUBLE, LogicalTypeId::DOUBLE},
                                       LogicalTypeId::BOOLEAN, AnofoxMoneyInRangeFunction);
    loader.RegisterFunction(money_in_range_func);

    // Scalar function: anofox_money_same_currency(money1, money2) -> BOOLEAN
    ScalarFunction money_same_currency_func("anofox_money_same_currency", {GetMoneyType(), GetMoneyType()},
                                            LogicalTypeId::BOOLEAN, AnofoxMoneySameCurrencyFunction);
    loader.RegisterFunction(money_same_currency_func);

    AnofoxTrace(AnofoxLogLevel::Info, "[anofox] Money module functions registered");
}

} // namespace anofox
} // namespace duckdb

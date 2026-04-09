#include "anofox_money.hpp"
#include "anofox_money_currency.hpp"
#include "anofox_function_alias.hpp"
#include "anofox_trace.hpp"
#include "telemetry.hpp"
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

// Telemetry bind functions for scalar functions
unique_ptr<FunctionData> MoneyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money");
    return nullptr;
}

unique_ptr<FunctionData> MoneyFromCentsBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_from_cents");
    return nullptr;
}

unique_ptr<FunctionData> MoneyAmountBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_amount");
    return nullptr;
}

unique_ptr<FunctionData> MoneyCurrencyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_currency");
    return nullptr;
}

unique_ptr<FunctionData> IsValidCurrencyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("is_valid_currency");
    return nullptr;
}

unique_ptr<FunctionData> CurrencySymbolBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("currency_symbol");
    return nullptr;
}

unique_ptr<FunctionData> CurrencyNameBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("currency_name");
    return nullptr;
}

unique_ptr<FunctionData> MoneyFormatBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_format");
    return nullptr;
}

unique_ptr<FunctionData> MoneyIsPositiveBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_is_positive");
    return nullptr;
}

unique_ptr<FunctionData> MoneyIsNegativeBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_is_negative");
    return nullptr;
}

unique_ptr<FunctionData> MoneyIsZeroBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_is_zero");
    return nullptr;
}

unique_ptr<FunctionData> MoneyAbsBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_abs");
    return nullptr;
}

unique_ptr<FunctionData> MoneyAddBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_add");
    return nullptr;
}

unique_ptr<FunctionData> MoneySubtractBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_subtract");
    return nullptr;
}

unique_ptr<FunctionData> MoneyMultiplyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_multiply");
    return nullptr;
}

unique_ptr<FunctionData> MoneyInRangeBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_in_range");
    return nullptr;
}

unique_ptr<FunctionData> MoneySameCurrencyBind(ClientContext &, ScalarFunction &, vector<unique_ptr<Expression>> &) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("money_same_currency");
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
        RegisterScalarFunctionWithAlias(loader, money_func, "money", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Creates a money value from an integer amount in cents and ISO 4217 currency code.";
        desc.parameter_names = {"cents", "currency_code"};
        desc.parameter_types = {LogicalType::BIGINT, LogicalType::VARCHAR};
        desc.examples = {"SELECT money_from_cents(1999, 'USD');"};
        desc.categories = {"money"};
        ScalarFunction money_from_cents_func("anofox_tab_money_from_cents", {LogicalTypeId::BIGINT, LogicalTypeId::VARCHAR}, GetMoneyType(), AnofoxMoneyFromCentsFunction);
        money_from_cents_func.bind = MoneyFromCentsBind;
        RegisterScalarFunctionWithAlias(loader, money_from_cents_func, "money_from_cents", {std::move(desc)});
    }
    {
        FunctionDescription desc;
        desc.description = "Extracts the numeric amount from a money value.";
        desc.parameter_names = {"money"};
        desc.parameter_types = {GetMoneyType()};
        desc.examples = {"SELECT money_amount(money(19.99, 'USD'));"};
        desc.categories = {"money"};
        ScalarFunction money_amount_func("anofox_tab_money_amount", {GetMoneyType()}, LogicalTypeId::DOUBLE, AnofoxMoneyAmountFunction);
        money_amount_func.bind = MoneyAmountBind;
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
        RegisterScalarFunctionWithAlias(loader, money_same_currency_func, "money_same_currency", {std::move(desc)});
    }

    AnofoxTrace(AnofoxLogLevel::Info, "[anofox] Money module functions registered");
}

} // namespace anofox
} // namespace duckdb

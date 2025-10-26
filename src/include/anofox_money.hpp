#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {
namespace anofox {

// ============================================================================
// Helper: Money Struct Data Accessor
// ============================================================================
struct MoneyStructData {
  Vector& amount_vec;
  Vector& currency_vec;
  UnifiedVectorFormat amount_data;
  UnifiedVectorFormat currency_data;
  double* amount_values;
  string_t* currency_values;
};

inline MoneyStructData ExtractMoneyStruct(Vector& money_vec, idx_t count) {
  auto& children = StructVector::GetEntries(money_vec);
  if (children.size() != 2) {
    throw InvalidInputException("Money struct must have exactly 2 fields");
  }

  MoneyStructData data{
    *children[0],
    *children[1],
  };

  data.amount_vec.ToUnifiedFormat(count, data.amount_data);
  data.currency_vec.ToUnifiedFormat(count, data.currency_data);
  data.amount_values = reinterpret_cast<double*>(data.amount_data.data);
  data.currency_values = reinterpret_cast<string_t*>(data.currency_data.data);

  return data;
}

// ============================================================================
// Helper: Money Result Builder
// ============================================================================
struct MoneyResultBuilder {
  double* amount_ptr;
  string_t* currency_ptr;
  ValidityMask& amount_validity;
  ValidityMask& currency_validity;
};

inline MoneyResultBuilder PrepareMoneyResult(Vector& result) {
  auto& children = StructVector::GetEntries(result);
  if (children.size() != 2) {
    throw InvalidInputException("Money struct must have exactly 2 fields");
  }

  auto& amount_vec = *children[0];
  auto& currency_vec = *children[1];

  amount_vec.SetVectorType(VectorType::FLAT_VECTOR);
  currency_vec.SetVectorType(VectorType::FLAT_VECTOR);

  return MoneyResultBuilder{
    FlatVector::GetData<double>(amount_vec),
    FlatVector::GetData<string_t>(currency_vec),
    FlatVector::Validity(amount_vec),
    FlatVector::Validity(currency_vec),
  };
}

inline void SetMoneyResult(MoneyResultBuilder& builder, idx_t i,
                          double amount, const std::string& currency,
                          Vector& result) {
  builder.amount_ptr[i] = amount;
  auto& children = StructVector::GetEntries(result);
  builder.currency_ptr[i] = StringVector::AddString(*children[1], currency);
}

// ============================================================================
// Currency Input Iterator Template
// ============================================================================
template <typename Op>
inline void IterateCurrencyCode(DataChunk& args, Vector& result, Op op) {
  auto& code_vec = args.data[0];
  idx_t count = args.size();

  UnifiedVectorFormat code_data;
  code_vec.ToUnifiedFormat(count, code_data);
  auto code_values = reinterpret_cast<string_t*>(code_data.data);

  for (idx_t i = 0; i < count; i++) {
    auto idx = code_data.sel->get_index(i);
    if (!code_data.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
    } else {
      auto currency_code = code_values[idx].GetString();
      op(currency_code, i);
    }
  }
}

// ============================================================================
// Money Comparison Iterator Template
// ============================================================================
template <typename Predicate>
inline void IterateMoneyComparison(DataChunk& args, Vector& result,
                                   Predicate pred) {
  auto& money_vec = args.data[0];
  idx_t count = args.size();

  auto data = ExtractMoneyStruct(money_vec, count);
  auto result_data = FlatVector::GetData<bool>(result);

  for (idx_t i = 0; i < count; i++) {
    auto amount_idx = data.amount_data.sel->get_index(i);
    if (!data.amount_data.validity.RowIsValid(amount_idx)) {
      FlatVector::SetNull(result, i, true);
    } else {
      result_data[i] = pred(data.amount_values[amount_idx]);
    }
  }
}

// ============================================================================
// Binary Money Operation Iterator Template
// ============================================================================
template <typename Op>
inline void IterateBinaryMoneyOp(DataChunk& args, Vector& result,
                                 bool check_same_currency, Op op) {
  auto& money1_vec = args.data[0];
  auto& money2_vec = args.data[1];
  idx_t count = args.size();

  auto data1 = ExtractMoneyStruct(money1_vec, count);
  auto data2 = ExtractMoneyStruct(money2_vec, count);
  auto builder = PrepareMoneyResult(result);

  for (idx_t i = 0; i < count; i++) {
    auto amount1_idx = data1.amount_data.sel->get_index(i);
    auto currency1_idx = data1.currency_data.sel->get_index(i);
    auto amount2_idx = data2.amount_data.sel->get_index(i);
    auto currency2_idx = data2.currency_data.sel->get_index(i);

    if (!data1.amount_data.validity.RowIsValid(amount1_idx) ||
        !data1.currency_data.validity.RowIsValid(currency1_idx) ||
        !data2.amount_data.validity.RowIsValid(amount2_idx) ||
        !data2.currency_data.validity.RowIsValid(currency2_idx)) {
      builder.amount_validity.SetInvalid(i);
      builder.currency_validity.SetInvalid(i);
    } else {
      auto currency1 = data1.currency_values[currency1_idx].GetString();
      auto currency2 = data2.currency_values[currency2_idx].GetString();

      if (check_same_currency && currency1 != currency2) {
        throw InvalidInputException(
            "Cannot operate on money with different currencies: %s and %s",
            currency1.c_str(), currency2.c_str());
      }

      double amount1 = data1.amount_values[amount1_idx];
      double amount2 = data2.amount_values[amount2_idx];

      op(builder, i, amount1, amount2, currency1, result);
    }
  }
}

// Registration functions
void RegisterMoneyOptions(ExtensionLoader &loader);
void RegisterMoneyFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

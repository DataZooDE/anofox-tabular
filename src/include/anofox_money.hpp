#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace anofox {

// ============================================================================
// Helper: Money Struct Data Accessor
// ============================================================================
struct MoneyStructData {
  Vector& amount_vec;
  Vector& currency_vec;
  UnifiedVectorFormat struct_data;
  UnifiedVectorFormat amount_data;
  UnifiedVectorFormat currency_data;
  double* amount_values;
  string_t* currency_values;

  //! A money row is only usable if the parent struct and both children are non-NULL.
  //! A struct with NULL children is treated as a NULL money value (issue #43).
  //! The children belong to the row space of the (possibly dictionary-wrapped)
  //! struct vector, so they are indexed through the parent selection first.
  bool RowIsValid(idx_t i) const {
    auto row = struct_data.sel->get_index(i);
    return struct_data.validity.RowIsValid(row) &&
           amount_data.validity.RowIsValid(amount_data.sel->get_index(row)) &&
           currency_data.validity.RowIsValid(currency_data.sel->get_index(row));
  }

  double Amount(idx_t i) const {
    auto row = struct_data.sel->get_index(i);
    return amount_values[amount_data.sel->get_index(row)];
  }

  std::string Currency(idx_t i) const {
    auto row = struct_data.sel->get_index(i);
    return currency_values[currency_data.sel->get_index(row)].GetString();
  }
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

  money_vec.ToUnifiedFormat(count, data.struct_data);
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

//! Marks a money result row as SQL NULL. FlatVector::SetNull on a STRUCT vector
//! sets the parent validity and recursively invalidates the children, so the
//! row satisfies the invariant `f(NULL) IS NULL`.
inline void SetMoneyResultNull(Vector& result, idx_t i) {
  FlatVector::SetNull(result, i, true);
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
    if (!data.RowIsValid(i)) {
      FlatVector::SetNull(result, i, true);
    } else {
      result_data[i] = pred(data.Amount(i));
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
    if (!data1.RowIsValid(i) || !data2.RowIsValid(i)) {
      SetMoneyResultNull(result, i);
    } else {
      auto currency1 = data1.Currency(i);
      auto currency2 = data2.Currency(i);

      // Codes are canonicalized at construction; compare case-insensitively so
      // manually constructed structs with mixed-case codes still work.
      if (check_same_currency && !StringUtil::CIEquals(currency1, currency2)) {
        throw InvalidInputException(
            "Cannot operate on money with different currencies: %s and %s",
            currency1.c_str(), currency2.c_str());
      }

      op(builder, i, data1.Amount(i), data2.Amount(i), currency1, result);
    }
  }
}

// Registration functions
void RegisterMoneyOptions(ExtensionLoader &loader);
void RegisterMoneyFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

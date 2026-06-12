#pragma once

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace anofox {

// ============================================================================
// Money representation (issue #57)
//
// Money amounts are stored as DECIMAL(18,3): exact decimal semantics, three
// fractional digits (covers 2-decimal currencies plus 3-decimal currencies
// such as BHD/KWD/TND). DuckDB stores DECIMAL(18,3) physically as an int64
// scaled by 10^3, so all C++ arithmetic below is checked integer math on the
// scaled representation. NaN/Inf are unrepresentable by construction and
// overflow raises a clear error instead of wrapping or producing Inf.
// ============================================================================

//! Number of fractional digits of the money DECIMAL type.
static constexpr uint8_t MONEY_SCALE = 3;
//! Total precision (width) of the money DECIMAL type.
static constexpr uint8_t MONEY_WIDTH = 18;
//! Scaling factor between major units and the stored representation (10^MONEY_SCALE).
static constexpr int64_t MONEY_SCALE_FACTOR = 1000;
//! Largest scaled value representable in DECIMAL(18,3): 18 nines.
static constexpr int64_t MONEY_MAX_SCALED = 999999999999999999LL;

// ============================================================================
// Helper: Money Struct Data Accessor
// ============================================================================
struct MoneyStructData {
  Vector& amount_vec;
  Vector& currency_vec;
  UnifiedVectorFormat struct_data;
  UnifiedVectorFormat amount_data;
  UnifiedVectorFormat currency_data;
  int64_t* amount_values;
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

  //! Returns the amount in the scaled DECIMAL(18,3) representation (milli-units).
  int64_t Amount(idx_t i) const {
    auto row = struct_data.sel->get_index(i);
    return amount_values[amount_data.sel->get_index(row)];
  }

  //! Returns the currency code as a view into the input vector (no copy).
  string_t Currency(idx_t i) const {
    auto row = struct_data.sel->get_index(i);
    return currency_values[currency_data.sel->get_index(row)];
  }
};

//! Case-insensitive comparison of two currency codes without allocating.
inline bool CurrencyCIEquals(string_t left, string_t right) {
  return StringUtil::CIEquals(left.GetData(), left.GetSize(), right.GetData(), right.GetSize());
}

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
  data.amount_values = reinterpret_cast<int64_t*>(data.amount_data.data);
  data.currency_values = reinterpret_cast<string_t*>(data.currency_data.data);

  return data;
}

// ============================================================================
// Helper: Money Result Builder
// ============================================================================
//! Holds the child-data pointers and the currency child vector of a money
//! result struct, hoisted out of the row loops (no per-row GetEntries).
struct MoneyResultBuilder {
  int64_t* amount_ptr;
  string_t* currency_ptr;
  Vector& currency_vec;
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
    FlatVector::GetData<int64_t>(amount_vec),
    FlatVector::GetData<string_t>(currency_vec),
    currency_vec,
  };
}

inline void SetMoneyResult(MoneyResultBuilder& builder, idx_t i,
                          int64_t scaled_amount, const std::string& currency) {
  builder.amount_ptr[i] = scaled_amount;
  builder.currency_ptr[i] = StringVector::AddString(builder.currency_vec, currency);
}

inline void SetMoneyResult(MoneyResultBuilder& builder, idx_t i,
                          int64_t scaled_amount, string_t currency) {
  builder.amount_ptr[i] = scaled_amount;
  builder.currency_ptr[i] = StringVector::AddString(builder.currency_vec, currency);
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
      if (check_same_currency && !CurrencyCIEquals(currency1, currency2)) {
        throw InvalidInputException(
            "Cannot operate on money with different currencies: %s and %s",
            currency1.GetString(), currency2.GetString());
      }

      op(builder, i, data1.Amount(i), data2.Amount(i), currency1);
    }
  }
}

// Registration functions
void RegisterMoneyOptions(ExtensionLoader &loader);
void RegisterMoneyFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

#pragma once

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

// ============================================================================
// Generic Single-Input String Iterator
// ============================================================================
// Handles all boilerplate: vector format conversion, validity checking, loop,
// and result verification. The operation lambda/function receives each valid
// input string and the output index.
//
// Usage:
//   IterateSingleStringInput(args, result, [&](const std::string& input, size_t idx) {
//       auto value = registry.DoSomething(input);
//       SetBoolResult(result, idx, value);
//   });
template <typename Operation>
inline void IterateSingleStringInput(DataChunk& args, Vector& result,
                                     Operation op) {
  auto& input_vec = args.data[0];
  size_t count = args.size();

  UnifiedVectorFormat idata;
  input_vec.ToUnifiedFormat(count, idata);
  auto input_strings = (string_t*)idata.data;

  for (size_t i = 0; i < count; i++) {
    size_t idx = idata.sel->get_index(i);
    if (!idata.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    std::string input = input_strings[idx].GetString();
    op(input, i);
  }

  result.Verify(count);
}

// ============================================================================
// Generic Two-Input String Iterator (for functions with 2 string inputs)
// ============================================================================
template <typename Operation>
inline void IterateTwoStringInputs(DataChunk& args, Vector& result,
                                   Operation op) {
  auto& input1_vec = args.data[0];
  auto& input2_vec = args.data[1];
  size_t count = args.size();

  UnifiedVectorFormat idata1, idata2;
  input1_vec.ToUnifiedFormat(count, idata1);
  input2_vec.ToUnifiedFormat(count, idata2);
  auto input1_strings = (string_t*)idata1.data;
  auto input2_strings = (string_t*)idata2.data;

  for (size_t i = 0; i < count; i++) {
    size_t idx1 = idata1.sel->get_index(i);
    size_t idx2 = idata2.sel->get_index(i);

    if (!idata1.validity.RowIsValid(idx1) ||
        !idata2.validity.RowIsValid(idx2)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    std::string input1 = input1_strings[idx1].GetString();
    std::string input2 = input2_strings[idx2].GetString();
    op(input1, input2, i);
  }

  result.Verify(count);
}

// ============================================================================
// Output Result Setters
// ============================================================================

// Set a boolean result value
inline void SetBoolResult(Vector& result, size_t idx, bool value) {
  auto result_data = FlatVector::GetData<bool>(result);
  result_data[idx] = value;
}

// Set a string result value
inline void SetStringResult(Vector& result, size_t idx,
                           const std::string& value) {
  auto result_strings = FlatVector::GetData<string_t>(result);
  result_strings[idx] = StringVector::AddString(result, value);
}

// Set struct result (country, digits)
inline void SetVATStructResult(Vector& result, size_t idx,
                               const std::string& country,
                               const std::string& digits) {
  auto& struct_entries = StructVector::GetEntries(result);
  auto& country_vec = *struct_entries[0];
  auto& digits_vec = *struct_entries[1];
  auto country_strings = FlatVector::GetData<string_t>(country_vec);
  auto digits_strings = FlatVector::GetData<string_t>(digits_vec);

  country_strings[idx] = StringVector::AddString(country_vec, country);
  digits_strings[idx] = StringVector::AddString(digits_vec, digits);
}

}  // namespace duckdb

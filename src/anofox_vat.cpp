#include "anofox_vat.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

// ============================================================================
// Helper: Get VAT struct type
// ============================================================================
static LogicalType GetVATType() {
  child_list_t<LogicalType> struct_children;
  struct_children.push_back(make_pair("country", LogicalType::VARCHAR));
  struct_children.push_back(make_pair("digits", LogicalType::VARCHAR));
  return LogicalType::STRUCT(struct_children);
}

// ============================================================================
// Helper: Create VAT struct value
// ============================================================================
static Value CreateVATValue(const std::string& country,
                           const std::string& digits) {
  child_list_t<Value> children;
  children.push_back(make_pair("country", Value(country)));
  children.push_back(make_pair("digits", Value(digits)));
  return Value::STRUCT(children);
}

// ============================================================================
// Phase 2: Basic VAT Operations (5 functions)
// ============================================================================

// anofox_vat(number) -> STRUCT(country, digits)
static void VATParseFunc(DataChunk& args, ExpressionState& state,
                        Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();

  UnifiedVectorFormat idata;
  input_vec.ToUnifiedFormat(count, idata);
  auto input_strings = (string_t*)idata.data;

  auto& struct_entries = StructVector::GetEntries(result);
  auto& country_vec = *struct_entries[0];
  auto& digits_vec = *struct_entries[1];
  auto country_strings = FlatVector::GetData<string_t>(country_vec);
  auto digits_strings = FlatVector::GetData<string_t>(digits_vec);

  for (size_t i = 0; i < count; i++) {
    size_t idx = idata.sel->get_index(i);
    if (!idata.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    std::string input = input_strings[idx].GetString();
    auto split_result = registry.SplitVAT(input);

    if (!split_result.has_value()) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    auto [country, digits] = split_result.value();
    country_strings[i] = StringVector::AddString(country_vec, country);
    digits_strings[i] = StringVector::AddString(digits_vec, digits);
  }

  result.Verify(count);
}

// anofox_is_valid_vat_country(code) -> BOOLEAN
static void IsValidVATCountryFunc(DataChunk& args, ExpressionState& state,
                                 Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();
  auto result_data = FlatVector::GetData<bool>(result);

  UnifiedVectorFormat idata;
  input_vec.ToUnifiedFormat(count, idata);
  auto input_strings = (string_t*)idata.data;

  for (size_t i = 0; i < count; i++) {
    size_t idx = idata.sel->get_index(i);
    if (!idata.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    std::string country = input_strings[idx].GetString();
    result_data[i] = registry.IsValidCountry(country);
  }

  result.Verify(count);
}

// anofox_vat_normalize(number) -> VARCHAR
static void VATNormalizeFunc(DataChunk& args, ExpressionState& state,
                            Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();
  auto result_strings = FlatVector::GetData<string_t>(result);

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
    std::string normalized = registry.NormalizeVAT(input);
    result_strings[i] = StringVector::AddString(result, normalized);
  }

  result.Verify(count);
}

// ============================================================================
// Phase 3: Syntax Validation (3 functions)
// ============================================================================

// anofox_vat_is_valid_syntax(vat) -> BOOLEAN
static void VATIsValidSyntaxFunc(DataChunk& args, ExpressionState& state,
                                Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();
  auto result_data = FlatVector::GetData<bool>(result);

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
    auto split_result = registry.SplitVAT(input);

    if (!split_result.has_value()) {
      result_data[i] = false;
      continue;
    }

    auto [country, digits] = split_result.value();
    result_data[i] = registry.IsValidSyntax(country, digits);
  }

  result.Verify(count);
}

// anofox_vat_split(number) -> STRUCT(country, digits)
static void VATSplitFunc(DataChunk& args, ExpressionState& state,
                        Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();

  UnifiedVectorFormat idata;
  input_vec.ToUnifiedFormat(count, idata);
  auto input_strings = (string_t*)idata.data;

  auto& struct_entries = StructVector::GetEntries(result);
  auto& country_vec = *struct_entries[0];
  auto& digits_vec = *struct_entries[1];
  auto country_strings = FlatVector::GetData<string_t>(country_vec);
  auto digits_strings = FlatVector::GetData<string_t>(digits_vec);

  for (size_t i = 0; i < count; i++) {
    size_t idx = idata.sel->get_index(i);
    if (!idata.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    std::string input = input_strings[idx].GetString();
    auto split_result = registry.SplitVAT(input);

    if (!split_result.has_value()) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    auto [country, digits] = split_result.value();
    country_strings[i] = StringVector::AddString(country_vec, country);
    digits_strings[i] = StringVector::AddString(digits_vec, digits);
  }

  result.Verify(count);
}

// anofox_vat_exists(number) -> BOOLEAN
static void VATExistsFunc(DataChunk& args, ExpressionState& state,
                         Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();
  auto result_data = FlatVector::GetData<bool>(result);

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
    auto split_result = registry.SplitVAT(input);
    result_data[i] = split_result.has_value();
  }

  result.Verify(count);
}

// ============================================================================
// Phase 5: EU Utilities (3 functions)
// ============================================================================

// anofox_vat_is_eu_member(country_code) -> BOOLEAN
static void VATIsEUMemberFunc(DataChunk& args, ExpressionState& state,
                             Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();
  auto result_data = FlatVector::GetData<bool>(result);

  UnifiedVectorFormat idata;
  input_vec.ToUnifiedFormat(count, idata);
  auto input_strings = (string_t*)idata.data;

  for (size_t i = 0; i < count; i++) {
    size_t idx = idata.sel->get_index(i);
    if (!idata.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    std::string country = input_strings[idx].GetString();
    result_data[i] = registry.IsEUMember(country);
  }

  result.Verify(count);
}

// anofox_vat_country_name(country_code) -> VARCHAR
static void VATCountryNameFunc(DataChunk& args, ExpressionState& state,
                              Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();
  auto result_strings = FlatVector::GetData<string_t>(result);

  UnifiedVectorFormat idata;
  input_vec.ToUnifiedFormat(count, idata);
  auto input_strings = (string_t*)idata.data;

  for (size_t i = 0; i < count; i++) {
    size_t idx = idata.sel->get_index(i);
    if (!idata.validity.RowIsValid(idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    std::string country = input_strings[idx].GetString();
    std::string name = registry.GetCountryName(country);

    if (name.empty()) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    result_strings[i] = StringVector::AddString(result, name);
  }

  result.Verify(count);
}

// anofox_vat_format(vat, style) -> VARCHAR
static void VATFormatFunc(DataChunk& args, ExpressionState& state,
                         Vector& result) {
  // Style: 'plain' = digits only, 'iso' = with country prefix
  auto& vat_vec = args.data[0];
  auto& style_vec = args.data[1];
  size_t count = args.size();
  auto result_strings = FlatVector::GetData<string_t>(result);

  UnifiedVectorFormat vat_data, style_data;
  vat_vec.ToUnifiedFormat(count, vat_data);
  style_vec.ToUnifiedFormat(count, style_data);

  for (size_t i = 0; i < count; i++) {
    size_t vat_idx = vat_data.sel->get_index(i);
    size_t style_idx = style_data.sel->get_index(i);

    if (!vat_data.validity.RowIsValid(vat_idx) ||
        !style_data.validity.RowIsValid(style_idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }

    // For now, simplified implementation
    FlatVector::SetNull(result, i, true);
  }

  result.Verify(count);
}

// ============================================================================
// Phase 6: Combined Validation (1 function)
// ============================================================================

// anofox_vat_is_valid(vat) -> BOOLEAN
static void VATIsValidFunc(DataChunk& args, ExpressionState& state,
                          Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& input_vec = args.data[0];
  size_t count = args.size();
  auto result_data = FlatVector::GetData<bool>(result);

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
    auto split_result = registry.SplitVAT(input);

    if (!split_result.has_value()) {
      result_data[i] = false;
      continue;
    }

    auto [country, digits] = split_result.value();
    result_data[i] = registry.IsValidSyntax(country, digits);
  }

  result.Verify(count);
}

// ============================================================================
// Extension registration
// ============================================================================

void RegisterVATOptions(ExtensionLoader& loader) {
  // No specific options needed for VAT module for now
}

void RegisterVATFunctions(ExtensionLoader& loader) {
  // Phase 2: Basic VAT Operations
  ScalarFunction vat_func("anofox_vat", {LogicalTypeId::VARCHAR}, GetVATType(),
                         VATParseFunc);
  loader.RegisterFunction(vat_func);

  ScalarFunction is_valid_country_func("anofox_is_valid_vat_country",
                                       {LogicalTypeId::VARCHAR},
                                       LogicalTypeId::BOOLEAN,
                                       IsValidVATCountryFunc);
  loader.RegisterFunction(is_valid_country_func);

  ScalarFunction normalize_func("anofox_vat_normalize",
                               {LogicalTypeId::VARCHAR},
                               LogicalTypeId::VARCHAR, VATNormalizeFunc);
  loader.RegisterFunction(normalize_func);

  // Phase 3: Syntax Validation
  ScalarFunction is_valid_syntax_func("anofox_vat_is_valid_syntax",
                                      {LogicalTypeId::VARCHAR},
                                      LogicalTypeId::BOOLEAN,
                                      VATIsValidSyntaxFunc);
  loader.RegisterFunction(is_valid_syntax_func);

  ScalarFunction split_func("anofox_vat_split", {LogicalTypeId::VARCHAR},
                           GetVATType(), VATSplitFunc);
  loader.RegisterFunction(split_func);

  ScalarFunction exists_func("anofox_vat_exists", {LogicalTypeId::VARCHAR},
                            LogicalTypeId::BOOLEAN, VATExistsFunc);
  loader.RegisterFunction(exists_func);

  // Phase 5: EU Utilities
  ScalarFunction is_eu_member_func("anofox_vat_is_eu_member",
                                   {LogicalTypeId::VARCHAR},
                                   LogicalTypeId::BOOLEAN, VATIsEUMemberFunc);
  loader.RegisterFunction(is_eu_member_func);

  ScalarFunction country_name_func("anofox_vat_country_name",
                                   {LogicalTypeId::VARCHAR},
                                   LogicalTypeId::VARCHAR, VATCountryNameFunc);
  loader.RegisterFunction(country_name_func);

  ScalarFunction format_func("anofox_vat_format",
                            {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
                            LogicalTypeId::VARCHAR, VATFormatFunc);
  loader.RegisterFunction(format_func);

  // Phase 6: Combined Validation
  ScalarFunction is_valid_func("anofox_vat_is_valid", {LogicalTypeId::VARCHAR},
                              LogicalTypeId::BOOLEAN, VATIsValidFunc);
  loader.RegisterFunction(is_valid_func);
}

}  // namespace duckdb

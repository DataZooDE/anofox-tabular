#include "anofox_vat.hpp"
#include "anofox_function_alias.hpp"
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
  struct_children.push_back(make_pair("country", LogicalTypeId::VARCHAR));
  struct_children.push_back(make_pair("digits", LogicalTypeId::VARCHAR));
  return LogicalType::STRUCT(struct_children);
}

// ============================================================================
// Phase 2: Basic VAT Operations (5 functions)
// ============================================================================

// anofox_vat(number) -> STRUCT(country, digits)
static void VATParseFunc(DataChunk& args, ExpressionState& state,
                        Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& input, size_t idx) {
    auto split_result = registry.SplitVAT(input);
    if (!split_result.has_value()) {
      FlatVector::SetNull(result, idx, true);
      return;
    }
    auto [country, digits] = split_result.value();
    SetVATStructResult(result, idx, country, digits);
  });
}

// anofox_is_valid_vat_country(code) -> BOOLEAN
static void IsValidVATCountryFunc(DataChunk& args, ExpressionState& state,
                                 Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& country, size_t idx) {
    SetBoolResult(result, idx, registry.IsValidCountry(country));
  });
}

// anofox_vat_normalize(number) -> VARCHAR
static void VATNormalizeFunc(DataChunk& args, ExpressionState& state,
                            Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& input, size_t idx) {
    SetStringResult(result, idx, registry.NormalizeVAT(input));
  });
}

// ============================================================================
// Phase 3: Syntax Validation (3 functions)
// ============================================================================

// anofox_vat_is_valid_syntax(vat) -> BOOLEAN
static void VATIsValidSyntaxFunc(DataChunk& args, ExpressionState& state,
                                Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& input, size_t idx) {
    auto split_result = registry.SplitVAT(input);
    if (!split_result.has_value()) {
      SetBoolResult(result, idx, false);
      return;
    }
    auto [country, digits] = split_result.value();
    SetBoolResult(result, idx, registry.IsValidSyntax(country, digits));
  });
}

// anofox_vat_split(number) -> STRUCT(country, digits)
static void VATSplitFunc(DataChunk& args, ExpressionState& state,
                        Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& input, size_t idx) {
    auto split_result = registry.SplitVAT(input);
    if (!split_result.has_value()) {
      FlatVector::SetNull(result, idx, true);
      return;
    }
    auto [country, digits] = split_result.value();
    SetVATStructResult(result, idx, country, digits);
  });
}

// anofox_vat_exists(number) -> BOOLEAN
static void VATExistsFunc(DataChunk& args, ExpressionState& state,
                         Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& input, size_t idx) {
    SetBoolResult(result, idx, registry.SplitVAT(input).has_value());
  });
}

// ============================================================================
// Phase 5: EU Utilities (3 functions)
// ============================================================================

// anofox_vat_is_eu_member(country_code) -> BOOLEAN
static void VATIsEUMemberFunc(DataChunk& args, ExpressionState& state,
                             Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& country, size_t idx) {
    SetBoolResult(result, idx, registry.IsEUMember(country));
  });
}

// anofox_vat_country_name(country_code) -> VARCHAR
static void VATCountryNameFunc(DataChunk& args, ExpressionState& state,
                              Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](const std::string& country, size_t idx) {
    std::string name = registry.GetCountryName(country);
    if (name.empty()) {
      FlatVector::SetNull(result, idx, true);
      return;
    }
    SetStringResult(result, idx, name);
  });
}

// anofox_vat_format(vat, style) -> VARCHAR
static void VATFormatFunc(DataChunk& args, ExpressionState& state,
                         Vector& result) {
  // Style: 'plain' = digits only, 'iso' = with country prefix
  auto& vat_vec = args.data[0];
  auto& style_vec = args.data[1];
  size_t count = args.size();

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
  IterateSingleStringInput(args, result, [&](const std::string& input, size_t idx) {
    auto split_result = registry.SplitVAT(input);
    if (!split_result.has_value()) {
      SetBoolResult(result, idx, false);
      return;
    }
    auto [country, digits] = split_result.value();
    SetBoolResult(result, idx, registry.IsValidSyntax(country, digits));
  });
}

// ============================================================================
// Extension registration
// ============================================================================

void RegisterVATOptions(ExtensionLoader& loader) {
  // No specific options needed for VAT module for now
}

void RegisterVATFunctions(ExtensionLoader& loader) {
  // Phase 2: Basic VAT Operations
  ScalarFunction vat_func("anofox_tab_vat", {LogicalTypeId::VARCHAR}, GetVATType(),
                         VATParseFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, vat_func, "vat");

  ScalarFunction is_valid_country_func("anofox_tab_is_valid_vat_country",
                                       {LogicalTypeId::VARCHAR},
                                       LogicalTypeId::BOOLEAN,
                                       IsValidVATCountryFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, is_valid_country_func, "is_valid_vat_country");

  ScalarFunction normalize_func("anofox_tab_vat_normalize",
                               {LogicalTypeId::VARCHAR},
                               LogicalTypeId::VARCHAR, VATNormalizeFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, normalize_func, "vat_normalize");

  // Phase 3: Syntax Validation
  ScalarFunction is_valid_syntax_func("anofox_tab_vat_is_valid_syntax",
                                      {LogicalTypeId::VARCHAR},
                                      LogicalTypeId::BOOLEAN,
                                      VATIsValidSyntaxFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, is_valid_syntax_func, "vat_is_valid_syntax");

  ScalarFunction split_func("anofox_tab_vat_split", {LogicalTypeId::VARCHAR},
                           GetVATType(), VATSplitFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, split_func, "vat_split");

  ScalarFunction exists_func("anofox_tab_vat_exists", {LogicalTypeId::VARCHAR},
                            LogicalTypeId::BOOLEAN, VATExistsFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, exists_func, "vat_exists");

  // Phase 5: EU Utilities
  ScalarFunction is_eu_member_func("anofox_tab_vat_is_eu_member",
                                   {LogicalTypeId::VARCHAR},
                                   LogicalTypeId::BOOLEAN, VATIsEUMemberFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, is_eu_member_func, "vat_is_eu_member");

  ScalarFunction country_name_func("anofox_tab_vat_country_name",
                                   {LogicalTypeId::VARCHAR},
                                   LogicalTypeId::VARCHAR, VATCountryNameFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, country_name_func, "vat_country_name");

  ScalarFunction format_func("anofox_tab_vat_format",
                            {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR},
                            LogicalTypeId::VARCHAR, VATFormatFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, format_func, "vat_format");

  // Phase 6: Combined Validation
  ScalarFunction is_valid_func("anofox_tab_vat_is_valid", {LogicalTypeId::VARCHAR},
                              LogicalTypeId::BOOLEAN, VATIsValidFunc);
  anofox::RegisterScalarFunctionWithAlias(loader, is_valid_func, "vat_is_valid");
}

}  // namespace duckdb

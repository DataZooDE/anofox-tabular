#include "anofox_vat.hpp"
#include "anofox_function_alias.hpp"
#include "telemetry.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include <algorithm>
#include <cctype>

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
  IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
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
  IterateSingleStringInput(args, result, [&](std::string_view country, size_t idx) {
    SetBoolResult(result, idx, registry.IsValidCountry(country));
  });
}

// anofox_vat_normalize(number) -> VARCHAR
static void VATNormalizeFunc(DataChunk& args, ExpressionState& state,
                            Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
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
  IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
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
  IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
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
  IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
    // A supported prefix followed by a number: a bare country code ('DE') or a
    // prefix with letters only is not a VAT number
    auto split_result = registry.SplitVAT(input);
    bool has_number = split_result.has_value() &&
                      std::any_of(split_result->second.begin(), split_result->second.end(),
                                  [](char c) { return c >= '0' && c <= '9'; });
    SetBoolResult(result, idx, has_number);
  });
}

// ============================================================================
// Phase 5: EU Utilities (3 functions)
// ============================================================================

// anofox_vat_is_eu_member(country_code) -> BOOLEAN
static void VATIsEUMemberFunc(DataChunk& args, ExpressionState& state,
                             Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](std::string_view country, size_t idx) {
    SetBoolResult(result, idx, registry.IsEUMember(country));
  });
}

// anofox_vat_country_name(country_code) -> VARCHAR
static void VATCountryNameFunc(DataChunk& args, ExpressionState& state,
                              Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](std::string_view country, size_t idx) {
    std::string name = registry.GetCountryName(country);
    if (name.empty()) {
      FlatVector::SetNull(result, idx, true);
      return;
    }
    SetStringResult(result, idx, name);
  });
}

// anofox_vat_format(vat, style) -> VARCHAR
// Style: 'plain' = the national part after the country prefix (normalized; it keeps the
// letters some countries use, e.g. ATU..., NL...B01), 'iso' = VAT country prefix (EL for
// Greece) + national part. Neither style validates the number.
static void VATFormatFunc(DataChunk& args, ExpressionState& state,
                         Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateTwoStringInputs(args, result, [&](std::string_view vat,
                                           std::string_view style,
                                           size_t idx) {
    auto split_result = registry.SplitVAT(vat);
    if (!split_result.has_value()) {
      FlatVector::SetNull(result, idx, true);
      return;
    }
    auto [country, digits] = split_result.value();
    if (StringUtil::CIEquals(style.data(), style.size(), "plain", 5)) {
      SetStringResult(result, idx, digits);
    } else if (StringUtil::CIEquals(style.data(), style.size(), "iso", 3)) {
      SetStringResult(result, idx, registry.ConvertISOToVAT(country) + digits);
    } else {
      throw InvalidInputException(
          "Unsupported VAT format style: %s (expected 'plain' or 'iso')",
          std::string(style));
    }
  });
}

// ============================================================================
// Phase 6: Combined Validation (1 function)
// ============================================================================

// anofox_vat_is_valid(vat) -> BOOLEAN
// Full validation: syntax plus country check digits where implemented.
static void VATIsValidFunc(DataChunk& args, ExpressionState& state,
                          Vector& result) {
  auto& registry = VATRegistry::Instance();
  IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
    auto split_result = registry.SplitVAT(input);
    if (!split_result.has_value()) {
      SetBoolResult(result, idx, false);
      return;
    }
    auto [country, digits] = split_result.value();
    SetBoolResult(result, idx, registry.IsValidSyntax(country, digits) &&
                                   registry.IsValidChecksum(country, digits));
  });
}

// anofox_vat_validate(vat) -> STRUCT(valid, country, reason)
// The same check as vat_is_valid, with the reason a number is rejected.
// country is the ISO code of a supported prefix (as in vat_split), else NULL.
static constexpr const char* VAT_REASON_MISSING_PREFIX = "missing_prefix";
static constexpr const char* VAT_REASON_UNSUPPORTED_COUNTRY = "unsupported_country";
static constexpr const char* VAT_REASON_INVALID_FORMAT = "invalid_format";
static constexpr const char* VAT_REASON_CHECKSUM_MISMATCH = "checksum_mismatch";

static LogicalType GetVATValidateType() {
  child_list_t<LogicalType> struct_children;
  struct_children.push_back(make_pair("valid", LogicalTypeId::BOOLEAN));
  struct_children.push_back(make_pair("country", LogicalTypeId::VARCHAR));
  struct_children.push_back(make_pair("reason", LogicalTypeId::VARCHAR));
  return LogicalType::STRUCT(struct_children);
}

static void VATValidateFunc(DataChunk& args, ExpressionState& state,
                            Vector& result) {
  auto& registry = VATRegistry::Instance();
  auto& entries = StructVector::GetEntries(result);
  auto& valid_vec = anofox::VectorRef(entries[0]);
  auto& country_vec = anofox::VectorRef(entries[1]);
  auto& reason_vec = anofox::VectorRef(entries[2]);
  auto valid_data = duckdb_compat::FlatData<bool>(valid_vec);
  auto country_data = duckdb_compat::FlatData<string_t>(country_vec);
  auto reason_data = duckdb_compat::FlatData<string_t>(reason_vec);

  IterateSingleStringInput(args, result, [&](std::string_view input, size_t idx) {
    const char* reason = nullptr;
    std::string country;
    auto normalized = registry.NormalizeVAT(input);
    bool has_letter_prefix = normalized.size() >= 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) &&
                             std::isalpha(static_cast<unsigned char>(normalized[1]));
    auto split_result = registry.SplitVAT(input);
    if (!has_letter_prefix) {
      reason = VAT_REASON_MISSING_PREFIX;
    } else if (!split_result.has_value()) {
      reason = VAT_REASON_UNSUPPORTED_COUNTRY;
    } else {
      country = split_result->first;
      const auto& digits = split_result->second;
      if (!registry.IsValidSyntax(country, digits)) {
        reason = VAT_REASON_INVALID_FORMAT;
      } else if (!registry.IsValidChecksum(country, digits)) {
        reason = VAT_REASON_CHECKSUM_MISMATCH;
      }
    }
    valid_data[idx] = reason == nullptr;
    FlatVector::SetNull(valid_vec, idx, false);
    FlatVector::SetNull(country_vec, idx, country.empty());
    if (!country.empty()) {
      country_data[idx] = StringVector::AddString(country_vec, country);
    }
    FlatVector::SetNull(reason_vec, idx, reason == nullptr);
    if (reason) {
      reason_data[idx] = StringVector::AddString(reason_vec, reason);
    }
  });
}

// ============================================================================
// Extension registration
// ============================================================================

// Telemetry bind functions for scalar functions
unique_ptr<FunctionData> VatBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat");
  return nullptr;
}

unique_ptr<FunctionData> IsValidVatCountryBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("is_valid_vat_country");
  return nullptr;
}

unique_ptr<FunctionData> VatNormalizeBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_normalize");
  return nullptr;
}

unique_ptr<FunctionData> VatIsValidSyntaxBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_is_valid_syntax");
  return nullptr;
}

unique_ptr<FunctionData> VatSplitBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_split");
  return nullptr;
}

unique_ptr<FunctionData> VatExistsBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_exists");
  return nullptr;
}

unique_ptr<FunctionData> VatIsEuMemberBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_is_eu_member");
  return nullptr;
}

unique_ptr<FunctionData> VatCountryNameBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_country_name");
  return nullptr;
}

unique_ptr<FunctionData> VatFormatBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_format");
  return nullptr;
}

unique_ptr<FunctionData> VatValidateBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_validate");
  return nullptr;
}

unique_ptr<FunctionData> VatIsValidBind(ClientContext &) {
  PostHogTelemetry::Instance().RecordFunctionCall("vat_is_valid");
  return nullptr;
}

void RegisterVATOptions(ExtensionLoader& loader) {
  // No specific options needed for VAT module for now
}

void RegisterVATFunctions(ExtensionLoader& loader) {
  {
    FunctionDescription desc;
    desc.description = "Parses a VAT number string and returns a struct with country code, normalized number, and validity flags.";
    desc.parameter_names = {"vat_number"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat('DE123456789');"};
    desc.categories = {"vat", "parsing"};
    ScalarFunction vat_func("anofox_tab_vat", {LogicalTypeId::VARCHAR}, GetVATType(), VATParseFunc);
    vat_func.SetBindCallback(&anofox::ScalarContextBind<VatBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, vat_func, "vat", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Returns TRUE if the 2-letter string is a country that uses VAT numbers.";
    desc.parameter_names = {"country_code"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT is_valid_vat_country('DE');"};
    desc.categories = {"vat", "validation"};
    ScalarFunction is_valid_country_func("anofox_tab_is_valid_vat_country", {LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN, IsValidVATCountryFunc);
    is_valid_country_func.SetBindCallback(&anofox::ScalarContextBind<IsValidVatCountryBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, is_valid_country_func, "is_valid_vat_country", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Normalizes a VAT number by removing spaces, dashes, and other formatting characters.";
    desc.parameter_names = {"vat_number"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_normalize('DE 123 456 789');"};
    desc.categories = {"vat"};
    ScalarFunction normalize_func("anofox_tab_vat_normalize", {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, VATNormalizeFunc);
    normalize_func.SetBindCallback(&anofox::ScalarContextBind<VatNormalizeBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, normalize_func, "vat_normalize", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Returns TRUE if the VAT number matches the expected syntax for its country prefix (no network check).";
    desc.parameter_names = {"vat_number"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_is_valid_syntax('DE123456789');"};
    desc.categories = {"vat", "validation"};
    ScalarFunction is_valid_syntax_func("anofox_tab_vat_is_valid_syntax", {LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN, VATIsValidSyntaxFunc);
    is_valid_syntax_func.SetBindCallback(&anofox::ScalarContextBind<VatIsValidSyntaxBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, is_valid_syntax_func, "vat_is_valid_syntax", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Splits a VAT number into a struct with 'country_code' and 'number' fields.";
    desc.parameter_names = {"vat_number"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_split('DE123456789');"};
    desc.categories = {"vat", "parsing"};
    ScalarFunction split_func("anofox_tab_vat_split", {LogicalTypeId::VARCHAR}, GetVATType(), VATSplitFunc);
    split_func.SetBindCallback(&anofox::ScalarContextBind<VatSplitBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, split_func, "vat_split", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Returns TRUE if the VAT number starts with a supported country prefix followed by a number (at least one digit); no length, pattern or check-digit validation.";
    desc.parameter_names = {"vat_number"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_exists('DE123456789');"};
    desc.categories = {"vat", "validation"};
    ScalarFunction exists_func("anofox_tab_vat_exists", {LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN, VATExistsFunc);
    exists_func.SetBindCallback(&anofox::ScalarContextBind<VatExistsBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, exists_func, "vat_exists", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Returns TRUE if the 2-letter country code (ISO, or the VAT prefix 'EL' for Greece) is an EU member state.";
    desc.parameter_names = {"country_code"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_is_eu_member('DE');"};
    desc.categories = {"vat", "utility"};
    ScalarFunction is_eu_member_func("anofox_tab_vat_is_eu_member", {LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN, VATIsEUMemberFunc);
    is_eu_member_func.SetBindCallback(&anofox::ScalarContextBind<VatIsEuMemberBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, is_eu_member_func, "vat_is_eu_member", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Returns the English country name for a 2-letter country code (e.g., 'Germany' for 'DE'), or NULL for an unsupported code.";
    desc.parameter_names = {"country_code"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_country_name('DE');"};
    desc.categories = {"vat", "utility"};
    ScalarFunction country_name_func("anofox_tab_vat_country_name", {LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, VATCountryNameFunc);
    country_name_func.SetBindCallback(&anofox::ScalarContextBind<VatCountryNameBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, country_name_func, "vat_country_name", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Formats a VAT number using the specified style: 'plain' (the national part without the country prefix, letters included, e.g. U13585627 for ATU13585627) or 'iso' (VAT country prefix + national part). Does not validate.";
    desc.parameter_names = {"vat_number", "format_style"};
    desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_format('DE123456789', 'iso');"};
    desc.categories = {"vat", "formatting"};
    ScalarFunction format_func("anofox_tab_vat_format", {LogicalTypeId::VARCHAR, LogicalTypeId::VARCHAR}, LogicalTypeId::VARCHAR, VATFormatFunc);
    format_func.SetBindCallback(&anofox::ScalarContextBind<VatFormatBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, format_func, "vat_format", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Returns TRUE if the VAT number has valid syntax and passes the country's check-digit validation where implemented.";
    desc.parameter_names = {"vat_number"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_is_valid('DE111111125');"};
    desc.categories = {"vat", "validation"};
    ScalarFunction is_valid_func("anofox_tab_vat_is_valid", {LogicalTypeId::VARCHAR}, LogicalTypeId::BOOLEAN, VATIsValidFunc);
    is_valid_func.SetBindCallback(&anofox::ScalarContextBind<VatIsValidBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, is_valid_func, "vat_is_valid", {std::move(desc)});
  }
  {
    FunctionDescription desc;
    desc.description = "Validates a VAT number like vat_is_valid and says why it is rejected: a struct with valid, country (ISO code of a supported prefix, else NULL) and reason (missing_prefix, unsupported_country, invalid_format, checksum_mismatch; NULL when valid). Runs locally; does not query VIES.";
    desc.parameter_names = {"vat"};
    desc.parameter_types = {LogicalType::VARCHAR};
    desc.examples = {"SELECT vat_validate('ATU13585627');", "SELECT (vat_validate('DE 811 128 136')).reason;"};
    desc.categories = {"vat", "validation"};
    ScalarFunction validate_func("anofox_tab_vat_validate", {LogicalTypeId::VARCHAR}, GetVATValidateType(), VATValidateFunc);
    validate_func.SetBindCallback(&anofox::ScalarContextBind<VatValidateBind>::Bind);
    anofox::RegisterScalarFunctionWithAlias(loader, validate_func, "vat_validate", {std::move(desc)});
  }
}

}  // namespace duckdb

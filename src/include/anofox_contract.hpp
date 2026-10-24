#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

// anofox integration contract (anofox-visualization docs/plans/integration-contract.md, #107):
// contract producers carry the duckdb_functions() tags
// {'anofox.output': '<schema>', 'anofox.contract': '1'}, and every anofox extension answers
// anofox_contract_version() with the same version.
constexpr const char *ANOFOX_CONTRACT_VERSION = "1";

// Registers anofox_contract_version() unless another anofox extension (statistics,
// forecast, ...) already did: a second identical overload would make the load fail.
void RegisterContractVersionFunction(ExtensionLoader &loader);

// Tags each contract producer of this extension (primary name and alias) with its
// output schema. Runs after every module has registered its functions.
void ApplyContractTags(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

#define DUCKDB_EXTENSION_MAIN

#include "anofox_tabular_extension.hpp"
#include "anofox_diff.hpp"
#include "anofox_email.hpp"
#include "anofox_postal.hpp"
#include "anofox_phonenumber.hpp"
#include "anofox_metric.hpp"

namespace duckdb {

void LoadInternal(ExtensionLoader &loader) {
	anofox::RegisterPostalOptions(loader);
	anofox::RegisterPostalFunctions(loader);
	anofox::RegisterPhonenumberOptions(loader);
	anofox::RegisterPhonenumberFunctions(loader);
	anofox::RegisterEmailOptions(loader);
	anofox::RegisterEmailFunctions(loader);
	anofox::RegisterDiffFunctions(loader);
	anofox::RegisterMetricFunctions(loader);
}

void AnofoxTabularExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AnofoxTabularExtension::Name() {
	return "anofox_tabular";
}

std::string AnofoxTabularExtension::Version() const {
#ifdef EXT_VERSION_ANOFOX_TABULAR
	return EXT_VERSION_ANOFOX_TABULAR;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_tabular, loader) {
	duckdb::LoadInternal(loader);
}

}

#define DUCKDB_EXTENSION_MAIN

#include "anofox_tabular_extension.hpp"
#include "anofox/postal/postal_functions.hpp"

namespace duckdb {

void LoadInternal(ExtensionLoader &loader) {
	anofox::RegisterPostalFunctions(loader);
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

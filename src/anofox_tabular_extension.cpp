#define DUCKDB_EXTENSION_MAIN

#include "anofox_tabular_extension.hpp"
#include "anofox_diff.hpp"
#include "anofox_email.hpp"
#if HAVE_LIBPOSTAL
#include "anofox_postal.hpp"
#endif
#include "anofox_phonenumber.hpp"
#include "anofox_metric.hpp"
#include "anofox_money.hpp"
#include "anofox_vat.hpp"
#include "telemetry.hpp"

namespace duckdb {

namespace {

void OnTelemetryEnabled(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_telemetry_enabled cannot be NULL");
	}
	auto &telemetry = PostHogTelemetry::Instance();
	telemetry.SetEnabled(BooleanValue::Get(parameter));
}

void OnTelemetryKey(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("anofox_telemetry_key cannot be NULL");
	}
	auto &telemetry = PostHogTelemetry::Instance();
	telemetry.SetAPIKey(StringValue::Get(parameter));
}

} // anonymous namespace

static void RegisterTelemetryOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());

	config.AddExtensionOption("anofox_telemetry_enabled",
	                          "Enable or disable anonymous usage telemetry",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true), OnTelemetryEnabled);

	config.AddExtensionOption("anofox_telemetry_key",
	                          "PostHog API key for telemetry",
	                          LogicalType::VARCHAR,
	                          Value("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t"),
	                          OnTelemetryKey);
}

void LoadInternal(ExtensionLoader &loader) {
	// Register telemetry options FIRST (allows users to disable via SQL settings)
	// Note: Environment variable DATAZOO_DISABLE_TELEMETRY is always checked by posthog-telemetry
	RegisterTelemetryOptions(loader);

	// Initialize and capture extension load event
	auto &telemetry = PostHogTelemetry::Instance();
	telemetry.SetAPIKey("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t");

	std::string version;
#ifdef EXT_VERSION_ANOFOX_TABULAR
	version = EXT_VERSION_ANOFOX_TABULAR;
#else
	version = "0.1.0";
#endif
	telemetry.CaptureExtensionLoad("anofox_tabular", version);

#if HAVE_LIBPOSTAL
	anofox::RegisterPostalOptions(loader);
	anofox::RegisterPostalFunctions(loader);
#endif
	anofox::RegisterPhonenumberOptions(loader);
	anofox::RegisterPhonenumberFunctions(loader);
	anofox::RegisterEmailOptions(loader);
	anofox::RegisterEmailFunctions(loader);
	anofox::RegisterDiffFunctions(loader);
	anofox::RegisterMetricFunctions(loader);
	anofox::RegisterMoneyOptions(loader);
	anofox::RegisterMoneyFunctions(loader);
	RegisterVATOptions(loader);
	RegisterVATFunctions(loader);
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

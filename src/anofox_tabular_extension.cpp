#define DUCKDB_EXTENSION_MAIN

#include "anofox_tabular_extension.hpp"
#include "anofox_diff.hpp"
#include "anofox_email.hpp"
#if HAVE_LIBPOSTAL
#include "anofox_postal.hpp"
#endif
#include "anofox_phonenumber.hpp"
#include "anofox_metric.hpp"
#include "anofox_profile.hpp"
#include "anofox_outlier_tree.hpp"
#include "anofox_money.hpp"
#include "anofox_vat.hpp"
#include "anofox_pii.hpp"
#include "anofox_ner.hpp"
#include "telemetry.hpp"

#include <cstdlib>

namespace duckdb {

namespace {

// True if the DATAZOO_DISABLE_TELEMETRY environment variable requests an opt-out.
// This mirrors the check posthog-telemetry performs at send time, but evaluating it
// here lets us disable the singleton before any event is enqueued (and before the
// background queue thread is ever started).
bool IsTelemetryDisabledByEnv() {
	const char *env = std::getenv("DATAZOO_DISABLE_TELEMETRY");
	if (!env) {
		return false;
	}
	const std::string value(env);
	return value == "1" || value == "true" || value == "yes";
}

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

	// The default reflects the DATAZOO_DISABLE_TELEMETRY environment variable so
	// `current_setting('anofox_telemetry_enabled')` is honest about the effective state.
	config.AddExtensionOption("anofox_telemetry_enabled",
	                          "Enable or disable anonymous usage telemetry",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(!IsTelemetryDisabledByEnv()),
	                          OnTelemetryEnabled);

	config.AddExtensionOption("anofox_telemetry_key",
	                          "PostHog API key for telemetry",
	                          LogicalType::VARCHAR,
	                          Value("phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t"),
	                          OnTelemetryKey);
}

void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription(
	    "Data quality validation and improvement primitives: "
	    "email, phone, postal address, VAT, PII detection/masking, "
	    "money, diffing, profiling, and anomaly detection.");

	// Register telemetry options first so they exist before any event is emitted.
	RegisterTelemetryOptions(loader);

	// Determine the effective telemetry opt-out BEFORE emitting the load event.
	// SQL `SET anofox_telemetry_enabled = false` can only run after the extension is
	// loaded, so it cannot suppress this event; the pre-load opt-outs are:
	//   - the DATAZOO_DISABLE_TELEMETRY environment variable, or
	//   - pre-setting anofox_telemetry_enabled through DBConfig / client config
	//     (with allow_unrecognized_options) — AddExtensionOption adopts such values
	//     without invoking the SET callback, so read the effective value here.
	auto &db = loader.GetDatabaseInstance();
	bool telemetry_enabled = !IsTelemetryDisabledByEnv();
	if (telemetry_enabled) {
		Value enabled_value;
		if (db.TryGetCurrentSetting("anofox_telemetry_enabled", enabled_value) && !enabled_value.IsNull()) {
			telemetry_enabled = BooleanValue::Get(enabled_value.DefaultCastAs(LogicalType::BOOLEAN));
		}
	}

	// Sync the singleton with the effective state so capture call sites are
	// consistent with the configuration from the very start.
	auto &telemetry = PostHogTelemetry::Instance();
	telemetry.SetEnabled(telemetry_enabled);

	std::string version;
#ifdef EXT_VERSION_ANOFOX_TABULAR
	version = EXT_VERSION_ANOFOX_TABULAR;
#else
	version = "0.1.0";
#endif
	if (telemetry_enabled) {
		Value key_value;
		if (db.TryGetCurrentSetting("anofox_telemetry_key", key_value) && !key_value.IsNull()) {
			telemetry.SetAPIKey(StringValue::Get(key_value.DefaultCastAs(LogicalType::VARCHAR)));
		}
		telemetry.CaptureExtensionLoad("anofox_tabular", version);
	} else {
		// Still record the extension name so later re-enabling via SQL produces
		// correctly attributed function-execution events.
		telemetry.SetExtensionName("anofox_tabular");
	}

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
	anofox::RegisterProfileFunctions(loader);
	anofox::RegisterOutlierTreeFunctions(loader);
	anofox::RegisterMoneyOptions(loader);
	anofox::RegisterMoneyFunctions(loader);
	RegisterVATOptions(loader);
	RegisterVATFunctions(loader);
	anofox::RegisterNEROptions(loader);
	anofox::RegisterPIIOptions(loader);
	anofox::RegisterPIIFunctions(loader);
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

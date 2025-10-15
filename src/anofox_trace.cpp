#include "anofox_trace.hpp"

#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif
#ifdef SPDLOG_COMPILED_LIB
#undef SPDLOG_COMPILED_LIB
#endif
#ifdef SPDLOG_FMT_EXTERNAL
#undef SPDLOG_FMT_EXTERNAL
#endif
#include <spdlog/spdlog.h>
#include <string>

namespace duckdb {
namespace anofox {

namespace {

spdlog::level::level_enum ToSpdLevel(AnofoxLogLevel level) {
	switch (level) {
	case AnofoxLogLevel::Trace:
		return spdlog::level::trace;
	case AnofoxLogLevel::Debug:
		return spdlog::level::debug;
	case AnofoxLogLevel::Info:
		return spdlog::level::info;
	case AnofoxLogLevel::Warn:
		return spdlog::level::warn;
	case AnofoxLogLevel::Error:
		return spdlog::level::err;
	case AnofoxLogLevel::Critical:
		return spdlog::level::critical;
	case AnofoxLogLevel::Off:
		return spdlog::level::off;
	default:
		return spdlog::level::info;
	}
}

std::string LevelToString(AnofoxLogLevel level) {
	switch (level) {
	case AnofoxLogLevel::Trace:
		return "trace";
	case AnofoxLogLevel::Debug:
		return "debug";
	case AnofoxLogLevel::Info:
		return "info";
	case AnofoxLogLevel::Warn:
		return "warn";
	case AnofoxLogLevel::Error:
		return "error";
	case AnofoxLogLevel::Critical:
		return "critical";
	case AnofoxLogLevel::Off:
		return "off";
	default:
		return "info";
	}
}

} // namespace

AnofoxTraceConfig::AnofoxTraceConfig() : enabled(true), level(static_cast<int>(AnofoxLogLevel::Info)) {
}

AnofoxTraceConfig &AnofoxTraceConfig::Get() {
	static AnofoxTraceConfig instance;
	return instance;
}

void AnofoxTraceConfig::SetEnabled(bool value) {
	enabled.store(value, std::memory_order_relaxed);
}

bool AnofoxTraceConfig::GetEnabled() const {
	return enabled.load(std::memory_order_relaxed);
}

void AnofoxTraceConfig::SetLevel(AnofoxLogLevel value) {
	level.store(static_cast<int>(value), std::memory_order_relaxed);
}

AnofoxLogLevel AnofoxTraceConfig::GetLevel() const {
	return static_cast<AnofoxLogLevel>(level.load(std::memory_order_relaxed));
}

bool AnofoxTraceConfig::ShouldLog(AnofoxLogLevel target) const {
	if (!GetEnabled()) {
		return false;
	}
	auto current = GetLevel();
	if (current == AnofoxLogLevel::Off) {
		return false;
	}
	return ToSpdLevel(target) >= ToSpdLevel(current);
}

std::string AnofoxTraceConfig::GetLevelString() const {
    return LevelToString(GetLevel());
}

void AnofoxTrace(AnofoxLogLevel level, const std::string &message) {
	auto &config = AnofoxTraceConfig::Get();
	if (!config.ShouldLog(level)) {
		return;
	}
	if (level == AnofoxLogLevel::Off) {
		return;
	}
	spdlog::log(ToSpdLevel(level), "[anofox] {}", message);
}

} // namespace anofox
} // namespace duckdb

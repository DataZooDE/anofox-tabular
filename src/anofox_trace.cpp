#include "anofox_trace.hpp"

#include <iostream>
#include <mutex>
#include <string>

namespace duckdb {
namespace anofox {

namespace {

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
	return static_cast<int>(target) >= static_cast<int>(current);
}

std::string AnofoxTraceConfig::GetLevelString() const {
    return LevelToString(GetLevel());
}

void AnofoxTrace(AnofoxLogLevel level, const std::string &message) {
	auto &config = AnofoxTraceConfig::Get();
	if (level == AnofoxLogLevel::Off || !config.ShouldLog(level)) {
		return;
	}
	// Compose the full line first (including the level of the message, not the
	// configured threshold) and emit it as a single write guarded by a mutex so
	// lines from parallel tasks cannot interleave (#51).
	const std::string line = "[anofox] [" + LevelToString(level) + "] " + message + "\n";
	static std::mutex output_mutex;
	std::lock_guard<std::mutex> guard(output_mutex);
	std::cerr << line << std::flush;
}

} // namespace anofox
} // namespace duckdb

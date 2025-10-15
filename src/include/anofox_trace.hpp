#pragma once

#include <atomic>
#include <string>

namespace duckdb {
namespace anofox {

enum class AnofoxLogLevel { Trace, Debug, Info, Warn, Error, Critical, Off };

class AnofoxTraceConfig {
public:
	static AnofoxTraceConfig &Get();

	void SetEnabled(bool enabled);
	bool GetEnabled() const;

	void SetLevel(AnofoxLogLevel level);
	AnofoxLogLevel GetLevel() const;

	bool ShouldLog(AnofoxLogLevel level) const;

	std::string GetLevelString() const;

private:
	AnofoxTraceConfig();

	std::atomic<bool> enabled;
	std::atomic<int> level;
};

void AnofoxTrace(AnofoxLogLevel level, const std::string &message);

} // namespace anofox
} // namespace duckdb

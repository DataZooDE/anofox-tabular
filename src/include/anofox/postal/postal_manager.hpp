#pragma once

#include "duckdb/common/common.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
class ClientContext;
class FileSystem;

namespace anofox {
namespace postal {

struct PostalStatus {
	bool initialized = false;
	bool data_present = false;
	std::string data_dir;
};

struct PostalComponent {
	std::string label;
	std::string value;
};

class PostalManager {
public:
	static PostalManager &Instance();

	void EnsureInitialized(ClientContext &context);
	std::vector<PostalComponent> ParseAddress(const std::string &input);
	std::vector<std::string> ExpandAddress(const std::string &input);

	void LoadData(ClientContext &context);
	PostalStatus GetStatus(ClientContext &context);

private:
	PostalManager() noexcept = default;

	void Initialize(ClientContext &context);
	void Teardown();
	static void AtExitCallback();

	std::string ResolveDataDir() const;
	bool DataPresent(FileSystem &fs, const std::string &data_dir) const;

	std::atomic<bool> initialized {false};
	std::atomic<bool> data_downloaded {false};
	std::once_flag init_flag;
};

} // namespace postal
} // namespace anofox
} // namespace duckdb

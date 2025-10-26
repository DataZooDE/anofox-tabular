#pragma once

#if HAVE_LIBPOSTAL

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/client_context.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

void RegisterPostalOptions(ExtensionLoader &loader);
void RegisterPostalFunctions(ExtensionLoader &loader);

namespace postal {

struct PostalComponent {
	std::string label;
	std::string value;
};

struct PostalStatus {
	bool initialized = false;
	bool data_present = false;
	std::string data_dir;
};

class PostalManager {
public:
	static PostalManager &Instance();

	void EnsureInitialized(ClientContext &context);
	void LoadData(ClientContext &context);
	std::vector<PostalComponent> ParseAddress(const std::string &input);
	std::vector<std::string> ExpandAddress(const std::string &input);
	PostalStatus GetStatus(ClientContext &context);

	void SetDataDirectory(const std::string &path);
	std::string GetDataDirectory() const;

private:
	PostalManager();
	~PostalManager();

	void Initialize(ClientContext &context);

	std::atomic<bool> initialized {false};
	std::mutex init_lock;
	std::string data_directory;
};

} // namespace postal

} // namespace anofox
} // namespace duckdb

#endif // HAVE_LIBPOSTAL

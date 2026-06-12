#pragma once

#if HAVE_LIBPOSTAL

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/client_context.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

void RegisterPostalOptions(ExtensionLoader &loader);
void RegisterPostalFunctions(ExtensionLoader &loader);

namespace postal {

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
	//! Parses `input` and invokes `visit(labels, values, count)` on the raw
	//! libpostal component arrays. The callback writes the values straight into
	//! DuckDB vectors, so no intermediate std::string copies are made; the
	//! libpostal response is released on every exit path.
	void ParseAddress(const std::string &input,
	                  const std::function<void(char **labels, char **values, size_t count)> &visit);
	//! Expands `input` and invokes `visit(expansions, count)` on the raw
	//! libpostal expansion array (same direct-write contract as ParseAddress).
	void ExpandAddress(const std::string &input,
	                   const std::function<void(char **expansions, size_t count)> &visit);
	PostalStatus GetStatus(ClientContext &context);

	void SetDataDirectory(const std::string &path);
	std::string GetDataDirectory() const;

private:
	PostalManager();
	~PostalManager();

	//! Performs libpostal setup. Caller must hold init_lock.
	void Initialize(ClientContext &context);
	//! Downloads and extracts the data bundle. Caller must hold init_lock.
	void LoadDataInternal(ClientContext &context);
	//! Computes the current status. Caller must hold init_lock.
	PostalStatus GetStatusInternal(ClientContext &context) const;
	//! Tears down exactly the libpostal stages that completed setup.
	void TeardownInitializedStages();

	std::atomic<bool> initialized {false};
	//! Guards data_directory and all libpostal setup/download state.
	//! Mutable so const accessors can lock it.
	mutable std::mutex init_lock;
	std::string data_directory;
	//! Per-stage setup tracking so a failed initialization only rolls back
	//! the stages that actually completed.
	bool core_ready = false;
	bool parser_ready = false;
	bool classifier_ready = false;
};

} // namespace postal

} // namespace anofox
} // namespace duckdb

#endif // HAVE_LIBPOSTAL

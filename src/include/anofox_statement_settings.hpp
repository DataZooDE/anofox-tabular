#pragma once

#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace duckdb {
namespace anofox {

/**
 * One value of process-wide settings per statement.
 *
 * The PII and email options are process-wide: a SET in one connection changes
 * them for every connection. A statement that reads them per data chunk would
 * see a SET from another connection partway through and mix two
 * configurations in one result. This client-context state holds the snapshot
 * the first chunk of a statement takes; every later chunk, on any thread, of
 * that statement reads the same snapshot, and the end of the statement drops
 * it so the next statement sees the current settings.
 */
template <class SNAPSHOT>
class StatementSettingsState : public ClientContextState {
public:
    template <class TAKE>
    SNAPSHOT Get(TAKE &&take) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!snapshot_) {
            snapshot_ = std::make_unique<SNAPSHOT>(take());
        }
        return *snapshot_;
    }

    void QueryBegin(ClientContext &) override {
        Reset();
    }

    void QueryEnd(ClientContext &, optional_ptr<ErrorData>) override {
        Reset();
    }

private:
    void Reset() {
        std::lock_guard<std::mutex> guard(mutex_);
        snapshot_.reset();
    }

    std::mutex mutex_;
    std::unique_ptr<SNAPSHOT> snapshot_;
};

/**
 * The snapshot of the settings named `key` for the statement running in
 * `context`: taken by `take()` on the first call of the statement and
 * returned unchanged until the statement ends. Without a client context
 * (constant folding) a fresh snapshot is taken.
 */
template <class SNAPSHOT, class TAKE>
SNAPSHOT StatementSettings(optional_ptr<ClientContext> context, const std::string &key, TAKE &&take) {
    if (!context) {
        return take();
    }
    return context->registered_state->GetOrCreate<StatementSettingsState<SNAPSHOT>>(key)->Get(
        std::forward<TAKE>(take));
}

/**
 * The client context of a scalar function call, nullptr when the expression
 * is evaluated without one
 */
inline optional_ptr<ClientContext> StatementContext(ExpressionState &state) {
    return state.HasContext() ? &state.GetContext() : nullptr;
}

} // namespace anofox
} // namespace duckdb

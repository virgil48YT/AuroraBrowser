// Aurora — session persistence.
//
// On a clean shutdown the browser writes the open windows and their tabs to
// the sessions table. On startup it can restore them. Writes are atomic per
// session (one transaction), so a crash during shutdown leaves either the
// previous complete session or the new one — never a partial mix.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "storage/Database.hpp"

namespace aurora::core {
struct TabState;
}

namespace aurora::session {

struct SavedTab {
    std::string url;
    std::string title;
    bool pinned = false;
    int window_index = 0;
    int tab_index = 0;
};

struct SavedSession {
    int64_t id = 0;
    std::chrono::system_clock::time_point created_at;
    std::vector<SavedTab> tabs;
};

// The result of validating a saved session before it is handed to the UI.
// Restore is the single most crash-prone startup path in a browser: the rows
// come from a file on disk that may have been written by an older build, a
// crashed process, or a corrupted profile. Everything the UI acts on goes
// through here first.
struct RestorePlan {
    // Tabs that are safe to open, ordered by (window_index, tab_index).
    std::vector<SavedTab> tabs;
    // How many stored rows were rejected (empty/unsupported/oversized URLs).
    int skipped = 0;
};

// Upper bound on tabs restored in one go. A corrupted or malicious session
// row count must never be able to open unbounded windows/renderers.
constexpr size_t kMaxRestoredTabs = 200;

// True when `url` is something the browser may navigate to on startup.
// Rejects empty/whitespace strings, absurdly long values, and non-navigable
// schemes (javascript:, data:, blob:, vbscript:) which must never be revived
// automatically because the original page's origin is gone.
bool is_restorable_url(std::string_view url);

// Validate + order the rows of a saved session. Never throws; always returns
// a plan whose tabs are individually safe to open.
RestorePlan plan_restore(const SavedSession& session,
                         size_t max_tabs = kMaxRestoredTabs);

class SessionService {
public:
    explicit SessionService(storage::Database& db) : db_(db) {}

    // Persist the current set of tabs as a new session, returning its id.
    // Any prior sessions older than `keep` are pruned so the table stays
    // bounded (default: keep the last 10 sessions).
    int64_t save(const std::vector<SavedTab>& tabs, size_t keep = 10);

    // Load the most recent session, or nullopt if none exists.
    std::optional<SavedSession> load_last() const;

    // Load a specific session by id.
    std::optional<SavedSession> load(int64_t id) const;

private:
    storage::Database& db_;
    std::optional<SavedSession> load_one(int64_t id) const;
};

} // namespace aurora::session

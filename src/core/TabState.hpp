// Aurora — tab state.
//
// The pure-data model of a single tab, independent of any renderer or widget.
// The TabManager owns a collection of these; the UI layer observes the
// manager and renders derived views.
#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "core/Types.hpp"
#include "core/Url.hpp"
#include "core/NavigationEntry.hpp"

namespace aurora::core {

enum class TabLoadingState {
    Idle,
    Loading,
    Crashed,
};

// Crash classification influences the recovery UI shown to the user.
enum class TabCrashReason {
    None,
    RendererGone,
    OutOfMemory,
    NetworkLost,
    Unknown,
};

struct TabState {
    TabId id;
    std::string title;            // Last known page title.
    Url url;                      // Current committed URL.
    std::string favicon_url;

    bool pinned = false;
    bool muted = false;
    bool audible = false;
    bool loading = false;
    bool suspended = false;
    bool exclude_from_suspension = false; // User's per-tab automatic exclusion.
    size_t pending_downloads = 0;
    // Monotonic timestamps are runtime-only (never written to the session).
    // Starting the idle interval on deactivation gives a tab that spent a
    // long time in the foreground a full grace period after switching away.
    std::chrono::steady_clock::time_point last_activated{};
    std::chrono::steady_clock::time_point inactive_since{};
    std::chrono::steady_clock::time_point suspended_at{};
    // 0..100; negative means no progress to display.
    int load_progress = -1;

    bool crashed = false;
    TabCrashReason crash_reason = TabCrashReason::None;

    // CEF's real back/forward capability for the main browser, reported by
    // OnLoadingStateChange. The toolbar buttons use these — NOT the tab's own
    // history stack below, which only tracks omnibox-initiated entries and
    // misses link clicks, redirects, and form navigations.
    bool cef_can_go_back = false;
    bool cef_can_go_forward = false;

    // Back/forward stack owned by this tab. Index points at the current entry;
    // entries before it are "back", after it are "forward".
    std::vector<NavigationEntry> history;
    size_t current_index = 0;

    bool can_go_back() const { return current_index > 0; }
    bool can_go_forward() const {
        return current_index + 1 < history.size();
    }

    const NavigationEntry* current_entry() const {
        if (history.empty()) return nullptr;
        return &history[current_index];
    }
};

} // namespace aurora::core

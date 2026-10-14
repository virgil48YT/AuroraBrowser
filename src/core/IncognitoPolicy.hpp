// Aurora — incognito window policy.
//
// Pure core: no Qt, no CEF. MainWindow consults this when it decides which
// services an incognito window may talk to and how snapshots behave.
//
// An incognito window must never write to the regular profile. History,
// downloads metadata and session snapshots are skipped entirely; preferences
// and bookmarks are shared read-only for the first implementation (so the
// user keeps their homepage, search engine and bookmarks), and any *writes*
// to them from a private window are rejected.
// Preferences stay attached through a window-owned read-only Settings view:
// the Settings layer suppresses setters, rather than nulling the prefs
// pointer and disabling the settings page (or changing the normal store).
#pragma once

#include <string_view>

namespace aurora::core {

// Decisions the chrome must make for a window.
struct IncognitoPolicy {
    // A window is either a normal profile window or a private (incognito) one.
    bool is_incognito = false;

    // History: never recorded for an incognito window, not even in memory
    // (the existing in-memory TabManager history is fine; this refers to the
    // HistoryService that persists to disk).
    constexpr bool record_history() const { return !is_incognito; }

    // Downloads metadata: never persisted for an incognito window. The file
    // itself is still written to disk — that is documented behaviour.
    constexpr bool record_downloads() const { return !is_incognito; }

    // Session snapshots: an incognito window never saves and is never
    // restored into.
    constexpr bool save_session_snapshot() const { return !is_incognito; }
    constexpr bool restore_session_snapshot() const { return !is_incognito; }

    // Extensions: enabled for a normal window; disabled in an incognito
    // window because an extension's service worker/cookie jar is global to
    // the browser, and private-context extension isolation is not implemented.
    constexpr bool enable_extensions() const { return !is_incognito; }

    // Bookmarks are read-only in an incognito window (adding a bookmark in
    // private mode is rejected rather than silently persisted).
    constexpr bool may_write_bookmarks() const { return !is_incognito; }

    // Preferences are read from the normal profile but never written from a
    // private window. Enforced by Settings' read-only view, not a null pointer.
    constexpr bool may_write_preferences() const { return !is_incognito; }
};

} // namespace aurora::core

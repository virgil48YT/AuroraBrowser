// Aurora — settings store.
//
// A typed key-value preferences store. Values are JSON-encoded into a single
// TEXT column so any serializable type (bool/int/string/arrays) can live in
// one table without schema churn. Defaults are declared in code so a fresh
// profile has sensible behavior without a seeded database.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include "storage/Database.hpp"

namespace aurora::settings {

// Defaults shared by the settings page and the startup path, so a fresh
// profile and the UI never disagree about what "default" means.
struct PrefDefaults {
    static constexpr std::string_view kSearchEngine = "duckduckgo";
    static constexpr std::string_view kTheme = "system";  // system|light|dark
    static constexpr bool kRestoreSession = true;
    static constexpr bool kDownloadsAskWhere = false;
    static constexpr bool kHttpsOnly = true;
    static constexpr bool kClearOnExitCookies = false;
    static constexpr bool kClearOnExitHistory = false;
    static constexpr bool kClearOnExitDownloads = false;
    static constexpr bool kCertAudit = true;
    static constexpr bool kRefererTrim = true;
    static constexpr int kTabSuspendAfterMinutes = 30;  // 0 = disabled
    static constexpr int kTabSuspendMinMinutes = 0;
    static constexpr int kTabSuspendMaxMinutes = 24 * 60;
};

// All known preference keys. Centralizing them prevents typos and makes the
// settings page enumerable. Add a key here before referencing it anywhere.
struct PrefKeys {
    static constexpr std::string_view kStartupBehavior = "startup.behavior";
    // There is deliberately no homepage key: the Home button and new tabs open
    // the chosen search engine's home page (see core::search_engine_home).
    // "Continue where you left off" — read by the session-restore path.
    static constexpr std::string_view kRestoreSession = "startup.restore_session";
    static constexpr std::string_view kSearchEngine = "search.engine";
    static constexpr std::string_view kTheme = "appearance.theme";
    static constexpr std::string_view kAccentColor = "appearance.accent";
    static constexpr std::string_view kShowBookmarkBar = "appearance.bookmark_bar";
    static constexpr std::string_view kTabSuspendAfterMinutes = "tabs.suspend_minutes";
    static constexpr std::string_view kDownloadsPath = "downloads.path";
    static constexpr std::string_view kDownloadsAskWhere = "downloads.ask_where";
    static constexpr std::string_view kBlockThirdPartyCookies = "privacy.block_3p_cookies";
    static constexpr std::string_view kSendDoNotTrack = "privacy.dnt";
    // Global kill switch for Aurora Shields (network ad blocking). Per-site
    // exceptions live in their own table, not here. Default ON.
    static constexpr std::string_view kAdblockEnabled = "privacy.adblock";
    static constexpr std::string_view kHttpsOnly = "privacy.https_only";
    static constexpr std::string_view kClearOnExitCookies = "privacy.clear_on_exit.cookies";
    static constexpr std::string_view kClearOnExitHistory = "privacy.clear_on_exit.history";
    static constexpr std::string_view kClearOnExitDownloads = "privacy.clear_on_exit.downloads";
    static constexpr std::string_view kCertAudit = "privacy.cert_audit";
    static constexpr std::string_view kRefererTrim = "privacy.referer_trim";
    static constexpr std::string_view kReducedMotion = "accessibility.reduced_motion";
};

class Settings {
public:
    explicit Settings(storage::Database& db, bool read_only = false)
        : db_(db), read_only_(read_only) {}

    bool is_read_only() const { return read_only_; }
    // A window-owned view of the same profile, without changing the normal
    // window's store. Only the database must outlive the view, not this object.
    std::unique_ptr<Settings> read_only_view() const;

    // Ensure the preferences table exists.
    void ensure_schema();

    // Get with a default fallback. Specialized for bool/int/string below.
    bool get_bool(std::string_view key, bool fallback) const;
    int64_t get_int(std::string_view key, int64_t fallback) const;
    std::string get_string(std::string_view key, std::string_view fallback) const;

    void set_bool(std::string_view key, bool value);
    void set_int(std::string_view key, int64_t value);
    void set_string(std::string_view key, std::string_view value);

    // Download preferences have named accessors because the settings page and
    // the CEF adapter both read them live. An empty path means the platform's
    // standard Downloads folder.
    std::string downloads_path() const;
    void set_downloads_path(std::string_view path);
    bool downloads_ask_where() const;
    void set_downloads_ask_where(bool ask);

private:
    storage::Database& db_;
    bool read_only_ = false;
    mutable std::mutex cache_mutex_;
    // A read cache so repeated omnibox queries for the search engine pref
    // don't hit SQLite on every keystroke. Writes invalidate the cache.
    mutable std::map<std::string, std::string> cache_;

    std::string raw_get(std::string_view key) const;
    void raw_set(std::string_view key, std::string_view value);
};

} // namespace aurora::settings

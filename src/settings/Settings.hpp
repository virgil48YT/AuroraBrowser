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
#include <utility>
#include <vector>
#include "storage/Database.hpp"

namespace aurora::settings {

// Defaults shared by the settings page and the startup path, so a fresh
// profile and the UI never disagree about what "default" means.
struct PrefDefaults {
    static constexpr std::string_view kSearchEngine = "duckduckgo";
    static constexpr std::string_view kTheme = "system";  // system|light|dark
    static constexpr bool kRestoreSession = true;
    // Only consulted when startup.mode is absent (profiles from before the
    // three-way Startup card): kRestoreSession then decides continue/home.
    static constexpr std::string_view kStartupMode = "continue";
    static constexpr bool kDownloadsAskWhere = false;
    static constexpr bool kHttpsOnly = true;
    // Address-bar dropdown: ask the default search engine for query
    // completions while typing. History, bookmark and open-tab matches are
    // local and always shown; this only controls the network lookups.
    static constexpr bool kSearchSuggestions = true;
    static constexpr bool kClearOnExitCookies = false;
    static constexpr bool kClearOnExitHistory = false;
    static constexpr bool kClearOnExitDownloads = false;
    static constexpr bool kCertAudit = true;
    static constexpr bool kRefererTrim = true;
    // The settings page has always shown third-party cookie blocking as on
    // for a fresh profile; the engine now enforces exactly that default.
    static constexpr bool kBlockThirdPartyCookies = true;
    static constexpr bool kSendDoNotTrack = false;
    static constexpr int kTabSuspendAfterMinutes = 30;  // 0 = disabled
    static constexpr int kTabSuspendMinMinutes = 0;
    static constexpr int kTabSuspendMaxMinutes = 24 * 60;
};

// All known preference keys. Centralizing them prevents typos and makes the
// settings page enumerable. Add a key here before referencing it anywhere.
struct PrefKeys {
    // What the first window shows at launch: "home" | "continue" |
    // "specific" (see StartupMode / resolve_startup_mode below). This
    // replaced an older, never-read "startup.behavior" key.
    static constexpr std::string_view kStartupMode = "startup.mode";
    // The page "specific" opens. The Home button and new tabs never use it:
    // they keep opening aurora://home (see MainWindow::new_tab_url).
    static constexpr std::string_view kStartupHomeUrl = "startup.home_url";
    // "Continue where you left off" — the pre-startup.mode boolean. Still
    // written alongside kStartupMode so an older build reading the same
    // profile behaves the same, and still the fallback when kStartupMode is
    // absent.
    static constexpr std::string_view kRestoreSession = "startup.restore_session";
    static constexpr std::string_view kSearchEngine = "search.engine";
    static constexpr std::string_view kSearchSuggestions = "search.suggestions";
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
    // Bookkeeping for the in-app filter-list updater (adblock::ListUpdater):
    // unix seconds of the last successful install and the URL it came from.
    // Absent until the first in-app update; a list fetched by
    // scripts/fetch_filter_lists.sh leaves them untouched.
    static constexpr std::string_view kAdblockListUpdatedAt = "privacy.adblock.list_updated_at";
    static constexpr std::string_view kAdblockListSource = "privacy.adblock.list_source";
    static constexpr std::string_view kHttpsOnly = "privacy.https_only";
    static constexpr std::string_view kClearOnExitCookies = "privacy.clear_on_exit.cookies";
    static constexpr std::string_view kClearOnExitHistory = "privacy.clear_on_exit.history";
    static constexpr std::string_view kClearOnExitDownloads = "privacy.clear_on_exit.downloads";
    static constexpr std::string_view kCertAudit = "privacy.cert_audit";
    static constexpr std::string_view kRefererTrim = "privacy.referer_trim";
    static constexpr std::string_view kReducedMotion = "accessibility.reduced_motion";
    // User-collected sites (JSON arrays of {title, url}); both start empty.
    static constexpr std::string_view kHomeShortcuts = "home.shortcuts";
    static constexpr std::string_view kDockPins = "dock.pins";
    // Homepage weather: "auto" locates by IP; a JSON {name,lat,lon} pins a city.
    static constexpr std::string_view kWeatherLocation = "home.weather_location";
    // Main window placement from the last run: "maximized" or "normal", plus
    // the restored (non-maximized) rectangle as "x,y,w,h". Absent on first run,
    // where the browser starts maximized.
    static constexpr std::string_view kWindowState = "window.state";
    static constexpr std::string_view kWindowRect = "window.rect";
    // Per-site zoom: "zoom.<origin>" = Chromium zoom level as text (0 is
    // 100%; an absent key means default). See settings::SiteZoomStore.
    static constexpr std::string_view kZoomPrefix = "zoom.";
    // Night mode for page content: the global default (bool) and per-site
    // overrides "nightmode.<origin>" = "on" | "off". See SiteNightModeStore.
    static constexpr std::string_view kNightModeDefault = "content.night_mode";
    static constexpr std::string_view kNightModePrefix = "nightmode.";
    // Tor incognito windows: the SOCKS5 proxy of the user's own Tor daemon.
    // AURORA_TOR_PROXY overrides it for a single run.
    static constexpr std::string_view kTorProxy = "tor.proxy";
};

// The three Startup-card choices. Home opens one aurora://home tab, Continue
// restores the last session, Specific opens one tab at kStartupHomeUrl.
enum class StartupMode { Home, Continue, Specific };

// Stored spelling: "home" | "continue" | "specific". parse_startup_mode()
// returns false for anything else (instead of guessing Home) so the caller
// can fall back to the legacy kRestoreSession boolean.
std::string_view startup_mode_name(StartupMode mode);
bool parse_startup_mode(std::string_view text, StartupMode& out);

class Settings;

// The launch behaviour the stored preferences ask for. kStartupMode wins when
// it holds a known value; otherwise (an older profile, or a corrupted value)
// the kRestoreSession boolean decides, exactly as before the Startup card
// existed. "specific" with an empty URL degrades to Home so a half-finished
// choice never opens a blank tab.
StartupMode resolve_startup_mode(const Settings& prefs);

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

    // Every stored (key, value) pair whose key starts with `prefix`, sorted
    // by key, skipping empty values (PermissionStore "forgets" a decision by
    // writing an empty string). Reads SQLite directly — the cache only holds
    // keys someone asked for — so it is meant for listing pages, not hot
    // paths.
    std::vector<std::pair<std::string, std::string>>
    entries_with_prefix(std::string_view prefix) const;

    // Delete a key outright (the next get_* returns its fallback). No-op on a
    // read-only view, like every other write.
    void remove(std::string_view key);

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

// Aurora — in-app filter-list updater.
//
// Replaces the profile's adblock-rules.txt (the EasyList file
// scripts/fetch_filter_lists.sh writes and adblock::load_rules reads) with a
// freshly downloaded copy, but only once the download has proven itself:
//
//   1. fetch the list URL through a transport the host injects (the Qt app
//      uses QNetworkAccessManager, like WeatherService; tests use a fake), so
//      this file stays Qt-free and CEF-free like the rest of aurora_core;
//   2. validate the bytes — non-empty, text rather than an HTML error page or
//      a binary blob, a plausible number of filter lines, and (when the Rust
//      engine is linked) actually parseable by the engine;
//   3. write them to "<list>.part" and atomically rename over the live file,
//      keeping the previous list as "<list>.bak";
//   4. re-read the installed file and roll back to the backup if it does not
//      validate any more (a short write the OS did not report).
//
// A failure at any step leaves the live list exactly as it was. Nothing here
// runs on a timer: an update happens only when the user asks for one, in
// keeping with Aurora's no-background-traffic stance.
//
// Bookkeeping (last-success time, source URL) goes to the prefs store via
// record_success()/last_update(), which the caller invokes on the thread that
// owns the Settings object; install() itself only touches the filesystem, so
// the host may run it on a worker thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace aurora::settings { class Settings; }

namespace aurora::adblock {

// The list scripts/fetch_filter_lists.sh downloads (keep the two in sync).
inline constexpr std::string_view kDefaultFilterListUrl =
    "https://easylist.to/easylist/easylist.txt";
// File name inside the profile directory; main.cpp's load_rules() candidate.
inline constexpr std::string_view kFilterListFileName = "adblock-rules.txt";
// EasyList carries tens of thousands of filters. A list below this is a
// truncated download, a captive-portal page or the wrong file — never an
// update worth replacing a working list with.
inline constexpr std::size_t kMinFilterLines = 1000;
// Upper bound on what the host transport should accept (EasyList is a few
// MB). Protects the profile from an endless or hostile response.
inline constexpr std::size_t kMaxFilterListBytes = 32u * 1024u * 1024u;

struct FilterListCheck {
    bool ok = false;
    std::size_t filter_lines = 0;
    std::string error;  // human-readable reason when !ok
};

// Pure content validation (no filesystem). `min_filter_lines` is a parameter
// so tests can validate small fixtures.
FilterListCheck validate_filter_list(std::string_view content,
                                     std::size_t min_filter_lines = kMinFilterLines);

struct UpdateRecord {
    int64_t last_success_unix = 0;  // 0 = never updated in-app
    std::string source_url;
};

class ListUpdater {
public:
    struct Result {
        bool ok = false;
        std::string message;       // user-facing summary (success or failure)
        std::size_t filter_lines = 0;
        std::string source_url;
    };

    // Completion of one transport request. `body` is the full response on
    // success; `error` explains a failure (HTTP status, network error).
    using FetchDone =
        std::function<void(bool ok, std::string body, std::string error)>;
    // Host-supplied transport: GET `url`, then call `done` exactly once.
    using Fetcher = std::function<void(const std::string& url, FetchDone done)>;
    using ResultCallback = std::function<void(const Result&)>;

    explicit ListUpdater(std::string profile_dir,
                         std::string source_url = std::string(kDefaultFilterListUrl),
                         std::size_t min_filter_lines = kMinFilterLines);

    const std::string& source_url() const { return source_url_; }
    std::string list_path() const;    // <profile>/adblock-rules.txt
    std::string backup_path() const;  // <profile>/adblock-rules.txt.bak

    // True while an update() is between its fetch and its completion.
    bool busy() const { return busy_.load(); }

    // Fetch through `fetch`, then install() the body. `done` runs on
    // whichever thread the transport completes on, exactly once. A second
    // call while busy() fails immediately instead of racing the first.
    void update(const Fetcher& fetch, ResultCallback done);

    // Validate `content` and atomically make it the live list, keeping the
    // previous one as the backup. Filesystem only (no prefs), so it is safe
    // to call off the UI thread.
    Result install(std::string_view content) const;

    // Put the backup back in place of the live list. False when there is no
    // backup or the rename fails (the live list is then left untouched).
    bool rollback() const;

    // Prefs bookkeeping; call on the Settings owner's thread.
    static void record_success(settings::Settings& prefs, const Result& result,
                               int64_t now_unix);
    static UpdateRecord last_update(const settings::Settings& prefs);

private:
    std::string profile_dir_;
    std::string source_url_;
    std::size_t min_filter_lines_;
    std::atomic<bool> busy_{false};
};

} // namespace aurora::adblock

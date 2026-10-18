// Aurora — omnibox suggestions.
//
// Pure logic behind the dropdown that appears under the address bar while the
// user types: it merges the sources the window can read cheaply (history,
// bookmarks, open tabs) with the search engine's own query suggestions, ranks
// them, removes duplicates, and returns the rows to paint.
//
// This file has no Qt and no CEF dependency on purpose: the ranking is the
// part of the feature most likely to regress quietly, so it lives in the core
// library where it can be unit-tested headlessly. The UI layer only gathers
// the candidates and paints the result.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::omnibox {

enum class Kind {
    Navigate,      // what Enter does for URL-looking input: open it
    Search,        // what Enter does otherwise: search the default engine
    History,       // a page the user visited before
    Bookmark,      // a saved page
    OpenTab,       // a page that is already open — "switch to tab"
    RemoteSearch,  // a query the search engine suggested
};

struct Suggestion {
    Kind kind = Kind::Search;
    // Text placed into the address bar when the row is highlighted or chosen.
    // A URL for page rows, the search terms for search rows.
    std::string fill;
    // Primary label: page title, or the search terms.
    std::string title;
    // Secondary label: the URL for page rows, empty for search rows.
    std::string url;
    // Only meaningful for OpenTab.
    uint64_t tab_id = 0;
};

struct HistoryCandidate {
    std::string url;
    std::string title;
    int visit_count = 0;
    int typed_count = 0;
    int64_t last_visit_epoch = 0;  // seconds since the Unix epoch
};

struct BookmarkCandidate {
    std::string url;
    std::string title;
};

struct TabCandidate {
    std::string url;
    std::string title;
    uint64_t tab_id = 0;
    bool active = false;  // the tab the user is already on is never offered
};

struct Inputs {
    std::string query;
    // Name of the default search engine, for the "Search DuckDuckGo for ..."
    // label. May be empty.
    std::string engine_name;
    std::vector<HistoryCandidate> history;
    std::vector<BookmarkCandidate> bookmarks;
    std::vector<TabCandidate> tabs;
    // Query suggestions returned by the search engine, best first.
    std::vector<std::string> remote;
    int64_t now_epoch = 0;
};

inline constexpr size_t kDefaultMaxRows = 8;

// Ranked rows for `in.query`. The first row is always the default action —
// exactly what Enter would do for the typed text — so the list is never empty
// for non-blank input and the highlighted row never surprises the user.
std::vector<Suggestion> build_suggestions(const Inputs& in,
                                          size_t max_rows = kDefaultMaxRows);

// Whether typed text is worth sending to the search engine for completions.
// URL-looking input, internal pages and anything containing a scheme stay
// local: a half-typed address has no business leaving the machine.
bool should_request_remote(std::string_view query);

// The suggestion endpoint for a built-in engine key ("duckduckgo", "google",
// "bing"), as a template with one "%s". Empty when the engine has no known
// public endpoint (Startpage, custom engines): those get local rows only.
std::string suggest_url_template(std::string_view engine_key);

// Parses the OpenSearch suggestion payload every supported engine returns:
//   ["query", ["completion 1", "completion 2", ...], ...]
// Tolerant of extra trailing elements. Returns an empty list on malformed
// input; never more than `max_results` entries.
std::vector<std::string> parse_remote_suggestions(std::string_view json,
                                                  size_t max_results = 8);

} // namespace aurora::omnibox

// Aurora — URL type.
//
// A normalized, owning representation of a URL. This is NOT a full RFC 3986
// parser — Chromium/CEF already contains one of the best. This type exists so
// the browser chrome (omnibox, history, bookmarks) can hold, compare, and
// display URLs without dragging the entire CEF API into core. It parses the
// components we actually need for display and matching, and defers to CEF for
// anything security-sensitive (canonicalization of dangerous schemes, etc.).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aurora::core {

class Url {
public:
    Url() = default;
    explicit Url(std::string_view raw);

    // True if the input parses to a usable http/https/file/about/aurora/
    // chrome-extension URL or is a plausible search query. Used by the omnibox
    // to decide routing.
    bool is_valid() const { return valid_; }

    // Was the input treated as a search query rather than a URL?
    bool is_search_query() const { return search_query_; }

    // True if this is a chrome-extension://<id>/<path> URL. These are in-app
    // extension surfaces: never a search query, never dispatched externally.
    bool is_extension_url() const { return scheme_ == "chrome-extension"; }

    // True for schemes Aurora handles inside the browser rather than handing
    // to the OS default browser: aurora, chrome-extension, about, chrome.
    bool is_internal_scheme() const {
        return scheme_ == "aurora" || scheme_ == "chrome-extension" ||
               scheme_ == "about" || scheme_ == "chrome";
    }

    std::string_view scheme() const { return scheme_; }
    std::string_view host() const { return host_; }
    std::string_view path() const { return path_; }
    std::string_view query() const { return query_; }

    // The text the user typed, verbatim. Preserved for history and recovery.
    const std::string& raw() const { return raw_; }

    // A normalized, displayable string (scheme + host + path, no credentials).
    std::string display() const;

    // A simplified string suitable for history deduplication and matching.
    std::string normalized() const;

    // The origin (scheme://host[:port]) — empty for search queries.
    std::string origin() const;

    bool operator==(const Url& o) const { return normalized() == o.normalized(); }
    bool operator!=(const Url& o) const { return !(*this == o); }

private:
    std::string raw_;
    std::string scheme_;
    std::string host_;
    std::string path_;
    std::string query_;
    bool valid_ = false;
    bool search_query_ = false;
};

// Decide whether a typed string should be treated as a URL or a search query,
// mirroring the omnibox heuristic. Returns a Url whose is_search_query()
// reflects the decision.
Url parse_omnibox_input(std::string_view input);

// Search engines the settings page can choose between. The value stored in
// preferences is the identifier ("duckduckgo", "google", "bing", "startpage").
// Unknown identifiers fall back to DuckDuckGo.
std::string search_engine_template(std::string_view engine_id);
// The search engine's home page (not the search-results URL template).
// Used for the Home button and new-tab page: the home page IS the search
// engine, with no separate "startup homepage" option.
std::string search_engine_home(std::string_view engine_id);

// Resolve a parsed Url to a fully-loadable URL string. Search queries are
// routed to a search engine (URL-encoded); everything else is the display URL.
// The overload taking an engine template is what the omnibox uses so the
// user's configured engine is honored; the default keeps DuckDuckGo.
// A scheme-only upgrade, with no network probe or insecure retry. The host,
// explicit port, credentials, path, query and fragment are left untouched.
std::string upgrade_http_url(std::string_view url, bool https_only = true);
std::string resolve_navigation(const Url& url, bool https_only = true);
std::string resolve_navigation(const Url& url, std::string_view engine_template,
                               bool https_only = true);
// Keep string-literal engine templates from selecting the bool overload.
inline std::string resolve_navigation(const Url& url, const char* engine_template,
                                      bool https_only = true) {
    return resolve_navigation(url, std::string_view(engine_template), https_only);
}

} // namespace aurora::core

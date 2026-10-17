// Aurora — internal (aurora://) page URLs.
//
// The browser ships seven native pages — home (the new-tab page), history,
// downloads, bookmarks, extensions, settings and site settings — that are
// rendered by Qt widgets rather than by the web engine. They are addressable
// so the omnibox, the browser menu and session restore all speak the same
// language: `aurora://home`, `aurora://history`, `aurora://downloads`,
// `aurora://bookmarks`, `aurora://extensions`, `aurora://settings`,
// `aurora://site-settings`.
//
// chrome-extension:// URLs are not Qt pages, but they are also in-app: they
// must load inside Aurora and must never be handed to an external browser.
//
// This lives in core (no Qt, no CEF) so the routing rules are unit-tested
// without a display.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace aurora::core {

enum class InternalPage {
    Home,       // the aurora://home new-tab page (aurora://newtab aliases it)
    History,
    Downloads,
    Bookmarks,
    Extensions,
    Settings,
    SiteSettings,  // per-site permissions, zoom and Shields exceptions
};

// Scheme used by the internal pages, without the "://".
constexpr std::string_view kInternalScheme = "aurora";

// Scheme used by packaged/unpacked Chrome extensions loaded in-app.
constexpr std::string_view kExtensionScheme = "chrome-extension";

// Parse an omnibox string into an internal page. Accepts, case-insensitively
// and with optional trailing slash/whitespace:
//   aurora://history, aurora:history, AURORA://History/
// Returns nullopt for anything else (including an unknown aurora:// host,
// which the caller should treat as "not an internal page").
// chrome-extension:// URLs are in-app but are not InternalPage values — use
// is_extension_scheme() / is_internal_url() for those.
std::optional<InternalPage> parse_internal_url(std::string_view input);

// True if `input` uses the chrome-extension: scheme (case-insensitive, with
// optional whitespace). The rest of the URL (id/path) is not validated here.
bool is_extension_scheme(std::string_view input);

// True if `input` is an in-app navigation that must never be dispatched to
// an external browser: an aurora: URL (known page or otherwise) or a
// chrome-extension: URL.
bool is_internal_url(std::string_view input);

// The canonical URL for a page, e.g. "aurora://settings".
std::string internal_url(InternalPage page);

// Human-readable title, used for the tab label and page header.
std::string internal_page_title(InternalPage page);

} // namespace aurora::core

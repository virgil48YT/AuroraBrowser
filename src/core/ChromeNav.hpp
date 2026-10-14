// Aurora — chrome-level (per-tab) back/forward stack.
//
// The web engine keeps its own history for real pages, but Aurora's internal
// pages (aurora://history, aurora://bookmarks, …) are Qt widgets the engine
// knows nothing about: navigating to one does not touch CEF's stack, so
// CanGoBack() stays false and the Back button was dead on every internal page.
//
// This stack records BOTH kinds of navigation — internal pages and the web
// URLs they were opened from — so the toolbar can step back out of an internal
// page and land on whatever the tab showed before it. It deliberately does NOT
// try to mirror CEF's per-page history (link clicks, redirects, form posts):
// while a tab shows web content the engine remains the authority, and this
// stack only supplies the boundary entries around internal pages.
//
// chrome-extension:// URLs are in-app (like aurora:// pages): they must never
// be treated as an external / OS-browser target.
//
// Pure value type, no Qt and no CEF, so the rules are unit-tested headlessly.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/InternalUrl.hpp"
#include "core/Url.hpp"

namespace aurora::core {

// One step of the chrome-level history: either an internal page or a web URL.
struct ChromeNavEntry {
    // Set for internal pages; nullopt means "web content at `url`".
    std::optional<InternalPage> page;
    Url url;
    std::string title;

    bool is_internal() const { return page.has_value(); }

    // aurora:// Qt pages AND chrome-extension:// surfaces. Callers that would
    // otherwise open the OS default browser must treat these as in-app.
    bool is_in_app() const {
        return page.has_value() || url.is_extension_url() ||
               url.is_internal_scheme() || is_internal_url(url.raw());
    }
};

// True when a URL must stay inside Aurora (internal pages and
// chrome-extension:// URLs). Never hand these to an external browser.
inline bool is_in_app_navigation(const Url& url) {
    if (url.is_extension_url() || url.is_internal_scheme()) return true;
    return is_internal_url(url.raw());
}

class ChromeNavStack {
public:
    // Push a navigation, truncating any forward entries (standard browser
    // behavior). Re-recording the entry the stack is already sitting on only
    // refreshes its title/URL instead of growing the stack, so repeatedly
    // opening the same internal page cannot bury the page behind it.
    void record(const ChromeNavEntry& entry);

    bool can_go_back() const { return index_ > 0 && !entries_.empty(); }
    bool can_go_forward() const {
        return !entries_.empty() && index_ + 1 < entries_.size();
    }

    const ChromeNavEntry* current() const;
    // The entry Back / Forward would land on, without moving.
    const ChromeNavEntry* peek_back() const;
    const ChromeNavEntry* peek_forward() const;

    // Move one step and return the new current entry (nullptr if not possible).
    const ChromeNavEntry* go_back();
    const ChromeNavEntry* go_forward();

    // A main-frame address change reported by the engine. The tab is on web
    // content, so it refines the current web entry rather than pushing a new
    // one (CEF owns intra-page history). Pushes a first entry when the stack is
    // empty, and does nothing when the current entry is an internal page or a
    // chrome-extension:// surface.
    void note_web_url(const Url& url, const std::string& title = {});

    size_t size() const { return entries_.size(); }
    size_t index() const { return index_; }
    bool empty() const { return entries_.empty(); }

private:
    std::vector<ChromeNavEntry> entries_;
    size_t index_ = 0;
};

} // namespace aurora::core

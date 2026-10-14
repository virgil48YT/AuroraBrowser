// Aurora — browsing history service.
//
// History is append-mostly and high-write. To keep the UI thread free, the
// browser process owns the HistoryService on a background sequence and posts
// visit notifications to it. Reads (omnibox suggestions, the history page)
// also run off the UI thread and return results via a callback.
//
// The service itself is synchronous and stateless beyond its Database handle,
// which keeps it trivially unit-testable: a test opens an in-memory Database,
// migrates it, and asserts on visit queries.
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "core/Url.hpp"
#include "storage/Database.hpp"

namespace aurora::core {
struct NavigationEntry;
}

namespace aurora::history {

struct Visit {
    int64_t id = 0;
    std::string url;
    std::string title;
    std::string favicon;
    int visit_count = 0;
    int typed_count = 0;
    std::chrono::system_clock::time_point last_visit;
};

class HistoryService {
public:
    explicit HistoryService(storage::Database& db) : db_(db) {}

    // Record (or bump) a visit. Idempotent on url: a repeated visit increments
    // visit_count and updates last_visit instead of inserting a duplicate.
    void add_visit(const core::Url& url, std::string_view title,
                   std::string_view favicon, bool typed);

    // Set the title (and optionally the favicon) of an already-recorded visit.
    // Titles arrive *after* the navigation commits: at the moment a URL is
    // recorded the page has not parsed its <title> yet, so the visit is stored
    // with an empty title and the history page falls back to showing the URL.
    // The host calls this when the title callback arrives. No-op if the URL was
    // never visited, and an empty title never overwrites a good one.
    void set_title(const core::Url& url, std::string_view title,
                   std::string_view favicon = {});

    // Up to `limit` most-recent visits, newest first.
    std::vector<Visit> recent(size_t limit) const;

    // Full-text search across url + title, ranked by relevance then recency.
    std::vector<Visit> search(std::string_view query, size_t limit) const;

    // Remove a single entry by id. Returns true if a row was deleted.
    bool remove(int64_t id);

    // Remove everything older than `cutoff`. Returns the number deleted.
    int remove_older_than(std::chrono::system_clock::time_point cutoff);

    // Clear all history.
    int clear();

private:
    storage::Database& db_;
};

} // namespace aurora::history

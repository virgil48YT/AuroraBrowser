#include "session/SessionService.hpp"

#include "core/InternalUrl.hpp"
#include "core/Url.hpp"
#include "util/Log.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>

namespace aurora::session {

namespace {
int64_t to_epoch(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               t.time_since_epoch())
        .count();
}
std::chrono::system_clock::time_point from_epoch(int64_t s) {
    return std::chrono::system_clock::from_time_t(static_cast<time_t>(s));
}

// Longest URL we will ever hand to the engine from a stored session. Chromium
// itself gives up well before this; a longer value means the row is corrupt.
constexpr size_t kMaxRestoredUrlLength = 8192;

bool all_whitespace(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

std::string lowered(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}
} // namespace

bool is_restorable_url(std::string_view url) {
    if (url.empty() || all_whitespace(url)) return false;
    if (url.size() > kMaxRestoredUrlLength) return false;

    // Aurora's own internal pages are restorable and are not web URLs.
    if (core::parse_internal_url(url).has_value()) return true;

    const std::string lower = lowered(url);
    static constexpr std::string_view kBlockedSchemes[] = {
        "javascript:", "data:", "blob:", "vbscript:", "filesystem:"};
    for (auto blocked : kBlockedSchemes) {
        if (lower.compare(0, blocked.size(), blocked) == 0) return false;
    }

    // file:// URLs have no host, so core::Url treats them as opaque; accept
    // them here as long as they carry a path.
    if (lower.compare(0, 8, "file:///") == 0 && lower.size() > 8) return true;

    // Anything else must parse as a real URL — not a search query, which would
    // silently turn a corrupt row into a web search on startup.
    const core::Url parsed(url);
    if (!parsed.is_valid() || parsed.is_search_query()) return false;
    const auto scheme = parsed.scheme();
    return scheme == "http" || scheme == "https" || scheme == "file" ||
           scheme == "about" || scheme == "aurora";
}

RestorePlan plan_restore(const SavedSession& session, size_t max_tabs) {
    RestorePlan plan;
    plan.tabs.reserve(session.tabs.size() < max_tabs ? session.tabs.size()
                                                     : max_tabs);
    for (const auto& tab : session.tabs) {
        if (!is_restorable_url(tab.url)) {
            ++plan.skipped;
            continue;
        }
        if (plan.tabs.size() >= max_tabs) {
            ++plan.skipped;
            continue;
        }
        plan.tabs.push_back(tab);
    }
    // Stored rows can arrive in any order if the table was written by an older
    // build or edited by hand; the UI relies on tab order being meaningful.
    std::stable_sort(plan.tabs.begin(), plan.tabs.end(),
                     [](const SavedTab& a, const SavedTab& b) {
                         if (a.window_index != b.window_index) {
                             return a.window_index < b.window_index;
                         }
                         return a.tab_index < b.tab_index;
                     });
    return plan;
}

int64_t SessionService::save(const std::vector<SavedTab>& tabs, size_t keep) {
    storage::Transaction txn(db_);
    int64_t now = to_epoch(std::chrono::system_clock::now());
    int64_t session_id = 0;
    if (auto s = db_.prepare(
            "INSERT INTO sessions(created_at, is_incognito) VALUES (?1, 0);")) {
        s->bind(1, now);
        if (auto r = s->step(); r.has_value() && !*r) {
            session_id = sqlite3_last_insert_rowid(db_.handle());
        }
    }
    if (session_id == 0) {
        txn.rollback();
        return 0;
    }
    auto ins = db_.prepare(
        "INSERT INTO session_tabs(session_id, window_index, tab_index, url, title, pinned) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
    if (!ins) {
        txn.rollback();
        return 0;
    }
    for (const auto& t : tabs) {
        ins->bind(1, session_id);
        ins->bind(2, t.window_index);
        ins->bind(3, t.tab_index);
        ins->bind(4, t.url);
        ins->bind(5, t.title);
        ins->bind(6, t.pinned ? 1 : 0);
        ins->step();
        ins->reset();
    }
    // Prune old sessions beyond `keep`.
    if (auto prune = db_.prepare(
            "DELETE FROM sessions WHERE id NOT IN "
            "(SELECT id FROM sessions ORDER BY created_at DESC, id DESC LIMIT ?1);")) {
        prune->bind(1, static_cast<int64_t>(keep));
        prune->step();
    }
    txn.commit();
    return session_id;
}

std::optional<SavedSession> SessionService::load_last() const {
    int64_t id = 0;
    if (auto s = db_.prepare(
            "SELECT id FROM sessions ORDER BY created_at DESC, id DESC LIMIT 1;")) {
        for (auto r = s->step(); r && *r; r = s->step()) {
            id = s->column_int64(0);
        }
    }
    if (id == 0) return std::nullopt;
    return load_one(id);
}

std::optional<SavedSession> SessionService::load(int64_t id) const {
    return load_one(id);
}

std::optional<SavedSession> SessionService::load_one(int64_t id) const {
    SavedSession out;
    out.id = id;
    if (auto s = db_.prepare("SELECT created_at FROM sessions WHERE id = ?1;")) {
        s->bind(1, id);
        bool found = false;
        for (auto r = s->step(); r && *r; r = s->step()) {
            out.created_at = from_epoch(s->column_int64(0));
            found = true;
        }
        if (!found) return std::nullopt;
    }
    if (auto tabs = db_.prepare(
            "SELECT window_index, tab_index, url, title, pinned "
            "FROM session_tabs WHERE session_id = ?1 ORDER BY window_index, tab_index;")) {
        tabs->bind(1, id);
        for (auto r = tabs->step(); r && *r; r = tabs->step()) {
            SavedTab t;
            t.window_index = tabs->column_int(0);
            t.tab_index = tabs->column_int(1);
            t.url = tabs->column_text(2);
            t.title = tabs->column_text(3);
            t.pinned = tabs->column_int(4) != 0;
            out.tabs.push_back(t);
        }
    }
    return out;
}

} // namespace aurora::session

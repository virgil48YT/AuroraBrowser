#include "history/HistoryService.hpp"

#include "util/Log.hpp"

#include <sqlite3.h>

namespace aurora::history {

namespace {
int64_t to_epoch(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               t.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point from_epoch(int64_t s) {
    return std::chrono::system_clock::from_time_t(static_cast<time_t>(s));
}

Visit row_to_visit(const storage::Statement& stmt) {
    Visit v;
    v.id = stmt.column_int64(0);
    v.url = stmt.column_text(1);
    v.title = stmt.column_text(2);
    v.favicon = stmt.column_text(6);
    v.visit_count = stmt.column_int(3);
    v.typed_count = stmt.column_int(4);
    v.last_visit = from_epoch(stmt.column_int64(5));
    return v;
}
} // namespace

void HistoryService::add_visit(const core::Url& url, std::string_view title,
                              std::string_view favicon, bool typed) {
    std::string url_str = url.normalized();
    if (url_str.empty()) return;

    // Upsert: bump visit_count + typed_count on conflict, else insert.
    auto stmt = db_.prepare(R"SQL(
        INSERT INTO history(url, title, favicon, visit_count, typed_count, last_visit)
        VALUES (?1, ?2, ?3, 1, ?4, ?5)
        ON CONFLICT(url) DO UPDATE SET
            visit_count = visit_count + 1,
            typed_count = typed_count + ?4,
            last_visit = excluded.last_visit,
            -- Never let a re-visit blank out a title or favicon we already
            -- know. A visit is recorded the instant the navigation commits,
            -- before the page has a title, so excluded.title is usually empty;
            -- assigning it unconditionally wiped the stored title and made
            -- every history row fall back to displaying its URL.
            title = CASE WHEN excluded.title <> '' THEN excluded.title
                         ELSE history.title END,
            favicon = CASE WHEN excluded.favicon <> '' THEN excluded.favicon
                           ELSE history.favicon END;
    )SQL");
    if (!stmt) return;
    int typed_inc = typed ? 1 : 0;
    int64_t now = to_epoch(std::chrono::system_clock::now());
    stmt->bind(1, url_str);
    stmt->bind(2, title);
    stmt->bind(3, favicon);
    stmt->bind(4, typed_inc);
    stmt->bind(5, now);
    stmt->step();
}

void HistoryService::set_title(const core::Url& url, std::string_view title,
                               std::string_view favicon) {
    const std::string url_str = url.normalized();
    if (url_str.empty() || title.empty()) return;
    auto stmt = db_.prepare(
        "UPDATE history SET title = ?1, "
        "favicon = CASE WHEN ?2 <> '' THEN ?2 ELSE favicon END "
        "WHERE url = ?3;");
    if (!stmt) return;
    stmt->bind(1, title);
    stmt->bind(2, favicon);
    stmt->bind(3, url_str);
    stmt->step();
}

std::vector<Visit> HistoryService::recent(size_t limit) const {
    std::vector<Visit> out;
    auto stmt = db_.prepare(
        "SELECT id, url, title, visit_count, typed_count, last_visit, favicon "
        "FROM history ORDER BY last_visit DESC, id DESC LIMIT ?1;");
    if (!stmt) return out;
    stmt->bind(1, static_cast<int64_t>(limit));
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        out.push_back(row_to_visit(*stmt));
    }
    return out;
}

std::vector<Visit> HistoryService::search(std::string_view query,
                                        size_t limit) const {
    std::vector<Visit> out;
    // FTS5 MATCH with a trailing wildcard so partial matches surface, e.g.
    // typing "git" matches "github.com".
    std::string q(query);
    // Escape double quotes by doubling them.
    for (size_t i = 0; i < q.size(); ++i) {
        if (q[i] == '"') q.insert(i++, 1, '"');
    }
    std::string match = std::string("\"") + q + "\"*";
    std::string sql =
        "SELECT h.id, h.url, h.title, h.visit_count, h.typed_count, "
        "h.last_visit, h.favicon "
        "FROM history_fts JOIN history h ON h.id = history_fts.rowid "
        "WHERE history_fts MATCH ?1 ORDER BY rank, h.last_visit DESC LIMIT ?2;";
    auto stmt = db_.prepare(sql);
    if (!stmt) return out;
    stmt->bind(1, match);
    stmt->bind(2, static_cast<int64_t>(limit));
    // step() returns nullopt on error; a malformed MATCH lands here.
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        out.push_back(row_to_visit(*stmt));
    }
    return out;
}

bool HistoryService::remove(int64_t id) {
    auto stmt = db_.prepare("DELETE FROM history WHERE id = ?1;");
    if (!stmt) return false;
    stmt->bind(1, id);
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

int HistoryService::remove_older_than(std::chrono::system_clock::time_point cutoff) {
    auto stmt = db_.prepare("DELETE FROM history WHERE last_visit < ?1;");
    if (!stmt) return 0;
    stmt->bind(1, to_epoch(cutoff));
    stmt->step();
    return sqlite3_changes(db_.handle());
}

int HistoryService::clear() {
    auto stmt = db_.prepare("DELETE FROM history;");
    if (!stmt) return 0;
    stmt->step();
    return sqlite3_changes(db_.handle());
}

} // namespace aurora::history

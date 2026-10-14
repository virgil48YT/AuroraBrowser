#include "downloads/DownloadService.hpp"

#include "util/Log.hpp"

#include <sqlite3.h>

namespace aurora::downloads {

namespace {

int64_t to_epoch(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch())
        .count();
}

std::chrono::system_clock::time_point from_epoch(int64_t s) {
    return std::chrono::system_clock::from_time_t(static_cast<time_t>(s));
}

constexpr const char* kSelectColumns =
    "id, url, target_path, mime, total_bytes, received, state, started_at, "
    "finished_at";

DownloadItem row_to_item(const storage::Statement& stmt) {
    DownloadItem d;
    d.id = stmt.column_int64(0);
    d.url = stmt.column_text(1);
    d.target_path = stmt.column_text(2);
    d.mime = stmt.column_text(3);
    d.total_bytes = stmt.column_int64(4);
    d.received = stmt.column_int64(5);
    d.state = state_from_string(stmt.column_text(6));
    d.started_at = from_epoch(stmt.column_int64(7));
    if (!stmt.column_is_null(8)) {
        d.finished_at = from_epoch(stmt.column_int64(8));
    }
    return d;
}

} // namespace

std::string to_string(DownloadState state) {
    switch (state) {
        case DownloadState::InProgress:  return "in_progress";
        case DownloadState::Paused:      return "paused";
        case DownloadState::Complete:    return "complete";
        case DownloadState::Cancelled:   return "cancelled";
        case DownloadState::Interrupted: return "interrupted";
    }
    return "in_progress";
}

DownloadState state_from_string(std::string_view text) {
    if (text == "complete") return DownloadState::Complete;
    if (text == "cancelled") return DownloadState::Cancelled;
    if (text == "interrupted") return DownloadState::Interrupted;
    if (text == "paused") return DownloadState::Paused;
    return DownloadState::InProgress;
}

std::string DownloadItem::file_name() const {
    const auto slash = target_path.find_last_of("/\\");
    if (slash == std::string::npos) return target_path;
    return target_path.substr(slash + 1);
}

int DownloadItem::percent() const {
    if (total_bytes <= 0) return -1;
    const auto capped = received > total_bytes ? total_bytes : received;
    return static_cast<int>((capped * 100) / total_bytes);
}

int64_t DownloadService::begin(std::string_view url, std::string_view target_path,
                               std::string_view mime, int64_t total_bytes) {
    if (url.empty()) return 0;
    auto stmt = db_.prepare(
        "INSERT INTO downloads(url, target_path, mime, total_bytes, received, "
        "state, started_at) VALUES (?1, ?2, ?3, ?4, 0, ?5, ?6);");
    if (!stmt) return 0;
    stmt->bind(1, url);
    stmt->bind(2, target_path);
    stmt->bind(3, mime);
    stmt->bind(4, total_bytes);
    stmt->bind(5, to_string(DownloadState::InProgress));
    stmt->bind(6, to_epoch(std::chrono::system_clock::now()));
    auto r = stmt->step();
    if (!r.has_value() || *r) {
        AURORA_LOG(util::LogLevel::Warn, "downloads")
            << "begin failed: " << db_.last_error_message();
        return 0;
    }
    return sqlite3_last_insert_rowid(db_.handle());
}

bool DownloadService::update_progress(int64_t id, int64_t received,
                                      int64_t total_bytes) {
    auto stmt = db_.prepare(
        "UPDATE downloads SET received = ?2, total_bytes = ?3, "
        "state = CASE WHEN state = 'interrupted' THEN 'in_progress' "
        "ELSE state END, "
        "finished_at = CASE WHEN state = 'interrupted' THEN NULL "
        "ELSE finished_at END "
        "WHERE id = ?1 AND state IN ('in_progress', 'paused', 'interrupted');");
    if (!stmt) return false;
    stmt->bind(1, id);
    stmt->bind(2, received);
    stmt->bind(3, total_bytes);
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

bool DownloadService::set_target_path(int64_t id, std::string_view path) {
    if (path.empty()) return false;
    auto stmt = db_.prepare(
        "UPDATE downloads SET target_path = ?2 WHERE id = ?1;");
    if (!stmt) return false;
    stmt->bind(1, id);
    stmt->bind(2, path);
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

bool DownloadService::finish(int64_t id, DownloadState state) {
    if (state == DownloadState::InProgress || state == DownloadState::Paused)
        return false;
    // Called with a terminal state reported by the engine. A paused row may
    // legitimately receive one: the engine can cancel (or complete a nearly
    // done transfer) while it is paused, and nothing else would ever move the
    // row out of 'paused' — so finish accepts paused as a source state too.
    // 'complete'/'cancelled' remain monotonic, and interrupted keeps its
    // "fully received => actually complete" correction.
    auto stmt = db_.prepare(
        "UPDATE downloads SET "
        "state = CASE WHEN ?2 = 'interrupted' AND total_bytes > 0 "
        "AND received >= total_bytes THEN 'complete' ELSE ?2 END, "
        "finished_at = ?3 "
        // Terminal rows are never rewritten. The guard lives in the WHERE
        // clause, not just a CASE fallback, because SQLite counts a matched
        // row as a change even when every written value is identical — a
        // CASE-only guard would leave the state correct but report a
        // transition (sqlite3_changes) that did not happen.
        "WHERE id = ?1 AND state NOT IN ('complete', 'cancelled');");
    if (!stmt) return false;
    stmt->bind(1, id);
    stmt->bind(2, to_string(state));
    stmt->bind(3, to_epoch(std::chrono::system_clock::now()));
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

bool DownloadService::pause(int64_t id) {
    auto stmt = db_.prepare(
        "UPDATE downloads SET state = 'paused' "
        "WHERE id = ?1 AND state = 'in_progress';");
    if (!stmt) return false;
    stmt->bind(1, id);
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

bool DownloadService::resume(int64_t id) {
    auto stmt = db_.prepare(
        "UPDATE downloads SET state = 'in_progress' "
        "WHERE id = ?1 AND state = 'paused';");
    if (!stmt) return false;
    stmt->bind(1, id);
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

// True for CEF/Chromium component & extension-update payload URLs served
// from clients2.googleusercontent.com (or the legacy clients2.google.com)
// with a .crx/.crx3 file at the end of the path. These are not user
// downloads; they are intercepted elsewhere (CefAdapter) and rerouted, but
// older rows may already exist in the DB (e.g. ones that slipped through
// before detection was broadened) and show as stuck 0% entries in the
// popover -- filter them out of the list shown to the user.
static bool is_google_component_crx(const std::string& url) {
    if (url.find("clients2.googleusercontent.com") == std::string::npos &&
        url.find("clients2.google.com") == std::string::npos) {
        return false;
    }
    return url.ends_with(".crx") || url.ends_with(".crx3");
}

std::vector<DownloadItem> DownloadService::recent(size_t limit) const {
    std::vector<DownloadItem> out;
    auto stmt = db_.prepare(std::string("SELECT ") + kSelectColumns +
                            " FROM downloads ORDER BY started_at DESC, id DESC "
                            "LIMIT ?1;");
    if (!stmt) return out;
    stmt->bind(1, static_cast<int64_t>(limit));
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        auto item = row_to_item(*stmt);
        if (is_google_component_crx(item.url)) continue;  // hide CEF component downloads
        out.push_back(std::move(item));
    }
    return out;
}

std::optional<DownloadItem> DownloadService::find(int64_t id) const {
    auto stmt = db_.prepare(std::string("SELECT ") + kSelectColumns +
                            " FROM downloads WHERE id = ?1;");
    if (!stmt) return std::nullopt;
    stmt->bind(1, id);
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        return row_to_item(*stmt);
    }
    return std::nullopt;
}

bool DownloadService::remove(int64_t id) {
    auto stmt = db_.prepare("DELETE FROM downloads WHERE id = ?1;");
    if (!stmt) return false;
    stmt->bind(1, id);
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

int DownloadService::clear_finished() {
    auto stmt = db_.prepare(
        "DELETE FROM downloads WHERE state NOT IN ('in_progress', 'paused');");
    if (!stmt) return 0;
    stmt->step();
    return sqlite3_changes(db_.handle());
}

int DownloadService::clear() {
    auto stmt = db_.prepare("DELETE FROM downloads;");
    if (!stmt) return 0;
    stmt->step();
    return sqlite3_changes(db_.handle());
}

} // namespace aurora::downloads

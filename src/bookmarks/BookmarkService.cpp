#include "bookmarks/BookmarkService.hpp"

#include "util/Log.hpp"

#include <sqlite3.h>

namespace aurora::bookmarks {

using util::LogLevel;

namespace {
std::chrono::system_clock::time_point from_epoch(int64_t s) {
    return std::chrono::system_clock::from_time_t(static_cast<time_t>(s));
}
int64_t to_epoch(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               t.time_since_epoch())
        .count();
}

BookmarkNode row_to_node(const storage::Statement& stmt) {
    BookmarkNode n;
    n.id = stmt.column_int64(0);
    n.parent_id = stmt.column_int64(1);
    n.title = stmt.column_text(2);
    n.url = stmt.column_text(3);
    n.position = stmt.column_int(4);
    n.is_folder = stmt.column_int(5) != 0;
    n.added_at = from_epoch(stmt.column_int64(6));
    return n;
}
} // namespace

std::vector<BookmarkNode> BookmarkService::children(int64_t parent_id) const {
    std::vector<BookmarkNode> out;
    auto stmt = db_.prepare(
        "SELECT id, parent_id, title, url, position, is_folder, added_at "
        "FROM bookmarks WHERE parent_id = ?1 AND id != 0 ORDER BY position, id;");
    if (!stmt) return out;
    stmt->bind(1, parent_id);
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        out.push_back(row_to_node(*stmt));
    }
    return out;
}

int64_t BookmarkService::add(int64_t parent_id, std::string_view title,
                              std::string_view url, bool is_folder,
                              std::chrono::system_clock::time_point added_at) {
    auto parent = db_.prepare("SELECT is_folder FROM bookmarks WHERE id = ?1;");
    if (!parent) return 0;
    parent->bind(1, parent_id);
    const auto found = parent->step();
    if (!found || !*found || parent->column_int(0) == 0) return 0;
    // Next position = current max + 1 for the parent.
    int position = 0;
    if (auto s = db_.prepare(
            "SELECT COALESCE(MAX(position)+1, 0) FROM bookmarks WHERE parent_id = ?1 AND id != 0;")) {
        s->bind(1, parent_id);
        if (auto r = s->step(); r && *r) position = s->column_int(0);
    }
    auto stmt = db_.prepare(
        "INSERT INTO bookmarks(parent_id, title, url, position, is_folder, added_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
    if (!stmt) return 0;
    stmt->bind(1, parent_id);
    stmt->bind(2, title);
    stmt->bind(3, url);
    stmt->bind(4, position);
    stmt->bind(5, is_folder ? 1 : 0);
    stmt->bind(6, to_epoch(added_at));
    auto r = stmt->step();
    if (!r.has_value() || *r) {
        AURORA_LOG(LogLevel::Warn, "bookmarks") << "add failed: " << db_.last_error_message();
        return 0;
    }
    return sqlite3_last_insert_rowid(db_.handle());
}

bool BookmarkService::update(int64_t id, std::string_view title,
                            std::string_view url) {
    auto stmt = db_.prepare(
        "UPDATE bookmarks SET title = ?2, url = ?3 WHERE id = ?1 AND is_folder = 0;");
    if (!stmt) return false;
    stmt->bind(1, id);
    stmt->bind(2, title);
    stmt->bind(3, url);
    auto r = stmt->step();
    return r.has_value() && !*r;
}

bool BookmarkService::move(int64_t id, int64_t new_parent_id, int new_position) {
    storage::Transaction txn(db_);
    renumber_siblings(new_parent_id);
    // Shift siblings at/after the target position to make room.
    auto shift = db_.prepare(
        "UPDATE bookmarks SET position = position + 1 "
        "WHERE parent_id = ?1 AND position >= ?2;");
    if (!shift) return false;
    shift->bind(1, new_parent_id);
    shift->bind(2, new_position);
    shift->step();
    auto update = db_.prepare(
        "UPDATE bookmarks SET parent_id = ?2, position = ?3 WHERE id = ?1;");
    if (!update) return false;
    update->bind(1, id);
    update->bind(2, new_parent_id);
    update->bind(3, new_position);
    update->step();
    renumber_siblings(new_parent_id);
    // Also compact the old parent (we don't know it here; do a global compact
    // of all parents to be safe — cheap for typical bookmark counts).
    if (auto s = db_.prepare("SELECT DISTINCT parent_id FROM bookmarks;")) {
        for (auto rr = s->step(); rr && *rr; rr = s->step()) {
            renumber_siblings(s->column_int64(0));
        }
    }
    txn.commit();
    return true;
}

bool BookmarkService::remove(int64_t id) {
    auto stmt = db_.prepare("DELETE FROM bookmarks WHERE id = ?1;");
    if (!stmt) return false;
    stmt->bind(1, id);
    auto r = stmt->step();
    if (!(r.has_value() && !*r)) return false;
    // Compact sibling positions so reordering stays consistent.
    if (auto s = db_.prepare("SELECT DISTINCT parent_id FROM bookmarks;")) {
        for (auto rr = s->step(); rr && *rr; rr = s->step()) {
            renumber_siblings(s->column_int64(0));
        }
    }
    return true;
}

std::vector<BookmarkNode> BookmarkService::all_bookmarks() const {
    std::vector<BookmarkNode> out;
    auto stmt = db_.prepare(
        "SELECT id, parent_id, title, url, position, is_folder, added_at "
        "FROM bookmarks WHERE is_folder = 0 ORDER BY title;");
    if (!stmt) return out;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        out.push_back(row_to_node(*stmt));
    }
    return out;
}

void BookmarkService::renumber_siblings(int64_t parent_id) {
    auto stmt = db_.prepare(
        "SELECT id FROM bookmarks WHERE parent_id = ?1 AND id != 0 ORDER BY position, id;");
    if (!stmt) return;
    stmt->bind(1, parent_id);
    std::vector<int64_t> ids;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        ids.push_back(stmt->column_int64(0));
    }
    int pos = 0;
    for (int64_t id : ids) {
        if (auto u = db_.prepare("UPDATE bookmarks SET position = ?2 WHERE id = ?1;")) {
            u->bind(1, id);
            u->bind(2, pos);
            u->step();
        }
        ++pos;
    }
}

} // namespace aurora::bookmarks

#include "storage/Migration.hpp"

#include "util/Log.hpp"

#include <sqlite3.h>

namespace aurora::storage {

using util::LogLevel;

namespace {

// V0 -> V1: history table.
DbError create_history(Database& db) {
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS history (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            url         TEXT NOT NULL UNIQUE,
            title       TEXT NOT NULL DEFAULT '',
            visit_count INTEGER NOT NULL DEFAULT 1,
            typed_count INTEGER NOT NULL DEFAULT 0,
            last_visit  INTEGER NOT NULL,  -- epoch seconds
            favicon     TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_history_last_visit ON history(last_visit DESC);
        CREATE INDEX IF NOT EXISTS idx_history_url ON history(url);
        CREATE VIRTUAL TABLE IF NOT EXISTS history_fts USING fts5(
            url, title, content='history', content_rowid='id'
        );
    )SQL";
    auto err = db.execute(sql);
    if (err != DbError::Ok) return err;
    // Trigger to keep the FTS table in sync.
    return db.execute(R"SQL(
        CREATE TRIGGER IF NOT EXISTS history_ai AFTER INSERT ON history BEGIN
            INSERT INTO history_fts(rowid, url, title)
            VALUES (new.id, new.url, new.title);
        END;
        CREATE TRIGGER IF NOT EXISTS history_ad AFTER DELETE ON history BEGIN
            INSERT INTO history_fts(history_fts, rowid, url, title)
            VALUES ('delete', old.id, old.url, old.title);
        END;
        CREATE TRIGGER IF NOT EXISTS history_au AFTER UPDATE ON history BEGIN
            INSERT INTO history_fts(history_fts, rowid, url, title)
            VALUES ('delete', old.id, old.url, old.title);
            INSERT INTO history_fts(rowid, url, title)
            VALUES (new.id, new.url, new.title);
        END;
    )SQL");
}

// V1 -> V2: bookmarks (folders + items, self-referential parent).
// parent_id = 0 is the implicit root. A self-referential FK is DEFERRABLE so
// the root row (id=0, parent_id=0) can be inserted without violating the
// constraint before it exists.
DbError create_bookmarks(Database& db) {
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS bookmarks (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            parent_id INTEGER NOT NULL DEFAULT 0
                      REFERENCES bookmarks(id) ON DELETE CASCADE
                      DEFERRABLE INITIALLY DEFERRED,
            title     TEXT NOT NULL DEFAULT '',
            url       TEXT NOT NULL DEFAULT '',
            position  INTEGER NOT NULL DEFAULT 0,
            is_folder INTEGER NOT NULL DEFAULT 0,
            added_at INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_bookmarks_parent ON bookmarks(parent_id, position);
        INSERT OR IGNORE INTO bookmarks(id, parent_id, title, url, position, is_folder, added_at)
        VALUES (0, 0, 'Bookmarks', '', 0, 1, 0);
    )SQL";
    return db.execute(sql);
}

// V2 -> V3: downloads and sessions.
DbError create_downloads_and_sessions(Database& db) {
    return db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS downloads (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            url         TEXT NOT NULL,
            target_path TEXT NOT NULL,
            mime        TEXT NOT NULL DEFAULT '',
            total_bytes INTEGER NOT NULL DEFAULT -1,
            received    INTEGER NOT NULL DEFAULT 0,
            state       TEXT NOT NULL DEFAULT 'in_progress',
            started_at  INTEGER NOT NULL,
            finished_at INTEGER
        );
        CREATE TABLE IF NOT EXISTS sessions (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at INTEGER NOT NULL,
            is_incognito INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS session_tabs (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id  INTEGER NOT NULL,
            window_index INTEGER NOT NULL DEFAULT 0,
            tab_index   INTEGER NOT NULL DEFAULT 0,
            url         TEXT NOT NULL,
            title       TEXT NOT NULL DEFAULT '',
            pinned      INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );
        CREATE INDEX IF NOT EXISTS idx_session_tabs ON session_tabs(session_id, tab_index);
    )SQL");
}

// V3 -> V4: the Chrome-extension registry. Rows point at unpacked extension
// directories; `path` is unique so re-adding a folder refreshes it instead of
// duplicating it, and the engine is handed the enabled paths at startup.
DbError create_extensions(Database& db) {
    return db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS extensions (
            id               INTEGER PRIMARY KEY AUTOINCREMENT,
            path             TEXT NOT NULL UNIQUE,
            name             TEXT NOT NULL DEFAULT '',
            version          TEXT NOT NULL DEFAULT '',
            description      TEXT NOT NULL DEFAULT '',
            manifest_version INTEGER NOT NULL DEFAULT 0,
            enabled          INTEGER NOT NULL DEFAULT 1
        );
        CREATE INDEX IF NOT EXISTS idx_extensions_enabled ON extensions(enabled);
    )SQL");
}

// V4 -> V5: per-site ad-block exceptions ("shields down" sites). One row per
// excepted site key (registrable host, "www." stripped — see
// adblock::site_key); a missing row means blocking stays on. The engine keeps
// an in-memory snapshot for the request hot path and only touches this table
// from the UI thread.
DbError create_adblock_exceptions(Database& db) {
    return db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS adblock_exceptions (
            site TEXT PRIMARY KEY
        );
    )SQL");
}

// V5 -> V6: first-seen, per-origin leaf-certificate SHA-256 fingerprints.
// This is an audit alarm, not a TLS trust override or HPKP enforcement.
DbError create_certificate_audit(Database& db) {
    return db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS certificate_audit (
            origin TEXT PRIMARY KEY,
            sha256 TEXT NOT NULL CHECK(length(sha256) = 64)
        );
    )SQL");
}

// V6 -> V7: user-defined search engines. The built-in engines are not rows:
// they live in code (core::search_engine_template), and the default engine is
// still the `search.engine` preference ("duckduckgo", ..., or "custom:<id>"
// for a row here). Names are unique case-insensitively; the CHECKs mirror
// searchengines::validate_template() as a backstop (https only, a "%s"
// placeholder) so a bad row cannot be written even by a buggy caller.
DbError create_search_engines(Database& db) {
    return db.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS search_engines (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            name         TEXT NOT NULL UNIQUE COLLATE NOCASE
                         CHECK(length(name) BETWEEN 1 AND 64),
            url_template TEXT NOT NULL
                         CHECK(lower(substr(url_template, 1, 8)) = 'https://'
                               AND instr(url_template, '%s') > 0),
            created_at   INTEGER NOT NULL
        );
    )SQL");
}

} // namespace

std::vector<Migration> Migrator::builtin_migrations() {
    return {
        {0, "create history + fts", create_history},
        {1, "create bookmarks", create_bookmarks},
        {2, "create downloads + sessions", create_downloads_and_sessions},
        {3, "create extensions", create_extensions},
        {4, "create adblock exceptions", create_adblock_exceptions},
        {5, "create certificate fingerprint audit", create_certificate_audit},
        {6, "create search engines", create_search_engines},
    };
}

DbError Migrator::migrate(Database& db) {
    // PRAGMA user_version is a cheap, durable schema-version counter.
    int current = 0;
    if (auto v = db.prepare("PRAGMA user_version;")) {
        for (auto r = v->step(); r && *r; r = v->step()) {
            current = v->column_int(0);
        }
    }
    auto migrations = builtin_migrations();
    for (const auto& m : migrations) {
        if (current < m.from_version + 1) {
            AURORA_LOG(LogLevel::Info, "db") << "applying migration v"
                                             << m.from_version << "->v"
                                             << (m.from_version + 1) << ": "
                                             << m.description;
            Transaction txn(db);
            auto err = m.apply(db);
            if (err != DbError::Ok) {
                AURORA_LOG(LogLevel::Error, "db") << "migration failed: "
                                                 << db.last_error_message();
                txn.rollback();
                return err;
            }
            sqlite3_exec(db.handle(),
                         ("PRAGMA user_version = " +
                          std::to_string(m.from_version + 1) + ";")
                             .c_str(),
                         nullptr, nullptr, nullptr);
            current = m.from_version + 1;
            txn.commit();
        }
    }
    return DbError::Ok;
}

} // namespace aurora::storage

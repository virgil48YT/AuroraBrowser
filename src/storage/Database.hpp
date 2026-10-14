// Aurora — SQLite database wrapper.
//
// Design goals:
//   * RAII for connections, statements, and transactions.
//   * WAL mode + busy timeout for concurrency between the UI thread (reads)
//     and a background writer thread (history, downloads).
//   * Atomic writes via transactions so a crash mid-write cannot corrupt the
//     profile database. SQLite's own journaling provides durability; we layer
//     explicit transactions on top so a multi-statement update is all-or-nothing.
//   * No blocking work on the UI thread: callers post tasks to a background
//     sequence and invoke the completion callback on the UI thread.
//
// This file links against the system SQLite (or a vendored amalgamation). It
// intentionally depends on the C API only, so the core library stays free of
// Qt and CEF.
#pragma once

#include <sqlite3.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::storage {

enum class DbError {
    Ok,
    Busy,
    Constraint,
    Corrupt,
    IoError,
    InvalidArgument,
    Unknown,
};

// Convert a sqlite result code into our enum.
DbError to_db_error(int rc);

class Statement;
class Transaction;

// A single SQLite connection. Not thread-safe to share across threads by
// default; each background worker owns its own connection to the same file
// (SQLite handles cross-process and cross-thread locking).
class Database {
public:
    Database();
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Open (creating if necessary) a database file. Enables WAL mode and a
    // generous busy timeout. Returns false on failure.
    bool open(std::string_view path);

    // Run a pragma and return its string value (used for diagnostics).
    std::string pragma(std::string_view name);

    // Execute one or more statements that take no parameters and return no
    // rows. Returns the last error code.
    DbError execute(std::string_view sql);

    // Prepare a statement. Returns nullopt on error.
    std::optional<Statement> prepare(std::string_view sql);

    sqlite3* handle() const { return db_; }
    bool is_open() const { return db_ != nullptr; }
    DbError last_error() const { return last_error_; }
    std::string last_error_message() const;

private:
    sqlite3* db_ = nullptr;
    DbError last_error_ = DbError::Ok;
};

// A prepared statement. Lightweight RAII wrapper that binds parameters and
// steps rows.
class Statement {
public:
    Statement() = default;
    Statement(sqlite3* db, sqlite3_stmt* stmt);
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    explicit operator bool() const { return stmt_ != nullptr; }

    // Bind by 1-based index.
    DbError bind(int index, int value);
    DbError bind(int index, int64_t value);
    DbError bind(int index, double value);
    DbError bind(int index, std::string_view value);
    DbError bind(int index, const std::vector<unsigned char>& blob);
    DbError bind_null(int index);

    // Step. Returns true when a row is available, false when done.
    std::optional<bool> step();
    DbError reset();

    int column_int(int index) const;
    int64_t column_int64(int index) const;
    double column_double(int index) const;
    std::string column_text(int index) const;
    std::vector<unsigned char> column_blob(int index) const;
    bool column_is_null(int index) const;

private:
    sqlite3_stmt* stmt_ = nullptr;
};

// RAII transaction. Commits on destruction unless rollback() is called.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    DbError commit();
    DbError rollback();

private:
    Database& db_;
    bool finished_ = false;
};

} // namespace aurora::storage

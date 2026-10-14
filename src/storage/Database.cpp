#include "storage/Database.hpp"

#include "util/Log.hpp"

#include <cstring>
#include <utility>

namespace aurora::storage {

using util::LogLevel;

DbError to_db_error(int rc) {
    switch (rc) {
        case SQLITE_OK:   return DbError::Ok;
        case SQLITE_BUSY: return DbError::Busy;
        case SQLITE_CONSTRAINT: return DbError::Constraint;
        case SQLITE_CORRUPT: return DbError::Corrupt;
        case SQLITE_IOERR: return DbError::IoError;
        case SQLITE_MISUSE: return DbError::InvalidArgument;
        default: return DbError::Unknown;
    }
}

Database::Database() = default;

Database::Database(Database&& other) noexcept
    : db_(other.db_), last_error_(other.last_error_) {
    other.db_ = nullptr;
    other.last_error_ = DbError::Ok;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = other.db_;
        last_error_ = other.last_error_;
        other.db_ = nullptr;
        other.last_error_ = DbError::Ok;
    }
    return *this;
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::open(std::string_view path) {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    std::string p(path);
    int rc = sqlite3_open(p.c_str(), &db_);
    if (rc != SQLITE_OK) {
        AURORA_LOG(LogLevel::Error, "db") << "open failed: " << sqlite3_errmsg(db_);
        last_error_ = to_db_error(rc);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    // WAL: readers never block writers, writers never block readers. This is
    // what lets the UI read history while a background thread writes it.
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    // 10s busy timeout: if the DB is locked, sleep instead of failing fast —
    // history writes are not latency-critical but must not lose data.
    sqlite3_busy_timeout(db_, 10000);
    // foreign keys on: bookmarks reference folders; cascade deletes keep
    // referential integrity.
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    // Synchronous=NORMAL is safe under WAL and meaningfully faster than FULL.
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    last_error_ = DbError::Ok;
    return true;
}

std::string Database::pragma(std::string_view name) {
    std::string sql = "PRAGMA ";
    sql += name;
    sql += ";";
    std::string result;
    if (auto stmt = prepare(sql)) {
        for (auto r = stmt->step(); r && *r; r = stmt->step()) {
            result = stmt->column_text(0);
        }
    }
    return result;
}

DbError Database::execute(std::string_view sql) {
    if (!db_) return DbError::InvalidArgument;
    char* err = nullptr;
    std::string s(sql);
    int rc = sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK && err) {
        AURORA_LOG(LogLevel::Warn, "db") << "execute: " << err;
        sqlite3_free(err);
    }
    last_error_ = to_db_error(rc);
    return last_error_;
}

std::optional<Statement> Database::prepare(std::string_view sql) {
    if (!db_) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    std::string s(sql);
    int rc = sqlite3_prepare_v2(db_, s.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = to_db_error(rc);
        AURORA_LOG(LogLevel::Warn, "db") << "prepare failed: " << sqlite3_errmsg(db_)
                                        << " :: " << sql;
        return std::nullopt;
    }
    return Statement(db_, stmt);
}

std::string Database::last_error_message() const {
    if (!db_) return "database not open";
    return sqlite3_errmsg(db_);
}

// --- Statement --------------------------------------------------------------
Statement::Statement(sqlite3* db, sqlite3_stmt* stmt) : stmt_(stmt) {
    (void)db;
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
    other.stmt_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

DbError Statement::bind(int index, int value) {
    return to_db_error(sqlite3_bind_int(stmt_, index, value));
}
DbError Statement::bind(int index, int64_t value) {
    return to_db_error(sqlite3_bind_int64(stmt_, index, value));
}
DbError Statement::bind(int index, double value) {
    return to_db_error(sqlite3_bind_double(stmt_, index, value));
}
DbError Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT makes SQLite copy the data, so the caller's buffer
    // does not need to outlive the statement.
    return to_db_error(sqlite3_bind_text(stmt_, index, value.data(),
                                         static_cast<int>(value.size()),
                                         SQLITE_TRANSIENT));
}
DbError Statement::bind(int index, const std::vector<unsigned char>& blob) {
    return to_db_error(sqlite3_bind_blob(stmt_, index, blob.data(),
                                         static_cast<int>(blob.size()),
                                         SQLITE_TRANSIENT));
}
DbError Statement::bind_null(int index) {
    return to_db_error(sqlite3_bind_null(stmt_, index));
}

std::optional<bool> Statement::step() {
    if (!stmt_) return std::nullopt;
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    AURORA_LOG(LogLevel::Warn, "db") << "step failed: " << sqlite3_errmsg(sqlite3_db_handle(stmt_));
    return std::nullopt;
}

DbError Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    return DbError::Ok;
}

int Statement::column_int(int index) const {
    return sqlite3_column_int(stmt_, index);
}
int64_t Statement::column_int64(int index) const {
    return sqlite3_column_int64(stmt_, index);
}
double Statement::column_double(int index) const {
    return sqlite3_column_double(stmt_, index);
}
std::string Statement::column_text(int index) const {
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    if (!text) return {};
    int bytes = sqlite3_column_bytes(stmt_, index);
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(bytes));
}
std::vector<unsigned char> Statement::column_blob(int index) const {
    const void* data = sqlite3_column_blob(stmt_, index);
    int bytes = sqlite3_column_bytes(stmt_, index);
    if (!data || bytes <= 0) return {};
    const auto* p = static_cast<const unsigned char*>(data);
    return std::vector<unsigned char>(p, p + static_cast<size_t>(bytes));
}
bool Statement::column_is_null(int index) const {
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

// --- Transaction ------------------------------------------------------------
Transaction::Transaction(Database& db) : db_(db) {
    db_.execute("BEGIN IMMEDIATE;");
}

DbError Transaction::commit() {
    if (finished_) return DbError::Ok;
    finished_ = true;
    return db_.execute("COMMIT;");
}

DbError Transaction::rollback() {
    if (finished_) return DbError::Ok;
    finished_ = true;
    return db_.execute("ROLLBACK;");
}

Transaction::~Transaction() {
    if (!finished_) {
        // An exception or early return left the transaction open: roll back so
        // partial writes never reach the database.
        db_.execute("ROLLBACK;");
    }
}

} // namespace aurora::storage

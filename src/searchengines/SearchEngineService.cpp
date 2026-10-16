#include "searchengines/SearchEngineService.hpp"

#include <cctype>
#include <ctime>
#include <future>
#include <optional>

#include "storage/Migration.hpp"
#include "util/Log.hpp"

#include <sqlite3.h>

namespace aurora::searchengines {

using util::LogLevel;

namespace {

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

MutationResult failed(EngineError error, int64_t id = 0) {
    MutationResult r;
    r.error = error;
    r.id = id;
    return r;
}

} // namespace

std::string MutationResult::message() const {
    switch (error) {
        case EngineError::Ok: return {};
        case EngineError::InvalidName: return std::string(describe(name_error));
        case EngineError::InvalidTemplate:
            return std::string(describe(template_error));
        case EngineError::DuplicateName:
            return "A search engine with that name already exists.";
        case EngineError::NotFound:
            return "That search engine no longer exists.";
        case EngineError::Storage:
            return "Search engines could not be saved to the profile.";
    }
    return "The search engine could not be saved.";
}

// --- SearchEngineStore -------------------------------------------------------

std::vector<SearchEngine> SearchEngineStore::all() const {
    std::vector<SearchEngine> out;
    auto stmt = db_.prepare(
        "SELECT id, name, url_template FROM search_engines "
        "ORDER BY name COLLATE NOCASE, id;");
    if (!stmt) return out;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        SearchEngine e;
        e.id = stmt->column_int64(0);
        e.name = stmt->column_text(1);
        e.url_template = stmt->column_text(2);
        out.push_back(std::move(e));
    }
    return out;
}

bool SearchEngineStore::name_taken(int64_t id, const std::string& name) const {
    for (const auto& builtin : builtin_engines()) {
        if (iequals_ascii(builtin.name, name)) return true;
    }
    auto stmt = db_.prepare(
        "SELECT 1 FROM search_engines "
        "WHERE name = ?1 COLLATE NOCASE AND id <> ?2 LIMIT 1;");
    if (!stmt) return false;  // the UNIQUE constraint is the backstop
    stmt->bind(1, std::string_view(name));
    stmt->bind(2, id);
    const auto r = stmt->step();
    return r.has_value() && *r;
}

MutationResult SearchEngineStore::validate(int64_t id, const std::string& name,
                                           const std::string& url_template) const {
    MutationResult r;
    r.id = id;
    r.name_error = validate_name(name);
    if (r.name_error != NameError::Ok) {
        r.error = EngineError::InvalidName;
        return r;
    }
    r.template_error = validate_template(url_template);
    if (r.template_error != TemplateError::Ok) {
        r.error = EngineError::InvalidTemplate;
        return r;
    }
    if (name_taken(id, name)) r.error = EngineError::DuplicateName;
    return r;
}

MutationResult SearchEngineStore::add(std::string_view raw_name,
                                      std::string_view raw_template) {
    const std::string name = normalize_input(raw_name);
    const std::string url_template = normalize_input(raw_template);
    auto r = validate(0, name, url_template);
    if (!r.ok()) return r;

    auto stmt = db_.prepare(
        "INSERT INTO search_engines(name, url_template, created_at) "
        "VALUES (?1, ?2, ?3);");
    if (!stmt) return failed(EngineError::Storage);
    stmt->bind(1, std::string_view(name));
    stmt->bind(2, std::string_view(url_template));
    stmt->bind(3, static_cast<int64_t>(std::time(nullptr)));
    const auto step = stmt->step();
    if (!step.has_value()) {
        // Lost a race with another connection inserting the same name.
        return failed(sqlite3_extended_errcode(db_.handle()) ==
                              SQLITE_CONSTRAINT_UNIQUE
                          ? EngineError::DuplicateName
                          : EngineError::Storage);
    }
    r.id = sqlite3_last_insert_rowid(db_.handle());
    return r;
}

MutationResult SearchEngineStore::update(int64_t id, std::string_view raw_name,
                                         std::string_view raw_template) {
    {
        auto exists = db_.prepare("SELECT 1 FROM search_engines WHERE id = ?1;");
        if (!exists) return failed(EngineError::Storage, id);
        exists->bind(1, id);
        const auto found = exists->step();
        if (!found.has_value()) return failed(EngineError::Storage, id);
        if (!*found) return failed(EngineError::NotFound, id);
    }
    const std::string name = normalize_input(raw_name);
    const std::string url_template = normalize_input(raw_template);
    auto r = validate(id, name, url_template);
    if (!r.ok()) return r;

    auto stmt = db_.prepare(
        "UPDATE search_engines SET name = ?2, url_template = ?3 WHERE id = ?1;");
    if (!stmt) return failed(EngineError::Storage, id);
    stmt->bind(1, id);
    stmt->bind(2, std::string_view(name));
    stmt->bind(3, std::string_view(url_template));
    if (!stmt->step().has_value()) {
        return failed(sqlite3_extended_errcode(db_.handle()) ==
                              SQLITE_CONSTRAINT_UNIQUE
                          ? EngineError::DuplicateName
                          : EngineError::Storage,
                      id);
    }
    if (sqlite3_changes(db_.handle()) == 0) return failed(EngineError::NotFound, id);
    return r;
}

MutationResult SearchEngineStore::remove(int64_t id) {
    auto stmt = db_.prepare("DELETE FROM search_engines WHERE id = ?1;");
    if (!stmt) return failed(EngineError::Storage, id);
    stmt->bind(1, id);
    if (!stmt->step().has_value()) return failed(EngineError::Storage, id);
    if (sqlite3_changes(db_.handle()) == 0) return failed(EngineError::NotFound, id);
    MutationResult r;
    r.id = id;
    return r;
}

// --- SearchEngineService -----------------------------------------------------

SearchEngineService::SearchEngineService(std::string db_path)
    : worker_([this, path = std::move(db_path)]() mutable { run(std::move(path)); }) {}

SearchEngineService::~SearchEngineService() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stopping_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SearchEngineService::run(std::string db_path) {
    // This thread's own connection: never shared with the UI thread.
    storage::Database db;
    const bool ok = db.open(db_path) &&
                    storage::Migrator::migrate(db) == storage::DbError::Ok;
    std::optional<SearchEngineStore> store;
    std::vector<SearchEngine> initial;
    if (ok) {
        store.emplace(db);
        initial = store->all();
    } else {
        AURORA_LOG(LogLevel::Error, "search")
            << "search engine storage unavailable: " << db_path;
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        storage_available_ = ok;
        snapshot_ = std::move(initial);
        loaded_ = true;
    }
    loaded_cv_.notify_all();

    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;  // stopping, and every task has run
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task(store ? &*store : nullptr);
    }
}

void SearchEngineService::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(task));
    }
    queue_cv_.notify_one();
}

void SearchEngineService::publish(std::vector<SearchEngine> engines) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = std::move(engines);
}

void SearchEngineService::add(std::string name, std::string url_template,
                              Done done) {
    post([this, name = std::move(name), url_template = std::move(url_template),
          done = std::move(done)](SearchEngineStore* store) {
        MutationResult r = failed(EngineError::Storage);
        if (store) {
            r = store->add(name, url_template);
            if (r.ok()) publish(store->all());
        }
        if (done) done(r);
    });
}

void SearchEngineService::update(int64_t id, std::string name,
                                 std::string url_template, Done done) {
    post([this, id, name = std::move(name),
          url_template = std::move(url_template),
          done = std::move(done)](SearchEngineStore* store) {
        MutationResult r = failed(EngineError::Storage, id);
        if (store) {
            r = store->update(id, name, url_template);
            if (r.ok()) publish(store->all());
        }
        if (done) done(r);
    });
}

void SearchEngineService::remove(int64_t id, Done done) {
    post([this, id, done = std::move(done)](SearchEngineStore* store) {
        MutationResult r = failed(EngineError::Storage, id);
        if (store) {
            r = store->remove(id);
            if (r.ok()) publish(store->all());
        }
        if (done) done(r);
    });
}

std::vector<SearchEngine> SearchEngineService::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

ResolvedEngine SearchEngineService::resolve(std::string_view pref_value) const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return resolve_default_engine(pref_value, snapshot_);
}

bool SearchEngineService::loaded() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return loaded_;
}

bool SearchEngineService::storage_available() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return storage_available_;
}

bool SearchEngineService::wait_until_loaded(std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(snapshot_mutex_);
    return loaded_cv_.wait_for(lock, timeout, [this] { return loaded_; });
}

void SearchEngineService::flush() {
    std::promise<void> drained;
    auto future = drained.get_future();
    post([&drained](SearchEngineStore*) { drained.set_value(); });
    future.wait();
}

} // namespace aurora::searchengines

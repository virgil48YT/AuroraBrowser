// Aurora — custom search engine storage.
//
// Two layers, both Qt- and CEF-free:
//
//   * SearchEngineStore — synchronous CRUD over the `search_engines` table
//     (schema v7, storage/Migration.cpp). Borrowed Database, no threads: this
//     is what the unit tests drive directly, like HistoryService.
//
//   * SearchEngineService — what the browser uses. It owns a worker thread
//     that opens its OWN Database connection to the profile file (WAL mode,
//     so it never blocks the UI thread's connection) and runs every
//     SearchEngineStore call there. The UI thread only ever reads an
//     in-memory snapshot of the engine list, which the worker replaces after
//     the initial load and after each successful mutation. That keeps the
//     omnibox path (resolve the default engine on Enter) free of SQLite.
//
// Mutation callbacks run ON THE WORKER THREAD. Qt callers must marshal back
// to their own thread (QMetaObject::invokeMethod with a queued connection)
// before touching widgets.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "searchengines/SearchEngine.hpp"
#include "storage/Database.hpp"

namespace aurora::searchengines {

enum class EngineError {
    Ok,
    InvalidName,
    InvalidTemplate,
    DuplicateName,  // same name (case-insensitive) as another engine
    NotFound,
    Storage,        // the profile database could not be opened or written
};

struct MutationResult {
    EngineError error = EngineError::Ok;
    int64_t id = 0;  // the engine's row id (add/update/remove)
    NameError name_error = NameError::Ok;
    TemplateError template_error = TemplateError::Ok;

    bool ok() const { return error == EngineError::Ok; }
    // One sentence for a status message or inline form error.
    std::string message() const;
};

class SearchEngineStore {
public:
    explicit SearchEngineStore(storage::Database& db) : db_(db) {}

    // Every custom engine, ordered by name (case-insensitive), then id.
    std::vector<SearchEngine> all() const;

    // Validate (after trimming) and insert. Names must be unique across custom
    // engines AND the built-in engine labels, case-insensitively, so the
    // default-engine picker never shows two entries with the same label.
    MutationResult add(std::string_view name, std::string_view url_template);

    // Validate and replace an existing engine's name and template.
    MutationResult update(int64_t id, std::string_view name,
                          std::string_view url_template);

    MutationResult remove(int64_t id);

private:
    MutationResult validate(int64_t id, const std::string& name,
                            const std::string& url_template) const;
    bool name_taken(int64_t id, const std::string& name) const;

    storage::Database& db_;
};

class SearchEngineService {
public:
    using Done = std::function<void(const MutationResult&)>;

    // Starts the worker, which opens its own connection to `db_path`,
    // migrates it (idempotent: a no-op on an already-migrated profile) and
    // loads the initial snapshot. Never blocks the caller.
    explicit SearchEngineService(std::string db_path);
    // Finishes queued work, then stops and joins the worker.
    ~SearchEngineService();
    SearchEngineService(const SearchEngineService&) = delete;
    SearchEngineService& operator=(const SearchEngineService&) = delete;

    // --- Worker-side mutations (validated again on the worker). `done`, if
    //     set, runs on the worker thread after the snapshot is updated.
    void add(std::string name, std::string url_template, Done done = {});
    void update(int64_t id, std::string name, std::string url_template,
                Done done = {});
    void remove(int64_t id, Done done = {});

    // --- UI-thread-safe reads: in-memory only, no SQLite.
    std::vector<SearchEngine> snapshot() const;
    // Resolve the default-engine preference against the snapshot. Before the
    // initial load finishes (or if storage is unavailable) a "custom:<id>"
    // preference falls back to DuckDuckGo — `fell_back` reports it.
    ResolvedEngine resolve(std::string_view pref_value) const;
    bool loaded() const;
    // False when the worker could not open or migrate the profile database;
    // mutations then fail with EngineError::Storage.
    bool storage_available() const;

    // Block until the initial load has finished, up to `timeout`. Startup
    // calls this once, before the first window exists, so a custom default
    // engine is honoured by the very first navigation. Returns loaded().
    bool wait_until_loaded(std::chrono::milliseconds timeout) const;

    // Block until every task queued so far has run. For tests and shutdown;
    // never call it from a mutation callback (that runs on the worker).
    void flush();

private:
    using Task = std::function<void(SearchEngineStore*)>;

    void post(Task task);
    void run(std::string db_path);
    void publish(std::vector<SearchEngine> engines);

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Task> queue_;
    bool stopping_ = false;

    mutable std::mutex snapshot_mutex_;
    mutable std::condition_variable loaded_cv_;
    std::vector<SearchEngine> snapshot_;
    bool loaded_ = false;
    bool storage_available_ = false;

    std::thread worker_;  // last: started after every member above exists
};

} // namespace aurora::searchengines

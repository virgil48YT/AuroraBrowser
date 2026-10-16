// Custom search engines: template validation, default-engine resolution, the
// search_engines migration, the synchronous store and the worker-thread
// service the browser uses.
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/Url.hpp"
#include "searchengines/SearchEngine.hpp"
#include "searchengines/SearchEngineService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora;
using namespace aurora::searchengines;

namespace {

storage::Database open_profile() {
    storage::Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(storage::Migrator::migrate(db) == storage::DbError::Ok);
    return db;
}

int count_rows(storage::Database& db, const char* sql) {
    auto stmt = db.prepare(sql);
    REQUIRE(stmt);
    int n = 0;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) n = stmt->column_int(0);
    return n;
}

std::string temp_profile(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("aurora_search_engines_test_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return (dir / "profile.db").string();
}

} // namespace

// --- Template validation -----------------------------------------------------

TEST_CASE("valid https templates with exactly one %s are accepted") {
    for (const auto* t : {"https://search.example.com/?q=%s",
                          "https://example.com/search/%s",
                          "HTTPS://Example.com/s?q=%s&lang=en",
                          "https://example.com:8443/find?term=%s",
                          "https://[::1]:8443/?q=%s",
                          "https://example.com/?q=%s#results",
                          "https://example.com/?a=%20&q=%s"}) {
        CAPTURE(t);
        CHECK(validate_template(t) == TemplateError::Ok);
    }
}

TEST_CASE("templates must be https") {
    CHECK(validate_template("http://example.com/?q=%s") == TemplateError::NotHttps);
    CHECK(validate_template("ftp://example.com/?q=%s") == TemplateError::NotHttps);
    CHECK(validate_template("javascript:alert(%s)") == TemplateError::NotHttps);
    CHECK(validate_template("example.com/?q=%s") == TemplateError::NotHttps);
    CHECK(validate_template("//example.com/?q=%s") == TemplateError::NotHttps);
}

TEST_CASE("templates must contain exactly one %s placeholder") {
    CHECK(validate_template("https://example.com/?q=") ==
          TemplateError::MissingPlaceholder);
    CHECK(validate_template("https://example.com/?q=%S") ==
          TemplateError::MissingPlaceholder);
    CHECK(validate_template("https://example.com/?q=%s&r=%s") ==
          TemplateError::MultiplePlaceholders);
    CHECK(validate_template("https://example.com/%s%s") ==
          TemplateError::MultiplePlaceholders);
}

TEST_CASE("template hosts are validated") {
    CHECK(validate_template("https:///?q=%s") == TemplateError::InvalidHost);
    CHECK(validate_template("https://user:pw@example.com/?q=%s") ==
          TemplateError::InvalidHost);
    CHECK(validate_template("https://exa_mple.com/?q=%s") ==
          TemplateError::InvalidHost);
    CHECK(validate_template("https://.example.com/?q=%s") ==
          TemplateError::InvalidHost);
    CHECK(validate_template("https://example.com:0/?q=%s") ==
          TemplateError::InvalidHost);
    CHECK(validate_template("https://example.com:99999/?q=%s") ==
          TemplateError::InvalidHost);
    CHECK(validate_template("https://%s.example.com/") ==
          TemplateError::PlaceholderInHost);
}

TEST_CASE("empty, oversized and whitespace-containing templates are rejected") {
    CHECK(validate_template("") == TemplateError::Empty);
    CHECK(validate_template("https://example.com/?q=%s ") ==
          TemplateError::InvalidCharacter);
    CHECK(validate_template("https://exa mple.com/?q=%s") ==
          TemplateError::InvalidCharacter);
    CHECK(validate_template("https://example.com/?q=%s\n") ==
          TemplateError::InvalidCharacter);
    std::string huge = "https://example.com/?q=%s&pad=";
    huge.append(kMaxTemplateLength, 'a');
    CHECK(validate_template(huge) == TemplateError::TooLong);
    // Every error has a user-facing explanation.
    for (auto e : {TemplateError::Empty, TemplateError::TooLong,
                   TemplateError::InvalidCharacter, TemplateError::NotHttps,
                   TemplateError::InvalidHost, TemplateError::PlaceholderInHost,
                   TemplateError::MissingPlaceholder,
                   TemplateError::MultiplePlaceholders}) {
        CHECK_FALSE(describe(e).empty());
    }
}

TEST_CASE("engine names are validated and input is trimmed") {
    CHECK(validate_name("Kagi") == NameError::Ok);
    CHECK(validate_name("") == NameError::Empty);
    CHECK(validate_name(std::string(kMaxNameLength + 1, 'x')) == NameError::TooLong);
    CHECK(validate_name("tab\tname") == NameError::InvalidCharacter);
    CHECK(normalize_input("  Kagi \n") == "Kagi");
    CHECK(normalize_input("   ").empty());
}

// --- Expansion ---------------------------------------------------------------

TEST_CASE("templates expand with URL-encoded search terms") {
    CHECK(expand_template("https://example.com/?q=%s", "c++ & rust") ==
          "https://example.com/?q=c%2B%2B+%26+rust");
    CHECK(expand_template("https://example.com/search/%s/page", "a/b") ==
          "https://example.com/search/a%2Fb/page");
    // A '%' in the terms cannot re-introduce a placeholder.
    CHECK(expand_template("https://example.com/?q=%s", "%s") ==
          "https://example.com/?q=%25s");
    CHECK(template_home("https://search.example.com:8443/x?q=%s") ==
          "https://search.example.com:8443/");
    CHECK(template_home("http://bad.example/?q=%s").empty());
}

TEST_CASE("the omnibox resolution path substitutes %s and keeps legacy prefixes") {
    const core::Url query = core::parse_omnibox_input("aurora browser");
    REQUIRE(query.is_search_query());
    CHECK(core::resolve_navigation(query, "https://example.com/s?q=%s&x=1") ==
          "https://example.com/s?q=aurora+browser&x=1");
    // Built-in prefix templates still append, byte-for-byte as before.
    CHECK(core::resolve_navigation(query, core::search_engine_template("google")) ==
          "https://www.google.com/search?q=aurora+browser");
    // URLs are never sent to the search engine.
    CHECK(core::resolve_navigation(core::parse_omnibox_input("example.com"),
                                   "https://example.com/s?q=%s") ==
          "https://example.com/");
}

// --- Default-engine resolution -----------------------------------------------

TEST_CASE("custom engine keys round-trip and reject junk") {
    CHECK(custom_engine_key(42) == "custom:42");
    CHECK(parse_custom_engine_key("custom:42") == std::optional<int64_t>(42));
    CHECK_FALSE(parse_custom_engine_key("custom:").has_value());
    CHECK_FALSE(parse_custom_engine_key("custom:0").has_value());
    CHECK_FALSE(parse_custom_engine_key("custom:-3").has_value());
    CHECK_FALSE(parse_custom_engine_key("custom:4x").has_value());
    CHECK_FALSE(parse_custom_engine_key("google").has_value());
}

TEST_CASE("built-in engines resolve to the existing templates and home pages") {
    REQUIRE(builtin_engines().size() == 4);
    CHECK(builtin_engines()[0].key == "duckduckgo");
    for (const auto& b : builtin_engines()) {
        const auto r = resolve_default_engine(b.key, {});
        CAPTURE(b.key);
        CHECK(r.key == b.key);
        CHECK_FALSE(r.is_custom);
        CHECK_FALSE(r.fell_back);
        CHECK(validate_template(r.url_template) == TemplateError::Ok);
        CHECK(r.home == core::search_engine_home(b.key));
        // Same search URL the omnibox produced before custom engines existed.
        CHECK(expand_template(r.url_template, "x y") ==
              core::search_engine_template(b.key) + "x+y");
    }
}

TEST_CASE("a custom default resolves to its row; a missing one falls back") {
    const std::vector<SearchEngine> custom{
        {7, "Kagi", "https://kagi.com/search?q=%s"},
        {9, "Wiki", "https://en.wikipedia.org/w/index.php?search=%s"},
    };
    const auto kagi = resolve_default_engine("custom:7", custom);
    CHECK(kagi.is_custom);
    CHECK_FALSE(kagi.fell_back);
    CHECK(kagi.name == "Kagi");
    CHECK(kagi.home == "https://kagi.com/");
    CHECK(expand_template(kagi.url_template, "q") == "https://kagi.com/search?q=q");

    const auto deleted = resolve_default_engine("custom:8", custom);
    CHECK(deleted.fell_back);
    CHECK(deleted.key == "duckduckgo");

    CHECK(resolve_default_engine("no-such-engine", custom).fell_back);
    CHECK(resolve_default_engine("", custom).key == "duckduckgo");

    // A row that bypassed validation (hand-edited database) is never used.
    const std::vector<SearchEngine> tampered{{3, "Bad", "http://evil.test/?q=%s"}};
    const auto bad = resolve_default_engine("custom:3", tampered);
    CHECK(bad.fell_back);
    CHECK(bad.key == "duckduckgo");
}

// --- Migration ---------------------------------------------------------------

TEST_CASE("search_engines migration upgrades a v6 profile and is idempotent") {
    storage::Database db;
    REQUIRE(db.open(":memory:"));
    for (const auto& migration : storage::Migrator::builtin_migrations()) {
        if (migration.from_version < 6)
            REQUIRE(migration.apply(db) == storage::DbError::Ok);
    }
    REQUIRE(db.execute("PRAGMA user_version = 6;") == storage::DbError::Ok);
    REQUIRE(db.execute("INSERT INTO history(url,title,last_visit) "
                       "VALUES ('https://keep.test','Keep',1);") ==
            storage::DbError::Ok);

    REQUIRE(storage::Migrator::migrate(db) == storage::DbError::Ok);
    REQUIRE(storage::Migrator::migrate(db) == storage::DbError::Ok);
    CHECK(count_rows(db, "PRAGMA user_version;") == 7);
    CHECK(count_rows(db, "SELECT COUNT(*) FROM history;") == 1);
    CHECK(count_rows(db, "SELECT COUNT(*) FROM sqlite_master "
                         "WHERE type='table' AND name='search_engines';") == 1);
    CHECK(count_rows(db, "SELECT COUNT(*) FROM search_engines;") == 0);
}

TEST_CASE("the search_engines table rejects non-https and placeholder-less rows") {
    auto db = open_profile();
    CHECK(db.execute("INSERT INTO search_engines(name,url_template,created_at) "
                     "VALUES ('A','http://a.test/?q=%s',0);") !=
          storage::DbError::Ok);
    CHECK(db.execute("INSERT INTO search_engines(name,url_template,created_at) "
                     "VALUES ('B','https://b.test/?q=',0);") !=
          storage::DbError::Ok);
    CHECK(db.execute("INSERT INTO search_engines(name,url_template,created_at) "
                     "VALUES ('C','https://c.test/?q=%s',0);") ==
          storage::DbError::Ok);
    // Names are unique regardless of case.
    CHECK(db.execute("INSERT INTO search_engines(name,url_template,created_at) "
                     "VALUES ('c','https://c2.test/?q=%s',0);") !=
          storage::DbError::Ok);
}

// --- SearchEngineStore ---------------------------------------------------------

TEST_CASE("store adds, lists, updates and removes custom engines") {
    auto db = open_profile();
    SearchEngineStore store(db);
    CHECK(store.all().empty());

    const auto added = store.add("  Kagi ", " https://kagi.com/search?q=%s ");
    REQUIRE(added.ok());
    CHECK(added.id > 0);
    const auto wiki = store.add("Wiki", "https://en.wikipedia.org/w/index.php?search=%s");
    REQUIRE(wiki.ok());

    auto all = store.all();
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "Kagi");  // trimmed, ordered by name
    CHECK(all[0].url_template == "https://kagi.com/search?q=%s");
    CHECK(all[1].name == "Wiki");

    const auto edited = store.update(added.id, "Kagi Search",
                                     "https://kagi.com/search?q=%s&r=us");
    REQUIRE(edited.ok());
    all = store.all();
    CHECK(all[0].name == "Kagi Search");
    CHECK(all[0].url_template == "https://kagi.com/search?q=%s&r=us");

    CHECK(store.remove(added.id).ok());
    CHECK(store.remove(added.id).error == EngineError::NotFound);
    CHECK(store.update(added.id, "X", "https://x.test/?q=%s").error ==
          EngineError::NotFound);
    REQUIRE(store.all().size() == 1);
    CHECK(store.all()[0].name == "Wiki");
}

TEST_CASE("store rejects invalid input and duplicate names") {
    auto db = open_profile();
    SearchEngineStore store(db);

    auto r = store.add("Plain", "http://example.com/?q=%s");
    CHECK(r.error == EngineError::InvalidTemplate);
    CHECK(r.template_error == TemplateError::NotHttps);
    CHECK_FALSE(r.message().empty());

    r = store.add("Twice", "https://example.com/?q=%s&r=%s");
    CHECK(r.template_error == TemplateError::MultiplePlaceholders);

    r = store.add("   ", "https://example.com/?q=%s");
    CHECK(r.error == EngineError::InvalidName);
    CHECK(r.name_error == NameError::Empty);

    // Built-in labels are reserved so the picker never shows two "Google"s.
    CHECK(store.add("google", "https://example.com/?q=%s").error ==
          EngineError::DuplicateName);

    REQUIRE(store.add("Kagi", "https://kagi.com/search?q=%s").ok());
    CHECK(store.add("KAGI", "https://kagi2.test/?q=%s").error ==
          EngineError::DuplicateName);
    const auto other = store.add("Other", "https://other.test/?q=%s");
    REQUIRE(other.ok());
    CHECK(store.update(other.id, "kagi", "https://other.test/?q=%s").error ==
          EngineError::DuplicateName);
    // Renaming an engine to its own name (any case) is not a duplicate.
    CHECK(store.update(other.id, "OTHER", "https://other.test/?q=%s").ok());
    CHECK(store.all().size() == 2);
}

// --- SearchEngineService (worker thread) -------------------------------------

TEST_CASE("service loads on its worker and serves an in-memory snapshot") {
    const auto path = temp_profile("load");
    {
        // Seed the profile through an ordinary connection, as a previous
        // session would have.
        storage::Database db;
        REQUIRE(db.open(path));
        REQUIRE(storage::Migrator::migrate(db) == storage::DbError::Ok);
        SearchEngineStore store(db);
        REQUIRE(store.add("Kagi", "https://kagi.com/search?q=%s").ok());
    }
    SearchEngineService svc(path);
    REQUIRE(svc.wait_until_loaded(std::chrono::seconds(5)));
    CHECK(svc.storage_available());
    const auto engines = svc.snapshot();
    REQUIRE(engines.size() == 1);
    CHECK(engines[0].name == "Kagi");

    const auto r = svc.resolve(custom_engine_key(engines[0].id));
    CHECK(r.is_custom);
    CHECK_FALSE(r.fell_back);
    CHECK(r.home == "https://kagi.com/");
}

TEST_CASE("service mutations run off the calling thread and update the snapshot") {
    SearchEngineService svc(":memory:");
    REQUIRE(svc.wait_until_loaded(std::chrono::seconds(5)));

    const auto caller = std::this_thread::get_id();
    std::atomic<bool> off_thread{false};
    MutationResult added;
    svc.add("Kagi", "https://kagi.com/search?q=%s",
            [&](const MutationResult& r) {
                off_thread = std::this_thread::get_id() != caller;
                added = r;
            });
    svc.flush();
    CHECK(off_thread.load());
    REQUIRE(added.ok());
    REQUIRE(svc.snapshot().size() == 1);

    // Results are captured and asserted on this thread after flush(), which
    // orders the worker's writes before our reads.
    MutationResult rejected;
    MutationResult renamed;
    svc.add("Bad", "http://bad.test/?q=%s",
            [&](const MutationResult& r) { rejected = r; });
    svc.update(added.id, "Kagi Search", "https://kagi.com/search?q=%s",
               [&](const MutationResult& r) { renamed = r; });
    svc.flush();
    CHECK(rejected.error == EngineError::InvalidTemplate);
    CHECK(renamed.ok());
    REQUIRE(svc.snapshot().size() == 1);
    CHECK(svc.snapshot()[0].name == "Kagi Search");

    // Deleting the default custom engine makes resolution fall back.
    const std::string key = custom_engine_key(added.id);
    CHECK_FALSE(svc.resolve(key).fell_back);
    MutationResult removed;
    svc.remove(added.id, [&](const MutationResult& r) { removed = r; });
    svc.flush();
    CHECK(removed.ok());
    CHECK(svc.snapshot().empty());
    CHECK(svc.resolve(key).fell_back);
    CHECK(svc.resolve(key).key == "duckduckgo");
}

TEST_CASE("service reports unavailable storage instead of pretending to save") {
    const auto dir = std::filesystem::temp_directory_path() /
                     "aurora_search_engines_test_missing" / "nested";
    std::filesystem::remove_all(dir.parent_path());
    // The parent directory does not exist, so SQLite cannot create the file.
    SearchEngineService svc((dir / "profile.db").string());
    REQUIRE(svc.wait_until_loaded(std::chrono::seconds(5)));
    CHECK_FALSE(svc.storage_available());
    MutationResult r;
    svc.add("Kagi", "https://kagi.com/search?q=%s",
            [&](const MutationResult& result) { r = result; });
    svc.flush();
    CHECK(r.error == EngineError::Storage);
    CHECK(svc.snapshot().empty());
    CHECK(svc.resolve("google").key == "google");
}

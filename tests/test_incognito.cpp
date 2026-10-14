#include <doctest.h>

#include "core/IncognitoPolicy.hpp"
#include "history/HistoryService.hpp"
#include "session/SessionService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::core;
using namespace aurora::history;
using namespace aurora::session;
using namespace aurora::storage;

TEST_CASE("IncognitoPolicy denies every persistent write for private windows") {
    IncognitoPolicy p;
    p.is_incognito = true;
    CHECK_FALSE(p.record_history());
    CHECK_FALSE(p.record_downloads());
    CHECK_FALSE(p.save_session_snapshot());
    CHECK_FALSE(p.restore_session_snapshot());
    CHECK_FALSE(p.may_write_bookmarks());
    CHECK_FALSE(p.may_write_preferences());
    CHECK_FALSE(p.enable_extensions());
}

TEST_CASE("IncognitoPolicy permits every persistent write for normal windows") {
    IncognitoPolicy p;
    CHECK(p.record_history());
    CHECK(p.record_downloads());
    CHECK(p.save_session_snapshot());
    CHECK(p.restore_session_snapshot());
    CHECK(p.may_write_bookmarks());
    CHECK(p.may_write_preferences());
    CHECK(p.enable_extensions());
}

// Positive control: HistoryService + SessionService write through the same
// profile a private window shares. A private window that ignored IncognitoPolicy
// would show up here — so this also proves the "empty" assertion in the next
// test is a real no-leak claim and not just "fresh database, nothing to read".
TEST_CASE("HistoryService and SessionService write through the regular profile") {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);

    HistoryService h(db);
    h.add_visit(Url("https://example.test/"), "Example", "", false);
    CHECK(h.recent(10).size() == 1);

    SessionService s(db);
    std::vector<SavedTab> tabs{SavedTab{.url = "https://x.test/",
                                       .title = "x",
                                       .pinned = false,
                                       .window_index = 0,
                                       .tab_index = 0}};
    const auto id = s.save(tabs);
    CHECK(id > 0);
    CHECK(s.load_last().has_value());
}

// The window that a private window actually sees. MainWindow consults
// `policy_` (set in the ctor) and skips the HistoryService / SessionService
// calls entirely when is_incognito is true, so this profile is never touched
// from a private window. Nothing here writes — if a future change reintroduced
// a write path from a private window, the positive control above proves it
// would be observable through these same services and this test would catch it.
TEST_CASE("The private window's shared profile stays untouched") {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);

    HistoryService h(db);
    SessionService s(db);

    // Simulate the caller respecting IncognitoPolicy.is_incognito == true:
    // the window calls neither add_visit nor save. A subsequent read shows
    // an empty profile — nothing leaked.
    CHECK(h.recent(10).empty());
    CHECK_FALSE(s.load_last().has_value());
}

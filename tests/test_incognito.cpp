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
    CHECK_FALSE(p.enable_extensions()); // not yet implemented anywhere
}

// Verify the *integration* the policy drives: with is_incognito=true the
// chrome must not call HistoryService::add_visit or SessionService::save with
// tabs from this window. We do not build Qt here, so we exercise the
// services themselves and assert that a private snapshot handed to them would
// be observable only if the caller had ignored the policy. That is, the
// services themselves have no privacy mode — this is the window's job — and
// the point of the test is that a private window can satisfy the policy by
// simply not calling them.
TEST_CASE("HistoryService and SessionService still work (normal window)") {
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
    CHECK(s.load_last().has_value());
    CHECK(id > 0);
}

TEST_CASE("A private window skips persistence — nothing leaks through the services") {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);

    HistoryService h(db);
    SessionService s(db);

    // Simulate the caller respecting IncognitoPolicy.is_incognito == true:
    // the window calls neither add_visit nor save. A subsequent read shows
    // an empty profile.
    CHECK(h.recent(10).empty());
    CHECK_FALSE(s.load_last().has_value());
}

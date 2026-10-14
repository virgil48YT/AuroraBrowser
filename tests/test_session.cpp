#include <doctest.h>

#include "session/SessionService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::storage;
using namespace aurora::session;

static Database open_session() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

TEST_CASE("save and load round-trips tabs in order") {
    auto db = open_session();
    SessionService s(db);
    std::vector<SavedTab> tabs = {
        {"https://a.test", "A", false, 0, 0},
        {"https://b.test", "B", true,  0, 1},
        {"https://c.test", "C", false, 0, 2},
    };
    int64_t id = s.save(tabs);
    CHECK(id > 0);

    auto loaded = s.load_last();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->tabs.size() == 3);
    CHECK(loaded->tabs[0].url == "https://a.test");
    CHECK(loaded->tabs[0].pinned == false);
    CHECK(loaded->tabs[1].title == "B");
    CHECK(loaded->tabs[1].pinned == true);
    CHECK(loaded->tabs[2].url == "https://c.test");
}

TEST_CASE("load by id returns the saved session") {
    auto db = open_session();
    SessionService s(db);
    std::vector<SavedTab> tabs = {{"https://x.test", "X", false, 0, 0}};
    int64_t id = s.save(tabs);
    auto loaded = s.load(id);
    REQUIRE(loaded.has_value());
    CHECK(loaded->tabs[0].url == "https://x.test");
}

TEST_CASE("load_last returns nullopt when no sessions exist") {
    auto db = open_session();
    SessionService s(db);
    CHECK_FALSE(s.load_last().has_value());
}

TEST_CASE("pruning keeps only the most recent keep sessions") {
    auto db = open_session();
    SessionService s(db);
    for (int i = 0; i < 5; ++i) {
        s.save({{std::string("https://") + std::to_string(i) + ".test",
                 std::to_string(i), false, 0, 0}});
    }
    // Default keep = 10, so all 5 should still be present.
    auto last = s.load_last();
    REQUIRE(last.has_value());
    // Save 8 more with keep=3 -> only last 3 survive.
    for (int i = 5; i < 13; ++i) {
        s.save({{std::string("https://") + std::to_string(i) + ".test",
                 std::to_string(i), false, 0, 0}}, 3);
    }
    // The newest should be site 12.
    last = s.load_last();
    REQUIRE(last.has_value());
    CHECK(last->tabs[0].url == "https://12.test");
}

#include <doctest.h>

#include "history/HistoryService.hpp"
#include "storage/Migration.hpp"
#include "storage/Database.hpp"

using namespace aurora;
using namespace aurora::storage;
using namespace aurora::history;
using namespace aurora::core;

static Database open_history() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

TEST_CASE("add_visit inserts then bumps visit_count on repeat") {
    auto db = open_history();
    HistoryService h(db);
    Url u("https://example.com/page");
    h.add_visit(u, "Example", "https://example.com/favicon.ico", true);
    h.add_visit(u, "Example", "https://example.com/favicon.ico", false);

    auto recent = h.recent(10);
    REQUIRE(recent.size() == 1);
    CHECK(recent[0].visit_count == 2);
    CHECK(recent[0].typed_count == 1);
    CHECK(recent[0].title == "Example");
}

TEST_CASE("recent returns newest first") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://a.test"), "A", "", false);
    h.add_visit(Url("https://b.test"), "B", "", false);

    auto r = h.recent(10);
    REQUIRE(r.size() == 2);
    CHECK(r[0].url == "https://b.test/");
    CHECK(r[1].url == "https://a.test/");
}

TEST_CASE("search matches by title and url") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://github.com/aurora"), "Aurora repo", "", false);
    h.add_visit(Url("https://news.example.com"), "World News", "", false);

    auto by_url = h.search("github", 10);
    REQUIRE(by_url.size() >= 1);
    CHECK(by_url[0].url.find("github.com") != std::string::npos);

    auto by_title = h.search("news", 10);
    REQUIRE(by_title.size() >= 1);
    CHECK(by_title[0].title.find("News") != std::string::npos);
}

TEST_CASE("remove deletes a single entry") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://a.test"), "A", "", false);
    auto r = h.recent(10);
    REQUIRE(r.size() == 1);
    CHECK(h.remove(r[0].id));
    CHECK(h.recent(10).empty());
}

TEST_CASE("clear empties history") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://a.test"), "A", "", false);
    h.add_visit(Url("https://b.test"), "B", "", false);
    CHECK(h.clear() == 2);
    CHECK(h.recent(10).empty());
}

// --- titles arriving after the navigation ------------------------------------
// A visit is recorded the moment the address changes, which is before the page
// has parsed its <title>. These cases pin down the two halves of the fix that
// stopped the history page from showing raw URLs in place of page names.

TEST_CASE("set_title fills in a title recorded after the visit") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://example.com/page"), "", "", false);
    auto r = h.recent(10);
    REQUIRE(r.size() == 1);
    CHECK(r[0].title.empty());

    h.set_title(Url("https://example.com/page"), "Example Domain");
    r = h.recent(10);
    REQUIRE(r.size() == 1);
    CHECK(r[0].title == "Example Domain");
    CHECK(r[0].visit_count == 1);  // not a new visit
}

TEST_CASE("set_title never blanks an existing title and ignores unknown urls") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://example.com/"), "Example", "", false);
    h.set_title(Url("https://example.com/"), "");
    CHECK(h.recent(10)[0].title == "Example");
    h.set_title(Url("https://never-visited.test/"), "Nope");
    CHECK(h.recent(10).size() == 1);
}

TEST_CASE("a re-visit keeps the known title when none is supplied") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://example.com/"), "Example Domain", "icon.png", false);
    // Second navigation to the same URL: the title is not known yet.
    h.add_visit(Url("https://example.com/"), "", "", false);
    auto r = h.recent(10);
    REQUIRE(r.size() == 1);
    CHECK(r[0].visit_count == 2);
    CHECK(r[0].title == "Example Domain");
    CHECK(r[0].favicon == "icon.png");
}

TEST_CASE("a re-visit with a new title updates it") {
    auto db = open_history();
    HistoryService h(db);
    h.add_visit(Url("https://example.com/"), "Old", "", false);
    h.add_visit(Url("https://example.com/"), "New", "", false);
    CHECK(h.recent(10)[0].title == "New");
}

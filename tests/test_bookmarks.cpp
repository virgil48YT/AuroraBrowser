#include <doctest.h>

#include "bookmarks/BookmarkService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::storage;
using namespace aurora::bookmarks;

static Database open_bookmarks() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

TEST_CASE("add bookmark under root and list children") {
    auto db = open_bookmarks();
    BookmarkService b(db);
    int64_t id = b.add(0, "Aurora", "https://github.com/aurora", false);
    CHECK(id > 0);

    auto kids = b.children(0);
    REQUIRE(kids.size() == 1);
    CHECK(kids[0].title == "Aurora");
    CHECK(kids[0].url == "https://github.com/aurora");
    CHECK_FALSE(kids[0].is_folder);
    CHECK(kids[0].position == 0);
}

TEST_CASE("folder contains nested bookmarks") {
    auto db = open_bookmarks();
    BookmarkService b(db);
    int64_t folder = b.add(0, "Dev", "", true);
    CHECK(folder > 0);
    b.add(folder, "GitHub", "https://github.com", false);
    b.add(folder, "Docs", "https://docs.example.com", false);

    auto kids = b.children(folder);
    REQUIRE(kids.size() == 2);
    CHECK(kids[0].position == 0);
    CHECK(kids[1].position == 1);
}

TEST_CASE("update changes title and url") {
    auto db = open_bookmarks();
    BookmarkService b(db);
    int64_t id = b.add(0, "Old", "https://old.test", false);
    REQUIRE(b.update(id, "New", "https://new.test"));
    auto kids = b.children(0);
    REQUIRE(kids.size() == 1);
    CHECK(kids[0].title == "New");
    CHECK(kids[0].url == "https://new.test");
}

TEST_CASE("remove deletes and keeps positions contiguous") {
    auto db = open_bookmarks();
    BookmarkService b(db);
    b.add(0, "A", "https://a.test", false);
    int64_t mid = b.add(0, "B", "https://b.test", false);
    b.add(0, "C", "https://c.test", false);

    REQUIRE(b.remove(mid));
    auto kids = b.children(0);
    REQUIRE(kids.size() == 2);
    CHECK(kids[0].position == 0);
    CHECK(kids[1].position == 1);
    CHECK(kids[0].title == "A");
    CHECK(kids[1].title == "C");
}

TEST_CASE("all_bookmarks returns only non-folder items") {
    auto db = open_bookmarks();
    BookmarkService b(db);
    b.add(0, "Folder", "", true);
    b.add(0, "Link", "https://link.test", false);
    auto all = b.all_bookmarks();
    REQUIRE(all.size() == 1);
    CHECK(all[0].url == "https://link.test");
}

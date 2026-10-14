#include <doctest.h>

#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::storage;

static Database open_memory() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

TEST_CASE("in-memory database opens and migrates to the latest version") {
    Database db;
    CHECK(db.open(":memory:"));
    CHECK(Migrator::migrate(db) == DbError::Ok);
    // user_version tracks the number of applied builtin migrations.
    auto stmt = db.prepare("PRAGMA user_version;");
    REQUIRE(stmt);
    int version = 0;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        version = stmt->column_int(0);
    }
    CHECK(version == static_cast<int>(Migrator::builtin_migrations().size()));
}

TEST_CASE("transactions roll back on early return") {
    auto db = open_memory();
    db.execute("INSERT INTO history(url, title, last_visit) VALUES ('https://a.test', 'A', 0);");
    CHECK_THROWS_AS([&] {
        Transaction txn(db);
        db.execute("INSERT INTO history(url, title, last_visit) VALUES ('https://b.test', 'B', 0);");
        throw std::runtime_error("boom");
    }(), std::runtime_error);
    // The throw destroyed txn before commit -> rollback. Only 'A' should remain.
    auto count = db.prepare("SELECT COUNT(*) FROM history;");
    int n = 0;
    for (auto r = count->step(); r && *r; r = count->step()) n = count->column_int(0);
    CHECK(n == 1);
}

TEST_CASE("statement binding round-trips text and int") {
    auto db = open_memory();
    db.execute("INSERT INTO history(url, title, last_visit) VALUES ('https://x.test', 'X', 1);");
    auto stmt = db.prepare("SELECT url, last_visit FROM history WHERE url = ?1;");
    REQUIRE(stmt);
    stmt->bind(1, "https://x.test");
    int visits = 0;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        visits = stmt->column_int(1);
        CHECK(stmt->column_text(0) == "https://x.test");
    }
    CHECK(visits == 1);
}

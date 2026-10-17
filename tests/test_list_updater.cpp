// Aurora — in-app filter-list updater tests.
//
// The updater's whole promise is "a bad download never replaces a working
// list", so these drive adblock::ListUpdater through a fake transport against
// a throwaway profile directory and check the files on disk after every
// success and every failure mode.
#include <doctest.h>

#include "adblock/ListUpdater.hpp"
#include "settings/Settings.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace aurora::adblock;
namespace fs = std::filesystem;

namespace {

// A tiny but structurally real list: header, comment, N network rules.
std::string make_list(int rules, const std::string& tag = "a") {
    std::string out = "[Adblock Plus 2.0]\n! Title: test list\n";
    for (int i = 0; i < rules; ++i)
        out += "||" + tag + std::to_string(i) + ".example^\n";
    return out;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct TempProfile {
    fs::path dir;
    TempProfile() {
        dir = fs::temp_directory_path() /
              ("aurora_list_updater_" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()));
        fs::create_directories(dir);
    }
    ~TempProfile() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

ListUpdater::Fetcher fake_fetch(bool ok, std::string body,
                                std::string error = {}) {
    return [ok, body, error](const std::string&, ListUpdater::FetchDone done) {
        done(ok, body, error);
    };
}

} // namespace

TEST_CASE("validate_filter_list rejects empty, html, binary and short lists") {
    CHECK_FALSE(validate_filter_list("", 1).ok);
    CHECK_FALSE(validate_filter_list("   \n\r\n", 1).ok);
    CHECK_FALSE(validate_filter_list("<!doctype html><p>portal</p>", 1).ok);
    CHECK_FALSE(validate_filter_list(std::string("||a^\n\0||b^\n", 11), 1).ok);
    const auto short_list = validate_filter_list(make_list(5), 10);
    CHECK_FALSE(short_list.ok);
    CHECK(short_list.filter_lines == 5);
    const auto good = validate_filter_list(make_list(12), 10);
    CHECK(good.ok);
    // Header and comment lines are not rules.
    CHECK(good.filter_lines == 12);
}

TEST_CASE("a valid download replaces the list and keeps a backup") {
    TempProfile profile;
    ListUpdater updater(profile.dir.string(), "https://lists.test/easylist.txt",
                        /*min_filter_lines=*/10);
    const fs::path live = updater.list_path();
    {
        std::ofstream(live, std::ios::binary) << make_list(20, "old");
    }

    bool called = false;
    updater.update(fake_fetch(true, make_list(30, "new")),
                   [&](const ListUpdater::Result& r) {
                       called = true;
                       CHECK(r.ok);
                       CHECK(r.filter_lines == 30);
                       CHECK(r.source_url == "https://lists.test/easylist.txt");
                   });
    CHECK(called);
    CHECK_FALSE(updater.busy());
    CHECK(slurp(live) == make_list(30, "new"));
    CHECK(slurp(updater.backup_path()) == make_list(20, "old"));
    CHECK_FALSE(fs::exists(updater.list_path() + ".part"));

    // And the backup can be put back.
    CHECK(updater.rollback());
    CHECK(slurp(live) == make_list(20, "old"));
}

TEST_CASE("failed or invalid downloads leave the live list untouched") {
    TempProfile profile;
    ListUpdater updater(profile.dir.string(), "https://lists.test/l.txt", 10);
    const fs::path live = updater.list_path();
    {
        std::ofstream(live, std::ios::binary) << make_list(20, "keep");
    }
    for (const auto& fetch :
         {fake_fetch(false, {}, "HTTP 503"), fake_fetch(true, ""),
          fake_fetch(true, "<html>captive portal</html>"),
          fake_fetch(true, make_list(3))}) {
        bool ok = true;
        updater.update(fetch, [&](const ListUpdater::Result& r) { ok = r.ok; });
        CHECK_FALSE(ok);
        CHECK(slurp(live) == make_list(20, "keep"));
        CHECK_FALSE(fs::exists(updater.backup_path()));
    }
}

TEST_CASE("first install works without a previous list") {
    TempProfile profile;
    ListUpdater updater((profile.dir / "fresh").string(), "u", 10);
    const auto r = updater.install(make_list(10));
    CHECK(r.ok);
    CHECK(fs::exists(updater.list_path()));
    CHECK_FALSE(updater.rollback());  // nothing to roll back to
}

TEST_CASE("success is recorded in prefs; failures are not") {
    aurora::storage::Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(aurora::storage::Migrator::migrate(db) ==
            aurora::storage::DbError::Ok);
    aurora::settings::Settings prefs(db);
    prefs.ensure_schema();
    CHECK(ListUpdater::last_update(prefs).last_success_unix == 0);

    ListUpdater::Result failed;
    failed.source_url = "https://x.test";
    ListUpdater::record_success(prefs, failed, 123);
    CHECK(ListUpdater::last_update(prefs).last_success_unix == 0);

    ListUpdater::Result ok = failed;
    ok.ok = true;
    ListUpdater::record_success(prefs, ok, 1700000000);
    const auto rec = ListUpdater::last_update(prefs);
    CHECK(rec.last_success_unix == 1700000000);
    CHECK(rec.source_url == "https://x.test");
}

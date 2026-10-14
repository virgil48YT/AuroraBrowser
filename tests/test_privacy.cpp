#include <doctest.h>

#include "core/Url.hpp"
#include "privacy/BrowsingData.hpp"
#include "privacy/CertificateAudit.hpp"
#include "settings/Settings.hpp"
#include "storage/Migration.hpp"
#include "util/Sha256.hpp"

using namespace aurora;
using privacy::CertificateObservation;

static storage::Database privacy_database() {
    storage::Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(storage::Migrator::migrate(db) == storage::DbError::Ok);
    return db;
}

TEST_CASE("privacy defaults are secure with destructive exit actions opt-in") {
    auto db = privacy_database();
    settings::Settings prefs(db);
    prefs.ensure_schema();
    using K = settings::PrefKeys;
    using D = settings::PrefDefaults;
    CHECK(prefs.get_bool(K::kHttpsOnly, D::kHttpsOnly));
    CHECK(prefs.get_bool(K::kCertAudit, D::kCertAudit));
    CHECK(prefs.get_bool(K::kRefererTrim, D::kRefererTrim));
    CHECK_FALSE(prefs.get_bool(K::kClearOnExitCookies, D::kClearOnExitCookies));
    CHECK_FALSE(prefs.get_bool(K::kClearOnExitHistory, D::kClearOnExitHistory));
    CHECK_FALSE(prefs.get_bool(K::kClearOnExitDownloads, D::kClearOnExitDownloads));
    for (auto key : {K::kHttpsOnly, K::kCertAudit, K::kRefererTrim}) {
        prefs.set_bool(key, false);
        CHECK_FALSE(prefs.get_bool(key, true));
    }
    for (auto key : {K::kClearOnExitCookies, K::kClearOnExitHistory,
                     K::kClearOnExitDownloads}) {
        prefs.set_bool(key, true);
        CHECK(prefs.get_bool(key, false));
    }
}

TEST_CASE("HTTPS-only upgrades just the scheme without changing URL bytes") {
    CHECK(core::resolve_navigation(core::Url("http://example.com/path?q=1#part")) ==
          "https://example.com/path?q=1#part");
    CHECK(core::upgrade_http_url("HTTP://User:PASS@Example.com:8080?Token=A#B") ==
          "https://User:PASS@Example.com:8080?Token=A#B");
    CHECK(core::upgrade_http_url("http://[::1]:8000/a%2Fb?c=%23#d") ==
          "https://[::1]:8000/a%2Fb?c=%23#d");
    CHECK(core::upgrade_http_url("http://localhost:80/") ==
          "https://localhost:80/");
    CHECK(core::resolve_navigation(core::Url("http://example.com?q=1#f"), false) ==
          "http://example.com?q=1#f");
    CHECK(core::upgrade_http_url("http://example.com/", false) ==
          "http://example.com/");
}

TEST_CASE("HTTPS-only leaves secure internal extension and opaque URLs alone") {
    for (const auto* value : {"https://example.com?x=1#f", "about:blank",
                              "aurora://settings", "chrome-extension://abc/index.html",
                              "file:///tmp/a", "data:text/html,http://example.com",
                              "http://", "http:///no-host", "http://?no-host",
                              "http://#no-host", "http:/not-hierarchical"}) {
        CHECK(core::upgrade_http_url(value) == value);
    }
    CHECK(core::resolve_navigation(core::Url("find this"),
                                   "http://search.test/?q=") ==
          "https://search.test/?q=find+this");
    CHECK(core::resolve_navigation(core::Url("find this"),
                                   "http://search.test/?q=", false) ==
          "http://search.test/?q=find+this");
}

TEST_CASE("certificate audit migration upgrades v5 and is idempotent") {
    storage::Database db;
    REQUIRE(db.open(":memory:"));
    const auto migrations = storage::Migrator::builtin_migrations();
    for (const auto& migration : migrations) {
        if (migration.from_version < 5)
            REQUIRE(migration.apply(db) == storage::DbError::Ok);
    }
    REQUIRE(db.execute("PRAGMA user_version = 5;") == storage::DbError::Ok);
    REQUIRE(db.execute(
        "INSERT INTO history(url,title,last_visit) VALUES ('https://keep.test','Keep',1);")
        == storage::DbError::Ok);
    REQUIRE(storage::Migrator::migrate(db) == storage::DbError::Ok);
    REQUIRE(storage::Migrator::migrate(db) == storage::DbError::Ok);
    privacy::CertificateAudit audit(db);
    CHECK(audit.observe("https://keep.test", util::sha256_hex("leaf")) ==
          CertificateObservation::Recorded);
    history::HistoryService history(db);
    CHECK(history.recent(10).size() == 1);
}

TEST_CASE("certificate audit remembers first visit and alarms without replacing") {
    auto db = privacy_database();
    privacy::CertificateAudit audit(db);
    const auto first = util::sha256_hex("DER leaf 1");
    const auto second = util::sha256_hex("DER leaf 2");
    CHECK(audit.observe("https://example.test", first) ==
          CertificateObservation::Recorded);
    privacy::CertificateAudit reopened(db);
    CHECK(reopened.observe("https://example.test", first) ==
          CertificateObservation::Unchanged);
    CHECK(reopened.observe("https://example.test", second) ==
          CertificateObservation::Changed);
    CHECK(reopened.observe("https://example.test", first) ==
          CertificateObservation::Unchanged);
    CHECK(reopened.observe("https://example.test:8443", second) ==
          CertificateObservation::Recorded);
    CHECK(reopened.observe("https://sub.example.test", second) ==
          CertificateObservation::Recorded);
}

TEST_CASE("certificate audit never seeds TLS errors or accepts a change implicitly") {
    auto db = privacy_database();
    privacy::CertificateAudit audit(db);
    const auto first = util::sha256_hex("valid certificate");
    const auto second = util::sha256_hex("invalid certificate");
    CHECK(audit.observe("https://example.test", second, {}, false) ==
          CertificateObservation::Ignored);
    CHECK(audit.observe("https://example.test", first) ==
          CertificateObservation::Recorded);
    CHECK(audit.observe("https://example.test", second, {}, false) ==
          CertificateObservation::Changed);
    CHECK(audit.observe("https://example.test", second, {}, false, true) ==
          CertificateObservation::Accepted);
    CHECK(audit.observe("https://example.test", second) ==
          CertificateObservation::Unchanged);
}

TEST_CASE("certificate audit is read-only for every incognito observation") {
    auto db = privacy_database();
    privacy::CertificateAudit audit(db);
    const core::IncognitoPolicy private_policy{true};
    const auto first = util::sha256_hex("private leaf");
    const auto second = util::sha256_hex("changed private leaf");
    CHECK_FALSE(private_policy.may_write_certificate_audit());
    CHECK(audit.observe("https://private.test", first, private_policy) ==
          CertificateObservation::Ignored);
    CHECK(audit.observe("https://private.test", second) ==
          CertificateObservation::Recorded);
    CHECK(audit.observe("https://private.test", first, private_policy) ==
          CertificateObservation::Changed);
    CHECK(audit.observe("https://private.test", first, private_policy, true, true) ==
          CertificateObservation::Accepted);
    CHECK(audit.observe("https://private.test", second) ==
          CertificateObservation::Unchanged);
}

TEST_CASE("certificate audit ignores malformed inputs and reports unavailable storage") {
    auto db = privacy_database();
    privacy::CertificateAudit audit(db);
    for (const auto* origin : {"", "http://example.test", "https://",
                               "https://example.test/path", "https://example.test?x",
                               "https://example.test#f", "https://user@example.test"}) {
        CHECK(audit.observe(origin, util::sha256_hex("leaf")) ==
              CertificateObservation::Ignored);
    }
    CHECK(audit.observe("https://example.test", "") == CertificateObservation::Ignored);
    CHECK(audit.observe("https://example.test", std::string(64, 'z')) ==
          CertificateObservation::Ignored);
    REQUIRE(db.execute("DROP TABLE certificate_audit;") == storage::DbError::Ok);
    CHECK(audit.observe("https://example.test", util::sha256_hex("leaf")) ==
          CertificateObservation::StorageError);
}

TEST_CASE("shared browsing-data clear is selective and maintains history FTS") {
    auto db = privacy_database();
    history::HistoryService history(db);
    downloads::DownloadService downloads(db);
    history.add_visit(core::Url("https://example.test"), "Private marker", "", true);
    REQUIRE(downloads.begin("https://example.test/file", "/tmp/keep-file.txt", "", 100) > 0);
    const core::IncognitoPolicy normal;
    const auto none = privacy::clear_browsing_data(normal, &history, &downloads, false, false);
    CHECK(none.history == 0);
    CHECK(none.downloads == 0);
    const auto only_history = privacy::clear_browsing_data(normal, &history, &downloads, true, false);
    CHECK(only_history.history == 1);
    CHECK(only_history.downloads == 0);
    CHECK(history.recent(10).empty());
    CHECK(history.search("marker", 10).empty());
    CHECK(downloads.recent(10).size() == 1);
    const auto only_downloads = privacy::clear_browsing_data(normal, &history, &downloads, false, true);
    CHECK(only_downloads.downloads == 1);
    CHECK(downloads.recent(10).empty());
    CHECK(privacy::clear_browsing_data(normal, nullptr, nullptr, true, true).history == 0);
}

TEST_CASE("incognito exit cannot clear shared history or downloads") {
    auto db = privacy_database();
    history::HistoryService history(db);
    downloads::DownloadService downloads(db);
    history.add_visit(core::Url("https://keep.test"), "Keep", "", false);
    REQUIRE(downloads.begin("https://keep.test/file", "/tmp/keep.txt", "", 1) > 0);
    const core::IncognitoPolicy private_policy{true};
    CHECK_FALSE(private_policy.may_clear_profile_data());
    const auto cleared = privacy::clear_browsing_data(
        private_policy, &history, &downloads, true, true);
    CHECK(cleared.history == 0);
    CHECK(cleared.downloads == 0);
    CHECK(history.recent(10).size() == 1);
    CHECK(downloads.recent(10).size() == 1);
}

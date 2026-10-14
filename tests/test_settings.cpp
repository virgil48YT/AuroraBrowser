#include <doctest.h>

#include "permissions/PermissionStore.hpp"
#include "settings/Settings.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::storage;
using namespace aurora::settings;

static Database open_settings() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

TEST_CASE("defaults are returned when key is absent") {
    auto db = open_settings();
    Settings s(db);
    s.ensure_schema();
    CHECK(s.get_bool("never.set", true) == true);
    CHECK(s.get_int("never.set", 42) == 42);
    CHECK(s.get_string("never.set", "default") == "default");
}

TEST_CASE("set and get round-trips typed values") {
    auto db = open_settings();
    Settings s(db);
    s.ensure_schema();
    s.set_bool(PrefKeys::kShowBookmarkBar, true);
    s.set_int(PrefKeys::kTabSuspendAfterMinutes, 30);
    s.set_string(PrefKeys::kSearchEngine, "duckduckgo");

    CHECK(s.get_bool(PrefKeys::kShowBookmarkBar, false) == true);
    CHECK(s.get_int(PrefKeys::kTabSuspendAfterMinutes, 0) == 30);
    CHECK(s.get_string(PrefKeys::kSearchEngine, "") == "duckduckgo");
}

TEST_CASE("overwriting an existing key updates in place") {
    auto db = open_settings();
    Settings s(db);
    s.ensure_schema();
    s.set_string(PrefKeys::kTheme, "light");
    s.set_string(PrefKeys::kTheme, "dark");
    CHECK(s.get_string(PrefKeys::kTheme, "") == "dark");
}

TEST_CASE("read-only settings read the profile but every setter is a no-op") {
    auto db = open_settings();
    Settings normal(db);
    normal.ensure_schema();
    CHECK_FALSE(normal.is_read_only());
    normal.set_bool(PrefKeys::kRestoreSession, true);
    normal.set_int(PrefKeys::kTabSuspendAfterMinutes, 30);
    normal.set_string(PrefKeys::kSearchEngine, "google");
    normal.set_downloads_path("/saved/downloads");
    normal.set_downloads_ask_where(true);

    Settings private_prefs(db, true);
    REQUIRE(private_prefs.is_read_only());
    CHECK(private_prefs.get_bool(PrefKeys::kRestoreSession, false));
    CHECK(private_prefs.get_int(PrefKeys::kTabSuspendAfterMinutes, 0) == 30);
    CHECK(private_prefs.get_string(PrefKeys::kSearchEngine, "") == "google");
    const int changes = sqlite3_total_changes(db.handle());
    private_prefs.ensure_schema();
    private_prefs.set_bool(PrefKeys::kRestoreSession, false);
    private_prefs.set_int(PrefKeys::kTabSuspendAfterMinutes, 1);
    private_prefs.set_string(PrefKeys::kSearchEngine, "bing");
    private_prefs.set_downloads_path("/private/downloads");
    private_prefs.set_downloads_ask_where(false);
    private_prefs.set_string("private.only", "must not persist");

    CHECK(sqlite3_total_changes(db.handle()) == changes);
    CHECK(private_prefs.get_bool(PrefKeys::kRestoreSession, false));
    CHECK(private_prefs.get_int(PrefKeys::kTabSuspendAfterMinutes, 0) == 30);
    CHECK(private_prefs.get_string(PrefKeys::kSearchEngine, "") == "google");
    CHECK(private_prefs.downloads_path() == "/saved/downloads");
    CHECK(private_prefs.downloads_ask_where());
    CHECK(private_prefs.get_string("private.only", "absent") == "absent");
    // A fresh, uncached reader also proves no writes reached SQLite.
    Settings persisted(db);
    CHECK(persisted.get_string(PrefKeys::kSearchEngine, "") == "google");
    CHECK(persisted.downloads_path() == "/saved/downloads");
    CHECK(persisted.get_string("private.only", "absent") == "absent");
}

TEST_CASE("private views do not freeze normal writes or cache stale preferences") {
    auto db = open_settings();
    Settings normal(db);
    normal.ensure_schema();
    normal.set_string(PrefKeys::kTheme, "light");
    auto first = normal.read_only_view();
    auto sibling = first->read_only_view();
    CHECK_FALSE(normal.is_read_only());
    CHECK(first->is_read_only());
    CHECK(sibling->is_read_only());
    CHECK(first->get_string(PrefKeys::kTheme, "") == "light");
    CHECK(sibling->get_string(PrefKeys::kTheme, "") == "light");
    first->set_string(PrefKeys::kTheme, "system");
    normal.set_string(PrefKeys::kTheme, "dark");
    CHECK(first->get_string(PrefKeys::kTheme, "") == "dark");
    CHECK(sibling->get_string(PrefKeys::kTheme, "") == "dark");
    // A sibling borrows the database, never the source view's lifetime.
    first.reset();
    CHECK(sibling->get_string(PrefKeys::kTheme, "") == "dark");
    sibling->set_string(PrefKeys::kTheme, "light");
    CHECK(normal.get_string(PrefKeys::kTheme, "") == "dark");
}

TEST_CASE("private settings suppress remembered permissions as well") {
    auto db = open_settings();
    Settings normal(db);
    normal.ensure_schema();
    auto private_prefs = normal.read_only_view();
    aurora::permissions::PermissionStore store(*private_prefs);
    using aurora::permissions::PermissionDecision;
    using aurora::permissions::PermissionKind;
    const std::string origin = "https://private.example/";
    const int changes = sqlite3_total_changes(db.handle());
    store.remember(origin, PermissionKind::AudioCapture, PermissionDecision::Allow);
    store.forget(origin);
    CHECK(sqlite3_total_changes(db.handle()) == changes);
    CHECK(store.decision(origin, PermissionKind::AudioCapture) ==
          PermissionDecision::Ask);
}

// --- Site permissions (permissions::PermissionStore on top of Settings) ---

TEST_CASE("an unknown origin asks; remembered decisions stick per origin/kind") {
    auto db = open_settings();
    Settings s(db);
    s.ensure_schema();
    aurora::permissions::PermissionStore store(s);
    using aurora::permissions::PermissionDecision;
    using aurora::permissions::PermissionKind;

    const std::string origin = "https://meet.example.com";
    // Default-deny's first half: unknown means prompt, never silent allow.
    CHECK(store.decision(origin, PermissionKind::AudioCapture) ==
          PermissionDecision::Ask);
    CHECK(store.decision(origin, PermissionKind::VideoCapture) ==
          PermissionDecision::Ask);

    store.remember(origin, PermissionKind::AudioCapture,
                   PermissionDecision::Allow);
    CHECK(store.decision(origin, PermissionKind::AudioCapture) ==
          PermissionDecision::Allow);
    // Per-kind: allowing the mic must not open the camera.
    CHECK(store.decision(origin, PermissionKind::VideoCapture) ==
          PermissionDecision::Ask);
    // Per-origin: another site is unaffected.
    CHECK(store.decision("https://other.example.com",
                         PermissionKind::AudioCapture) ==
          PermissionDecision::Ask);

    store.remember(origin, PermissionKind::AudioCapture,
                   PermissionDecision::Deny);
    CHECK(store.decision(origin, PermissionKind::AudioCapture) ==
          PermissionDecision::Deny);
}

TEST_CASE("origin_of scopes decisions to scheme://host[:port]") {
    using aurora::permissions::origin_of;
    CHECK(origin_of("https://example.com/path?q=1") == "https://example.com");
    CHECK(origin_of("https://EXAMPLE.com:8443/x") == "https://example.com:8443");
    CHECK(origin_of("http://user:pass@example.com/") == "http://example.com");
    // Same host, different scheme/port -> different permission scope.
    CHECK(origin_of("http://example.com") != origin_of("https://example.com"));
    // Non-web and host-less URLs are unkeyable (always prompt / deny).
    CHECK(origin_of("data:text/html,hi").empty());
    CHECK(origin_of("file:///etc/passwd").empty());
    CHECK(origin_of("not a url").empty());
    CHECK(origin_of("").empty());
}

TEST_CASE("forget clears every kind for the origin") {
    auto db = open_settings();
    Settings s(db);
    s.ensure_schema();
    aurora::permissions::PermissionStore store(s);
    using aurora::permissions::PermissionDecision;
    using aurora::permissions::PermissionKind;
    const std::string origin = "https://meet.example.com";
    store.remember(origin, PermissionKind::AudioCapture, PermissionDecision::Allow);
    store.remember(origin, PermissionKind::VideoCapture, PermissionDecision::Deny);
    store.forget(origin);
    CHECK(store.decision(origin, PermissionKind::AudioCapture) ==
          PermissionDecision::Ask);
    CHECK(store.decision(origin, PermissionKind::VideoCapture) ==
          PermissionDecision::Ask);
    // An unkeyable origin can never remember anything.
    store.remember("", PermissionKind::AudioCapture, PermissionDecision::Allow);
    CHECK(store.decision("", PermissionKind::AudioCapture) ==
          PermissionDecision::Ask);
}

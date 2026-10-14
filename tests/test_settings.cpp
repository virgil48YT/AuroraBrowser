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

// Aurora — per-site preference listing: startup mode resolution, prefix
// enumeration, remembered permissions and per-site zoom.
//
// These back aurora://site-settings and the startup path, so they are pinned
// against a real (in-memory) profile database without Qt or CEF.
#include <doctest.h>

#include "permissions/PermissionStore.hpp"
#include "settings/Settings.hpp"
#include "settings/SiteZoomStore.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::settings;
using namespace aurora::permissions;

namespace {
aurora::storage::Database open_profile() {
    aurora::storage::Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(aurora::storage::Migrator::migrate(db) ==
            aurora::storage::DbError::Ok);
    return db;
}
} // namespace

TEST_CASE("startup mode falls back to the restore-session boolean") {
    auto db = open_profile();
    Settings prefs(db);
    prefs.ensure_schema();
    // Fresh profile: kRestoreSession defaults on => continue.
    CHECK(resolve_startup_mode(prefs) == StartupMode::Continue);
    prefs.set_bool(PrefKeys::kRestoreSession, false);
    CHECK(resolve_startup_mode(prefs) == StartupMode::Home);
    // An explicit mode wins over the boolean...
    prefs.set_string(PrefKeys::kStartupMode, "continue");
    CHECK(resolve_startup_mode(prefs) == StartupMode::Continue);
    // ...but an unknown value does not.
    prefs.set_string(PrefKeys::kStartupMode, "bogus");
    CHECK(resolve_startup_mode(prefs) == StartupMode::Home);
    // "specific" needs a URL; without one it opens the home page.
    prefs.set_string(PrefKeys::kStartupMode, "specific");
    CHECK(resolve_startup_mode(prefs) == StartupMode::Home);
    prefs.set_string(PrefKeys::kStartupHomeUrl, "https://example.com");
    CHECK(resolve_startup_mode(prefs) == StartupMode::Specific);
}

TEST_CASE("entries_with_prefix lists exact-prefix keys and skips empties") {
    auto db = open_profile();
    Settings prefs(db);
    prefs.ensure_schema();
    prefs.set_string("perm.audio.https://a.test", "allow");
    prefs.set_string("perm.video.https://b.test", "");
    prefs.set_string("permx.audio.https://c.test", "allow");
    prefs.set_string("zoom.https://a.test", "1.5");
    const auto perms = prefs.entries_with_prefix("perm.");
    REQUIRE(perms.size() == 1);
    CHECK(perms[0].first == "perm.audio.https://a.test");

    prefs.remove("zoom.https://a.test");
    CHECK(prefs.entries_with_prefix("zoom.").empty());
    CHECK(prefs.get_string("zoom.https://a.test", "none") == "none");
}

TEST_CASE("PermissionStore lists remembered decisions and forgets them") {
    auto db = open_profile();
    Settings prefs(db);
    prefs.ensure_schema();
    PermissionStore store(prefs);
    store.remember("https://a.test", PermissionKind::AudioCapture,
                   PermissionDecision::Allow);
    store.remember("https://a.test:8443", PermissionKind::Notification,
                   PermissionDecision::Deny);
    auto list = store.list();
    REQUIRE(list.size() == 2);
    CHECK(list[0].origin == "https://a.test");
    CHECK(list[0].kind == PermissionKind::AudioCapture);
    CHECK(list[0].decision == PermissionDecision::Allow);
    CHECK(list[1].origin == "https://a.test:8443");
    CHECK(list[1].decision == PermissionDecision::Deny);

    store.remember("https://a.test", PermissionKind::AudioCapture,
                   PermissionDecision::Ask);
    list = store.list();
    REQUIRE(list.size() == 1);
    CHECK(list[0].kind == PermissionKind::Notification);
}

TEST_CASE("SiteZoomStore round-trips levels and clears the default") {
    auto db = open_profile();
    Settings prefs(db);
    prefs.ensure_schema();
    SiteZoomStore zoom(prefs);
    double level = 0.0;
    CHECK_FALSE(zoom.level("https://a.test", level));
    zoom.set_level("https://a.test", 1.5);
    REQUIRE(zoom.level("https://a.test", level));
    CHECK(level == doctest::Approx(1.5));
    zoom.set_level("https://b.test", -0.3);
    CHECK(zoom.list().size() == 2);
    // Back to 100% removes the key instead of storing 0.
    zoom.set_level("https://a.test", 0.0);
    CHECK_FALSE(zoom.level("https://a.test", level));
    zoom.clear("https://b.test");
    CHECK(zoom.list().empty());
    CHECK(SiteZoomStore::percent(0.0) == 100);
    CHECK(SiteZoomStore::percent(1.0) == 120);
    // Corrupt values read as default rather than garbage.
    prefs.set_string("zoom.https://c.test", "1,5");
    CHECK_FALSE(zoom.level("https://c.test", level));
}

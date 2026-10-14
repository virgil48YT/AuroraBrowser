// Aurora — ad-block unit tests.
//
// Two layers are covered here without any web engine:
//   * site_key() host normalization — the key everything else (exceptions,
//     the engine snapshot, the shield toggle) agrees on;
//   * AdblockService exception persistence against the real SQLite schema;
//   * AdblockEngine rule behavior when the Rust engine is linked in
//     (AURORA_ADBLOCK_FFI). In a stub build those tests still run and assert
//     the stub's contract: nothing is ever blocked, nothing crashes.

#include <doctest.h>

#include "adblock/AdblockEngine.hpp"
#include "adblock/AdblockService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

#include <algorithm>
#include <utility>

using namespace aurora::adblock;
using namespace aurora::storage;

// --- site_key ----------------------------------------------------------------

TEST_CASE("site_key extracts and normalizes the host") {
    CHECK(site_key("https://Example.COM/page?x=1") == "example.com");
    CHECK(site_key("https://www.example.com/") == "example.com");
    CHECK(site_key("http://user:pass@site.test/a#b") == "site.test");
    CHECK(site_key("https://site.test:8443/x") == "site.test");
    // Subdomains are distinct sites (no eTLD+1 guessing — the key matches
    // what the user sees in the omnibox).
    CHECK(site_key("https://a.example.com/") == "a.example.com");
    CHECK(site_key("https://example.com/") != site_key("https://a.example.com/"));
    // Only one leading "www." is stripped; deeper prefixes stay.
    CHECK(site_key("https://www2.example.com/") == "www2.example.com");
}

TEST_CASE("site_key is empty for URLs without a host") {
    CHECK(site_key("").empty());
    CHECK(site_key("about:blank").empty());
    CHECK(site_key("data:text/html,hello").empty());
}

TEST_CASE("site_key is idempotent for already-normalized keys") {
    // The engine snapshot stores normalized keys; callers hand bare hosts
    // back to the service. Round-tripping must not change the value.
    CHECK(site_key("example.com") == "example.com");
    CHECK(site_key(site_key("https://www.Example.com/x")) == "example.com");
}

// --- AdblockService exceptions ----------------------------------------------

static Database open_adblock() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

TEST_CASE("exceptions persist per site key") {
    auto db = open_adblock();
    AdblockService s(db);

    CHECK_FALSE(s.is_site_allowed("https://example.com/page"));
    CHECK(s.set_site_allowed("https://www.example.com/", true));
    // The www-less key matches: the toggle normalizes before writing.
    CHECK(s.is_site_allowed("https://example.com/other"));
    // Different subdomain is a different site.
    CHECK_FALSE(s.is_site_allowed("https://a.example.com/"));

    // Clearing restores blocking, and re-clearing reports no change.
    CHECK(s.set_site_allowed("https://example.com/", false));
    CHECK_FALSE(s.is_site_allowed("https://example.com/"));
    CHECK_FALSE(s.set_site_allowed("https://example.com/", false));
}

TEST_CASE("toggle_site flips and reports the new state") {
    auto db = open_adblock();
    AdblockService s(db);

    CHECK(s.toggle_site("https://news.test/article") == true);
    CHECK(s.is_site_allowed("https://news.test/"));
    CHECK(s.toggle_site("https://news.test/") == false);
    CHECK_FALSE(s.is_site_allowed("https://news.test/"));
}

TEST_CASE("list_exception_sites returns every key, sorted") {
    auto db = open_adblock();
    AdblockService s(db);
    REQUIRE(s.set_site_allowed("https://zeta.test/", true));
    REQUIRE(s.set_site_allowed("https://alpha.test/", true));

    const auto sites = s.list_exception_sites();
    REQUIRE(sites.size() == 2);
    CHECK(sites[0] == "alpha.test");
    CHECK(sites[1] == "zeta.test");
}

TEST_CASE("empty or hostless urls are never excepted") {
    auto db = open_adblock();
    AdblockService s(db);
    CHECK_FALSE(s.set_site_allowed("", true));
    CHECK_FALSE(s.set_site_allowed("data:text/html,x", true));
    CHECK(s.list_exception_sites().empty());
}

// --- AdblockEngine -----------------------------------------------------------

TEST_CASE("starter rules are non-empty, newline separated, and self-authored") {
    const std::string rules = starter_rules();
    CHECK_FALSE(rules.empty());
    // Every line is either a comment or a filter; no blank runs that would
    // trip the FFI parser.
    size_t filters = 0;
    size_t pos = 0;
    while (pos < rules.size()) {
        const auto end = rules.find('\n', pos);
        const std::string line =
            rules.substr(pos, end == std::string::npos ? std::string::npos
                                                       : end - pos);
        if (!line.empty() && line[0] != '!') ++filters;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    CHECK(filters >= 10);
    CHECK(rules.find("##.video-ads") == std::string::npos);
    CHECK(rules.find("##.ytp-ad-skip-button") == std::string::npos);
    CHECK(rules.find("##.ytp-cued-thumbnail-overlay") == std::string::npos);
}

#if defined(AURORA_ADBLOCK_FFI)
TEST_CASE("engine blocks a known ad host and allows everything else") {
    AdblockEngine e(starter_rules());
    REQUIRE(e.available());

    CHECK(e.should_block(
        "https://doubleclick.net/instream/ad_status.js",
        "https://example.com/page", "script"));
    CHECK(e.should_block(
        "https://tpc.googlesyndication.com/pagead/js/adsbygoogle.js",
        "https://example.com/", "script"));
    CHECK(e.should_block(
        "https://cdn.adnxs.com/v?id=1", "https://example.com/", "image"));

    // First-party content on the same page is untouched.
    CHECK_FALSE(e.should_block(
        "https://example.com/app.js", "https://example.com/", "script"));
    CHECK_FALSE(e.should_block(
        "https://example.com/photo.jpg", "https://example.com/", "image"));
    // Unrelated hosts are untouched.
    CHECK_FALSE(e.should_block(
        "https://cdn.example.org/lib.js", "https://example.com/", "script"));
}

TEST_CASE("engine with an empty rule list blocks nothing") {
    AdblockEngine e("");
    REQUIRE(e.available());
    CHECK_FALSE(e.should_block(
        "https://doubleclick.net/x", "https://example.com/", "script"));
    CHECK_FALSE(e.should_block("https://anything.test/", "", "other"));
}

TEST_CASE("engine_available matches a constructed engine") {
    CHECK(engine_available());
    AdblockEngine e(starter_rules());
    CHECK(e.available());
}

TEST_CASE("cosmetic resource snapshots include selectors and survive engine moves") {
    AdblockEngine e("example.com##.sponsor\nexample.com##.ad\n");
    REQUIRE(e.available());
    const auto resources = e.cosmetic_resources("https://example.com/");
    CHECK_FALSE(resources.empty());
    CHECK(resources.json.find("\"hide_selectors\"") != std::string::npos);
    CHECK(resources.json.find("\".ad\"") != std::string::npos);
    CHECK(resources.json.find("\".sponsor\"") != std::string::npos);
    CHECK(resources.scripts.empty());  // no scriptlet resources in this engine

    const auto selectors = e.cosmetic_filters("https://example.com/");
    CHECK(std::find(selectors.begin(), selectors.end(), ".ad") != selectors.end());
    AdblockEngine moved(std::move(e));
    CHECK(e.cosmetic_resources("https://example.com/").empty());
    CHECK(moved.cosmetic_resources("https://example.com/").json == resources.json);
}

TEST_CASE("engine cosmetic exceptions are not replaced by local rule parsing") {
    AdblockEngine e("example.com##.ad\nexample.com#@#.ad\nexample.com##.sponsor\n");
    REQUIRE(e.available());
    const auto selectors = e.cosmetic_filters("https://example.com/");
    CHECK(std::find(selectors.begin(), selectors.end(), ".ad") == selectors.end());
    CHECK(std::find(selectors.begin(), selectors.end(), ".sponsor") != selectors.end());
    const auto resources = e.cosmetic_resources("https://example.com/");
    CHECK(resources.json.find("\"exceptions\"") != std::string::npos);
}
#else
TEST_CASE("stub engine blocks nothing and reports unavailable") {
    // Built without the Rust static library: every call must be a safe
    // no-op, and the UI layer reports the engine as missing.
    CHECK_FALSE(engine_available());
    AdblockEngine e(starter_rules());
    CHECK_FALSE(e.available());
    CHECK_FALSE(e.should_block(
        "https://doubleclick.net/instream/ad_status.js",
        "https://example.com/", "script"));
    CHECK_FALSE(e.should_block("https://anything/", "https://anywhere/", "other"));
    const auto resources = e.cosmetic_resources("https://www.youtube.com/watch?v=test");
    CHECK(resources.empty());
    CHECK(resources.json.empty());
    CHECK(resources.scripts.empty());
    CHECK(e.cosmetic_filters("https://example.com/").empty());

    AdblockEngine moved(std::move(e));
    CHECK_FALSE(moved.available());
    CHECK(moved.cosmetic_resources("https://example.com/").empty());
    CHECK(e.cosmetic_resources("").empty());
}
#endif

TEST_CASE("default cosmetic resources are an empty owning value") {
    CosmeticResources resources;
    CHECK(resources.empty());
    resources.scripts = "void 0;";
    CHECK_FALSE(resources.empty());
    const auto copy = resources;
    resources.scripts.clear();
    CHECK(copy.scripts == "void 0;");
    CHECK(resources.empty());
}

TEST_CASE("load_rules combines starter rules with an optional user file") {
    // A path that does not exist must degrade to the starter list, not fail.
    const std::string rules = load_rules("/nonexistent/adblock-rules.txt");
    CHECK(rules.find(starter_rules()) == 0);
}

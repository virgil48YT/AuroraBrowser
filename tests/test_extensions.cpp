#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "extensions/ExtensionService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::storage;
using namespace aurora::extensions;

static Database open_extensions() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

// Write a manifest.json into a fresh temp directory and return its path.
static std::filesystem::path make_extension_dir(const std::string& name,
                                                const std::string& manifest) {
    auto dir = std::filesystem::temp_directory_path() /
               ("aurora_ext_test_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "manifest.json");
    out << manifest;
    return dir;
}

TEST_CASE("manifest parsing reads the fields the extensions page shows") {
    auto m = parse_manifest(R"JSON({
        "manifest_version": 3,
        "name": "uBlock Origin Lite",
        "version": "2.1.4",
        "description": "Content blocker",
        "permissions": ["declarativeNetRequest", "storage"],
        "action": { "default_title": "Click me" }
    })JSON");
    REQUIRE(m.has_value());
    CHECK(m->name == "uBlock Origin Lite");
    CHECK(m->version == "2.1.4");
    CHECK(m->description == "Content blocker");
    CHECK(m->manifest_version == 3);
}

TEST_CASE("manifest parsing survives nested objects, escapes and comments") {
    // A "name" inside a nested object must not be mistaken for the real one,
    // and Chrome tolerates comments in manifest.json.
    auto m = parse_manifest(R"JSON({
        // the extension's own name comes first
        "name": "Tab \"Manager\" \u00e9",
        "icons": { "16": "a.png", "48": "b.png" },
        "background": { "service_worker": "sw.js", "name": "not-this" },
        /* block comment */
        "version": "0.9"
    })JSON");
    REQUIRE(m.has_value());
    CHECK(m->name == "Tab \"Manager\" \xc3\xa9");
    CHECK(m->version == "0.9");
}

TEST_CASE("a manifest without a name or with broken syntax is rejected") {
    CHECK_FALSE(parse_manifest(R"JSON({"version": "1.0"})JSON").has_value());
    CHECK_FALSE(parse_manifest("not json at all").has_value());
    CHECK_FALSE(parse_manifest(R"JSON({"name": "unterminated)JSON").has_value());
    CHECK_FALSE(parse_manifest("").has_value());
}

TEST_CASE("adding an unpacked extension registers its manifest metadata") {
    auto db = open_extensions();
    ExtensionService svc(db);
    const auto dir = make_extension_dir(
        "basic", R"JSON({"name": "Reader", "version": "1.2", "manifest_version": 3})JSON");

    const int64_t id = svc.add_unpacked(dir.string());
    CHECK(id > 0);

    const auto all = svc.all();
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "Reader");
    CHECK(all[0].version == "1.2");
    CHECK(all[0].manifest_version == 3);
    CHECK(all[0].enabled);
    CHECK(svc.enabled_paths().size() == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE("a folder without a manifest is not an extension") {
    auto db = open_extensions();
    ExtensionService svc(db);
    auto dir = std::filesystem::temp_directory_path() / "aurora_ext_test_empty";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    CHECK(svc.add_unpacked(dir.string()) == 0);
    CHECK(svc.all().empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("re-adding a folder refreshes it instead of duplicating it") {
    auto db = open_extensions();
    ExtensionService svc(db);
    const auto dir = make_extension_dir(
        "upgrade", R"JSON({"name": "Notes", "version": "1.0"})JSON");
    const int64_t first = svc.add_unpacked(dir.string());
    REQUIRE(first > 0);

    // The user edits the extension and re-adds the same folder.
    {
        std::ofstream out(dir / "manifest.json");
        out << R"JSON({"name": "Notes", "version": "2.0"})JSON";
    }
    const int64_t second = svc.add_unpacked(dir.string());
    CHECK(second == first);

    const auto all = svc.all();
    REQUIRE(all.size() == 1);
    CHECK(all[0].version == "2.0");

    std::filesystem::remove_all(dir);
}

TEST_CASE("disabling an extension keeps it listed but off the load list") {
    auto db = open_extensions();
    ExtensionService svc(db);
    const auto dir = make_extension_dir(
        "toggle", R"JSON({"name": "Picker", "version": "3.0"})JSON");
    const int64_t id = svc.add_unpacked(dir.string());
    REQUIRE(id > 0);

    CHECK(svc.set_enabled(id, false));
    CHECK(svc.all().size() == 1);
    CHECK(svc.enabled_paths().empty());

    CHECK(svc.set_enabled(id, true));
    CHECK(svc.enabled_paths().size() == 1);

    CHECK(svc.remove(id));
    CHECK(svc.all().empty());

    std::filesystem::remove_all(dir);
}

// --- Packaged (.crx) installs: validate honestly, decline clearly ----------

static std::filesystem::path make_crx(const std::string& name,
                                      const std::string& bytes) {
    auto path = std::filesystem::temp_directory_path() /
                ("aurora_crx_test_" + name + ".crx");
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    return path;
}

static std::string crx_bytes(std::uint32_t version, std::uint32_t header_len,
                             const std::string& header,
                             const std::string& payload) {
    std::string out("Cr24", 4);
    const auto put32 = [&out](std::uint32_t v) {
        out.push_back(static_cast<char>(v & 0xff));
        out.push_back(static_cast<char>((v >> 8) & 0xff));
        out.push_back(static_cast<char>((v >> 16) & 0xff));
        out.push_back(static_cast<char>((v >> 24) & 0xff));
    };
    put32(version);
    put32(header_len);
    out += header;
    out += payload;
    return out;
}

TEST_CASE("a well-formed crx2 package is recognized and honestly declined") {
    auto db = open_extensions();
    ExtensionService svc(db);
    const auto path = make_crx(
        "v2", crx_bytes(2, 4, "HDR!", "PK\x03\x04" "fake-zip-bytes"));
    const auto r = svc.install_from_file(path.string());
    CHECK(r.result == CrxInstallResult::Unsupported);
    CHECK(r.format_version == 2);
    CHECK_FALSE(r.detail.empty());
    // Nothing was registered as a side effect.
    CHECK(svc.all().empty());
    std::filesystem::remove(path);
}

TEST_CASE("a well-formed crx3 package is recognized and honestly declined") {
    auto db = open_extensions();
    ExtensionService svc(db);
    const auto path = make_crx(
        "v3", crx_bytes(3, 3, "abc", "PK\x03\x04" "fake-zip-bytes"));
    const auto r = svc.install_from_file(path.string());
    CHECK(r.result == CrxInstallResult::Unsupported);
    CHECK(r.format_version == 3);
    std::filesystem::remove(path);
}

TEST_CASE("malformed and non-crx files are reported as not-a-crx") {
    auto db = open_extensions();
    ExtensionService svc(db);

    const auto missing = std::filesystem::temp_directory_path() /
                         "aurora_crx_test_missing.crx";
    std::filesystem::remove(missing);
    CHECK(svc.install_from_file(missing.string()).result ==
          CrxInstallResult::NotACrx);

    const auto tiny = make_crx("tiny", "Cr2");
    CHECK(svc.install_from_file(tiny.string()).result ==
          CrxInstallResult::NotACrx);

    const auto no_magic = make_crx("nomagic", "MZ.." "rest-of-file-ok");
    CHECK(svc.install_from_file(no_magic.string()).result ==
          CrxInstallResult::NotACrx);

    const auto bad_version = make_crx(
        "badver", crx_bytes(9, 4, "HDR!", "PK\x03\x04" "zip"));
    CHECK(svc.install_from_file(bad_version.string()).result ==
          CrxInstallResult::NotACrx);

    // Header length points past the end of the file.
    const auto truncated = make_crx(
        "trunc", crx_bytes(3, 1024, "abc", "PK\x03\x04"));
    CHECK(svc.install_from_file(truncated.string()).result ==
          CrxInstallResult::NotACrx);

    // Header fits but the payload is not a ZIP archive.
    const auto not_zip = make_crx(
        "notzip", crx_bytes(2, 4, "HDR!", "NOPE" "not-a-zip"));
    CHECK(svc.install_from_file(not_zip.string()).result ==
          CrxInstallResult::NotACrx);

    for (const auto& p : {tiny, no_magic, bad_version, truncated, not_zip})
        std::filesystem::remove(p);
}

// --------------------------------------------------------------------------
// Manifest artwork: what the extensions page shows on each card.
// --------------------------------------------------------------------------

TEST_CASE("manifest parsing keeps the largest declared icon") {
    auto m = parse_manifest(R"JSON({
        "name": "Blocker",
        "icons": { "16": "s.png", "128": "big.png", "48": "m.png" }
    })JSON");
    REQUIRE(m.has_value());
    CHECK(m->icon_path == "big.png");
}

TEST_CASE("an action default_icon is the fallback when there is no icons map") {
    auto str = parse_manifest(R"JSON({
        "name": "Picker",
        "action": { "default_title": "Pick", "default_icon": "toolbar.png" }
    })JSON");
    REQUIRE(str.has_value());
    CHECK(str->icon_path == "toolbar.png");

    auto map = parse_manifest(R"JSON({
        "name": "Picker",
        "browser_action": { "default_icon": { "19": "a.png", "38": "b.png" } }
    })JSON");
    REQUIRE(map.has_value());
    CHECK(map->icon_path == "b.png");

    // A top-level icons map wins over the action's icon, as in Chrome.
    auto both = parse_manifest(R"JSON({
        "name": "Picker",
        "icons": { "64": "real.png" },
        "action": { "default_icon": "toolbar.png" }
    })JSON");
    REQUIRE(both.has_value());
    CHECK(both->icon_path == "real.png");

    // No artwork declared at all: the UI falls back to a monogram tile.
    auto none = parse_manifest(R"JSON({"name": "Bare"})JSON");
    REQUIRE(none.has_value());
    CHECK(none->icon_path.empty());
}

TEST_CASE("icon_file_for_dir resolves real files and refuses escapes") {
    const auto dir = make_extension_dir(
        "icons",
        R"JSON({"name": "Arty", "version": "1.0",
                "icons": { "128": "img/icon.png" }})JSON");
    std::filesystem::create_directories(dir / "img");
    { std::ofstream out(dir / "img" / "icon.png"); out << "not-really-a-png"; }

    const auto manifest = read_manifest_dir(dir.string());
    REQUIRE(manifest.has_value());
    CHECK(manifest->icon_path == "img/icon.png");
    const auto resolved = icon_file_for_dir(dir.string(), *manifest);
    // icon_file_for_dir canonicalizes, and on Windows the temp path arrives as
    // an 8.3 short name (RUNNER~1) — compare canonical forms, not raw strings.
    CHECK(resolved ==
          std::filesystem::weakly_canonical(dir / "img" / "icon.png").string());

    // Declared but missing on disk -> no icon.
    Manifest missing = *manifest;
    missing.icon_path = "img/absent.png";
    CHECK(icon_file_for_dir(dir.string(), missing).empty());

    // A path escaping the extension folder is refused, not read.
    Manifest escape = *manifest;
    escape.icon_path = "../../etc/passwd";
    CHECK(icon_file_for_dir(dir.string(), escape).empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("all_detailed re-reads the manifest for name, version and icon") {
    auto db = open_extensions();
    ExtensionService svc(db);
    const auto dir = make_extension_dir(
        "detailed",
        R"JSON({"name": "Old Name", "version": "1.0"})JSON");
    REQUIRE(svc.add_unpacked(dir.string()) > 0);

    // The user updates the extension in place; the registry row is stale.
    {
        std::ofstream out(dir / "manifest.json");
        out << R"JSON({"name": "New Name", "version": "2.0",
                       "icons": { "48": "icon.png" }})JSON";
    }
    { std::ofstream out(dir / "icon.png"); out << "png"; }

    const auto plain = svc.all();
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].name == "Old Name");     // registry rows are not re-read
    CHECK(plain[0].icon_file.empty());

    const auto detailed = svc.all_detailed();
    REQUIRE(detailed.size() == 1);
    CHECK(detailed[0].name == "New Name");
    CHECK(detailed[0].version == "2.0");
    CHECK(detailed[0].icon_file ==
          std::filesystem::weakly_canonical(dir / "icon.png").string());

    std::filesystem::remove_all(dir);
}

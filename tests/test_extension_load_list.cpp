// The engine's extension load list: which enabled registry paths reach
// Chromium's --load-extension switch, and the exact switch value.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "extensions/ExtensionLoadList.hpp"
#include "extensions/ExtensionService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

using namespace aurora::extensions;
using namespace aurora::storage;

namespace {

std::filesystem::path fresh_dir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() /
               ("aurora_load_list_test_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path make_extension(const std::string& name) {
    auto dir = fresh_dir(name);
    std::ofstream(dir / "manifest.json")
        << R"JSON({"name": ")JSON" << name
        << R"JSON(", "version": "1.0", "manifest_version": 3})JSON";
    return dir;
}

} // namespace

TEST_CASE("load list joins enabled directories with commas in registry order") {
    const std::vector<std::string> paths{"/ext/a", "/ext/b", "/ext/c"};
    const auto list = build_load_list(paths, [](const std::string&) { return true; });
    CHECK(list.dirs == paths);
    CHECK(list.skipped.empty());
    CHECK(list.switch_value() == "/ext/a,/ext/b,/ext/c");
}

TEST_CASE("an empty load list produces no switch value") {
    const auto list = build_load_list({}, nullptr);
    CHECK(list.dirs.empty());
    CHECK(list.switch_value().empty());
    CHECK(load_extension_switch_value({}).empty());
}

TEST_CASE("paths that cannot be expressed in the switch are skipped, not mangled") {
    const std::vector<std::string> paths{"/ext/a", "", "/ext/with,comma",
                                         "/ext/a", "/ext/b"};
    const auto list = build_load_list(paths, nullptr);
    CHECK(list.dirs == std::vector<std::string>{"/ext/a", "/ext/b"});
    CHECK(list.switch_value() == "/ext/a,/ext/b");
    REQUIRE(list.skipped.size() == 3);
    CHECK(list.skipped[0].reason == LoadSkipReason::EmptyPath);
    CHECK(list.skipped[1].path == "/ext/with,comma");
    CHECK(list.skipped[1].reason == LoadSkipReason::ContainsComma);
    CHECK(list.skipped[2].path == "/ext/a");
    CHECK(list.skipped[2].reason == LoadSkipReason::Duplicate);
    // The CEF-side helper applies the same filesystem-free rules.
    CHECK(load_extension_switch_value(paths) == "/ext/a,/ext/b");
}

TEST_CASE("the predicate filters directories that are no longer loadable") {
    const std::set<std::string> present{"/ext/kept"};
    const auto list = build_load_list(
        {"/ext/kept", "/ext/deleted"},
        [&](const std::string& dir) { return present.count(dir) > 0; });
    CHECK(list.dirs == std::vector<std::string>{"/ext/kept"});
    REQUIRE(list.skipped.size() == 1);
    CHECK(list.skipped[0].path == "/ext/deleted");
    CHECK(list.skipped[0].reason == LoadSkipReason::NotLoadable);
    CHECK_FALSE(load_skip_reason_name(LoadSkipReason::NotLoadable).empty());
}

TEST_CASE("the filesystem predicate requires a directory with manifest.json") {
    const auto good = make_extension("good");
    const auto no_manifest = fresh_dir("no_manifest");
    const auto missing = std::filesystem::temp_directory_path() /
                         "aurora_load_list_test_does_not_exist";
    std::filesystem::remove_all(missing);

    CHECK(is_loadable_extension_dir(good.string()));
    CHECK_FALSE(is_loadable_extension_dir(no_manifest.string()));
    CHECK_FALSE(is_loadable_extension_dir(missing.string()));
    // A manifest.json that is itself a directory is not a manifest.
    const auto odd = fresh_dir("manifest_is_dir");
    std::filesystem::create_directories(odd / "manifest.json");
    CHECK_FALSE(is_loadable_extension_dir(odd.string()));

    std::filesystem::remove_all(good);
    std::filesystem::remove_all(no_manifest);
    std::filesystem::remove_all(odd);
}

TEST_CASE("registry to switch: disabled and deleted extensions are not loaded") {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    ExtensionService svc(db);

    const auto kept = make_extension("kept");
    const auto disabled = make_extension("disabled");
    const auto deleted = make_extension("deleted");
    const int64_t kept_id = svc.add_unpacked(kept.string());
    const int64_t disabled_id = svc.add_unpacked(disabled.string());
    const int64_t deleted_id = svc.add_unpacked(deleted.string());
    REQUIRE(kept_id > 0);
    REQUIRE(disabled_id > 0);
    REQUIRE(deleted_id > 0);
    REQUIRE(svc.set_enabled(disabled_id, false));

    // The user removes a "Load unpacked" folder outside Aurora: the row stays
    // registered (and enabled) but must not be handed to Chromium.
    std::filesystem::remove_all(deleted);

    // add_unpacked() stores weakly-canonical paths (temp_directory_path()
    // may itself be a symlink), so compare against the stored form.
    const std::string kept_path = std::filesystem::weakly_canonical(kept).string();
    const std::string deleted_path =
        std::filesystem::weakly_canonical(deleted).string();

    const auto list =
        build_load_list(svc.enabled_paths(), is_loadable_extension_dir);
    CHECK(list.dirs == std::vector<std::string>{kept_path});
    CHECK(list.switch_value() == kept_path);
    REQUIRE(list.skipped.size() == 1);
    CHECK(list.skipped[0].path == deleted_path);
    CHECK(list.skipped[0].reason == LoadSkipReason::NotLoadable);
    // Filtering never edits the registry.
    CHECK(svc.all().size() == 3);

    std::filesystem::remove_all(kept);
    std::filesystem::remove_all(disabled);
}

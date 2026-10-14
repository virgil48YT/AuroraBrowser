// Per-version layout and extraction. Three processes have to agree on these
// paths (packaging tool, bootstrap launcher, browser), and the interesting cases
// are the ones a user hits after an update: a half-extracted directory, a
// directory from the previous CEF, two launches racing, and a developer build
// with no manifest at all.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/BuildId.hpp"
#include "core/CefLayout.hpp"
#include "core/CefMaterialize.hpp"
#include "util/Sha256.hpp"

using namespace aurora::core;
namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        path_ = fs::temp_directory_path() /
                ("aurora_layout_" + tag + "_" + std::to_string(::rand()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string str() const { return path_.generic_string(); }
    fs::path path() const { return path_; }

    void write(const std::string& rel, const std::string& content) const {
        const fs::path p = path_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << content;
    }
    std::string read(const std::string& rel) const {
        std::ifstream in(path_ / rel, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
    bool exists(const std::string& rel) const {
        return fs::exists(path_ / rel);
    }

private:
    fs::path path_;
};

// A packaged application directory: CEF resources, an engine binary, a Qt DLL
// that must be ignored, and the generated manifest.
struct FakePackage {
    TempDir dir;
    BuildManifest manifest;

    explicit FakePackage(const std::string& tag, const std::string& pak = "images")
        : dir(tag) {
        dir.write("chrome_100_percent.pak", pak);
        dir.write("resources.pak", "resources");
        dir.write("icudtl.dat", "icu data");
        dir.write("v8_context_snapshot.bin", "snapshot");
        dir.write("locales/en-US.pak", "english");
        dir.write("libcef.dll", "engine");
        dir.write("Qt6Core.dll", "qt");
        manifest = scan_build_tree(dir.str(), default_cef_resource_globs(),
                                   default_cef_identity_globs());
        manifest.app_version = "0.1.0";
        manifest.cef_version = "141.0.5+g1234567+chromium-141.0.7390.55";
        manifest.platform = "windows64";
        finalize(manifest);
        dir.write(kManifestFileName, serialize_manifest(manifest));
    }
};

LayoutEnv env_with_home(const std::string& home) {
    LayoutEnv e;
    e.aurora_home = home;
    return e;
}

} // namespace

TEST_CASE("join_path normalizes separators and duplicated slashes") {
    CHECK(join_path("C:\\Users\\me\\AppData\\Local\\Aurora", "cef") ==
          "C:/Users/me/AppData/Local/Aurora/cef");
    CHECK(join_path("/opt/aurora/", "/cef/") == "/opt/aurora/cef");
    CHECK(join_path("", "cef") == "cef");
    CHECK(join_path("/opt/aurora", "") == "/opt/aurora");
}

TEST_CASE("the base directory follows the platform's convention") {
    LayoutEnv e;
    e.local_app_data = "C:\\Users\\me\\AppData\\Local";
    CHECK(aurora_base_dir(e) == "C:/Users/me/AppData/Local/Aurora");

    LayoutEnv posix;
    posix.xdg_data_home = "/home/me/.local/share";
    CHECK(aurora_base_dir(posix) == "/home/me/.local/share/Aurora");

    LayoutEnv home_only;
    home_only.home = "/home/me";
    CHECK(aurora_base_dir(home_only) == "/home/me/.local/share/Aurora");

    // AURORA_HOME wins over everything, which is what makes these tests (and a
    // portable install) possible.
    LayoutEnv overridden = e;
    overridden.aurora_home = "D:/portable/aurora";
    CHECK(aurora_base_dir(overridden) == "D:/portable/aurora");
}

TEST_CASE("a per-version directory is only ever named after a valid build id") {
    const auto e = env_with_home("/tmp/aurora-home");
    CHECK(cef_versions_root(e) == "/tmp/aurora-home/cef");
    CHECK(cef_dir_for_build(e, "0123456789abcdef") ==
          "/tmp/aurora-home/cef/0123456789abcdef");
    // Anything that is not a build ID yields no path at all, so a corrupt
    // manifest can never be turned into "delete/create this directory".
    CHECK(cef_dir_for_build(e, "").empty());
    CHECK(cef_dir_for_build(e, "../../..").empty());
    CHECK(cef_dir_for_build(e, "0123456789ABCDEF").empty());
}

TEST_CASE("--cef-dir is parsed out of argv") {
    char a0[] = "aurora_browser";
    char a1[] = "--cef-dir=D:\\cef\\resources";
    char a2[] = "https://example.com";
    char* argv[] = {a0, a1, a2};
    const auto got = cef_dir_from_args(3, argv);
    REQUIRE(got.has_value());
    CHECK(*got == "D:/cef/resources");

    char* argv_none[] = {a0, a2};
    CHECK_FALSE(cef_dir_from_args(2, argv_none).has_value());
}

TEST_CASE("first extraction links or copies every resource and stamps the tree") {
    FakePackage pkg("materialize");
    TempDir home("home");
    const auto env = env_with_home(home.str());
    const std::string target = cef_dir_for_build(env, pkg.manifest.build_id);
    REQUIRE_FALSE(target.empty());

    MaterializeOptions o;
    const auto r = materialize_cef_resources(pkg.dir.str(), target,
                                             pkg.manifest, o);
    CHECK(r.ok);
    CHECK_FALSE(r.reused);
    CHECK(r.linked + r.copied == 5);  // 4 top-level resources + locales/en-US
    CHECK(fs::exists(fs::path(target) / "icudtl.dat"));
    CHECK(fs::exists(fs::path(target) / "locales/en-US.pak"));
    // Identity files stay where the OS loader expects them.
    CHECK_FALSE(fs::exists(fs::path(target) / "libcef.dll"));
    // Nothing that was not in the manifest came along for the ride.
    CHECK_FALSE(fs::exists(fs::path(target) / "Qt6Core.dll"));
    // The stamp is the last thing written and holds the build ID.
    const auto stamp = read_stamp(target);
    REQUIRE(stamp.has_value());
    CHECK(*stamp == pkg.manifest.build_id);
    // No temporary files left behind.
    for (fs::recursive_directory_iterator it(target), end; it != end; ++it) {
        CHECK(it->path().string().find(".aurora-tmp") == std::string::npos);
    }

    SUBCASE("a second launch reuses the tree and writes nothing") {
        const auto again = materialize_cef_resources(pkg.dir.str(), target,
                                                     pkg.manifest, o);
        CHECK(again.ok);
        CHECK(again.reused);
        CHECK(again.linked == 0);
        CHECK(again.copied == 0);
        CHECK(again.verified == 5);
    }

    SUBCASE("a deleted file is repaired") {
        fs::remove(fs::path(target) / "icudtl.dat");
        const auto repaired = materialize_cef_resources(pkg.dir.str(), target,
                                                        pkg.manifest, o);
        CHECK(repaired.ok);
        CHECK_FALSE(repaired.reused);
        CHECK(repaired.linked + repaired.copied == 1);
        CHECK(fs::exists(fs::path(target) / "icudtl.dat"));
        CHECK(read_stamp(target).value_or("") == pkg.manifest.build_id);
    }

    SUBCASE("a corrupted file of the same size is repaired under full verify") {
        // Replace the file rather than writing through it: the extracted copy is
        // a hard link to the packaged file, so an in-place write would corrupt
        // the source too. Replacing is also what actually happens in the field
        // (an installer, a sync client or antivirus swaps the file).
        fs::remove(fs::path(target) / "icudtl.dat");
        {
            std::ofstream out(fs::path(target) / "icudtl.dat",
                              std::ios::binary | std::ios::trunc);
            out << "ICU DATA";  // same size, different bytes
        }
        MaterializeOptions full;
        full.full_verify = true;
        const auto repaired = materialize_cef_resources(pkg.dir.str(), target,
                                                        pkg.manifest, full);
        CHECK(repaired.ok);
        CHECK(repaired.linked + repaired.copied == 1);
    }

    SUBCASE("an interrupted extraction (no stamp) is redone") {
        fs::remove(fs::path(target) / kStampFileName);
        const auto redone = materialize_cef_resources(pkg.dir.str(), target,
                                                      pkg.manifest, o);
        CHECK(redone.ok);
        CHECK(read_stamp(target).value_or("") == pkg.manifest.build_id);
    }

    SUBCASE("the copy path produces the same tree as the hard-link path") {
        TempDir other_home("home-copy");
        const auto env2 = env_with_home(other_home.str());
        const std::string target2 =
            cef_dir_for_build(env2, pkg.manifest.build_id);
        MaterializeOptions copy_only;
        copy_only.allow_hardlinks = false;
        const auto copied = materialize_cef_resources(pkg.dir.str(), target2,
                                                      pkg.manifest, copy_only);
        CHECK(copied.ok);
        CHECK(copied.copied == 5);
        CHECK(copied.linked == 0);
        CHECK(verify_resource_tree(target2, pkg.manifest, VerifyMode::Full).ok);
    }
}

TEST_CASE("a missing source file fails loudly instead of stamping a partial tree") {
    FakePackage pkg("missing-source");
    TempDir home("home-missing");
    const auto env = env_with_home(home.str());
    const std::string target = cef_dir_for_build(env, pkg.manifest.build_id);
    fs::remove(pkg.dir.path() / "resources.pak");

    const auto r = materialize_cef_resources(pkg.dir.str(), target,
                                             pkg.manifest, {});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("missing source file") != std::string::npos);
    CHECK_FALSE(read_stamp(target).has_value());
}

TEST_CASE("two CEF versions coexist and the old one is collectable") {
    FakePackage old_pkg("v1", "images v1");
    FakePackage new_pkg("v2", "images v2");
    CHECK(old_pkg.manifest.build_id != new_pkg.manifest.build_id);

    TempDir home("home-two");
    const auto env = env_with_home(home.str());
    const std::string old_dir = cef_dir_for_build(env, old_pkg.manifest.build_id);
    const std::string new_dir = cef_dir_for_build(env, new_pkg.manifest.build_id);
    CHECK(materialize_cef_resources(old_pkg.dir.str(), old_dir,
                                    old_pkg.manifest, {}).ok);
    CHECK(materialize_cef_resources(new_pkg.dir.str(), new_dir,
                                    new_pkg.manifest, {}).ok);
    // Both trees are intact: the new extraction did not touch the old one, which
    // is what lets a still-running old browser keep working through an update.
    CHECK(verify_resource_tree(old_dir, old_pkg.manifest, VerifyMode::Full).ok);
    CHECK(verify_resource_tree(new_dir, new_pkg.manifest, VerifyMode::Full).ok);

    const auto stale = stale_cef_dirs(env, new_pkg.manifest.build_id);
    REQUIRE(stale.size() == 1);
    CHECK(stale.front().find(old_pkg.manifest.build_id) != std::string::npos);

    // A directory that is not named after a build ID is never a cleanup
    // candidate, whatever it is doing under cef/.
    fs::create_directories(fs::path(cef_versions_root(env)) / "user-notes");
    CHECK(stale_cef_dirs(env, new_pkg.manifest.build_id).size() == 1);

    CHECK(remove_cef_dir(stale.front()));
    CHECK(stale_cef_dirs(env, new_pkg.manifest.build_id).empty());
    CHECK(verify_resource_tree(new_dir, new_pkg.manifest, VerifyMode::Full).ok);
}

TEST_CASE("resolution order: argument, environment, per-version, application") {
    FakePackage pkg("resolve");
    TempDir home("home-resolve");
    LayoutEnv env = env_with_home(home.str());
    const std::string versioned = cef_dir_for_build(env, pkg.manifest.build_id);

    SUBCASE("no per-version directory yet: the application directory is used") {
        const auto r = resolve_cef_dir(env, pkg.dir.str(), std::nullopt);
        CHECK(r.source == CefDirSource::Application);
        CHECK(r.dir == join_path(pkg.dir.str(), ""));
        CHECK(r.build_id == pkg.manifest.build_id);
    }

    SUBCASE("a stamped per-version directory wins") {
        REQUIRE(materialize_cef_resources(pkg.dir.str(), versioned,
                                          pkg.manifest, {}).ok);
        const auto r = resolve_cef_dir(env, pkg.dir.str(), std::nullopt);
        CHECK(r.source == CefDirSource::Versioned);
        CHECK(r.dir == versioned);
        CHECK(r.build_id == pkg.manifest.build_id);
    }

    SUBCASE("a directory stamped for another build is not used") {
        REQUIRE(materialize_cef_resources(pkg.dir.str(), versioned,
                                          pkg.manifest, {}).ok);
        CHECK(write_stamp(versioned, "0123456789abcdef"));
        const auto r = resolve_cef_dir(env, pkg.dir.str(), std::nullopt);
        CHECK(r.source == CefDirSource::Application);
    }

    SUBCASE("AURORA_CEF_DIR overrides the per-version directory") {
        REQUIRE(materialize_cef_resources(pkg.dir.str(), versioned,
                                          pkg.manifest, {}).ok);
        TempDir elsewhere("elsewhere");
        env.cef_dir_override = elsewhere.str();
        const auto r = resolve_cef_dir(env, pkg.dir.str(), std::nullopt);
        CHECK(r.source == CefDirSource::Environment);
        CHECK(r.dir == elsewhere.str());
    }

    SUBCASE("--cef-dir overrides AURORA_CEF_DIR") {
        TempDir from_env("env-dir");
        TempDir from_arg("arg-dir");
        env.cef_dir_override = from_env.str();
        const auto r = resolve_cef_dir(env, pkg.dir.str(), from_arg.str());
        CHECK(r.source == CefDirSource::Argument);
        CHECK(r.dir == from_arg.str());
    }

    SUBCASE("an override that does not exist is ignored, not obeyed") {
        env.cef_dir_override = home.str() + "/definitely-not-here";
        const auto r = resolve_cef_dir(env, pkg.dir.str(),
                                       home.str() + "/nor-here");
        CHECK(r.source == CefDirSource::Application);
    }

    SUBCASE("a developer build with no manifest resolves to the exe directory") {
        TempDir plain("plain-build");
        plain.write("chrome_100_percent.pak", "images");
        const auto r = resolve_cef_dir(env, plain.str(), std::nullopt);
        CHECK(r.source == CefDirSource::Application);
        CHECK(r.build_id.empty());
        CHECK_FALSE(load_manifest_from_dir(plain.str()).has_value());
    }
}

TEST_CASE("the manifest on disk is what the launcher reads back") {
    FakePackage pkg("manifest-io");
    const auto loaded = load_manifest_from_dir(pkg.dir.str());
    REQUIRE(loaded.has_value());
    CHECK(loaded->build_id == pkg.manifest.build_id);
    CHECK(compute_build_id(*loaded) == pkg.manifest.build_id);
    CHECK(loaded->cef_version == pkg.manifest.cef_version);
    CHECK(loaded->files.size() == pkg.manifest.files.size());
}

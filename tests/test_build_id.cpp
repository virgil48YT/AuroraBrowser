// The build ID is what keeps two CEF versions from sharing a directory, so the
// properties asserted here are the ones the whole scheme rests on:
//
//   * reproducible   — same files, same ID, regardless of scan order
//   * sensitive      — any change to a hashed file, the CEF version, the format
//                      version or a file's role changes the ID
//   * round-trippable— serialize -> parse -> same ID, including through paths
//                      with spaces and Windows separators
//   * fail-closed    — a malformed or tampered manifest does not parse, and a
//                      resource tree is never declared valid on a size match
//                      alone when a full verify was asked for
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/BuildId.hpp"
#include "core/CefLayout.hpp"
#include "util/Sha256.hpp"

using namespace aurora::core;
namespace fs = std::filesystem;

namespace {

BuildFile make_file(std::string path, std::string content,
                    FileRole role = FileRole::Resource) {
    BuildFile f;
    f.path = std::move(path);
    f.size = content.size();
    f.sha256 = aurora::util::sha256_hex(content);
    f.role = role;
    return f;
}

BuildManifest sample_manifest() {
    BuildManifest m;
    m.app_version = "0.1.0";
    m.cef_version = "141.0.5+g1234567+chromium-141.0.7390.55";
    m.platform = "windows64";
    m.files.push_back(make_file("icudtl.dat", "icu data"));
    m.files.push_back(make_file("locales/en-US.pak", "english"));
    m.files.push_back(make_file("chrome_100_percent.pak", "images"));
    m.files.push_back(make_file("libcef.dll", "engine", FileRole::Identity));
    finalize(m);
    return m;
}

// A scratch directory that cleans up after itself, so the tests can exercise
// the real filesystem paths (which is the only way to catch a rename or a
// hard-link mistake).
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        path_ = fs::temp_directory_path() /
                ("aurora_buildid_" + tag + "_" +
                 std::to_string(::rand()));  // NOLINT: test-only uniqueness
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string str() const { return path_.string(); }
    fs::path path() const { return path_; }

    void write(const std::string& rel, const std::string& content) const {
        const fs::path p = path_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << content;
    }

private:
    fs::path path_;
};

} // namespace

TEST_CASE("a build id is 16 lowercase hex characters") {
    const auto m = sample_manifest();
    CHECK(m.build_id.size() == kBuildIdLength);
    CHECK(is_valid_build_id(m.build_id));
    CHECK_FALSE(is_valid_build_id(""));
    CHECK_FALSE(is_valid_build_id("ABCDEF0123456789"));  // uppercase
    CHECK_FALSE(is_valid_build_id("abcdef012345678"));   // too short
    CHECK_FALSE(is_valid_build_id("abcdef01234567890")); // too long
    CHECK_FALSE(is_valid_build_id("../../etc/passwd"));  // never a directory
}

TEST_CASE("the id does not depend on the order files were discovered in") {
    BuildManifest a = sample_manifest();
    BuildManifest b;
    b.app_version = a.app_version;
    b.cef_version = a.cef_version;
    b.platform = a.platform;
    b.files.assign(a.files.rbegin(), a.files.rend());
    finalize(b);
    CHECK(a.build_id == b.build_id);
    CHECK(canonical_manifest_text(a) == canonical_manifest_text(b));
}

TEST_CASE("windows separators normalize to the canonical form") {
    BuildManifest a = sample_manifest();
    BuildManifest b = sample_manifest();
    for (auto& f : b.files) {
        if (f.path == "locales/en-US.pak") f.path = "locales\\en-US.pak";
    }
    finalize(b);
    CHECK(a.build_id == b.build_id);
}

TEST_CASE("every input that defines the runtime moves the id") {
    const auto base = sample_manifest();

    SUBCASE("a resource file's content") {
        auto m = sample_manifest();
        m.files[0].sha256 = aurora::util::sha256_hex("different");
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
    SUBCASE("a resource file's size") {
        auto m = sample_manifest();
        m.files[0].size += 1;
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
    SUBCASE("the engine binary (an identity file)") {
        auto m = sample_manifest();
        for (auto& f : m.files) {
            if (f.role == FileRole::Identity) {
                f.sha256 = aurora::util::sha256_hex("newer engine");
            }
        }
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
    SUBCASE("a file's role") {
        auto m = sample_manifest();
        m.files.front().role = m.files.front().role == FileRole::Identity
                                   ? FileRole::Resource
                                   : FileRole::Identity;
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
    SUBCASE("the CEF version string") {
        auto m = sample_manifest();
        m.cef_version = "142.0.1+gabcdef0+chromium-142.0.1234.5";
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
    SUBCASE("the Aurora version") {
        auto m = sample_manifest();
        m.app_version = "0.2.0";
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
    SUBCASE("the platform") {
        auto m = sample_manifest();
        m.platform = "linux64";
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
    SUBCASE("an added file") {
        auto m = sample_manifest();
        m.files.push_back(make_file("locales/de.pak", "german"));
        finalize(m);
        CHECK(m.build_id != base.build_id);
    }
}

TEST_CASE("serialize and parse round-trip") {
    auto m = sample_manifest();
    m.files.push_back(make_file("locales/pt-BR with space.pak", "spaced"));
    finalize(m);

    const std::string text = serialize_manifest(m);
    const auto parsed = parse_manifest(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->app_version == m.app_version);
    CHECK(parsed->cef_version == m.cef_version);
    CHECK(parsed->platform == m.platform);
    CHECK(parsed->files.size() == m.files.size());
    CHECK(parsed->build_id == m.build_id);
    // The authoritative ID is recomputed from the content, and it agrees.
    CHECK(compute_build_id(*parsed) == m.build_id);
    CHECK(canonical_manifest_text(*parsed) == canonical_manifest_text(m));
    // A path containing a space survived, which is why the path is last on the
    // line.
    bool found_spaced = false;
    for (const auto& f : parsed->files) {
        if (f.path == "locales/pt-BR with space.pak") found_spaced = true;
    }
    CHECK(found_spaced);
}

TEST_CASE("a tampered manifest is detected, not trusted") {
    auto m = sample_manifest();
    std::string text = serialize_manifest(m);
    // Swap a hash: the stated build-id line still says the old ID.
    const std::string old_hash = m.files.front().sha256;
    const std::string new_hash = aurora::util::sha256_hex("tampered");
    const size_t at = text.find(old_hash);
    REQUIRE(at != std::string::npos);
    text.replace(at, old_hash.size(), new_hash);

    const auto parsed = parse_manifest(text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->build_id == m.build_id);              // what the file claims
    CHECK(compute_build_id(*parsed) != m.build_id);     // what it actually is
}

TEST_CASE("malformed manifests are rejected") {
    CHECK_FALSE(parse_manifest("").has_value());
    CHECK_FALSE(parse_manifest("nothing to see here\n").has_value());
    // Missing header.
    CHECK_FALSE(parse_manifest("app-version 0.1.0\n").has_value());
    // Unsupported format version: a future manifest must not be half-read.
    CHECK_FALSE(parse_manifest("aurora-build-manifest 99\n").has_value());
    // Truncated file line.
    CHECK_FALSE(parse_manifest("aurora-build-manifest 1\nfile resource 3\n")
                    .has_value());
    // Unknown role.
    CHECK_FALSE(parse_manifest(
                    "aurora-build-manifest 1\nfile bogus 3 " +
                    std::string(64, 'a') + " x.pak\n")
                    .has_value());
    // Hash that is not 64 lowercase hex characters.
    CHECK_FALSE(parse_manifest("aurora-build-manifest 1\nfile resource 3 XYZ "
                               "x.pak\n")
                    .has_value());
    // A minimal but valid document, including a comment and CRLF line endings.
    const auto ok = parse_manifest("# comment\r\naurora-build-manifest 1\r\n"
                                   "cef-version 1.2.3\r\n"
                                   "future-key ignored\r\n");
    REQUIRE(ok.has_value());
    CHECK(ok->cef_version == "1.2.3");
}

TEST_CASE("glob matching keeps unrelated files out of the manifest") {
    CHECK(glob_match("*.pak", "chrome_100_percent.pak"));
    CHECK_FALSE(glob_match("*.pak", "locales/en-US.pak"));  // '*' stops at '/'
    CHECK(glob_match("locales/*.pak", "locales/en-US.pak"));
    CHECK_FALSE(glob_match("locales/*.pak", "locales/sub/en-US.pak"));
    CHECK(glob_match("**/*.pak", "locales/sub/en-US.pak"));
    CHECK(glob_match("icudtl.dat", "icudtl.dat"));
    CHECK_FALSE(glob_match("icudtl.dat", "icudtl.dat.bak"));
    CHECK(glob_match("libcef.?ll", "libcef.dll"));
    CHECK_FALSE(glob_match("libcef.dll", "Qt6Core.dll"));
    CHECK_FALSE(glob_match("*.dll", "libcef.so"));
}

TEST_CASE("scanning a tree classifies resources and identity files") {
    TempDir dir("scan");
    dir.write("chrome_100_percent.pak", "images");
    dir.write("resources.pak", "resources");
    dir.write("icudtl.dat", "icu");
    dir.write("v8_context_snapshot.bin", "snapshot");
    dir.write("locales/en-US.pak", "english");
    dir.write("locales/de.pak", "german");
    dir.write("libcef.dll", "engine");
    // Files that must be ignored: Qt, Aurora itself, build leftovers.
    dir.write("Qt6Core.dll", "qt");
    dir.write("aurora_browser.exe", "aurora");
    dir.write("libcef.lib", "import lib");

    const auto m = scan_build_tree(dir.str(), default_cef_resource_globs(),
                                   default_cef_identity_globs());
    size_t resources = 0, identity = 0;
    for (const auto& f : m.files) {
        if (f.role == FileRole::Resource) ++resources;
        else ++identity;
        CHECK(f.sha256.size() == 64);
        CHECK(f.size > 0);
    }
    CHECK(resources == 6);
    CHECK(identity == 1);
    CHECK(is_valid_build_id(m.build_id));

    // Scanning the same tree twice is stable; changing one byte is not.
    const auto again = scan_build_tree(dir.str(), default_cef_resource_globs(),
                                       default_cef_identity_globs());
    CHECK(again.build_id == m.build_id);
    dir.write("locales/de.pak", "german (updated)");
    const auto changed = scan_build_tree(dir.str(), default_cef_resource_globs(),
                                         default_cef_identity_globs());
    CHECK(changed.build_id != m.build_id);
}

TEST_CASE("verification distinguishes missing, resized and corrupt files") {
    TempDir dir("verify");
    auto m = sample_manifest();

    // Nothing extracted yet: every resource is missing, identity files are not
    // expected there at all.
    auto report = verify_resource_tree(dir.str(), m, VerifyMode::Full);
    CHECK_FALSE(report.ok);
    CHECK(report.checked == 3);
    CHECK(report.missing.size() == 3);

    dir.write("icudtl.dat", "icu data");
    dir.write("locales/en-US.pak", "english");
    dir.write("chrome_100_percent.pak", "images");
    report = verify_resource_tree(dir.str(), m, VerifyMode::Full);
    CHECK(report.ok);
    CHECK(report.checked == 3);

    SUBCASE("a truncated file fails the size check") {
        dir.write("icudtl.dat", "icu");
        const auto r = verify_resource_tree(dir.str(), m, VerifyMode::Quick);
        CHECK_FALSE(r.ok);
        CHECK(r.wrong_size.size() == 1);
    }
    SUBCASE("same size, different bytes passes Quick and fails Full") {
        dir.write("icudtl.dat", "ICU DATA");  // same length, different content
        CHECK(verify_resource_tree(dir.str(), m, VerifyMode::Quick).ok);
        const auto r = verify_resource_tree(dir.str(), m, VerifyMode::Full);
        CHECK_FALSE(r.ok);
        CHECK(r.wrong_hash.size() == 1);
        CHECK(r.summary().find("wrong hash") != std::string::npos);
    }
}

TEST_CASE("the completion stamp only accepts a real build id") {
    TempDir dir("stamp");
    CHECK_FALSE(read_stamp(dir.str()).has_value());
    CHECK_FALSE(write_stamp(dir.str(), "not-a-build-id"));
    CHECK_FALSE(read_stamp(dir.str()).has_value());

    const auto m = sample_manifest();
    CHECK(write_stamp(dir.str(), m.build_id));
    const auto stamp = read_stamp(dir.str());
    REQUIRE(stamp.has_value());
    CHECK(*stamp == m.build_id);

    // Garbage in the stamp file reads as "no stamp", never as a directory name.
    dir.write(kStampFileName, "hello\n");
    CHECK_FALSE(read_stamp(dir.str()).has_value());
}

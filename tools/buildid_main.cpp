// aurora_buildid — the packaging side of the build ID.
//
// Runs on the build machine, after windeployqt has assembled the package
// directory and the CEF files have been copied in, and before the SFX is built:
//
//   aurora_buildid generate --root package
//       --cef-version "141.0.5+g1234567+chromium-141.0.7390.55"
//       --app-version 0.1.0 --platform windows64
//       --out package/aurora_build.manifest
//
// It deliberately shares core::scan_build_tree / core::compute_build_id with the
// bootstrap launcher, so the ID written here and the ID verified at runtime
// cannot drift apart — that is the whole reason this is a C++ tool built from
// aurora_core rather than a shell script with sha256sum.
//
// Other commands:
//   show   --manifest F                 print the manifest metadata and ID
//   id     --manifest F                 print just the build ID
//   verify --manifest F --root D [--full]
//                                      verify an extracted resource tree
#include "core/BuildId.hpp"
#include "core/CefLayout.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace core = aurora::core;

namespace {

void usage() {
    std::cout <<
        "usage:\n"
        "  aurora_buildid generate --root DIR [--out FILE]\n"
        "                 [--cef-version V] [--cef-root DIR] [--app-version V]\n"
        "                 [--platform NAME]\n"
        "  aurora_buildid show   --manifest FILE\n"
        "  aurora_buildid id     --manifest FILE\n"
        "  aurora_buildid verify --manifest FILE --root DIR [--full]\n"
        "\n"
        "generate scans DIR for the CEF resource files (role 'resource': the\n"
        ".pak bundles, icudtl.dat, the V8 snapshots, locales/) and the engine\n"
        "binaries (role 'identity': libcef.dll and the libraries the OS loader\n"
        "resolves next to the executable), hashes each one, and writes a\n"
        "manifest whose build ID is SHA-256 over that content.\n";
}

// --key value pairs plus a positional command.
struct Args {
    std::string command;
    std::map<std::string, std::string> named;
    bool has(const std::string& k) const { return named.count(k) > 0; }
    std::string get(const std::string& k, std::string def = {}) const {
        const auto it = named.find(k);
        return it == named.end() ? def : it->second;
    }
};

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            const size_t eq = s.find('=');
            if (eq != std::string::npos) {
                a.named[s.substr(2, eq - 2)] = s.substr(eq + 1);
            } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                a.named[s.substr(2)] = argv[++i];
            } else {
                a.named[s.substr(2)] = "1";  // boolean flag
            }
        } else if (a.command.empty()) {
            a.command = s;
        }
    }
    return a;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// CEF's binary distribution ships a README/VERSION with the version string.
// fetch_cef.sh writes third_party/cef/VERSION.txt as
// "CEF 141.0.5+g... (windows64, requested channel: auto)"; the CEF package
// itself carries cef_version in README.txt. Reading it here means the packaging
// step does not have to be told the version by hand (and therefore cannot be
// told the wrong one).
std::string cef_version_from_root(const std::string& cef_root) {
    if (cef_root.empty()) return {};
    const std::string txt = read_file(core::join_path(cef_root, "VERSION.txt"));
    if (!txt.empty()) {
        // First line, second token.
        std::istringstream in(txt);
        std::string word, version;
        in >> word >> version;
        if (word == "CEF" && !version.empty()) return version;
    }
    const std::string readme = read_file(core::join_path(cef_root, "README.txt"));
    if (!readme.empty()) {
        std::istringstream in(readme);
        std::string line;
        while (std::getline(in, line)) {
            const std::string key = "CEF Version:";
            const size_t at = line.find(key);
            if (at == std::string::npos) continue;
            std::istringstream ls(line.substr(at + key.size()));
            std::string version;
            ls >> version;
            if (!version.empty()) return version;
        }
    }
    return {};
}

int cmd_generate(const Args& a) {
    const std::string root = a.get("root");
    if (root.empty()) {
        std::cerr << "generate: --root is required\n";
        return 2;
    }
    core::BuildManifest m = core::scan_build_tree(
        root, core::default_cef_resource_globs(),
        core::default_cef_identity_globs());
    m.app_version = a.get("app-version", AURORA_VERSION_STRING);
    m.platform = a.get("platform", AURORA_PLATFORM_STRING);
    m.cef_version = a.get("cef-version");
    if (m.cef_version.empty()) {
        m.cef_version = cef_version_from_root(a.get("cef-root"));
    }
    if (m.cef_version.empty()) {
        // An unknown engine version is allowed (a Qt-only package has no CEF at
        // all) but it is recorded as such rather than left blank, so the field
        // is never ambiguous in a log.
        m.cef_version = "unknown";
    }
    core::finalize(m);

    size_t resources = 0, identity = 0;
    for (const auto& f : m.files) {
        if (f.role == core::FileRole::Resource) ++resources;
        else ++identity;
    }
    if (resources == 0) {
        std::cerr << "generate: no CEF resource files found under " << root
                  << " — refusing to write a manifest that would make the\n"
                     "          launcher believe an empty tree is valid\n";
        return 3;
    }

    const std::string text = core::serialize_manifest(m);
    const std::string out = a.get("out", core::join_path(root, core::kManifestFileName));
    std::ofstream os(out, std::ios::binary | std::ios::trunc);
    if (!os) {
        std::cerr << "generate: cannot write " << out << "\n";
        return 4;
    }
    os << text;
    os.flush();
    if (!os) {
        std::cerr << "generate: write failed for " << out << "\n";
        return 4;
    }
    std::cout << "build-id " << m.build_id << "\n"
              << "cef-version " << m.cef_version << "\n"
              << "files " << resources << " resource, " << identity
              << " identity\n"
              << "written " << out << "\n";
    return 0;
}

int cmd_show(const Args& a, bool id_only) {
    const std::string path = a.get("manifest");
    if (path.empty()) {
        std::cerr << "--manifest is required\n";
        return 2;
    }
    const auto m = core::parse_manifest(read_file(path));
    if (!m.has_value()) {
        std::cerr << "cannot parse " << path << "\n";
        return 3;
    }
    const std::string computed = core::compute_build_id(*m);
    if (id_only) {
        std::cout << computed << "\n";
        return m->build_id == computed ? 0 : 5;
    }
    size_t resources = 0, identity = 0;
    for (const auto& f : m->files) {
        if (f.role == core::FileRole::Resource) ++resources;
        else ++identity;
    }
    std::cout << "format      " << m->format_version << "\n"
              << "app-version " << m->app_version << "\n"
              << "cef-version " << m->cef_version << "\n"
              << "platform    " << m->platform << "\n"
              << "files       " << resources << " resource, " << identity
              << " identity\n"
              << "build-id    " << m->build_id
              << (m->build_id == computed ? " (verified)" : " (MISMATCH)") << "\n";
    if (m->build_id != computed) {
        std::cout << "recomputed  " << computed << "\n";
        return 5;
    }
    return 0;
}

int cmd_verify(const Args& a) {
    const std::string path = a.get("manifest");
    const std::string root = a.get("root");
    if (path.empty() || root.empty()) {
        std::cerr << "verify: --manifest and --root are required\n";
        return 2;
    }
    const auto m = core::parse_manifest(read_file(path));
    if (!m.has_value()) {
        std::cerr << "verify: cannot parse " << path << "\n";
        return 3;
    }
    const auto report = core::verify_resource_tree(
        root, *m, a.has("full") ? core::VerifyMode::Full : core::VerifyMode::Quick);
    std::cout << report.summary() << "\n";
    for (const auto& p : report.missing) std::cout << "  missing    " << p << "\n";
    for (const auto& p : report.wrong_size) std::cout << "  wrong size " << p << "\n";
    for (const auto& p : report.wrong_hash) std::cout << "  wrong hash " << p << "\n";
    return report.ok ? 0 : 6;
}

} // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    if (a.command.empty() || a.has("help")) {
        usage();
        return a.command.empty() ? 2 : 0;
    }
    if (a.command == "generate") return cmd_generate(a);
    if (a.command == "show") return cmd_show(a, false);
    if (a.command == "id") return cmd_show(a, true);
    if (a.command == "verify") return cmd_verify(a);
    std::cerr << "unknown command: " << a.command << "\n";
    usage();
    return 2;
}

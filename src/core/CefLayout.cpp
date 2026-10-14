#include "core/CefLayout.hpp"

#include "core/BuildId.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

namespace aurora::core {
namespace {

namespace fs = std::filesystem;

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v == nullptr ? std::string() : std::string(v);
}

bool is_dir(const std::string& p) {
    if (p.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::path(p), ec);
}

std::string normalize_separators(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    // Strip trailing separators, but keep a root ("/" or "C:/").
    while (s.size() > 1 && s.back() == '/' &&
           !(s.size() == 3 && s[1] == ':')) {
        s.pop_back();
    }
    return s;
}

} // namespace

LayoutEnv LayoutEnv::from_environment() {
    LayoutEnv e;
    e.aurora_home = env_or_empty("AURORA_HOME");
    e.cef_dir_override = env_or_empty("AURORA_CEF_DIR");
    e.local_app_data = env_or_empty("LOCALAPPDATA");
    e.xdg_data_home = env_or_empty("XDG_DATA_HOME");
    e.home = env_or_empty("HOME");
    if (e.home.empty()) e.home = env_or_empty("USERPROFILE");
    return e;
}

std::string join_path(std::string_view base, std::string_view leaf) {
    std::string b = normalize_separators(std::string(base));
    std::string l = normalize_separators(std::string(leaf));
    while (!l.empty() && l.front() == '/') l.erase(l.begin());
    if (b.empty()) return l;
    if (l.empty()) return b;
    if (b.back() == '/') return b + l;
    return b + "/" + l;
}

std::string aurora_base_dir(const LayoutEnv& env) {
    if (!env.aurora_home.empty()) return normalize_separators(env.aurora_home);
    if (!env.local_app_data.empty()) {
        return join_path(env.local_app_data, "Aurora");
    }
    if (!env.xdg_data_home.empty()) return join_path(env.xdg_data_home, "Aurora");
    if (!env.home.empty()) return join_path(env.home, ".local/share/Aurora");
    // Last resort: the current directory. Better than an empty path, which
    // would silently turn every derived path into a relative one at the root.
    return ".aurora";
}

std::string cef_versions_root(const LayoutEnv& env) {
    return join_path(aurora_base_dir(env), kCefVersionsDirName);
}

std::string cef_dir_for_build(const LayoutEnv& env, std::string_view build_id) {
    if (!is_valid_build_id(build_id)) return {};
    return join_path(cef_versions_root(env), build_id);
}

std::optional<std::string> cef_dir_from_args(int argc, char** argv) {
    const std::string prefix = "--cef-dir=";
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) continue;
        const std::string a = argv[i];
        if (a.rfind(prefix, 0) == 0 && a.size() > prefix.size()) {
            return normalize_separators(a.substr(prefix.size()));
        }
    }
    return std::nullopt;
}

std::string CefDirResolution::source_name() const {
    switch (source) {
        case CefDirSource::Argument: return "--cef-dir";
        case CefDirSource::Environment: return "AURORA_CEF_DIR";
        case CefDirSource::Versioned: return "per-version";
        case CefDirSource::Application: return "application dir";
    }
    return "unknown";
}

std::optional<BuildManifest> load_manifest_from_dir(const std::string& dir) {
    const fs::path p = fs::path(dir) / fs::path(kManifestFileName);
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return parse_manifest(buf.str());
}

CefDirResolution resolve_cef_dir(
    const LayoutEnv& env, const std::string& exe_dir,
    const std::optional<std::string>& arg_override) {
    CefDirResolution r;
    r.dir = normalize_separators(exe_dir);
    r.source = CefDirSource::Application;

    if (arg_override.has_value() && is_dir(*arg_override)) {
        r.dir = normalize_separators(*arg_override);
        r.source = CefDirSource::Argument;
        // Still record the build ID when a manifest is around; it is only used
        // for logging in this case.
        if (const auto m = load_manifest_from_dir(exe_dir)) r.build_id = m->build_id;
        return r;
    }
    if (is_dir(env.cef_dir_override)) {
        r.dir = normalize_separators(env.cef_dir_override);
        r.source = CefDirSource::Environment;
        if (const auto m = load_manifest_from_dir(exe_dir)) r.build_id = m->build_id;
        return r;
    }

    const auto manifest = load_manifest_from_dir(exe_dir);
    if (manifest.has_value() && is_valid_build_id(manifest->build_id)) {
        r.build_id = manifest->build_id;
        const std::string versioned = cef_dir_for_build(env, manifest->build_id);
        // Only trust a directory that finished materializing: the stamp is
        // written last and holds the build ID it was built for.
        const auto stamp = read_stamp(versioned);
        if (stamp.has_value() && *stamp == manifest->build_id) {
            r.dir = versioned;
            r.source = CefDirSource::Versioned;
        }
    }
    return r;
}

std::vector<std::string> stale_cef_dirs(const LayoutEnv& env,
                                        std::string_view keep_build_id) {
    std::vector<std::string> out;
    const std::string root = cef_versions_root(env);
    std::error_code ec;
    if (!fs::is_directory(fs::path(root), ec)) return out;
    for (fs::directory_iterator it(fs::path(root), ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        const std::string name = it->path().filename().string();
        // Only ever consider directories that look like a build ID, so a
        // mis-set AURORA_HOME can never turn the cleanup into "delete the
        // user's folders".
        if (!is_valid_build_id(name)) continue;
        if (name == keep_build_id) continue;
        out.push_back(normalize_separators(it->path().string()));
    }
    return out;
}

std::vector<std::string> default_cef_resource_globs() {
    return {
        // Chromium resource bundles and the CEF-specific one.
        "*.pak",
        "locales/*.pak",
        // ICU data and the V8 startup snapshots. Version-locked to the engine:
        // a snapshot from another Chromium build makes V8 abort at startup.
        // Named explicitly rather than as "*.bin", because the scan runs over
        // the whole packaged directory and must not adopt files that belong to
        // Qt or to Aurora itself.
        "icudtl.dat",
        "snapshot_blob.bin",
        "v8_context_snapshot.bin",
    };
}

std::vector<std::string> default_cef_identity_globs() {
    return {
        // The engine itself, plus the Windows loader shims and the ANGLE /
        // SwiftShader libraries CEF ships. None of these move into the
        // per-version directory (the OS loader looks next to the executable),
        // but all of them define which engine this build *is*.
        "libcef.dll",
        "libcef.so",
        "chrome_elf.dll",
        "d3dcompiler_47.dll",
        "libEGL.dll",
        "libGLESv2.dll",
        "vk_swiftshader.dll",
        "vulkan-1.dll",
        "dxil.dll",
        "dxcompiler.dll",
    };
}

std::string parent_dir(std::string_view path) {
    const std::string p = normalize_separators(std::string(path));
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return {};
    if (slash == 0) return "/";
    return p.substr(0, slash);
}

std::string current_executable_path() {
#if defined(_WIN32)
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return {};
    // Narrow to UTF-8 for the rest of the codebase, which is char-based.
    const int need = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), out.data(), need,
                        nullptr, nullptr);
    return normalize_separators(out);
#elif defined(__linux__)
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return normalize_separators(std::string(buf));
#else
    std::error_code ec;
    const auto p = fs::canonical("/proc/self/exe", ec);
    if (ec) return {};
    return normalize_separators(p.string());
#endif
}

} // namespace aurora::core

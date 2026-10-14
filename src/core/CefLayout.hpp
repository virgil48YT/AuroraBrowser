// Aurora — where the CEF runtime lives on disk.
//
// One place decides every path involved in per-version CEF extraction, because
// three different processes have to agree on them: the packaging tool that
// writes the manifest, the bootstrap launcher that materializes the tree, and
// the browser process that hands the directories to CefInitialize. Getting one
// of them wrong does not fail loudly — Chromium falls back to defaults and then
// misbehaves in ways that look like a rendering bug.
//
// Layout (Windows, the shipping target):
//
//   %LOCALAPPDATA%\Aurora\                     <- SFX extraction root
//       aurora_bootstrap.exe                   <- launched by the SFX
//       aurora_browser.exe, Qt6*.dll, libcef.dll, ...
//       aurora_build.manifest                  <- build ID + file hashes
//       cef\<build_id>\                         <- per-version resource tree
//           chrome_100_percent.pak, icudtl.dat, v8_context_snapshot.bin, ...
//           locales\en-US.pak, ...
//           .aurora-build-id                    <- completion stamp
//
// On POSIX the base directory is $XDG_DATA_HOME/Aurora (or ~/.local/share/
// Aurora) so the launcher and its tests are runnable on Linux, which is where
// they are developed.
//
// Everything here is string manipulation plus a few existence checks, and it is
// parameterized on an explicit environment struct rather than reading getenv()
// internally, so the resolution order is unit-testable without touching the
// real environment.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::core {

struct BuildManifest;

// Sub-directory of the Aurora base directory that holds one directory per CEF
// build ID.
inline constexpr char kCefVersionsDirName[] = "cef";

// Environment inputs used to locate the base directory. Empty fields mean
// "not set".
struct LayoutEnv {
    std::string aurora_home;      // AURORA_HOME       (explicit override)
    std::string cef_dir_override; // AURORA_CEF_DIR    (explicit override)
    std::string local_app_data;   // LOCALAPPDATA      (Windows)
    std::string xdg_data_home;    // XDG_DATA_HOME     (POSIX)
    std::string home;             // HOME / USERPROFILE

    // Reads the variables above from the process environment.
    static LayoutEnv from_environment();
};

// Joins with '/', collapsing a duplicated separator. '/' is accepted by every
// Windows API Aurora uses, and keeping one separator everywhere means the
// manifest paths and the runtime paths are directly comparable.
std::string join_path(std::string_view base, std::string_view leaf);

// The directory that holds everything Aurora writes for the installation
// (as opposed to the user profile, which stays in QStandardPaths::AppDataLocation).
std::string aurora_base_dir(const LayoutEnv& env);

// <base>/cef
std::string cef_versions_root(const LayoutEnv& env);

// <base>/cef/<build_id>. Returns an empty string for an invalid build ID: a
// directory name is being derived from it, so it is validated first.
std::string cef_dir_for_build(const LayoutEnv& env, std::string_view build_id);

// Parses `--cef-dir=PATH` out of argv. Returns nullopt when absent.
std::optional<std::string> cef_dir_from_args(int argc, char** argv);

// Where the browser process should point CEF's resources_dir_path /
// locales_dir_path.
enum class CefDirSource {
    Argument,     // --cef-dir=
    Environment,  // AURORA_CEF_DIR (set by the bootstrap launcher)
    Versioned,    // <base>/cef/<build_id>, stamped and complete
    Application,  // next to the executable (developer builds, no manifest)
};

struct CefDirResolution {
    std::string dir;
    CefDirSource source = CefDirSource::Application;
    std::string build_id;  // empty unless a manifest was found

    std::string source_name() const;
};

// Resolution order, first hit wins:
//   1. `arg_override` (--cef-dir=) if it names an existing directory
//   2. env.cef_dir_override (AURORA_CEF_DIR) if it names an existing directory
//   3. <base>/cef/<build_id> when aurora_build.manifest sits next to the
//      executable and that directory carries a matching completion stamp
//   4. `exe_dir` — the flat layout, which is what a local CMake build produces
//
// The browser process never extracts anything: if the versioned directory is
// not there and stamped, it falls back to the application directory rather than
// doing work the launcher is responsible for.
CefDirResolution resolve_cef_dir(const LayoutEnv& env,
                                 const std::string& exe_dir,
                                 const std::optional<std::string>& arg_override);

// Directories under <base>/cef that are not `keep_build_id` and look like a
// build directory (their name is a valid build ID). Used by the launcher to
// clean up after an update; returned rather than deleted so the caller decides.
std::vector<std::string> stale_cef_dirs(const LayoutEnv& env,
                                        std::string_view keep_build_id);

// Loads aurora_build.manifest from `dir`. Returns nullopt when the file is
// absent or malformed.
std::optional<BuildManifest> load_manifest_from_dir(const std::string& dir);

// The file sets that make up a CEF runtime, as globs relative to the packaged
// application directory.
//
// Resources are the version-locked data files Chromium loads through
// resources_dir_path / locales_dir_path — the .pak bundles, icudtl.dat and the
// V8 snapshots. These are what a per-version directory exists for: mixing a
// .pak from one Chromium with an engine from another is exactly the failure the
// build ID prevents.
std::vector<std::string> default_cef_resource_globs();

// Identity files are hashed into the build ID but stay next to the executable,
// because the OS loader — not CEF — resolves them. libcef.dll is the important
// one: it is what makes "same resources, newer engine" a different build ID.
std::vector<std::string> default_cef_identity_globs();

// Directory part of a path, without the trailing separator. "" for a bare name.
std::string parent_dir(std::string_view path);

// Absolute path of the running executable, or an empty string if it cannot be
// determined on this platform.
std::string current_executable_path();

} // namespace aurora::core

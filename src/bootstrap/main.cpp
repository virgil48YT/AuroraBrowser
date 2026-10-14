// Aurora — bootstrap launcher (aurora_bootstrap).
//
// This is the program the self-extracting installer runs. It is deliberately
// tiny: no Qt, no CEF, no SQLite — just aurora_core — because it runs before
// anything else and every millisecond and every DLL it needs is paid on every
// single launch.
//
// What it does, in order:
//
//   1. Reads aurora_build.manifest from its own directory (the SFX extraction
//      root) and recomputes the build ID from the manifest content. A manifest
//      whose stated ID does not match its content is rejected — that is a
//      tampered or truncated payload, not something to boot from.
//   2. Takes a per-build-ID lock so two launches racing (double-click twice)
//      cannot extract into the same directory at the same time.
//   3. Materializes %LOCALAPPDATA%\Aurora\cef\<build_id>\ from the files next to
//      the executable: hard links when possible, copies otherwise, verifies
//      hashes, then writes the completion stamp last. On every launch after the
//      first the stamp plus one CEF-binary existence/size check is the no-op
//      fast path; a requested verification still checks the complete manifest.
//   4. Collects per-version directories from older build IDs (best effort; a
//      still-running old browser keeps its files locked and is simply skipped).
//   5. Launches aurora_browser.exe with AURORA_CEF_DIR pointing at the
//      per-version directory and forwards every argument it was given.
//
// Flags it consumes itself (everything else is forwarded verbatim):
//
//   --shell                  allocate a console, log verbosely, wait for the
//                            browser and return its exit code. Also forwarded,
//                            so the browser attaches to the same console.
//   --aurora-force-extract   re-materialize even if the stamp matches
//   --aurora-full-verify     hash every file instead of checking sizes
//   --aurora-verify-only     do the work, print the report, do not launch
//   --aurora-print-build-id  print the build ID and exit
//   --aurora-keep-stale      do not collect old per-version directories
#include "core/BuildId.hpp"
#include "core/CefLayout.hpp"
#include "core/CefMaterialize.hpp"
#include "util/Log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using aurora::util::LogLevel;
namespace core = aurora::core;

namespace {

#if defined(_WIN32)
constexpr char kBrowserExeName[] = "aurora_browser.exe";
#else
constexpr char kBrowserExeName[] = "aurora_browser";
#endif

struct Options {
    bool shell = false;
    bool force_extract = false;
    bool full_verify = false;
    bool verify_only = false;
    bool print_build_id = false;
    bool keep_stale = false;
    std::vector<std::string> forward;  // arguments passed on to the browser
};

// Find the manifest's main CEF shared library and verify only that one anchor
// file. Build manifests intentionally classify libcef as Identity (loaded from
// the application directory) on Windows/Linux, while some package layouts may
// classify it as a materialized Resource; honor the role instead of assuming a
// root. Presence plus the recorded size is still constant-time and catches the
// common partial/corrupt extraction without touching every .pak and locale file.
std::optional<std::string> healthy_cef_binary(
    const std::string& app_dir, const std::string& cef_dir,
    const core::BuildManifest& manifest) {
    for (const auto& file : manifest.files) {
        const std::string name = fs::path(file.path).filename().string();
        if (name != "libcef.dll" && name != "libcef.so" &&
            name != "Chromium Embedded Framework") {
            continue;
        }
        const std::string& root =
            file.role == core::FileRole::Resource ? cef_dir : app_dir;
        const fs::path path = fs::path(root) / fs::path(file.path);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec) return std::nullopt;
        const auto size = fs::file_size(path, ec);
        if (ec || size != file.size) return std::nullopt;
        return path.string();
    }
    return std::nullopt;
}

Options parse_options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) continue;
        const std::string a = argv[i];
        if (a == "--aurora-force-extract") {
            o.force_extract = true;
        } else if (a == "--aurora-full-verify") {
            o.full_verify = true;
        } else if (a == "--aurora-verify-only") {
            o.verify_only = true;
        } else if (a == "--aurora-print-build-id") {
            o.print_build_id = true;
        } else if (a == "--aurora-keep-stale") {
            o.keep_stale = true;
        } else {
            // --shell is both consumed (console + wait) and forwarded (the
            // browser's own diagnostics behaviour keys off it).
            if (a == "--shell") o.shell = true;
            o.forward.push_back(a);
        }
    }
    return o;
}

void attach_console_if_requested(const Options& o) {
#if defined(_WIN32)
    if (!o.shell) return;
    // The launcher is a GUI-subsystem binary so double-clicking the SFX never
    // flashes a console. --shell opts in explicitly; AttachConsole first so
    // running it from an existing cmd.exe reuses that window.
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* unused = nullptr;
        freopen_s(&unused, "CONOUT$", "w", stdout);
        freopen_s(&unused, "CONOUT$", "w", stderr);
    }
#else
    (void)o;
#endif
}

// --- Cross-launch lock -------------------------------------------------------
// A lock file created with exclusive-create semantics, held for the duration of
// the extraction. Two simultaneous launches must not write the same tree; the
// loser waits for the winner's stamp instead of duplicating the work.
class ExtractionLock {
public:
    ExtractionLock() = default;
    ExtractionLock(const ExtractionLock&) = delete;
    ExtractionLock& operator=(const ExtractionLock&) = delete;
    ~ExtractionLock() { release(); }

    // Tries to take the lock, waiting up to `timeout` for a holder to finish.
    // A lock file older than kStaleAfter is assumed to belong to a launcher
    // that crashed and is stolen — otherwise a single crash would leave the
    // application permanently unable to start.
    bool acquire(const std::string& path, std::chrono::seconds timeout) {
        path_ = path;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (try_create()) {
                held_ = true;
                return true;
            }
            std::error_code ec;
            const auto age_ok = [&]() {
                const auto t = fs::last_write_time(fs::path(path_), ec);
                if (ec) return false;
                return (fs::file_time_type::clock::now() - t) > kStaleAfter;
            }();
            if (age_ok) {
                fs::remove(fs::path(path_), ec);
                continue;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool held() const { return held_; }

    void release() {
        if (!held_) return;
        std::error_code ec;
        fs::remove(fs::path(path_), ec);
        held_ = false;
    }

private:
    static constexpr std::chrono::minutes kStaleAfter{5};

    bool try_create() {
        std::error_code ec;
        fs::create_directories(fs::path(core::parent_dir(path_)), ec);
#if defined(_WIN32)
        const HANDLE h = CreateFileA(path_.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
#else
        const int fd = ::open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) return false;
        ::close(fd);
        return true;
#endif
    }

    std::string path_;
    bool held_ = false;
};

// --- Launching the browser ---------------------------------------------------

#if defined(_WIN32)
// Quotes one argument per the rules CommandLineToArgvW parses, so a path with
// spaces (every %LOCALAPPDATA% path on a machine whose user has a space in the
// name) survives the round trip.
std::string quote_arg(const std::string& arg) {
    const bool needs =
        arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
    if (!needs) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}
#endif

// Replaces the current process (POSIX) or spawns a child (Windows) running the
// browser. Returns the process exit code when `wait` is set, 0 when the child
// was started successfully and not waited for, and a non-zero code on failure.
int launch_browser(const std::string& exe, const std::string& cef_dir,
                   const std::string& app_dir,
                   const std::vector<std::string>& args, bool wait) {
    // The browser resolves its CEF resource directory from this variable; see
    // core::resolve_cef_dir. Passing it rather than a switch keeps it out of the
    // command line CEF forwards to its own subprocesses.
#if defined(_WIN32)
    SetEnvironmentVariableA("AURORA_CEF_DIR", cef_dir.c_str());
    // libcef.dll and the Qt DLLs live next to the executable, which the loader
    // already searches; prepending the directory to PATH additionally covers
    // any DLL that Chromium loads by name at runtime.
    {
        std::string path = app_dir;
        char* existing = nullptr;
        size_t len = 0;
        if (_dupenv_s(&existing, &len, "PATH") == 0 && existing != nullptr) {
            path += ";";
            path += existing;
            free(existing);
        }
        SetEnvironmentVariableA("PATH", path.c_str());
    }

    std::string cmd = quote_arg(exe);
    for (const auto& a : args) {
        cmd += " ";
        cmd += quote_arg(a);
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    if (!CreateProcessA(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, TRUE,
                        0, nullptr, app_dir.c_str(), &si, &pi)) {
        AURORA_LOG(LogLevel::Error, "bootstrap")
            << "CreateProcess failed for " << exe << " (error "
            << GetLastError() << ")";
        return 3;
    }
    CloseHandle(pi.hThread);
    if (!wait) {
        CloseHandle(pi.hProcess);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    ::setenv("AURORA_CEF_DIR", cef_dir.c_str(), 1);
    // The Windows branch above also fixes up PATH and the working directory;
    // on POSIX the loader uses rpath/LD_LIBRARY_PATH, so app_dir is unused.
    (void)app_dir;
    std::vector<std::string> owned;
    owned.push_back(exe);
    for (const auto& a : args) owned.push_back(a);
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& s : owned) argv.push_back(s.data());
    argv.push_back(nullptr);
    // exec replaces this process, so `wait` is implicit and the exit code is
    // the browser's by construction.
    (void)wait;
    ::execv(exe.c_str(), argv.data());
    AURORA_LOG(LogLevel::Error, "bootstrap")
        << "execv failed for " << exe;
    return 3;
#endif
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse_options(argc, argv);
    attach_console_if_requested(opt);
    aurora::util::Logger::instance().init_from_environment();
    if (opt.shell) {
        aurora::util::Logger::instance().set_level(LogLevel::Debug);
    }

    const std::string exe_path = core::current_executable_path();
    const std::string app_dir = exe_path.empty()
                                    ? std::string(".")
                                    : core::parent_dir(exe_path);
    const auto env = core::LayoutEnv::from_environment();

    // The browser executable can be redirected for tests and for a developer
    // who wants the launcher in front of a locally built binary.
    std::string browser_exe;
    if (const char* override_exe = std::getenv("AURORA_BROWSER_EXE")) {
        browser_exe = override_exe;
    } else {
        browser_exe = core::join_path(app_dir, kBrowserExeName);
    }

    const auto manifest = core::load_manifest_from_dir(app_dir);
    if (!manifest.has_value()) {
        // A developer build (plain CMake output) has no manifest. That is not an
        // error: run the browser out of the flat directory, exactly as before
        // Phase 4. Only packaged builds get per-version extraction.
        AURORA_LOG(LogLevel::Info, "bootstrap")
            << "no " << core::kManifestFileName << " in " << app_dir
            << "; launching with the application directory as the CEF root";
        if (opt.print_build_id) {
            std::printf("none\n");
            return 0;
        }
        if (opt.verify_only) {
            std::printf("no manifest in %s\n", app_dir.c_str());
            return 1;
        }
        return launch_browser(browser_exe, app_dir, app_dir, opt.forward,
                              opt.shell);
    }

    // The manifest states a build ID; the authoritative one is recomputed from
    // its content. A mismatch means the file was edited or truncated after
    // packaging, and the whole point of the ID is that we do not run in that
    // state.
    const std::string computed = core::compute_build_id(*manifest);
    if (manifest->build_id != computed) {
        AURORA_LOG(LogLevel::Error, "bootstrap")
            << "manifest build id mismatch: file says " << manifest->build_id
            << ", content hashes to " << computed
            << " — refusing to extract. Reinstall Aurora.";
        return 2;
    }
    const std::string build_id = computed;
    if (opt.print_build_id) {
        std::printf("%s\n", build_id.c_str());
        return 0;
    }
    AURORA_LOG(LogLevel::Info, "bootstrap")
        << "build id " << build_id << " (CEF " << manifest->cef_version
        << ", Aurora " << manifest->app_version << ", " << manifest->platform
        << ")";

    const std::string cef_dir = core::cef_dir_for_build(env, build_id);
    if (cef_dir.empty()) {
        AURORA_LOG(LogLevel::Error, "bootstrap")
            << "could not derive a per-version directory for " << build_id;
        return 2;
    }

    // Serialize concurrent launches on this build ID.
    ExtractionLock lock;
    const std::string lock_path =
        core::join_path(core::cef_versions_root(env), build_id + ".lock");
    if (!lock.acquire(lock_path, std::chrono::seconds(60))) {
        // Another launcher is extracting and did not finish in time. If it got
        // far enough to stamp the directory we can still use it; otherwise give
        // up rather than write into a tree somebody else owns.
        const auto stamp = core::read_stamp(cef_dir);
        if (!(stamp.has_value() && *stamp == build_id)) {
            AURORA_LOG(LogLevel::Error, "bootstrap")
                << "another Aurora launcher holds " << lock_path
                << " and the extraction is not complete; try again";
            return 4;
        }
        AURORA_LOG(LogLevel::Warn, "bootstrap")
            << "extraction lock is held elsewhere, but " << cef_dir
            << " is already complete; continuing";
    }

    // True no-op hot path. materialize_cef_resources() deliberately performs a
    // presence+size walk over every manifest Resource even for a stamped build;
    // on an SFX/antivirus-backed filesystem that "quick" walk can still cost
    // tens of seconds. The completion stamp was written only after a successful
    // full extraction+verification. For ordinary launches, matching it to this
    // computed BuildId and checking the main CEF binary is enough to launch
    // directly. Explicit repair/verification flags always take the slow path.
    const auto stamp = core::read_stamp(cef_dir);
    const auto cef_binary = healthy_cef_binary(app_dir, cef_dir, *manifest);
    const bool fast_path =
        !opt.force_extract && !opt.full_verify && !opt.verify_only &&
        stamp.has_value() && *stamp == build_id && cef_binary.has_value();
    if (fast_path) {
        lock.release();
        AURORA_LOG(LogLevel::Info, "bootstrap")
            << "cef resources: fast path for build " << build_id
            << " (stamp + " << *cef_binary << ")";
        return launch_browser(browser_exe, cef_dir, app_dir, opt.forward,
                              opt.shell);
    }
    if (stamp.has_value() && *stamp == build_id && !cef_binary.has_value()) {
        AURORA_LOG(LogLevel::Warn, "bootstrap")
            << "build stamp matches but the main CEF binary is missing or has "
               "the wrong size; falling back to extraction/verification";
    }

    core::MaterializeOptions mo;
    mo.force = opt.force_extract;
    mo.full_verify = opt.full_verify;
    const auto result =
        core::materialize_cef_resources(app_dir, cef_dir, *manifest, mo);
    lock.release();

    AURORA_LOG(result.ok ? LogLevel::Info : LogLevel::Error, "bootstrap")
        << "cef resources: " << result.summary();
    if (!result.ok) {
        if (opt.verify_only) {
            std::printf("FAILED %s\n", result.summary().c_str());
        }
        return 5;
    }

    // Collect obsolete versions once the current one is known good. Failures are
    // expected while an old browser is still shutting down, so they are logged
    // at Debug and forgotten.
    if (!opt.keep_stale) {
        for (const auto& dir : core::stale_cef_dirs(env, build_id)) {
            const bool removed = core::remove_cef_dir(dir);
            AURORA_LOG(removed ? LogLevel::Info : LogLevel::Debug, "bootstrap")
                << (removed ? "removed stale cef dir " : "could not remove ")
                << dir;
            // Its lock file, if any, goes with it.
            std::error_code ec;
            fs::remove(fs::path(dir + ".lock"), ec);
        }
    }

    if (opt.verify_only) {
        std::printf("ok build-id=%s dir=%s %s\n", build_id.c_str(),
                    cef_dir.c_str(), result.summary().c_str());
        return 0;
    }

    AURORA_LOG(LogLevel::Info, "bootstrap")
        << "launching " << browser_exe << " with AURORA_CEF_DIR=" << cef_dir;
    return launch_browser(browser_exe, cef_dir, app_dir, opt.forward, opt.shell);
}

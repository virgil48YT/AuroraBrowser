// Aurora — application entry point.
//
// Initializes logging and performance instrumentation, creates the profile
// directory, migrates the database, and shows the main window. When built
// with CEF, it runs the CEF subprocess check first (so renderer/GPU
// subprocesses never start the Qt UI) and initializes CEF before Qt.
//
// On startup the first window follows the Startup preference (see
// MainWindow::open_startup_tabs): restore the last session (the default),
// open the home page, or open one specific page. The open tabs are persisted
// on shutdown either way, so switching back to "continue" finds them.
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QFileDialog>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTimer>

#include <functional>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <cstdio>
#if defined(AURORA_WIN_BOOTSTRAP)
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#endif
#endif

#include "bookmarks/BookmarkService.hpp"
#include "adblock/AdblockEngine.hpp"
#include "adblock/AdblockService.hpp"
#include "downloads/DownloadService.hpp"
#include "extensions/ExtensionLoadList.hpp"
#include "extensions/ExtensionService.hpp"
#include "history/HistoryService.hpp"
#include "privacy/CertificateAudit.hpp"
#include "privacy/CookieSite.hpp"
#include "searchengines/SearchEngineService.hpp"
#include "core/CefLayout.hpp"
#include "core/InternalUrl.hpp"
#include "core/Url.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"
#include "session/SessionService.hpp"
#include "settings/Settings.hpp"
#include "ui/RoundedToolTip.hpp"
#include "ui/FaviconCache.hpp"
#include "ui/FilterListUpdater.hpp"
#include "ui/MainWindow.hpp"
#include "ui/ServiceBundle.hpp"
#include "util/Log.hpp"
#include "util/Perf.hpp"

#if defined(AURORA_HAS_CEF)
#include "cef/CefRuntime.hpp"

namespace aurora::cef {
// The CEF adapter is intentionally a Qt-free static library. The application
// supplies live Settings readers and the queued native-dialog implementation.
void configure_download_preferences(
    std::function<bool()> ask_where_provider,
    std::function<std::string()> downloads_dir_provider,
    std::function<void(std::string, std::function<void(std::string)>)>
        save_path_chooser);
} // namespace aurora::cef
#endif

using namespace aurora;

namespace {

// Write a synthetic session of `count` tabs so the next launch's restore
// exercises the bulk-restore path. CI-only test hook behind --seed-session=N;
// it touches nothing but the sessions tables of the profile it is pointed at.
void seed_session(session::SessionService& service, int count) {
    std::vector<session::SavedTab> tabs;
    tabs.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        session::SavedTab t;
        t.url = "https://seeded" + std::to_string(i) + ".test/page";
        t.title = "Seeded tab " + std::to_string(i);
        t.window_index = 0;
        t.tab_index = i;
        tabs.push_back(std::move(t));
    }
    const auto id = service.save(tabs);
    util::Logger::instance().log(
        util::LogLevel::Info, "smoke",
        "seeded session " + std::to_string(id) + " with " +
            std::to_string(count) + " tabs");
}

// Headless self-check used by CI and by `QT_QPA_PLATFORM=offscreen
// aurora_browser --smoke-test`: verifies the session-restore invariants, walks
// every internal page so their widgets are actually built and populated from the
// profile database, then quits. A violated invariant (or a crash in the tab
// bookkeeping) surfaces as a non-zero exit instead of a silent hang, which is
// the only way to exercise the chrome without a display or a CEF build.
void run_smoke_test(ui::MainWindow& window, int restored) {
    using core::InternalPage;

    // --- Session-restore invariants, checked before anything else touches the
    //     content area. A bulk restore of N tabs must leave exactly one shown
    //     host and one active tab; the crash this guards against was one web
    //     engine (with a native window) created per tab in one synchronous loop.
    int failures = 0;
    if (restored > 0) {
        const int shown = window.hosts_activated();
        const int created = window.hosts_created();
        if (shown != 1) {
            util::Logger::instance().log(
                util::LogLevel::Error, "smoke",
                "restore showed " + std::to_string(shown) +
                    " hosts, expected exactly 1");
            ++failures;
        }
        if (created > 1) {
            util::Logger::instance().log(
                util::LogLevel::Error, "smoke",
                "restore created " + std::to_string(created) +
                    " web engines, expected at most 1");
            ++failures;
        }
        const int native = window.hosts_with_native_window();
        if (native > 1) {
            util::Logger::instance().log(
                util::LogLevel::Error, "smoke",
                "restore forced native windows for " + std::to_string(native) +
                    " hosts, expected at most 1");
            ++failures;
        }
        if (!window.tabs().active().valid() ||
            window.shown_tab() != window.tabs().active()) {
            util::Logger::instance().log(
                util::LogLevel::Error, "smoke",
                "the shown tab is not the active tab after restore");
            ++failures;
        }
        if (static_cast<int>(window.tabs().count()) != restored) {
            util::Logger::instance().log(
                util::LogLevel::Error, "smoke",
                "restored " + std::to_string(restored) +
                    " tabs but the model holds " +
                    std::to_string(window.tabs().count()));
            ++failures;
        }
        util::Logger::instance().log(
            util::LogLevel::Info, "smoke",
            "restore of " + std::to_string(restored) + " tab(s): shown hosts=" +
                std::to_string(shown) + " engines=" + std::to_string(created) +
                " native windows=" + std::to_string(native));
    }

    for (auto page : {InternalPage::History, InternalPage::Downloads,
                      InternalPage::Bookmarks, InternalPage::Settings,
                      InternalPage::Extensions, InternalPage::SiteSettings}) {
        window.show_page(page);
        QApplication::processEvents();
    }
    // Exercise the tab strip: open a second tab, then close tabs down to the
    // last one, which is the path that used to crash.
    const auto second = window.tabs().open("about:blank");
    QApplication::processEvents();
    window.tabs().close(second);
    QApplication::processEvents();
    util::Logger::instance().log(util::LogLevel::Info, "smoke",
                                 "internal pages and tab strip exercised");
    QApplication::exit(failures == 0 ? 0 : 1);
}

#if defined(AURORA_HAS_CEF)
// Read the enabled unpacked extensions from the profile.
//
// Chromium decides which extensions to load while the engine initializes, and
// cef::initialize() runs before the application's own database handle is open,
// so this opens the profile database briefly, on its own, just to answer that
// one question. It is deliberately tolerant: a profile that cannot be opened
// yet simply means "no extensions", never a failed startup.
// Directories passed as --load-extension=dir[,dir...] on Aurora's own command
// line. Chromium has a switch of the same name, but the engine is initialized
// from the profile registry, so a switch that only reached Chromium would load
// an extension the browser's own extensions page knew nothing about. These
// directories are therefore registered in the profile first, exactly as if the
// user had added them from aurora://extensions, and then take part in the
// normal enabled/disabled bookkeeping.
std::vector<std::string> extension_dirs_from_argv(int argc, char** argv) {
    std::vector<std::string> dirs;
    const std::string prefix = "--load-extension=";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind(prefix, 0) != 0) continue;
        std::string list = arg.substr(prefix.size());
        size_t start = 0;
        while (start <= list.size()) {
            const size_t comma = list.find(',', start);
            const std::string one = list.substr(
                start, comma == std::string::npos ? std::string::npos
                                                  : comma - start);
            if (!one.empty()) dirs.push_back(one);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    return dirs;
}

std::vector<std::string> enabled_extension_dirs(const QString& profile,
                                                int argc, char** argv) {
    storage::Database db;
    if (!db.open((profile + "/profile.db").toStdString())) return {};
    if (storage::Migrator::migrate(db) != storage::DbError::Ok) return {};
    extensions::ExtensionService svc(db);
    for (const auto& dir : extension_dirs_from_argv(argc, argv)) {
        // add_unpacked validates the manifest and is idempotent per path, so
        // passing the same directory on every launch is harmless.
        if (svc.add_unpacked(dir) <= 0) {
            util::Logger::instance().log(
                util::LogLevel::Warn, "app",
                "--load-extension: not a valid unpacked extension: " + dir);
        }
    }
    // A registry row can outlive its folder (the user deleted or moved a
    // "Load unpacked" source tree). Handing Chromium a directory without a
    // manifest only produces a load error, so such rows are left out here —
    // still registered and still listed on aurora://extensions, just not
    // loaded. This runs before any window exists (the same pre-UI startup
    // window as the database open above), never on a live UI thread.
    const auto list = extensions::build_load_list(
        svc.enabled_paths(), extensions::is_loadable_extension_dir);
    for (const auto& skipped : list.skipped) {
        util::Logger::instance().log(
            util::LogLevel::Warn, "app",
            "not loading extension " + skipped.path + ": " +
                std::string(extensions::load_skip_reason_name(skipped.reason)));
    }
    util::Logger::instance().log(
        util::LogLevel::Info, "app",
        std::to_string(list.dirs.size()) +
            " enabled extension(s) handed to the engine");
    return list.dirs;
}

// Settings > Performance > "Use hardware acceleration when available".
//
// The GPU mode is fixed when the engine starts (OnBeforeCommandLineProcessing
// reads AURORA_GPU through cef::gpu_mode_from_environment), and that happens
// before the application's own database handle is open, so the preference is
// read here the same way the extension list is. Turning it off selects the
// software (SwiftShader) path, which keeps WebGL working without the GPU. An
// explicit AURORA_GPU in the environment always wins over the preference.
// Child processes inherit the variable from this process.
void apply_hardware_acceleration_preference(const QString& profile) {
    if (!qEnvironmentVariableIsEmpty("AURORA_GPU")) return;
    storage::Database db;
    if (!db.open((profile + "/profile.db").toStdString())) return;
    const settings::Settings prefs(db, /*read_only=*/true);
    if (prefs.get_bool(settings::PrefKeys::kHardwareAcceleration,
                       settings::PrefDefaults::kHardwareAcceleration)) {
        return;
    }
    qputenv("AURORA_GPU", QByteArrayLiteral("software"));
    util::Logger::instance().log(
        util::LogLevel::Info, "app",
        "hardware acceleration is off in settings; using software rendering");
}
#endif

} // namespace

#if defined(AURORA_HAS_CEF) && defined(AURORA_WIN_BOOTSTRAP)
// Windows sandbox, LPAC: Chromium runs its network service in a Less
// Privileged AppContainer, which can only read files whose ACL grants "ALL
// RESTRICTED APPLICATION PACKAGES" (S-1-15-2-2). Program Files grants that by
// default; %LOCALAPPDATA%\Aurora (where the SFX extracts) and an unzipped
// artifact folder do not, and the sandboxed network service then fails to
// start. CEF's own samples run `icacls <dir> /grant *S-1-15-2-2:(OI)(CI)(RX)`
// at build time (SET_LPAC_ACLS); a user install has to do it at runtime, once.
// Idempotent: returns early when an allow ACE for the SID is already present,
// so the (inheritance-propagating) DACL write happens on first run only.
static bool ensure_lpac_read_access(const std::string& utf8_dir) {
    if (utf8_dir.empty()) return false;
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_dir.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring dir(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_dir.c_str(), -1, dir.data(), wlen);
    dir.resize(static_cast<std::size_t>(wlen - 1));

    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-15-2-2", &sid)) return false;
    constexpr DWORD kReadExecute = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool ok = false;
    if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &dacl, nullptr, &sd) == ERROR_SUCCESS) {
        bool present = false;
        ACL_SIZE_INFORMATION info{};
        if (dacl && GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
            for (DWORD i = 0; i < info.AceCount && !present; ++i) {
                void* ace = nullptr;
                if (!GetAce(dacl, i, &ace)) continue;
                if (static_cast<ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE)
                    continue;
                auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
                const bool reads = (allowed->Mask & GENERIC_READ) != 0 ||
                                   (allowed->Mask & kReadExecute) == kReadExecute;
                present = reads && EqualSid(&allowed->SidStart, sid);
            }
        }
        if (present) {
            ok = true;
        } else {
            EXPLICIT_ACCESSW ea{};
            ea.grfAccessPermissions = kReadExecute;
            ea.grfAccessMode = GRANT_ACCESS;
            ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
            ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
            ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
            PACL updated = nullptr;
            if (SetEntriesInAclW(1, &ea, dacl, &updated) == ERROR_SUCCESS) {
                ok = SetNamedSecurityInfoW(dir.data(), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION, nullptr,
                                           nullptr, updated, nullptr) == ERROR_SUCCESS;
                LocalFree(updated);
            }
        }
        LocalFree(sd);
    }
    LocalFree(sid);
    return ok;
}
#endif

// The real entry point. On Windows with AURORA_WIN_SANDBOX the browser is a
// DLL and this is reached from RunWinMain (bottom of this file); everywhere
// else from main().
static int aurora_main(int argc, char** argv) {
#if defined(_WIN32)
    // --shell: allocate a console for diagnostics. The exe is a GUI-subsystem
    // app (WIN32_EXECUTABLE), so no console appears unless the user explicitly
    // passes --shell. CEF subprocesses (--type=...) are skipped so each
    // renderer/GPU helper doesn't pop its own console. Extra args after
    // --shell are forwarded to aurora_browser.exe by the SFX launcher.
    {
        bool want_shell = false;
        bool is_subprocess = false;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--shell") want_shell = true;
            if (a.rfind("--type=", 0) == 0) is_subprocess = true;
        }
        if (want_shell && !is_subprocess && AllocConsole()) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
    }
#endif
    util::Logger::instance().init_from_environment();

#if defined(AURORA_HAS_CEF)
    // CEF subprocesses (renderer, GPU, etc.) enter through this same main().
    // run_subprocess returns >= 0 for a subprocess; in that case we exit
    // immediately without touching Qt. The "process start" milestone is logged
    // only by the browser process (below) so the console is not flooded with
    // one "process start" line per subprocess on Windows, where the GPU and
    // renderer processes share the browser's stderr.
    //
    // macOS: libcef is not linked but loaded from the bundle's
    // Contents/Frameworks, and that has to happen before any CEF call. (Real
    // subprocesses on macOS are the separate "Aurora Helper*.app" executables,
    // see src/app/mac_helper_main.cpp; this call still returns -1 here.)
    if (!cef::load_library(/*helper=*/false)) return 1;
    int code = cef::run_subprocess(argc, argv);
    if (code >= 0) return code;
#if defined(__linux__)
    // CEF parents its browser windows to the widget's X11 window id, so the Qt
    // chrome must be an X11 client too; under Wayland that means XWayland.
    // The engine side is forced to Ozone/X11 in CefLifecycle.cpp. An explicit
    // QT_QPA_PLATFORM (e.g. offscreen for --smoke-test) is respected.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif
#endif

    util::startup_milestone("process start");

    // The application/organization names decide where QStandardPaths puts the
    // profile, so they MUST be set before the first writableLocation() call.
    // They used to be set after QApplication was constructed — which is after
    // the engine had already been initialized below — so the engine's cache and
    // the extension registry it reads were created one directory above the real
    // profile (~/.local/share/cef_cache instead of
    // ~/.local/share/Aurora/Aurora/cef_cache). Cookies still persisted, but in
    // the wrong place, and extensions registered before the engine started were
    // invisible to the running browser.
    QApplication::setApplicationName("Aurora");
    QApplication::setOrganizationName("Aurora");

    // Third-party cookie site boundaries use the real Public Suffix List,
    // compiled in as a resource. It must be installed before CEF starts
    // issuing resource requests (cookie_site() is called on CEF's IO thread);
    // until then, or if the resource is missing or truncated, CookieSite's
    // embedded table is the fallback. Browser process only: subprocesses
    // returned above.
    {
        QFile psl(QStringLiteral(":/psl/public_suffix_list.dat"));
        const QByteArray text = psl.open(QIODevice::ReadOnly) ? psl.readAll() : QByteArray();
        const bool installed = privacy::install_public_suffix_list(
            std::string_view(text.constData(), static_cast<std::size_t>(text.size())));
        util::Logger::instance().log(
            installed ? util::LogLevel::Info : util::LogLevel::Warn, "privacy",
            installed ? std::string("public suffix list loaded")
                      : std::string("public suffix list unavailable; using the "
                                    "embedded suffix table for cookie sites"));
    }

#if defined(AURORA_HAS_CEF)
    // Initialize CEF for the browser process before Qt takes over the UI.
    QString profile_dir = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    QDir().mkpath(profile_dir);

    // Where this build's CEF resource files live. A packaged build is launched
    // by aurora_bootstrap, which extracts them per build ID and passes the
    // directory in AURORA_CEF_DIR; a developer build has them next to the
    // executable and resolves to that. The browser never extracts anything
    // itself — if the per-version directory is not complete, resolution falls
    // back to the application directory rather than doing the launcher's job
    // halfway. --cef-dir=PATH overrides everything, for bisecting engines.
    const std::string exe_dir =
        core::parent_dir(core::current_executable_path());

    // --- DIAGNOSTIC (temporary): trace the adblock runtime to a file next to
    // the executable. Localizes why blocking is a no-op at runtime: whether the
    // engine is created, whether CEF consults GetResourceRequestHandler, and
    // whether OnBeforeResourceLoad fires + what should_block returns. Remove
    // once the bug is found and fixed. ---
    //
    // Windows only by default: on Linux the executable usually sits in a
    // read-only prefix (/opt, an AppImage mount), and on macOS writing into
    // the .app bundle would invalidate its code signature. Set
    // AURORA_DEBUG_LOG=1 to opt in there.
#if !defined(_WIN32)
    if (!qEnvironmentVariableIsEmpty("AURORA_DEBUG_LOG"))
#endif
    {
        static std::ofstream diag_log(exe_dir + "/aurora-debug.log",
                                      std::ios::app);
        if (diag_log.is_open()) {
            util::Logger::instance().set_stream(&diag_log);
            util::Logger::instance().set_level(util::LogLevel::Trace);
            util::Logger::instance().log(
                util::LogLevel::Info, "diag",
                "adblock diagnostic logging enabled (trace) -> " + exe_dir +
                    "/aurora-debug.log");
        }
    }
    const auto cef_paths = core::resolve_cef_dir(
        core::LayoutEnv::from_environment(), exe_dir,
        core::cef_dir_from_args(argc, argv));
    util::Logger::instance().log(
        util::LogLevel::Info, "app",
        "cef resources from " + cef_paths.source_name() + ": " + cef_paths.dir +
            (cef_paths.build_id.empty() ? std::string()
                                        : " (build " + cef_paths.build_id + ")"));

#if defined(AURORA_WIN_BOOTSTRAP)
    // Sandboxed network service (LPAC) must be able to read the runtime.
    for (const std::string& dir : {exe_dir, cef_paths.dir}) {
        if (!dir.empty() && !ensure_lpac_read_access(dir)) {
            util::Logger::instance().log(
                util::LogLevel::Warn, "app",
                "could not grant LPAC read access to " + dir +
                    "; the sandboxed network service may fail to start");
        }
    }
#endif
    apply_hardware_acceleration_preference(profile_dir);
    if (!cef::initialize((profile_dir + "/cef_cache").toStdString(),
                         enabled_extension_dirs(profile_dir, argc, argv),
                         cef_paths.dir)) {
        util::Logger::instance().log(util::LogLevel::Error, "app",
                                    "CEF initialization failed; exiting.");
        return 1;
    }
#endif

    QApplication app(argc, argv);
    aurora::ui::install_rounded_tooltips(&app);
    // Closing one browser window must never quit the process while another
    // MainWindow is still open. MainWindow::closeEvent quits explicitly when
    // the last visible MainWindow goes away, so Alt+F4 / the window X on the
    // last window still exits the app.
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/aurora_mark.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription("Aurora browser");
    parser.addHelpOption();
    QCommandLineOption smoke_opt(
        "smoke-test",
        "Build the chrome, open every internal page, then exit (headless CI).");
    parser.addOption(smoke_opt);
    QCommandLineOption seed_opt(
        "seed-session",
        "Before restoring, write a synthetic session of N tabs to the profile "
        "(CI hook: proves a large bulk restore does not crash).",
        "tabs");
    parser.addOption(seed_opt);
    // Documented here so --help lists it; the value is read before Qt exists
    // (core::cef_dir_from_args above), because CEF is initialized first.
    QCommandLineOption cef_dir_opt(
        "cef-dir",
        "Load the CEF resource files (*.pak, icudtl.dat, locales/) from DIR "
        "instead of the per-version directory chosen by the build ID.",
        "dir");
    parser.addOption(cef_dir_opt);
    parser.addPositionalArgument(
        "url", "URL(s) to open in new tabs on startup.", "[url...]");
    // Unknown arguments (CEF passes its own) must not abort startup, so parse
    // permissively instead of using process().
    parser.parse(QApplication::arguments());
    if (parser.isSet("help")) {
        parser.showHelp(0);
    }
    const bool smoke_test = parser.isSet(smoke_opt);
    int seed_tabs = 0;
    if (parser.isSet(seed_opt)) {
        bool ok = false;
        const int n = parser.value(seed_opt).toInt(&ok);
        if (ok && n > 0) seed_tabs = n;
    }
    util::startup_milestone("qt initialized");

    QString profile = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    QDir().mkpath(profile);

    // Open and migrate the profile database so history and bookmark services
    // can serve omnibox suggestions immediately.
    storage::Database db;
    if (!db.open((profile + "/profile.db").toStdString())) {
        util::Logger::instance().log(util::LogLevel::Error, "app",
                                     "failed to open profile db");
    } else {
        storage::Migrator::migrate(db);
    }
    // Ensure the preferences table exists.
    settings::Settings prefs(db);
    prefs.ensure_schema();

#if defined(AURORA_HAS_CEF)
    cef::configure_download_preferences(
        [&prefs] { return prefs.downloads_ask_where(); },
        [&prefs] { return prefs.downloads_path(); },
        [](std::string initial_path,
           std::function<void(std::string)> complete) {
            auto* application = QApplication::instance();
            if (!application) {
                complete({});
                return;
            }
            // OnBeforeDownload calls this on CEF's UI thread. Queue the
            // blocking native dialog so that callback can return first; the
            // retained CefBeforeDownloadCallback is continued after selection.
            QMetaObject::invokeMethod(
                application,
                [initial_path = std::move(initial_path),
                 complete = std::move(complete)]() mutable {
                    const QString chosen = QFileDialog::getSaveFileName(
                        QApplication::activeWindow(),
                        QStringLiteral("Save file as"),
                        QString::fromStdString(initial_path));
                    complete(chosen.toStdString());
                },
                Qt::QueuedConnection);
        });
#endif

    // The profile services. They are owned here, outlive the window, and are
    // borrowed by it through the ServiceBundle so the internal pages can read
    // and write real data.
    history::HistoryService history_svc(db);
    bookmarks::BookmarkService bookmark_svc(db);
    downloads::DownloadService download_svc(db);
    extensions::ExtensionService extension_svc(db);
    session::SessionService session_svc(db);
    adblock::AdblockService adblock_svc(db);
    privacy::CertificateAudit certificate_audit(db);

    // Custom search engines run on their own worker thread with their own
    // connection to the (already migrated) profile database, so neither the
    // omnibox nor the settings page ever touches SQLite for them on the UI
    // thread. The one bounded wait below happens before any window exists so
    // a custom default engine is honoured by the very first navigation; if
    // the worker is slower than that, resolution falls back to DuckDuckGo
    // until it has loaded rather than blocking startup.
    std::unique_ptr<searchengines::SearchEngineService> search_engine_svc;
    if (db.is_open()) {
        search_engine_svc = std::make_unique<searchengines::SearchEngineService>(
            (profile + "/profile.db").toStdString());
        if (!search_engine_svc->wait_until_loaded(std::chrono::milliseconds(500))) {
            util::Logger::instance().log(
                util::LogLevel::Warn, "app",
                "search engines still loading; custom default applies once loaded");
        }
    }

#if defined(AURORA_HAS_CEF)
    cef::configure_privacy_preferences(db.is_open() ? &prefs : nullptr,
                                      db.is_open() ? &certificate_audit : nullptr);
    // Refresh the network-thread snapshot when settings change in any window,
    // even for existing pages issuing requests without a new navigation.
    QTimer privacy_refresh;
    privacy_refresh.setInterval(250);
    QObject::connect(&privacy_refresh, &QTimer::timeout,
                     [] { cef::refresh_privacy_preferences(); });
    privacy_refresh.start();
#endif

    // Favicons are cached on disk per profile so the tab strip, history and
    // bookmarks show real site icons on the first paint after a restart
    // instead of re-fetching every icon each launch.
    ui::FaviconCache::instance().set_cache_dir(profile);

    ui::ServiceBundle services;
    // Profile list first (the file adblock::ListUpdater installs, named by
    // adblock::kFilterListFileName), then the legacy location (see below).
    const std::vector<std::string> adblock_rule_paths{
        (profile + "/adblock-rules.txt").toStdString(),
        (QDir::homePath() + "/.aurora/profile/adblock-rules.txt").toStdString()};
    // Outlives every window (declared before them) — windows only borrow it.
    std::unique_ptr<ui::FilterListUpdater> filter_list_updater;
    if (db.is_open()) {
        services.history = &history_svc;
        services.bookmarks = &bookmark_svc;
        services.downloads = &download_svc;
        services.extensions = &extension_svc;
        services.prefs = &prefs;
        services.session = &session_svc;
        services.adblock = &adblock_svc;
        services.search_engines = search_engine_svc.get();
        // Filter list from the profile directory, combined with the built-in
        // starter rules (see adblock::load_rules). The file is refreshed out
        // of band — by scripts/fetch_filter_lists.sh, or on request from
        // Settings > Privacy (ui::FilterListUpdater below) — and nothing here
        // downloads anything at startup. `profile` is a QString (paths are
        // UTF-16 on Windows), so convert exactly like the db.open() call above.
        //
        // The legacy path is tried second: older versions of
        // scripts/fetch_filter_lists.sh defaulted to ~/.aurora/profile, which
        // is not where QStandardPaths puts the profile on any platform, so a
        // user who ran the script got EasyList written somewhere the browser
        // never looked and silently kept running on starter rules only. The
        // script now defaults to the real profile directory; this fallback
        // keeps already-fetched lists working without a re-download.
        services.adblock_rules = adblock::load_rules(adblock_rule_paths);
        // Manual, opt-in updates only: the updater never starts on its own.
        filter_list_updater = std::make_unique<ui::FilterListUpdater>(
            profile, &prefs, adblock_rule_paths, services.adblock_rules);
        services.filter_lists = filter_list_updater.get();
    }

    // CI hook: seed a large session so the restore below exercises the bulk
    // path with a realistic tab count.
    if (seed_tabs > 0 && db.is_open()) seed_session(session_svc, seed_tabs);

    ui::MainWindow window(services);
    // Start where the last run left off; a first run starts maximized.
    window.restore_saved_window_state();
    window.show();
    util::startup_milestone("window shown");

    // Apply the Startup preference: restore the previous session, keep the
    // seed home tab, or point it at the user's specific page. Restoring MUST
    // go through the window: it validates every saved row, opens the tabs in
    // bulk mode so only the one chosen tab is ever made visible, and keeps
    // its own tab bookkeeping consistent. Opening restored tabs directly on
    // window.tabs() from here (as older builds did) bypasses that guard and
    // creates one web engine per restored tab in a single event-loop turn —
    // the startup crash. Only this first window does any of this: windows
    // opened later (Ctrl+N) always start on the home page.
    const int restored = window.open_startup_tabs();

    // URLs given on the command line (`aurora_browser https://example.com`, or
    // the OS handing a link to the chosen browser) open in their own tabs after
    // the session restore, so a restore never discards them. Anything that is
    // not a URL is treated the same way the omnibox treats it, which means a
    // bare word becomes a search on the configured engine — matching Chrome.
    for (const QString& arg : parser.positionalArguments()) {
        const std::string text = arg.toStdString();
        if (text.empty() || text[0] == '-') continue;
        core::Url url = core::parse_internal_url(text).has_value()
                            ? core::Url(text)
                            : core::parse_omnibox_input(text);
        if (!url.is_valid()) continue;
        window.open_url_in_new_tab(url);
    }

#if defined(AURORA_HAS_CEF)
    // CEF runs single-threaded against our UI thread: pump its message loop
    // from a high-frequency timer so rendering and input progress while Qt
    // owns the main event loop. ~8ms keeps it responsive without burning CPU
    // when idle (CEF yields when there is no work).
    QTimer cef_pump;
    cef_pump.setInterval(8);
    QObject::connect(&cef_pump, &QTimer::timeout, [] { cef::pump_message_loop(); });
    cef_pump.start();
#endif

    if (smoke_test) {
        QTimer::singleShot(0, &window,
                           [&window, restored] { run_smoke_test(window, restored); });
    }

    // The session is saved by MainWindow::closeEvent, which runs while the tab
    // model is still alive. Nothing to do here.
    int rc = app.exec();

#if defined(AURORA_HAS_CEF)
    // CEF requires every browser to be closed before CefShutdown. If we call
    // CefShutdown with browsers still open, the GPU/renderer subprocesses are
    // not reaped and keep aurora_browser.exe + the CEF DLLs mapped — which is
    // why Windows refused to delete the app folder with "something is still
    // running". So: stop the pump timer, close every browser and pump CEF
    // until the closes complete (MainWindow::prepare_shutdown), then shut the
    // CEF runtime down. The window and its host HWNDs are still alive here
    // (they are destroyed when this function returns), so CEF can close the
    // browsers cleanly.
    cef_pump.stop();
    privacy_refresh.stop();
    window.prepare_shutdown();
    cef::configure_privacy_preferences(nullptr, nullptr);
    const auto cef_shutdown_start = std::chrono::steady_clock::now();
    cef::shutdown();
    const auto cef_shutdown_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cef_shutdown_start).count();
    AURORA_LOG(util::LogLevel::Info, "cef")
        << "CefShutdown took " << cef_shutdown_ms << "ms";
#endif
    return rc;
}

#if defined(AURORA_WIN_BOOTSTRAP)
// Windows sandbox entry point. CEF's prebuilt bootstrap.exe — shipped as
// aurora_browser.exe and linked with Chromium's sandbox, which has to live in
// the executable image — loads aurora_browser.dll from its own directory and
// calls this export for the browser process AND for every subprocess, with
// the sandbox_info that CefExecuteProcess/CefInitialize need. Signature from
// include/cef_sandbox_win.h; the last parameter is cef_version_info_t*, which
// Aurora does not use (declared void* so this TU needs no CEF headers — the
// export is extern "C", so the name, not the C++ type, is what binds).
extern "C" __declspec(dllexport) int RunWinMain(HINSTANCE /*instance*/,
                                                LPWSTR /*command_line*/,
                                                int /*show*/,
                                                void* sandbox_info,
                                                void* /*version_info*/) {
    aurora::cef::set_windows_sandbox_info(sandbox_info);
    // Rebuild a UTF-8 argv from the full command line (bootstrap passes the
    // tail only). Static storage: QApplication keeps pointers into argv.
    static std::vector<std::string> storage;
    static std::vector<char*> pointers;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 0; wargv && i < wargc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0,
                                          nullptr, nullptr);
        std::string arg(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, arg.data(), n, nullptr,
                                nullptr);
        storage.push_back(std::move(arg));
    }
    if (wargv) LocalFree(wargv);
    if (storage.empty()) storage.emplace_back("aurora_browser.exe");
    for (auto& arg : storage) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    return aurora_main(static_cast<int>(storage.size()), pointers.data());
}
#else
int main(int argc, char** argv) {
    return aurora_main(argc, argv);
}
#endif

#include "cef/CefLifecycle.hpp"

#include "cef/CefAdapter.hpp"
#include "cef/GpuPolicy.hpp"
#include "cef/MediaPolicy.hpp"
#include "extensions/ExtensionLoadList.hpp"
#include "util/Log.hpp"

// CEF headers. These come from the CEF binary distribution pointed to by
// CEF_ROOT at configure time (see cmake/FindCEF.cmake).
#include "include/cef_app.h"
#include "include/cef_version.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <cstdio>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include "cef/CefMacSupport.hpp"
#include "include/wrapper/cef_library_loader.h"
#endif

namespace aurora::core { class TabId; }  // forward

namespace aurora::cef {

using util::LogLevel;

namespace {

// Unpacked Chrome-extension directories to load, set by initialize() before
// CefInitialize and read by the CefApp below when Chromium asks us to amend
// the browser process command line. A plain file-static is correct here: it is
// written once on the UI thread before CEF exists and only read afterwards.
std::vector<std::string> g_extension_dirs;

// Chromium's own switch for loading unpacked extensions. CEF removed the
// Alloy CefExtension/CefExtensionHandler/RequestContext::LoadExtension API in
// M128, so CEF 152 embedders using Alloy-style child browsers have no runtime
// load/unload handle. The Chrome bootstrap still consumes this startup switch.
// (Chrome-branded release builds ignore it since M137; CEF is not
// Chrome-branded, so the switch is honoured.)
//
// Incognito: the switch is process-wide and loads into the regular profile.
// A private window browses through a request context with an empty
// cache_path, which CEF documents as "incognito mode"; Chromium does not run
// extensions there unless the user grants "Allow in Incognito", and Aurora
// exposes no such toggle. On the chrome side every private window also drops
// its ExtensionService (core::IncognitoPolicy::enable_extensions()), so it
// shows no extensions button, popups or install path.
constexpr char kLoadExtensionSwitch[] = "load-extension";

// CefApp implementation shared by the browser process and all subprocesses.
// Process-type detection (CefBrowserProcessHandler vs CefRenderProcessHandler)
// happens here so the main executable stays thin.
// Private memory of THIS process in bytes, or -1 when the platform gives no
// answer. Run inside a renderer so a tab's card can show what its page costs.
double current_process_memory_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        return static_cast<double>(counters.PrivateUsage);
    }
    return -1.0;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1.0;
    char line[256];
    double bytes = -1.0;
    while (std::fgets(line, sizeof(line), f)) {
        long kb = 0;
        if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) {
            bytes = static_cast<double>(kb) * 1024.0;
            break;
        }
    }
    std::fclose(f);
    return bytes;
#elif defined(__APPLE__)
    // Resident size of this task, the closest analogue to Linux VmRSS.
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<double>(info.resident_size);
    }
    return -1.0;
#else
    return -1.0;
#endif
}

class AuroraApp : public CefApp,
                  public CefBrowserProcessHandler,
                  public CefRenderProcessHandler {
public:
    AuroraApp() = default;

    // Renderer-process hook: answers the browser's memory query.
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return this;
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> /*browser*/,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId /*source_process*/,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (!message.get() || message->GetName() != "aurora.memory.request") {
            return false;
        }
        const double bytes = current_process_memory_bytes();
        if (bytes >= 0.0 && frame.get()) {
            CefRefPtr<CefProcessMessage> reply =
                CefProcessMessage::Create("aurora.memory.reply");
            reply->GetArgumentList()->SetDouble(0, bytes);
            frame->SendProcessMessage(PID_BROWSER, reply);
        }
        return true;
    }

    // Browser-process-only hooks.
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    void OnContextInitialized() override {
        AURORA_LOG(LogLevel::Info, "cef") << "context initialized";
    }

    void OnBeforeChildProcessLaunch(
        CefRefPtr<CefCommandLine> command_line) override {
        // Mark subprocesses so crash reports and logs can attribute them.
        command_line->AppendSwitch("aurora");
    }

    // Applied to every process (browser, GPU, renderer, ...).
    //
    // The switch list itself is computed by aurora::cef::gpu_command_line_
    // switches() in the core library, which is unit-tested (tests/
    // test_gpu_policy.cpp) because CEF cannot be downloaded or run in CI. The
    // mode comes from AURORA_GPU: hardware (default, WebGL on with SwiftShader
    // as the fallback when no hardware GL context can be created), software
    // (force SwiftShader — headless machines and VMs without a GL driver), or
    // off (no GPU process at all, for debugging driver crashes).
    void OnBeforeCommandLineProcessing(
        const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line) override {
        // Chromium's first-run experience does not belong to an embedder.
        // Without this, a fresh profile opened Chromium's own first-run dialog
        // ("This Space Intentionally Blank" — the placeholder where Google
        // Chrome's terms of service would go) in front of Aurora's window, and
        // the browser sat there until the user clicked Accept. It also
        // suppresses first-run default-browser prompts and metrics opt-ins.
        command_line->AppendSwitch("no-first-run");
        // Aurora has no sign-in or sync UI, so the profile picker and account
        // consent surfaces have nothing to act on.
        command_line->AppendSwitch("disable-sync");
#if defined(__linux__)
        // Windowed browsers are parented to the Qt widget's X11 window id
        // (CefWindowInfo::SetAsChild on Linux takes an X11 Window), so
        // Chromium's Ozone layer must use X11 even inside a Wayland session.
        // main.cpp makes Qt use xcb (XWayland) for the same reason. An explicit
        // --ozone-platform from the user is respected.
        if (process_type.empty() && !command_line->HasSwitch("ozone-platform")) {
            command_line->AppendSwitchWithValue("ozone-platform", "x11");
        }
#endif
#if defined(__APPLE__)
        // Keep Chromium from prompting for (or writing to) the login keychain
        // for cookie encryption on every launch of an unsigned/ad-hoc build;
        // Aurora's profile lives in the user's Application Support directory.
        command_line->AppendSwitch("use-mock-keychain");
#endif

        // --- Extension support ---

        // JavaScript must be enabled for extension popups to work. CEF enables
        // JS by default, but these switches make it explicit and enable the
        // latest Harmony (ES6+) features that many extension popups rely on.
        command_line->AppendSwitch("enable-javascript-harmony-shipping");
        // Enable the Chromium Extensions system explicitly. Without this
        // switch, CEF may not fully initialize the extension subsystem that
        // content scripts, chrome.* APIs, and extension pages depend on.
        // Browser process only — child processes inherit it.
        if (process_type.empty()) {
            command_line->AppendSwitch("enable-extensions");
        }
        // Enable the Chromium Extensions feature flag.
        command_line->AppendSwitch("enable-features=Extensions");
        // Disable Chrome's internal extension blocklist that blocks certain
        // extension IDs (e.g. ad blockers, developer tools) which CEF users
        // have intentionally installed. Note: the feature name is
        // "ExtensionBlocklist" (singular, no 's').
        command_line->AppendSwitch("disable-features=ExtensionBlocklist");
        // Relax extension file-access checks and HTTP throttling that CEF
        // inherits from Chrome, which can prevent legitimately installed
        // unpacked extensions from running.
        command_line->AppendSwitch("disable-extensions-file-access-check");
        command_line->AppendSwitch("disable-extensions-http-throttling");
        // Allow extensions' content scripts to access file:// URLs. Many
        // extensions (ad blockers, userscripts) inject content scripts that
        // need this permission to modify page behavior.
        command_line->AppendSwitch("allow-file-access-from-files");

        // Suppress the "set as default browser" and default-apps prompts that
        // Chromium shows on launch and after extension installs.
        command_line->AppendSwitch("disable-default-apps");
        command_line->AppendSwitch("disable-default-browser-check");
        command_line->AppendSwitch("no-default-browser-check");

        const GpuMode mode = gpu_mode_from_environment();
        for (const auto& sw : gpu_command_line_switches(mode)) {
            // Switches arrive as "name" or "name=value".
            const auto eq = sw.find('=');
            if (eq == std::string::npos) {
                command_line->AppendSwitch(sw);
            } else {
                command_line->AppendSwitchWithValue(sw.substr(0, eq),
                                                    sw.substr(eq + 1));
            }
        }
        AURORA_LOG(LogLevel::Info, "cef")
            << "gpu mode: " << gpu_mode_name(mode)
            << " (webgl " << (webgl_available(mode) ? "enabled" : "disabled")
            << ")";

        // Twitch's live and VOD players start playback without a separate
        // browser-chrome click. CEF has no per-origin autoplay startup policy,
        // so apply the Chromium media policy to all web content. The regular
        // mic/camera permission prompts remain unchanged.
        for (const auto& sw : media_command_line_switches()) {
            const auto eq = sw.find('=');
            if (eq == std::string::npos) {
                command_line->AppendSwitch(sw);
            } else {
                command_line->AppendSwitchWithValue(sw.substr(0, eq),
                                                    sw.substr(eq + 1));
            }
        }

        // Extensions are loaded by the browser process only; the switch is
        // inherited by the child processes that need it. The value is built
        // by extensions::build_load_list() (core, unit-tested in
        // tests/test_extension_load_list.cpp): a comma is the switch's
        // separator, so a path containing one cannot be expressed and is
        // skipped rather than corrupting the list. main.cpp has already
        // dropped directories that no longer exist; this pass is the
        // filesystem-free safety net.
        if (!g_extension_dirs.empty() && process_type.empty()) {
            const auto list =
                extensions::build_load_list(g_extension_dirs, nullptr);
            for (const auto& skipped : list.skipped) {
                AURORA_LOG(LogLevel::Warn, "cef")
                    << "not loading extension " << skipped.path << ": "
                    << extensions::load_skip_reason_name(skipped.reason);
            }
            const std::string joined = list.switch_value();
            if (!joined.empty()) {
                command_line->AppendSwitchWithValue(kLoadExtensionSwitch, joined);
                AURORA_LOG(LogLevel::Info, "cef")
                    << "loading unpacked extensions: " << joined;
            }
        }
    }

private:
    IMPLEMENT_REFCOUNTING(AuroraApp);
};

} // namespace

bool initialize(std::string_view cache_path,
                const std::vector<std::string>& extension_dirs,
                std::string_view cef_resource_dir) {
    g_extension_dirs = extension_dirs;
#if defined(AURORA_CEF_CODEC_ARGS_ENABLED)
    AURORA_LOG(LogLevel::Info, "cef")
        << "CEF build records Chrome FFmpeg + proprietary codecs; "
           "verify H.264/AAC playback on this platform";
#else
    AURORA_LOG(LogLevel::Warn, "cef")
        << "CEF H.264/AAC support is unverified; stock CEF may lack "
           "the decoders (see docs/twitch-playback.md)";
#endif
    CefMainArgs main_args;
#if defined(__APPLE__)
    // NSApp must implement CefAppProtocol before CefInitialize, and must exist
    // before QApplication so Qt adopts it instead of creating QNSApplication.
    mac_prepare_application();
    if (!mac_application_is_cef_compatible()) {
        AURORA_LOG(LogLevel::Warn, "cef")
            << "NSApp does not implement CefAppProtocol; event handling may misbehave";
    }
#endif
    CefSettings settings;
    settings.multi_threaded_message_loop = false;  // We pump from Qt.
    settings.no_sandbox = true;  // Not linking cef_sandbox.lib; the sandbox
                                // requires its own static lib + process setup
                                // (tracked separately; see docs/security.md).

    // Persistent profile storage. cache_path is where Chromium keeps the
    // profile's Cookies, Local Storage and cache databases, so setting it is
    // what makes a login survive a restart; root_cache_path is the parent CEF
    // requires to be set explicitly when the cache lives outside its default
    // location. persist_session_cookies keeps session cookies across restarts,
    // which matches Aurora restoring the previous session's tabs — without it,
    // restored tabs came back logged out.
    CefString(&settings.cache_path) = std::string(cache_path);
    CefString(&settings.root_cache_path) = std::string(cache_path);
    settings.persist_session_cookies = 1;
    AURORA_LOG(LogLevel::Info, "cef") << "profile storage: " << cache_path;

    // Keep the platform portion of CEF's native UA, but advertise the exact
    // Chromium product version shipped with this build. No pinned browser
    // version or Twitch-only HTTP-header override: navigator.userAgent and
    // network requests must agree after redirects and across player frames.
    CefString(&settings.user_agent_product) = chrome_user_agent_product(
        CHROME_VERSION_MAJOR, CHROME_VERSION_MINOR,
        CHROME_VERSION_BUILD, CHROME_VERSION_PATCH);

    // Version-locked resource files. In a packaged build these do NOT sit next
    // to the executable: the bootstrap launcher materializes them into
    // %LOCALAPPDATA%\Aurora\cef\<build_id>\ so that a CEF update cannot leave
    // the previous engine's .pak files in place (see core/CefLayout.hpp).
    // Chromium refuses to start when the resource bundle and the engine
    // disagree, and it does not say so clearly, which is why the paths are made
    // explicit here instead of relying on "whatever is next to the exe".
    // CEF propagates these to its own subprocesses on their command lines, so
    // renderers and the GPU process inherit them.
#if defined(__APPLE__)
    // On macOS the resources live inside Chromium Embedded Framework.framework
    // (Contents/Frameworks/.../Resources) and CEF finds them on its own;
    // overriding the paths with the executable directory breaks startup.
    (void)cef_resource_dir;
#else
    if (!cef_resource_dir.empty()) {
        const std::string resources(cef_resource_dir);
        CefString(&settings.resources_dir_path) = resources;
        CefString(&settings.locales_dir_path) = resources + "/locales";
        AURORA_LOG(LogLevel::Info, "cef")
            << "cef resources: " << resources;
    }
#endif

    // Windowed (not off-screen) rendering with GPU compositing: CEF only
    // enables the accelerated compositor — and therefore WebGL — when the
    // browser is not forced into software-only OSR mode.
    settings.windowless_rendering_enabled = false;
    // Chromium's own GPU/SwiftShader diagnostics are worth keeping in the log
    // when the user reports "WebGL is not available".
    // log_severity is a cef_log_severity_t enum, not a CefString.
    settings.log_severity = LOGSEVERITY_WARNING;

    auto app = CefRefPtr<AuroraApp>(new AuroraApp);
    if (!CefInitialize(main_args, settings, app, nullptr)) {
        AURORA_LOG(LogLevel::Error, "cef") << "CefInitialize failed";
        return false;
    }
    return true;
}

void run_message_loop() {
    CefRunMessageLoop();
}

void pump_message_loop() {
    CefDoMessageLoopWork();
}

void shutdown() {
    CefShutdown();
}

bool load_library(bool helper) {
#if defined(__APPLE__)
    // Static: the framework must stay loaded until the process exits, after
    // CefShutdown. CefScopedLibraryLoader unloads it in its destructor.
    static CefScopedLibraryLoader loader;
    static bool loaded = helper ? loader.LoadInHelper() : loader.LoadInMain();
    if (!loaded) {
        AURORA_LOG(LogLevel::Error, "cef")
            << "could not load Chromium Embedded Framework.framework";
    }
    return loaded;
#else
    (void)helper;
    return true;
#endif
}

int run_subprocess(int argc, char** argv) {
#if defined(OS_WIN)
    CefMainArgs main_args;
#else
    CefMainArgs main_args(argc, argv);
#endif
    auto app = CefRefPtr<AuroraApp>(new AuroraApp);
    return CefExecuteProcess(main_args, app, nullptr);
}

std::unique_ptr<webview::WebViewController> make_webview_controller() {
    return std::make_unique<CefAdapter>();
}

} // namespace aurora::cef

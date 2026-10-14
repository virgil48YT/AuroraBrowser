#include "cef/CefLifecycle.hpp"

#include "cef/CefAdapter.hpp"
#include "cef/GpuPolicy.hpp"
#include "cef/MediaPolicy.hpp"
#include "util/Log.hpp"

// CEF headers. These come from the CEF binary distribution pointed to by
// CEF_ROOT at configure time (see cmake/FindCEF.cmake).
#include "include/cef_app.h"
#include "include/cef_version.h"

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
constexpr char kLoadExtensionSwitch[] = "load-extension";

// CefApp implementation shared by the browser process and all subprocesses.
// Process-type detection (CefBrowserProcessHandler vs CefRenderProcessHandler)
// happens here so the main executable stays thin.
class AuroraApp : public CefApp,
                  public CefBrowserProcessHandler {
public:
    AuroraApp() = default;

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
        // inherited by the child processes that need it.
        if (!g_extension_dirs.empty() && process_type.empty()) {
            std::string joined;
            for (const auto& dir : g_extension_dirs) {
                // A comma is the switch's separator, so a path containing one
                // cannot be expressed; skip it rather than corrupt the list.
                if (dir.find(',') != std::string::npos) {
                    AURORA_LOG(LogLevel::Warn, "cef")
                        << "skipping extension path with a comma: " << dir;
                    continue;
                }
                if (!joined.empty()) joined.push_back(',');
                joined += dir;
            }
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
    if (!cef_resource_dir.empty()) {
        const std::string resources(cef_resource_dir);
        CefString(&settings.resources_dir_path) = resources;
        CefString(&settings.locales_dir_path) = resources + "/locales";
        AURORA_LOG(LogLevel::Info, "cef")
            << "cef resources: " << resources;
    }

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

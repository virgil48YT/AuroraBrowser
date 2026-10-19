// CefRuntime.hpp — CEF-free facade over the CEF lifecycle and web-view factory.
//
// The browser application (Qt TUs) must NOT include CEF headers: Qt6 forces
// /permissive- and windows.h's min/max macros, both of which break CEF's
// headers. This header declares only C-free entry points (plain ints, string_view,
// and the abstract WebViewController), so app TUs depend on the interface alone.
// The implementation lives in aurora_cef (compiled with /permissive, no Qt).
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::webview { class WebViewController; }
namespace aurora::settings { class Settings; }
namespace aurora::privacy { class CertificateAudit; }

namespace aurora::cef {

// Initialize CEF for the browser process. `cache_path` is where CEF stores
// cookies, HTTP cache, service-worker state. `extension_dirs` are unpacked
// Chrome-extension directories Chromium should load at engine startup.
// `cef_resource_dir` is the directory that holds the version-locked CEF data
// files (*.pak, icudtl.dat, the V8 snapshots, locales/). Pass it when the
// resources live somewhere other than next to the executable — which is the
// normal case for a packaged build, where the bootstrap launcher materializes
// them into %LOCALAPPDATA%/Aurora/cef/<build_id>/ (see core/CefLayout.hpp).
// An empty value leaves CEF's own defaults in place.
// Returns false on failure.
bool initialize(std::string_view cache_path,
                const std::vector<std::string>& extension_dirs = {},
                std::string_view cef_resource_dir = {});

// Drive CEF's message loop from the UI thread (single-threaded embedding).
void pump_message_loop();

// Tear down CEF. Call once at process exit, after the Qt event loop ends.
void shutdown();

// macOS: load "Chromium Embedded Framework.framework" from the app bundle.
// On macOS libcef is not linked; it must be loaded at runtime before any other
// CEF call, from the main executable (`helper == false`) or from one of the
// "Aurora Helper*.app" subprocess executables (`helper == true`). The loaded
// library stays loaded until process exit. Returns false if the framework is
// missing (a broken bundle). Always returns true on Windows and Linux, where
// libcef is linked normally.
bool load_library(bool helper);

// Dispatch a CEF subprocess (renderer/GPU/utility). Returns >= 0 for a
// subprocess (which should then exit), -1 for the browser process.
int run_subprocess(int argc, char** argv);

// Construct the concrete web-view controller (a CefAdapter). Returns null if
// CEF is not available at runtime.
std::unique_ptr<aurora::webview::WebViewController> make_webview_controller();

// Borrowed profile objects; configure/refresh on the UI thread only. Request
// callbacks read atomic snapshots, never Settings or SQLite on CEF's IO thread.
void configure_privacy_preferences(settings::Settings* prefs,
                                   privacy::CertificateAudit* audit);
void refresh_privacy_preferences();

// Delete and flush the regular profile's CEF cookies before CEF shutdown.
// Call only after a normal window's browsers have drained, never in incognito.
// Returns false on failure/timeout rather than claiming an incomplete clear.
bool clear_profile_cookies();

} // namespace aurora::cef

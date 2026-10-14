// Aurora — CEF lifecycle.
//
// Owns CEF initialization and shutdown for the browser process, and the
// CefApp implementation that every CEF subprocess shares. This is the only
// translation unit that touches CefApp / CefSettings, so the rest of the
// browser never has to reason about the CEF message loop.
//
// Honesty note: this file is only compiled when AURORA_ENABLE_CEF is on and
// CEF_ROOT is set. It is written against the CEF C++ API and is intended to be
// built on a developer machine with the CEF binary distribution installed
// (run scripts/fetch_cef.sh). It is not unit-tested in CI because CEF cannot
// run headless in the sandbox; it is integration-tested on a desktop.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aurora::cef {

// Initialize CEF for the browser process. Must be called before any web view
// is created. `cache_path` is the profile's CEF cache directory (cookies,
// HTTP cache, service workers). `extension_dirs` are unpacked Chrome-extension
// directories to load; Chromium reads them while the engine starts, which is
// why they have to be known here and why installing or disabling an extension
// only takes effect on the next launch. Returns false if CEF failed to
// initialize — the application should show a recovery dialog and exit.
// (The default for `extension_dirs` lives on the CefRuntime.hpp facade that
// callers use; declaring it in both headers would be ill-formed.)
// `cef_resource_dir`, when non-empty, is the directory holding the CEF data
// files for this exact build (resources_dir_path / locales_dir_path). Aurora
// extracts them per build ID so a CEF update can never leave a .pak from the
// previous engine in place; see core/CefLayout.hpp.
bool initialize(std::string_view cache_path,
                const std::vector<std::string>& extension_dirs,
                std::string_view cef_resource_dir);

// Pump the CEF message loop. The browser's UI thread calls this instead of a
// native event loop, or integrates it into the Qt event loop via a timer.
void run_message_loop();

// Pump the CEF message loop once. Call this from the UI thread on a timer
// (or a native-event filter) so CEF processes rendering and input while Qt
// owns the main event loop. Required because we set
// multi_threaded_message_loop = false.
void pump_message_loop();

// Shut down CEF. After this returns, no CEF API may be called.
void shutdown();

// Run the CEF subprocess (renderer, GPU, etc.). Called from a thin
// subprocess executable's main(). Returns the process exit code.
int run_subprocess(int argc, char** argv);

} // namespace aurora::cef

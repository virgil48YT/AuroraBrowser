// macOS CEF helper executable ("Aurora Helper.app", "Aurora Helper (GPU).app",
// "Aurora Helper (Renderer).app", ...). Chromium starts every subprocess from
// one of these bundles inside Aurora.app/Contents/Frameworks. Each helper loads
// the framework from ../../.. relative to itself and runs the shared CefApp so
// renderer-side handlers (e.g. the per-tab memory query) behave exactly as on
// Windows and Linux, where the browser executable doubles as the subprocess.
#include "cef/CefRuntime.hpp"

int main(int argc, char** argv) {
    // The seatbelt sandbox has to be entered before the framework is loaded:
    // cef::initialize_helper_sandbox() wraps CefScopedSandboxContext, which
    // loads libcef_sandbox.dylib from the framework's Libraries directory and
    // calls cef_sandbox_initialize(). The context stays alive until exit.
    if (!aurora::cef::initialize_helper_sandbox(argc, argv)) return 1;
    if (!aurora::cef::load_library(/*helper=*/true)) return 1;
    return aurora::cef::run_subprocess(argc, argv);
}

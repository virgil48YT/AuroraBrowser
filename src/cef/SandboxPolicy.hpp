// Aurora — Chromium sandbox policy.
//
// Decides, per platform, whether CefSettings.no_sandbox may stay false (the
// Chromium sandbox is on) and, when it cannot, why. The sandbox is the single
// most important defence a browser has against a compromised renderer, so the
// policy is: ON wherever the platform can provide it, OFF only when Chromium
// itself would refuse to start (it aborts with "No usable sandbox!" on Linux
// rather than running unsandboxed), and never silently.
//
// What each platform needs (CEF M138+; see docs/security.md):
//
//   * Windows — the sandbox lives in the main *executable*: its interception
//     hooks must run in the process image, not in a DLL. Since M138 CEF no
//     longer ships cef_sandbox.lib (it can only be linked by code built with
//     Chromium's own clang/libc++), and instead ships bootstrap.exe: a
//     prebuilt, sandbox-linked launcher that loads "<exe name>.dll" and calls
//     its exported RunWinMain(..., void* sandbox_info, ...). Aurora's
//     browser is built as aurora_browser.dll and CEF's bootstrap.exe is
//     copied in as aurora_browser.exe (src/app/CMakeLists.txt). A non-null
//     sandbox_info is the proof the sandbox is available.
//   * macOS — the browser process needs nothing; each helper executable must
//     call cef_sandbox_initialize() (CefScopedSandboxContext, which loads
//     libcef_sandbox.dylib from the framework) before loading the framework.
//     See src/app/mac_helper_main.cpp. Always available.
//   * Linux — Chromium uses the setuid chrome-sandbox helper when it is
//     installed root-owned with mode 4755, else the user-namespace sandbox.
//     Neither works when the browser runs as root, and the namespace sandbox
//     is unavailable when unprivileged user namespaces are disabled (sysctl)
//     or restricted by AppArmor (Ubuntu 23.10+ without a profile). An
//     AppImage can never carry a setuid binary (it is mounted nosuid), so it
//     depends on user namespaces.
//
// Pure logic, no CEF headers: the probes (euid, helper stat, namespace test)
// are done by CefLifecycle and fed in, and the decision table is unit-tested
// in tests/test_sandbox_policy.cpp.
#pragma once

#include <string>
#include <string_view>

namespace aurora::cef {

enum class SandboxPlatform { Windows, MacOS, Linux };

struct SandboxProbe {
    SandboxPlatform platform = SandboxPlatform::Linux;
    // Windows: the sandbox_info pointer handed over by bootstrap.exe was
    // non-null (the executable is the sandbox-linked CEF bootstrap).
    bool windows_sandbox_info = false;
    // Linux: the process runs with effective uid 0.
    bool running_as_root = false;
    // Linux: chrome-sandbox sits next to the executable, owned by root with
    // the setuid bit and mode 4755.
    bool suid_helper_usable = false;
    // Linux: a child could create an unprivileged user namespace.
    bool user_namespaces_usable = false;
    // AURORA_REQUIRE_SANDBOX is set: refuse to start rather than fall back.
    bool require_sandbox = false;
    // AURORA_DISABLE_SANDBOX is set: the developer explicitly opted out
    // (debugging a renderer under gdb, for example). Logged loudly.
    bool developer_opt_out = false;
};

struct SandboxDecision {
    bool sandbox_enabled = false;  // CefSettings.no_sandbox = !sandbox_enabled
    bool refuse_to_start = false;  // require_sandbox was set and it is unavailable
    std::string reason;            // human-readable, logged at startup
};

SandboxDecision decide_sandbox(const SandboxProbe& probe);

// True for "1", "true", "yes", "on" (case-insensitive) — the spellings the
// AURORA_REQUIRE_SANDBOX / AURORA_DISABLE_SANDBOX variables accept.
bool env_flag_enabled(std::string_view value);

} // namespace aurora::cef

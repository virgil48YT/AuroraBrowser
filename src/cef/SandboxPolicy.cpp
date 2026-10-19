// Aurora — Chromium sandbox policy. See SandboxPolicy.hpp.
#include "cef/SandboxPolicy.hpp"

#include <cctype>

namespace aurora::cef {

bool env_flag_enabled(std::string_view value) {
    std::string v;
    v.reserve(value.size());
    for (const char c : value)
        v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

SandboxDecision decide_sandbox(const SandboxProbe& probe) {
    SandboxDecision d;
    if (probe.developer_opt_out) {
        // An explicit opt-out wins over everything except "require": both set
        // at once is a contradiction, and the safe reading is "require".
        if (probe.require_sandbox) {
            d.refuse_to_start = true;
            d.reason = "AURORA_DISABLE_SANDBOX and AURORA_REQUIRE_SANDBOX are both set";
            return d;
        }
        d.reason = "sandbox disabled by AURORA_DISABLE_SANDBOX (developer override)";
        return d;
    }

    switch (probe.platform) {
    case SandboxPlatform::MacOS:
        // The helpers initialize the seatbelt sandbox themselves before the
        // framework loads (mac_helper_main.cpp); nothing to probe.
        d.sandbox_enabled = true;
        d.reason = "macOS seatbelt sandbox (helpers call cef_sandbox_initialize)";
        return d;

    case SandboxPlatform::Windows:
        if (probe.windows_sandbox_info) {
            d.sandbox_enabled = true;
            d.reason = "Windows sandbox via CEF bootstrap.exe";
            return d;
        }
        d.reason =
            "Windows build is not running under CEF's bootstrap.exe "
            "(no sandbox_info); built without AURORA_WIN_SANDBOX";
        break;

    case SandboxPlatform::Linux:
        if (probe.running_as_root) {
            // Chromium refuses to sandbox a root browser process at all.
            d.reason = "running as root: Chromium cannot sandbox a root process";
            break;
        }
        if (probe.suid_helper_usable) {
            d.sandbox_enabled = true;
            d.reason = "Linux setuid sandbox (chrome-sandbox helper)";
            return d;
        }
        if (probe.user_namespaces_usable) {
            d.sandbox_enabled = true;
            d.reason = "Linux user-namespace sandbox";
            return d;
        }
        d.reason =
            "no usable Linux sandbox: chrome-sandbox is not setuid root and "
            "unprivileged user namespaces are disabled or restricted "
            "(kernel.unprivileged_userns_clone / AppArmor "
            "apparmor_restrict_unprivileged_userns)";
        break;
    }

    if (probe.require_sandbox) d.refuse_to_start = true;
    return d;
}

} // namespace aurora::cef

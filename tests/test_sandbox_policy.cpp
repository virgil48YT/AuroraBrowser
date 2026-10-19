// The sandbox decision table. CEF cannot run in CI, so the per-platform rules
// CefLifecycle applies to CefSettings.no_sandbox are pinned here.
#include <doctest.h>

#include "cef/SandboxPolicy.hpp"

using namespace aurora::cef;

TEST_CASE("sandbox: macOS is always sandboxed") {
    SandboxProbe p;
    p.platform = SandboxPlatform::MacOS;
    const auto d = decide_sandbox(p);
    CHECK(d.sandbox_enabled);
    CHECK_FALSE(d.refuse_to_start);
}

TEST_CASE("sandbox: Windows needs bootstrap sandbox_info") {
    SandboxProbe p;
    p.platform = SandboxPlatform::Windows;
    CHECK_FALSE(decide_sandbox(p).sandbox_enabled);
    p.windows_sandbox_info = true;
    CHECK(decide_sandbox(p).sandbox_enabled);
    p.windows_sandbox_info = false;
    p.require_sandbox = true;
    CHECK(decide_sandbox(p).refuse_to_start);
}

TEST_CASE("sandbox: Linux prefers setuid, then user namespaces") {
    SandboxProbe p;
    p.platform = SandboxPlatform::Linux;
    p.suid_helper_usable = true;
    CHECK(decide_sandbox(p).sandbox_enabled);
    p.suid_helper_usable = false;
    p.user_namespaces_usable = true;
    CHECK(decide_sandbox(p).sandbox_enabled);
    p.user_namespaces_usable = false;
    const auto none = decide_sandbox(p);
    CHECK_FALSE(none.sandbox_enabled);
    CHECK_FALSE(none.refuse_to_start);
    CHECK_FALSE(none.reason.empty());
}

TEST_CASE("sandbox: Linux root can never be sandboxed") {
    SandboxProbe p;
    p.platform = SandboxPlatform::Linux;
    p.running_as_root = true;
    p.suid_helper_usable = true;
    p.user_namespaces_usable = true;
    CHECK_FALSE(decide_sandbox(p).sandbox_enabled);
    p.require_sandbox = true;
    CHECK(decide_sandbox(p).refuse_to_start);
}

TEST_CASE("sandbox: developer opt-out and require") {
    SandboxProbe p;
    p.platform = SandboxPlatform::MacOS;
    p.developer_opt_out = true;
    CHECK_FALSE(decide_sandbox(p).sandbox_enabled);
    CHECK_FALSE(decide_sandbox(p).refuse_to_start);
    p.require_sandbox = true;
    CHECK(decide_sandbox(p).refuse_to_start);
}

TEST_CASE("sandbox: env flag spellings") {
    CHECK(env_flag_enabled("1"));
    CHECK(env_flag_enabled("TRUE"));
    CHECK(env_flag_enabled("yes"));
    CHECK(env_flag_enabled("On"));
    CHECK_FALSE(env_flag_enabled(""));
    CHECK_FALSE(env_flag_enabled("0"));
    CHECK_FALSE(env_flag_enabled("off"));
}

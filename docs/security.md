# Aurora — Security model

Aurora does not weaken Chromium's security model for convenience. This
document records the security-relevant decisions and the boundaries that must
not be crossed.

## Process isolation

Aurora preserves CEF's multi-process architecture: the browser process is
separated from renderer processes, the GPU process, and the network service.
Site isolation (per-origin renderer processes) is left at CEF defaults.

### Chromium sandbox

The sandbox is **on by default on every platform**. `cef::initialize` derives
`CefSettings.no_sandbox` from `cef::decide_sandbox()` (`src/cef/SandboxPolicy.*`,
unit-tested in `tests/test_sandbox_policy.cpp`) and logs the outcome as
`chromium sandbox ENABLED|DISABLED: <reason>` at startup.

| Platform | How it is enabled | What remains |
|---|---|---|
| Windows | CEF M138+ no longer ships a `cef_sandbox.lib` linkable by MSVC (it needs Chromium's clang/libc++). Aurora uses CEF's prebuilt, sandbox-linked `bootstrap.exe`: the browser is built as `aurora_browser.dll` exporting `RunWinMain`, and `bootstrap.exe` is copied in as `aurora_browser.exe` (`AURORA_WIN_SANDBOX`, default ON when the distribution has `bootstrap.exe`). The `sandbox_info` it passes is forwarded to both `CefExecuteProcess` and `CefInitialize`. The browser grants `ALL RESTRICTED APPLICATION PACKAGES` (S-1-15-2-2) read/execute on its install and CEF resource directories on first run, which the LPAC-sandboxed network service needs. | The bootstrap is CEF's unsigned binary with Aurora's icon patched in; it must be code-signed together with `aurora_browser.dll` for distribution (CEF verifies that the DLL's signing status matches the executable's). A build with `-DAURORA_WIN_SANDBOX=OFF` (or the Qt-only shell) is a plain executable and runs unsandboxed, with an Error-level log line. |
| macOS | Each `Aurora Helper*.app` calls `cef::initialize_helper_sandbox()` (`CefScopedSandboxContext`, which loads `libcef_sandbox.dylib` from the framework) before loading the framework. The browser process runs with `no_sandbox = false`. No extra entitlement is needed for Chromium's seatbelt sandbox; `com.apple.security.app-sandbox` must **not** be added (it is incompatible with it). | Nothing known. |
| Linux | `no_sandbox = false` when `chrome-sandbox` next to the executable is root-owned 4755 (setuid sandbox) or when a probe child can create an unprivileged user namespace and write its `uid_map` (namespace sandbox). `chrome-sandbox` is shipped (not setuid) in the tarball and AppImage. | When neither is available — running as root, `kernel.unprivileged_userns_clone=0`, or Ubuntu 23.10+'s AppArmor `apparmor_restrict_unprivileged_userns` without an Aurora AppArmor profile — Chromium would abort with "No usable sandbox!", so Aurora starts **unsandboxed** with an Error-level log line. Fix on the host: `sudo chown root:root chrome-sandbox && sudo chmod 4755 chrome-sandbox` in an extracted tarball (impossible inside an AppImage, whose FUSE mount is nosuid), or allow user namespaces / install an AppArmor profile granting `userns`. |

Environment overrides: `AURORA_REQUIRE_SANDBOX=1` makes startup fail instead
of falling back; `AURORA_DISABLE_SANDBOX=1` is a developer-only opt-out (for
example to attach a debugger to a renderer) and is logged loudly. The Android
edition uses the system WebView, whose renderer is sandboxed by Android.

## TLS / certificate handling

Aurora relies on CEF/Chromium for TLS, certificate verification, and
certificate-error presentation. The browser does not override certificate
verification or silently accept broken certs. When CEF reports a certificate
error, Aurora surfaces a full-page error (the same recovery-oriented page used
for network and DNS errors) rather than proceeding.

## Permissions

Camera, microphone, notifications, clipboard, geolocation, and similar
permissions are mediated by CEF's permission handlers. Aurora shows a
permission prompt per origin and stores decisions per-site in the preferences
table. The default for sensitive permissions is **deny**.

## Downloads

Dangerous downloads (executables, mismatched extensions, origins with a bad
reputation) are flagged through CEF's download safety checks. The user must
explicitly confirm keeping a dangerous file. Download state is persisted but
the target path is never auto-executed.

## Cookies, storage, and private browsing

Cookies, local storage, and service workers live in the per-profile CEF cache
(`cache_path` passed to `cef::initialize`). Clearing browsing data deletes the
relevant CEF-managed data. Incognito/private windows use a separate, in-memory
CEF request context (empty `cache_path`) whose only reference dies with the
window: persistent writes are suppressed per `IncognitoPolicy`,
cross-privacy-mode tab transfers are rejected, and internal pages are
read-only in private windows. Opt-in clear-on-exit covers CEF cookies,
history, and download metadata.

## Data at rest

The profile database (`profile.db`) holds history, bookmarks, downloads, and
preferences. It is **not encrypted**. Aurora does not claim disk encryption it
does not provide; users who need at-rest protection should use full-disk
encryption (FileVault, BitLocker, LUKS), which Aurora does not interfere with.

## Logging hygiene

The logger never writes cookies, form input, POST bodies, or auth tokens.
Navigation URLs are logged only at `Debug` level (off by default in release).
Log level is controlled by `AURORA_LOG_LEVEL`.

## Release-build guarantees

Security-sensitive toggles (cert verification off, mixed content allowed) are
compile-time defaults that must be `false` in release. The sandbox is decided
at runtime as described above and never disabled silently. Any
temporary dev override is isolated behind `#ifndef NDEBUG`-style guards and
documented here. See the honesty notes in `src/cef/CefLifecycle.hpp`.

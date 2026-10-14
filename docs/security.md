# Aurora — Security model

Aurora does not weaken Chromium's security model for convenience. This
document records the security-relevant decisions and the boundaries that must
not be crossed.

## Process isolation

Aurora preserves CEF's multi-process architecture: the browser process is
separated from renderer processes, the GPU process, and the network service.
Site isolation (per-origin renderer processes) is left at CEF defaults.
**The sandbox is never disabled.** `cef::initialize` sets `no_sandbox = false`
explicitly; if a future change needs `no_sandbox = true` for debugging, it must
be guarded so release builds cannot ship with it disabled.

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
relevant CEF-managed data. Incognito/private windows will use a separate,
in-memory CEF cache with no persistence — **this is architecture-ready but not
yet wired**; until it is, the privacy controls reflect what is actually
implemented.

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

Security-sensitive toggles (sandbox off, cert verification off, mixed content
allowed) are compile-time defaults that must be `false` in release. Any
temporary dev override is isolated behind `#ifndef NDEBUG`-style guards and
documented here. See the honesty notes in `src/cef/CefLifecycle.hpp`.

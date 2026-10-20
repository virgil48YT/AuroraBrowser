# Aurora Browser for Android

The Android edition of Aurora: a privacy-focused browser with built-in ad &
tracker blocking, incognito tabs and the Aurora look, written in Kotlin.
It lives entirely in this `android/` directory; the desktop C++/Qt6 code in
`src/` is untouched.

## Why the system WebView (and not CEF or a custom Chromium)

- **CEF does not support Android.** The Chromium Embedded Framework only
  ships Windows, macOS and Linux builds, so the desktop engine layer
  (`src/cef/`) cannot be reused on phones. CEF's own maintainers point
  Android developers to the system WebView instead.
- **Building Chromium for Android (as Brave does) is not feasible in CI.**
  A full Chromium Android build needs a ~100 GB checkout, many hours of
  compile time on large machines, and its own toolchain; that does not fit a
  standard GitHub Actions runner.
- **Android System WebView *is* Chromium.** It is updated through the Play
  Store independently of the OS, so pages render with a current Blink/V8
  engine — the same engine family as the desktop build.

Aurora's own features (tabs, omnibox, Shields, incognito, history, bookmarks,
downloads, settings, start page) are implemented in Kotlin on top of the
WebView.

## Features

| Feature | Android implementation |
|---|---|
| Multi-tab browsing | `TabManager` + one lazily created `WebView` per tab; Chrome-style grid tab switcher with thumbnails, swipe to close, separate incognito page, reopen closed tab |
| Omnibox | URL vs. search detection ported from `src/core/Url.cpp`; suggestions from local history and bookmarks (nothing leaves the device) |
| Search engines | DuckDuckGo (default), Google, Bing, Startpage, Brave Search, Ecosia, plus user-added `https://…%s` templates, validated like `src/searchengines` |
| Ad & tracker blocking | EasyList / Adblock Plus parser (`adblock/FilterParser.kt`): `||host^`, anchors, wildcards, `^`, `/regex/`, `@@` exceptions, type options, `$third-party`, `$domain=`, `##` / `#@#` element hiding. Requests are blocked in `WebViewClient.shouldInterceptRequest`. A small built-in list ships in `assets/filters/aurora-builtin.txt` (no download at build time); users can add rules in Settings |
| Shields | Per-site toggle (registrable domain), blocked-count badge per page, running total on the start page |
| Incognito tabs | No history, session, thumbnail or HTTP-cache persistence. On WebViews that support multiple profiles, incognito runs in its own profile (separate cookies/storage), which is erased when the last incognito tab closes |
| History / bookmarks | SQLite (`data/BrowserDb.kt`), searchable pages, edit/delete |
| Downloads | System `DownloadManager` (cookies and user agent forwarded), downloads page |
| Settings | Search engine, theme (system/light/dark), session restore, suggestions, Shields, custom filters, third-party cookies, HTTPS-only, JavaScript, desktop site, clear browsing data |
| Start page | `assets/brand/aurora_bg.jpg` backdrop, Aurora mark and wordmark, glowing search pill and glow cards (shortcuts, recently visited, quick actions) like the desktop home page |
| Built-in Tor | Like Brave's private windows with Tor: the tor daemon ships inside the APK (Guardian Project `tor-android`, `libtor.so` for every ABI) and runs in-process. **Settings › Tor › Browse through Tor** or **⋮ › New private tab with Tor** routes WebView traffic through tor's SOCKS5 port (`tor/TorManager.kt`); host names are resolved by Tor, so `.onion` sites work. Fail-closed: pages are held on a "Connecting to Tor…" screen until a circuit is ready. History is not saved in Tor mode, the favicon fetcher stays offline, downloads ask first (DownloadManager does not use Tor), and an onion badge in the omnibox shows status / "New Tor circuit" |
| Also | Find in page, share, desktop-site toggle, pull to refresh, fullscreen video, file uploads, external app links (`intent:`, `mailto:`, …), default-browser intent filters |

## Building

Requirements: JDK 17 and the Android SDK (platform 35, build-tools 35.0.0).
The Gradle wrapper (Gradle 8.11.1) is committed.

```bash
cd android
export ANDROID_HOME=/path/to/android-sdk     # or create local.properties with sdk.dir=...
./gradlew assembleDebug                      # -> app/build/outputs/apk/debug/app-debug.apk
./gradlew testDebugUnitTest                  # JVM tests: filter matching, omnibox, templates
./gradlew lintDebug                          # Android lint report
```

Install with `adb install app/build/outputs/apk/debug/app-debug.apk`.
The debug build uses the application id `com.aurora.browser.debug`, so it can
be installed next to a future release build.

CI: `.github/workflows/android-build.yml` runs the JVM tests and builds both
the debug APK and the release APK on every push to `main` or `mobile/android`
that touches `android/` (or on manual dispatch), uploading
`aurora-android-debug-apk` and `aurora-android-release-apk`. When the signing
secrets are available, CI also publishes the signed release APK to the rolling
[`android-ci`](https://github.com/virgil48YT/AuroraBrowser/releases/tag/android-ci)
pre-release as `Aurora-android-latest.apk` — a plain `.apk` you can install
directly. Actions artifacts download as `.zip`, which Android cannot install,
so prefer that release asset when you just want to install the app. Tagged
releases are built by `.github/workflows/release.yml`.

## Release signing

`./gradlew assembleRelease` always works. Signing is read only from the
environment — no keystore, password or alias is stored in the repository:

| Variable | Meaning |
|---|---|
| `AURORA_ANDROID_KEYSTORE` | Path to the upload/release keystore (`.jks`) |
| `AURORA_ANDROID_KEYSTORE_PASSWORD` | Keystore password |
| `AURORA_ANDROID_KEY_ALIAS` | Key alias |
| `AURORA_ANDROID_KEY_PASSWORD` | Key password |
| `AURORA_VERSION_NAME` / `AURORA_VERSION_CODE` | Optional version override (CI sets them from the tag and run number) |

With all four signing values set, the output is a signed
`app-release.apk`. Without them the output is `app-release-unsigned.apk`,
which Android refuses to install until it is signed, e.g.:

```bash
apksigner sign --ks my-release.jks --out aurora-release.apk app-release-unsigned.apk
```

In GitHub Actions the release workflow decodes the repository secret
`ANDROID_KEYSTORE_BASE64` (a base64-encoded keystore) together with
`ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD`.
Keep the keystore backed up: an app signed with a lost key can never be
updated in place.

Both Android workflows stamp `AURORA_VERSION_CODE` from the epoch (seconds),
so every APK produced by any workflow sorts newer than every older one —
a newer build always updates an older install instead of failing with
"App not installed" (a version downgrade).

Project facts: package `com.aurora.browser`, minSdk 26, targetSdk/compileSdk
35, Kotlin 2.0, AGP 8.7, Views + Material 3 (no Compose).

## Limitations

- **Incognito isolation depends on the WebView version.** Separate cookie and
  storage profiles need the WebView multi-profile feature
  (`WebViewFeature.MULTI_PROFILE`, recent WebView releases). On older
  WebViews, incognito tabs still keep no history, session, cache or
  thumbnails, but they share cookies with normal tabs; the incognito start
  page says which case applies. The profile also lives on disk while
  incognito tabs are open and is deleted when they close (and again at the
  next launch).
- **Resource types are inferred.** Android's WebView does not report a
  request's type, so `$script`, `$image`, etc. are guessed from the file
  extension and `Accept` header. Rules with type options that cannot be
  inferred are skipped rather than over-blocking.
- **Supported filter syntax.** Scriptlets (`+js()`), procedural and
  `:style()` cosmetic filters, `$redirect`, `$popup` and other rarely used
  options are ignored. Element hiding is injected when the page commits and
  finishes loading, so a placeholder can show briefly.
- **No WebExtensions** (the desktop's unpacked Chrome extensions need
  Chromium's extension system, which WebView doesn't have).
- **Tor mode is app-wide, not per tab.** WebView only supports a process-wide
  proxy override (`androidx.webkit.ProxyController`, WebView 74+), so while Tor
  is on every tab — normal and private — goes through Tor. This is Tor
  routing for a WebView browser, not the Tor Browser: there is no
  fingerprinting resistance beyond Shields, and per-site circuit isolation is
  tor's default (by SOCKS destination), not first-party isolation. The bundled
  native tor adds roughly 8–9 MB per ABI to the APK.
- Search suggestions come from local history and bookmarks only. No network
  suggestion API is used, which is deliberate for privacy.
- A release APK that can be installed needs your own signing key (not
  included; see [Release signing](#release-signing)). Without it CI still
  produces an unsigned APK, and the `android-ci` pre-release is not updated.

## Icon set

Chrome and home-page icons are rendered from **Font Awesome Free 6** (solid +
brands TTFs bundled in `res/font`), so the mobile iconography matches the
desktop build's tinted icon language and the shortcuts / recently visited rows
show real brand marks (Google, YouTube, Wikipedia, ...) with a globe fallback.

Font Awesome Free is © Fonticons, Inc., licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) (icons) and the
[SIL OFL 1.1](https://scripts.sil.org/OFL) (fonts).

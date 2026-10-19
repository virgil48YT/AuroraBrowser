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

CI: `.github/workflows/android-build.yml` builds the debug APK on every push
to `mobile/android` (or on manual dispatch) and uploads it as the
`aurora-android-debug-apk` artifact.

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
- Search suggestions come from local history and bookmarks only. No network
  suggestion API is used, which is deliberate for privacy.
- Only the debug APK is built in CI. A release build needs a signing key
  (not included).

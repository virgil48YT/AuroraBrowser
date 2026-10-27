# Aurora Browser

[![Linux core + tests](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/linux-core.yml/badge.svg?branch=main)](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/linux-core.yml)
[![Windows build](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/build-windows.yml/badge.svg?branch=main)](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/build-windows.yml)
[![macOS build](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/macos.yml)
[![Linux desktop](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/linux-desktop.yml/badge.svg?branch=main)](https://github.com/virgil48YT/AuroraBrowser/actions/workflows/linux-desktop.yml)
[![Latest release](https://img.shields.io/github/v/release/virgil48YT/AuroraBrowser?sort=semver)](https://github.com/virgil48YT/AuroraBrowser/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A privacy-focused, Chromium-based desktop browser with a fast native Qt6 UI.

Aurora is built around security and privacy by default: the Chromium
sandbox, a built-in ad and tracker blocker with per-site exceptions, and
incognito windows whose cookies, site storage, cache, history, downloads list
and session are kept in memory and discarded when the window closes. It is a
small, self-contained app — no telemetry, no background services, no vendor
accounts.

## Features

- Ad and tracker blocking with per-site allow lists; the EasyList filter
  list can be updated from Settings > Privacy & Security ("Update now" —
  manual only, never on a timer)
- Site settings page (`aurora://site-settings`) to review and clear
  remembered permissions, per-site zoom levels and Shields exceptions
- Incognito windows (Ctrl+Shift+N): cookies, local storage, cache and other
  site data live in an in-memory profile, and nothing is added to history,
  the downloads list, the session or the site-settings database. Files you
  explicitly download are of course written to disk, and the operating
  system may still page memory to swap
- Third-party cookies blocked by default (Settings > Privacy & Security),
  judged against the site in the address bar using the bundled
  [Public Suffix List](https://publicsuffix.org) (so `alice.github.io` and
  `bob.github.io`, or two `.co.uk` registrants, are different sites)
- Chromium sandbox enabled on Windows, macOS and Linux (see
  [docs/security.md](docs/security.md) for the per-platform details)
- Chrome-style layout: tabs, address bar, downloads
- History, bookmarks, downloads and settings pages
- Custom search engines (any `https://` search URL with `%s`), selectable as
  the omnibox default
- Unpacked Chrome extensions, loaded by the engine at startup (restart to
  apply changes; no Chrome Web Store installs, no CRX signature
  verification — see [docs/extensions.md](docs/extensions.md))
- Find in page (Ctrl+F), print and save as PDF (Ctrl+P / browser menu),
  fullscreen (F11). On Linux, Print uses the desktop's print dialog (the
  XDG print portal, or GTK); on a desktop with neither, use Save as PDF,
  which works everywhere
- Reader mode (Ctrl+Alt+R or the browser menu): the page's article in a
  clean, distraction-free `aurora://reader` tab with serif typography in the
  light and dark themes; text size buttons; images become their captions and
  nothing remote is loaded. Pages without a recognizable article show a
  message instead
- Screenshot tool: visible area (Ctrl+Shift+S), full page
  (Ctrl+Alt+Shift+S) or a selected region (Ctrl+Alt+S). The PNG is copied to
  the clipboard and can be saved from the preview. Full-page captures stop at
  16384 CSS pixels of height
- Night mode for page content (Ctrl+Alt+N): darkens the site you are on and
  remembers the choice per site, like zoom; Settings has a default for all
  sites. The browser chrome keeps following the normal theme
- Video right-click menu: playback speed (0.5x–2x) and picture in picture.
  Where the engine has no native PiP window for a video, the tab moves to
  Aurora's floating PiP window instead
- Share the current page as a QR code (tab right-click menu or browser
  menu), generated locally, with copy and save
- Tor incognito windows (browser menu > New Tor Incognito Window,
  Ctrl+Alt+Shift+N) — see [Tor incognito mode](#tor-incognito-mode)
- Per-site zoom that is remembered across visits and restarts
- Dark and light theme
- Opt-out update checks in Settings > Updates: Aurora asks GitHub for the
  latest release and offers a setup only when it is newer. The download is
  verified against the release's published SHA-256 before it can run
- Startup choice: continue where you left off (session restore), open the
  home page, or open a specific page
- Runs without the Chromium engine; the UI and internal pages still work

## Screenshots

<!-- TODO(screenshots): replace these placeholders with real captures of a
     release build (1280x800, dark and light theme). The files do not exist
     yet; see docs/img/README.md for what each one should show. -->

| Home page (dark) | Home page (light) |
|---|---|
| ![Aurora home page, dark theme — TODO placeholder](docs/img/home-dark.png) | ![Aurora home page, light theme — TODO placeholder](docs/img/home-light.png) |
| **Settings** | **Incognito window** |
| ![aurora://settings — TODO placeholder](docs/img/settings.png) | ![Incognito window — TODO placeholder](docs/img/incognito.png) |

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+Shift+T | Reopen closed tab |
| Ctrl+N | New window |
| Ctrl+Shift+N | New incognito window |
| Ctrl+L | Focus address bar |
| Ctrl+D (Cmd+D on macOS) | Bookmark this page |
| Ctrl+Alt+R | Reader mode |
| Ctrl+Shift+S | Screenshot of the visible area |
| Ctrl+Alt+Shift+S | Screenshot of the full page |
| Ctrl+Alt+S | Screenshot of a selected region |
| Ctrl+Alt+N | Night mode for this site |
| Ctrl+Alt+Shift+N | New Tor incognito window |
| Ctrl+F | Find in page |
| Ctrl+P | Print |
| F5 / Ctrl+R | Reload |
| Alt+Left / Alt+Right | Back / Forward |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| F11 | Toggle fullscreen |
| F12 | Developer tools |
| Ctrl+Shift+Del | Clear browsing data |

## Tor incognito mode

A Tor incognito window is an incognito window (same in-memory profile, nothing
written to history, downloads list, session or settings) whose traffic goes
through Tor. Like Brave's "New Private Window with Tor", **Tor comes with
Aurora**: release packages ship the Tor Project's Tor Expert Bundle next to the
browser (`tor/`), and Aurora starts it on demand — nothing to install.

1. Browser menu > **New Tor Incognito Window** (Ctrl+Alt+Shift+N). The first
   time, Aurora launches its built-in tor (client only, SOCKS on a free
   loopback port, state in the profile's `tor/` folder) and shows the
   bootstrap progress; the window opens once tor reports 100%. Later Tor
   windows open instantly. tor exits with Aurora (even after a crash, via
   `__OwningControllerProcess`).
2. Settings > Privacy > **Use Aurora's built-in Tor for Tor windows** turns
   the bundled tor off. Setting `AURORA_TOR_PROXY` (one run) or the
   `tor.proxy` preference also makes Tor windows use your own daemon (e.g.
   `socks5://127.0.0.1:9150` for a running Tor Browser). Without the bundle
   or with it turned off, Aurora checks that a SOCKS5 server answers at the
   proxy address (default `socks5://127.0.0.1:9050`) and explains how to
   start Tor if it does not.

Building from source: `bash scripts/fetch_tor.sh` downloads the Expert Bundle
for this platform into `third_party/tor/<platform>` (SHA-256 checked against
the release's `sha256sums-signed-build.txt`), and CMake copies it next to the
browser (`Contents/Resources/tor` on macOS). Override the release with
`TOR_BUNDLE_VERSION`, the location with `-DAURORA_TOR_DIR=...`.

The window has its own pink/purple theme and a "Tor Incognito" badge. Its
proxy is scoped to that window's private request context, so normal and
incognito windows are unaffected.

What it does: every HTTP(S)/WebSocket request of the window goes to the SOCKS5
proxy with the **host name** (Chromium resolves names through a SOCKS5 proxy,
so DNS lookups go through Tor, not your resolver). WebRTC is restricted to
proxied connections, and network prediction (DNS prefetch / preconnect) is off
for the window. If the engine cannot apply the proxy, the window refuses to
open rather than browse directly.

What it does **not** do — use [Tor Browser](https://www.torproject.org/) when
anonymity matters:

- It is not Tor Browser: there is no fingerprinting resistance (window size,
  fonts, user agent, canvas/WebGL are those of a normal Chromium), no
  first-party circuit isolation, and no security slider. Sites can still
  recognize the browser across visits within the window.
- Tor exit nodes see (and can tamper with) unencrypted traffic. Use HTTPS
  (Settings > HTTPS-only mode); the exit node still learns which sites you
  visit, but not the content of HTTPS pages.
- Logging in to an account identifies you, Tor or not.
- Some sites block or CAPTCHA Tor exit nodes.
- Downloads are fetched through Tor, but opening a downloaded file happens
  outside the browser and may connect to the internet directly.
- Extensions are disabled in all private windows, Tor included.

## Install

Download the latest release from the
[Releases](https://github.com/virgil48YT/AuroraBrowser/releases) page:

| Platform | Package |
|---|---|
| Windows 10/11 (x64) | `Aurora-VERSION-windows-x64-setup.exe` — the Aurora setup wizard installs to `C:\Program Files\Aurora Browser` by default (any folder can be chosen) and upgrades an existing per-user `%LOCALAPPDATA%\Aurora\app` install in place when it finds one |
| macOS 13.3+ (Apple silicon) | `Aurora-VERSION-macos-arm64.dmg` — drag Aurora to Applications |
| Linux (x86_64, glibc 2.35+) | `Aurora-VERSION-linux-x86_64.AppImage` (`chmod +x` and run) or the `.tar.gz` (extract, run `./aurora`) |
| Android 8.0+ | `Aurora-VERSION-android.apk`, or `Aurora-VERSION-android-unsigned.apk` (see below) |

Things to know before installing:

- **Windows:** the installer and the browser are **not code-signed**, so
  SmartScreen shows "Windows protected your PC". Choose *More info > Run
  anyway* only if you downloaded the file from this repository's Releases
  page and its SHA-256 matches the published `.sha256` file. The wizard asks
  for administrator permission only when the chosen folder needs it (the
  default Program Files location does); a folder under your user profile
  installs without elevation.
- **macOS:** the DMG is **ad-hoc signed and not notarized** unless the
  release was built with the Apple signing secrets configured, so Gatekeeper
  blocks the first launch. Right-click Aurora.app > *Open* (or *System
  Settings > Privacy & Security > Open Anyway*) once.
- **Linux AppImage:** AppImages need **FUSE 2** (`libfuse2`; on Ubuntu 22.04+
  `sudo apt install libfuse2`, on 24.04+ `libfuse2t64`). Without it, run
  `./Aurora-VERSION-linux-x86_64.AppImage --appimage-extract-and-run`, or use
  the `.tar.gz`.
- **Android:** the release workflow signs the APK only when the
  `ANDROID_KEYSTORE_*` secrets are configured; otherwise it publishes
  `Aurora-VERSION-android-unsigned.apk`. **Android refuses to install an
  unsigned APK** ("App not installed" / "package appears to be invalid"). Sign
  it yourself first (`apksigner sign --ks my.jks Aurora-VERSION-android-unsigned.apk`)
  or build a debug APK from source (see [android/README.md](android/README.md)).

How releases are built and signed: [docs/release.md](docs/release.md).

## Build from source

Requires [CMake](https://cmake.org/) and [Qt6](https://www.qt.io/).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAURORA_BUILD_TESTS=ON
cmake --build build
./build/bin/aurora_tests
```

### Full browser with CEF

The Chromium engine is needed for real web pages, WebGL and video:

```bash
./scripts/fetch_cef.sh
sudo apt-get install -y qt6-base-dev libqt6svg6-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_BUILD_TESTS=ON -DAURORA_ENABLE_CEF=ON -DAURORA_BUILD_APP=ON \
  -DCEF_ROOT=$PWD/third_party/cef
cmake --build build
./build/bin/aurora_browser
```

### macOS

```bash
brew install qt ninja
PLATFORM=macosarm64 ./scripts/fetch_cef.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_ENABLE_CEF=ON -DAURORA_BUILD_APP=ON -DCEF_ROOT=$PWD/third_party/cef \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
open build/bin/Aurora.app
```

### Android

See [android/README.md](android/README.md) (`cd android && ./gradlew assembleDebug`).

## Contributing and security

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Please
report security vulnerabilities privately as described in
[SECURITY.md](SECURITY.md), not in public issues.

## License

MIT — see [LICENSE](LICENSE). The bundled Public Suffix List
(`third_party/publicsuffix/`) is under the Mozilla Public License 2.0.

# Aurora Browser

A privacy-focused, Chromium-based desktop browser with a fast native Qt6 UI.

Aurora is built around security and privacy by default: a built-in ad and
tracker blocker with per-site exceptions, and incognito windows that keep
the session entirely in memory so nothing touches disk. It is a small,
self-contained app — no telemetry, no background services, no vendor
accounts.

## Features

- Ad and tracker blocking with per-site allow lists; the EasyList filter
  list can be updated from Settings > Privacy & Security ("Update now" —
  manual only, never on a timer)
- Site settings page (`aurora://site-settings`) to review and clear
  remembered permissions, per-site zoom levels and Shields exceptions
- Incognito windows (Ctrl+Shift+N) that leave no trace on disk
- Third-party cookies blocked by default (Settings > Privacy & Security),
  judged against the site in the address bar; site boundaries use a small
  built-in list of shared suffixes (co.uk, github.io, ...) rather than the
  full Public Suffix List
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
- Per-site zoom that is remembered across visits and restarts
- Dark and light theme
- Startup choice: continue where you left off (session restore), open the
  home page, or open a specific page
- Runs without the Chromium engine; the UI and internal pages still work

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+W | Close tab |
| Ctrl+Shift+T | Reopen closed tab |
| Ctrl+N | New window |
| Ctrl+Shift+N | New incognito window |
| Ctrl+L | Focus address bar |
| Ctrl+F | Find in page |
| Ctrl+P | Print |
| F5 / Ctrl+R | Reload |
| Alt+Left / Alt+Right | Back / Forward |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| F11 | Toggle fullscreen |
| F12 | Developer tools |
| Ctrl+Shift+Del | Clear browsing data |

## Install

Download the latest release from the
[Releases](https://github.com/virgil48YT/AuroraBrowser/releases) page:

| Platform | Package |
|---|---|
| Windows 10/11 (x64) | `Aurora-VERSION-windows-x64-setup.exe` — double-click; installs per user into `%LOCALAPPDATA%\Aurora` |
| macOS 13.3+ (Apple silicon) | `Aurora-VERSION-macos-arm64.dmg` — drag Aurora to Applications |
| Linux (x86_64, glibc 2.35+) | `Aurora-VERSION-linux-x86_64.AppImage` (`chmod +x` and run) or the `.tar.gz` (extract, run `./aurora`) |
| Android 8.0+ | `Aurora-VERSION-android.apk` |

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

## License

MIT

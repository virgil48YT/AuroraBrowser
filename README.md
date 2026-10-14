# Aurora Browser

A privacy-focused, Chromium-based desktop browser with a fast native Qt6 UI.

Aurora is built around security and privacy by default: a built-in ad and
tracker blocker with per-site exceptions, and incognito windows that keep
the session entirely in memory so nothing touches disk. It is a small,
self-contained app — no telemetry, no background services, no vendor
accounts.

## Features

- Ad and tracker blocking with per-site allow lists
- Incognito windows (Ctrl+Shift+N) that leave no trace on disk
- Chrome-style layout: tabs, address bar, downloads
- History, bookmarks, downloads and settings pages
- Find in page (Ctrl+F), fullscreen (F11)
- Dark and light theme
- Session restore — tabs come back after a restart
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
| F5 / Ctrl+R | Reload |
| Alt+Left / Alt+Right | Back / Forward |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| F11 | Toggle fullscreen |
| F12 | Developer tools |
| Ctrl+Shift+Del | Clear browsing data |

## Install

Download the latest release from the
[Releases](https://github.com/virgil48YT/AuroraBrowser/releases) page.

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

## License

MIT

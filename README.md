# Aurora Browser

A Chromium-based desktop browser with a native Qt6 UI.

## Features

- Chrome-style layout: tabs, address bar, downloads
- History, bookmarks, downloads and settings pages
- Find in page (Ctrl+F), fullscreen (F11)
- Incognito windows (Ctrl+Shift+N) that leave no trace in the regular profile
- Ad blocker with per-site exceptions
- Dark and light theme
- Session restore — your tabs come back after a restart
- Works without the CEF engine (the UI and internal pages still work)

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
| Alt+Home | Go home |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| F11 | Toggle fullscreen |
| Ctrl+Shift+I / F12 | Developer tools |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset |
| Esc | Close find bar |

## Install

Download the latest `aurora_setup.exe` from the
[Releases](https://github.com/virgil48YT/AuroraBrowser/releases) page and
double-click it. It installs to `%LOCALAPPDATA%\Aurora\app` and opens the
browser.

## Build from source

Needs [CMake](https://cmake.org/) and [Qt6](https://www.qt.io/) (Qt5 also
works but Qt6 is recommended).

```bash
# Quick build, tests only, no CEF
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAURORA_BUILD_TESTS=ON
cmake --build build
./build/bin/aurora_tests
```

### Full browser with CEF

The real Chromium engine (needed for actual web pages, WebGL, video):

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

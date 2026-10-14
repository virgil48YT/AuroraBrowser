# Aurora Browser

A premium desktop web browser built on Chromium/CEF with a native Qt6 UI.

For Twitch live streams, VODs and clips, see
[Twitch playback setup](docs/twitch-playback.md). A codec-enabled CEF build is
required for H.264/AAC playback; use `scripts/build_cef_codecs.sh` (or an
equivalent complete distribution) and configure with
`-DAURORA_REQUIRE_H264_AAC=ON`.

Aurora treats Chromium/CEF as the rendering engine and builds a real browser
product around it — a polished chrome, a tab manager engineered for hundreds
of tabs, an omnibox with history and bookmark suggestions, SQLite-backed
history/bookmarks/session storage, dark and light themes, and a performance
budget the architecture is shaped to meet.

## Status — what works today

**Implemented and unit-tested:**
- Omnibox URL/search parsing (`core::Url`, `parse_omnibox_input`)
- TabManager: open/close/switch/reorder/pin/mute, recently-closed, back/forward
- Session restore: validated restore plans plus bulk-restore sequencing that
  shows exactly one tab (`session::run_restore`), so a 20-tab restore creates
  one web host instead of twenty
  history per tab, crash + recovery state, session snapshot
- SQLite storage: RAII Database/Statement/Transaction, WAL mode, busy timeout,
  move semantics
- Versioned schema migrations (history + FTS5, bookmarks tree with deferred FK,
  downloads, sessions, preferences)
- HistoryService (FTS5 search, upsert, prune, clear), BookmarkService (tree,
  move, renumber), SessionService (atomic save/restore/prune), Settings
  (typed prefs with read cache)
- DownloadService over the existing `downloads` table (begin/progress/finish,
  recent, remove, clear finished)
- `aurora://` internal-URL parsing (`core::InternalUrl`) for history, downloads,
  bookmarks and settings
- Defensive session restore: `session::plan_restore` validates every stored row,
  rejects `javascript:`/`data:`/`blob:` and oversized URLs, caps restored tabs,
  and returns tabs in stable window/tab order
- GPU/WebGL command-line policy (`cef::GpuPolicy`) — unit-tested without CEF
- Logging with configurable levels; startup milestones + scoped timers

**Implemented, compiles and runs with Qt6** (verified on Linux with Qt6:
clean build under the strict warning set, `QT_QPA_PLATFORM=offscreen
./build/bin/aurora_browser --smoke-test` exits 0 after building every
internal page):
- Chrome-style layout: the tab strip sits at the very top of the window, the
  navigation/omnibox toolbar directly beneath it, then the content area, with
  the find bar docked below the content
- New-tab "+" button at the end of the tab strip
- Per-tab close button: a real 20x20 `QToolButton` per tab (not Qt's tiny
  close-button subcontrol) with hover/pressed feedback, closing the tab it is
  attached to; closing the last tab is deferred to the next event-loop turn and
  re-validated, which fixes the previous crash
- Native internal pages wired to the SQLite services (see below)
- Theme system + stylesheet builder (light/dark, Aurora palette), contrast
  checked by `python3 scripts/check_contrast.py`
- Application lifecycle: profile bootstrap, schema migration, startup instrumentation

**Browser UI features:**
- Fullscreen mode (F11) with proper content-area resizing and Escape to exit
- Find-in-page bar (Ctrl+F) with next/previous match navigation
- Tab context menu: duplicate, pin/unpin, mute/unmute, copy URL, reload, close,
  reopen closed tab
- Reload/Stop toggle button that adapts to loading state
- Home button and navigation back/forward with disabled states
- Browser menu (hamburger) with: new tab/window, zoom controls, find,
  fullscreen, DevTools, theme toggle
- Full keyboard shortcuts: Ctrl+T/W/Shift+T/L/F/Tab, F5/F11/F12,
  Ctrl+Shift+I/=/-/0, Alt+Left/Right/Home, Ctrl+PageUp/PageDown
- Dark/light theme toggle (system-detected default, toggle via menu)
- Session restore on startup: tabs from the previous session are restored
- Session save on shutdown: open tabs are persisted to SQLite
- Middle-click to close tabs
- Right-click context menus on tabs
- Omnibox with autocomplete (history + bookmark suggestions via QCompleter)
- Bookmarks bar under the toolbar: the children of a "Bookmarks bar" folder in
  `BookmarkService` as flat buttons (favicon + elided title), left-click opens
  in the current tab and middle-click in a new one, folders drop nested menus,
  items that do not fit collapse into a trailing `»` menu, right-click gives
  open / open in new tab / edit / delete and (on empty space) add current page /
  bookmark manager. Toggle it from the browser menu; the choice persists in
  `appearance.bookmark_bar`
- Live download progress in the toolbar's downloads dropdown: an in-progress row
  carries a slim progress bar under the file name (indeterminate when the server
  sends no length), refreshed while the panel is open
- Download pause/resume/cancel/retry, both in the toolbar dropdown and on
  `aurora://downloads`: running rows offer pause, paused rows offer resume (with
  the progress bar frozen in place), cancelled/interrupted rows offer retry,
  which restarts the transfer from its stored URL as a fresh row. Pause state
  survives the popover closing because it lives in the `downloads` table
  (`paused`), not in the UI
- Aurora Shields (network ad blocking, powered by
  [brave/adblock-rust](https://github.com/brave/adblock-rust)): a Brave-style
  shield button left of the downloads tray with a badge counting requests
  blocked on the current page, and a popup panel with the count plus a
  per-site "Block ads on this site" toggle that persists in the profile and
  reloads the page. A global switch lives in Settings → Privacy. Blocking is
  network-request-level only (script/image/xhr/subframe filtering via
  `OnBeforeResourceLoad`); **cosmetic filtering (hiding ad elements in an
  already-rendered page) is not implemented**
- Picture-in-picture window (`src/ui/PipWindow`), reachable from the tab
  context menu
- Incognito (private) windows (Ctrl+Shift+N, or Browser menu → New Incognito
  Window): a private window shares the visual chrome but never writes to the
  regular profile — history, download metadata, session snapshots, bookmarks,
  preferences and extensions are all skipped or read-only for it
  (`core::IncognitoPolicy` drives the decision points and is unit-tested in
  `tests/test_incognito.cpp`). The window's tabs live in a private in-memory
  CEF request context (empty `cache_path`) whose only reference is the window's
  `CefAdapter`, so cookies, cache and storage die with the window. The title
  carries the "(Incognito)" suffix. Popups opened from a private tab inherit the
  private context via CEF's own popup rule.
- Extensions button in the toolbar wired to `ExtensionService` and an
  `aurora://extensions` internal page (unpacking `.crx` via `ZipExtractor` is
  implemented but not covered by a passing test here)
- Favicon cache for tab and bookmark icons (`src/ui/FaviconCache`)

**Internal pages — real, data-backed Qt widgets** (reachable from the browser
menu or by typing `aurora://history`, `aurora://downloads`, `aurora://bookmarks`,
`aurora://settings` in the omnibox):
- **History** — visit list from `HistoryService` (title/URL/visit count/last
  visit), FTS5-backed search box, delete selected, clear all (with confirmation),
  double-click or Enter opens the entry in the current tab
- **Downloads** — items from the `downloads` table via `DownloadService` with
  file name, state, size/progress, source and start time; remove from list and
  clear finished (paused rows are kept — they are live transfers). In-progress
  and paused rows show a live progress bar in the Status column that updates
  while the page is on screen (selection is preserved across refreshes), with
  pause/resume/cancel/retry controls in the Actions column
- **Bookmarks** — the real bookmark tree from `BookmarkService` with open, new
  folder, rename and delete (recursion depth-capped)
- **Extensions** — the real extension list from `ExtensionService`
- **Settings** — typed preferences persisted through `settings::Settings`:
  homepage, restore-session-on-startup, default search engine (DuckDuckGo,
  Google, Bing, Startpage), theme (system/light/dark, applied immediately),
  reduced motion, download directory, third-party cookie blocking, Do Not Track,
  Aurora Shields (block ads and trackers)

Changing the homepage, search engine or theme in Settings takes effect in the
running window immediately; the search-engine preference is used by the omnibox
when the typed text is a search query rather than a URL.

**Implemented, compiles only when CEF is provided** (the CEF translation units
are kept behind `AURORA_ENABLE_CEF`; the GPU/WebGL switch logic they call is
unit-tested separately in `tests/test_gpu_policy.cpp`):
- CEF lifecycle (init/shutdown/subprocess dispatch)
- `CefAdapter` implementing `WebViewController` (load/back/forward/reload/find/zoom/devtools)
- CEF→tab-model page-info callbacks (title, loading, progress, security origin)
- WebGL/GPU acceleration with a SwiftShader software fallback (see below)
- Download pause/resume/cancel via `CefDownloadItemCallback`
  (Pause/Resume/Cancel, with requests that race the first update queued and
  applied on it) and retry via `CefBrowserHost::StartDownload`. The Qt-side
  state machine (`DownloadService` paused state, transitions, clear-finished
  semantics) is unit-tested; the CEF callback plumbing itself is not, for the
  same headless reason as above
- Ad-block request interception (`TabClient::OnBeforeResourceLoad`): every
  non-main-frame request is checked against the adblock-rust engine on CEF's IO
  thread under a mutex, blocked requests are counted per tab, and the badge
  update is posted to the UI thread (`CefPostTask`). Same caveat: reviewed but
  not compiled here
- Media codec policy (`src/cef/MediaPolicy.{hpp,cpp}`) — unit-tested without CEF
  in `tests/test_media_policy.cpp`

**Included but not validated here** (present in the tree but not
built or tested here): Windows self-extracting installer packaging
(`packaging/windows/build_sfx.sh`, `sfx_config.txt`, `rcedit-x64.exe`, the
`aurora_bootstrap` launcher and the build-ID mechanism described in
[docs/build-id.md](docs/build-id.md)) and the Qt-only `build-windows-local.bat`
script. These are not wired into CI; the release workflow ships the folder
artifact instead.

**Not yet implemented** (clearly marked in source, no fake UI): cosmetic ad
filtering (element hiding), background tab suspension.

## Keyboard shortcuts (addendum)

| Shortcut | Action |
|---|---|
| Ctrl+Shift+N | New incognito window |


## Ad blocking (Aurora Shields)

Network-level request filtering with
[brave/adblock-rust](https://github.com/brave/adblock-rust), linked through a
small C ABI crate in `third_party/adblock` and guarded by the
`AURORA_ADBLOCK_FFI` compile definition:

- **Rules** ship as a self-authored starter list (`adblock::starter_rules`, no
  third-party license attached). A full filter list can be layered on top:
  `scripts/fetch_filter_lists.sh` downloads EasyList into the profile directory
  as `adblock-rules.txt`, and the browser merges starter + user rules at
  startup. EasyList is deliberately NOT committed to this repository (its
  CC-BY-SA license requires attribution and the file is ~2 MB of churn)
- **Per-site exceptions** live in the `adblock_exceptions` SQLite table (schema
  migration v5) and are keyed by normalized host, so shields-down survives
  restarts and applies to every window
- **Building without Rust**: `AURORA_ENABLE_ADBLOCK=OFF` (or a machine with no
  cargo — the default `auto` mode) compiles `AdblockEngine` as an inert stub.
  Everything else runs unchanged; the shield popup honestly reports "the
  ad-block engine is not built into this build"
- **What it does not do yet**: cosmetic filtering — ads that load as part of the
  page's own DOM are hidden by element-hiding rules in real ad blockers, which
  requires injecting CSS into the renderer. Only network requests are filtered
  today

## GPU acceleration and WebGL

`src/cef/GpuPolicy.{hpp,cpp}` computes the Chromium command-line switches that
`AuroraApp::OnBeforeCommandLineProcessing` appends in every CEF process, and
`CefSettings::windowless_rendering_enabled` is left off so the accelerated
compositor (and therefore WebGL) is available. The mode is selected by the
`AURORA_GPU` environment variable:

| `AURORA_GPU` | Behavior |
|---|---|
| unset / `hardware` / `gpu` / `on` (default) | Hardware acceleration: ignore the GPU blocklist, GPU rasterization, zero-copy, WebGL and WebGL2 enabled. SwiftShader stays available (`--enable-unsafe-swiftshader`) so WebGL keeps working if no hardware GL context can be created. DirectComposition is disabled (`--disable-direct-composition`) to avoid a known AMD-driver crash in Chromium's video-processor path (`direct_composition_support.cc:616`, `AMD VideoProcessorGetOutputExtension failed: 0x80070057`); hardware GL/WebGL stay on, only the Windows compositor/overlay optimisations are forgone. |
| `software` / `swiftshader` / `sw` | Force SwiftShader: `--use-gl=angle --use-angle=swiftshader`. WebGL still reports as available. DirectComposition is also disabled here so the safety fallback cannot hit the same AMD video-processor failure. Use this on VMs and headless machines with no GL driver. |
| `off` / `disabled` / `none` | No GPU process at all (`--disable-gpu`, `--disable-gpu-compositing`): for isolating driver crashes. WebGL is unavailable in this mode. |

The switch lists, the environment parsing, and whether WebGL ends up available
are covered by `tests/test_gpu_policy.cpp`, which runs without CEF. The switches
themselves were not exercised end-to-end; a display, GPU and
the CEF SDK are required.

## Build

### Core + tests (headless, any OS)
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAURORA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Full browser (needs Qt6 + CEF)
```bash
./scripts/fetch_cef.sh
sudo apt-get install -y qt6-base-dev libqt6svg6-dev   # or brew install qt6 / winget install Qt.Qt6
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_BUILD_TESTS=ON -DAURORA_ENABLE_CEF=ON -DAURORA_BUILD_APP=ON \
  -DCEF_ROOT=$PWD/third_party/cef
cmake --build build
./build/bin/aurora_browser
```

The command above uses stock CEF and does not guarantee H.264/AAC. For a media
release, build the custom CEF distribution described in
[the Twitch playback guide](docs/twitch-playback.md), point `CEF_ROOT` at it,
and add `-DAURORA_REQUIRE_H264_AAC=ON`. Codec implementations are compiled into
CEF; there is no Aurora runtime flag that can supply them.

### Qt shell without CEF (headless smoke test)
The Qt shell builds and runs without CEF — the chrome, the internal pages and the
SQLite services are all fully functional; only web content is replaced by a clear
placeholder host widget. Drop `-DAURORA_ENABLE_CEF=ON`:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DAURORA_BUILD_TESTS=ON -DAURORA_BUILD_APP=ON
cmake --build build
QT_QPA_PLATFORM=offscreen ./build/bin/aurora_browser --smoke-test   # exits 0
python3 scripts/check_contrast.py                                   # theme contrast
```

`--smoke-test` builds the window, opens every internal page so their widgets are
really constructed and populated from the profile database, opens and closes a
tab, then exits 0 — enough to catch chrome regressions with no display.

## Releases

Push a tag matching `v*` (e.g. `git tag v0.1.0 && git push --tags`) and the
[release workflow](.github/workflows/release.yml) runs the same Qt6 Windows
build + unit tests, then publishes a GitHub release with
`aurora-windows-x64.zip` attached (a folder artifact built with `windeployqt`,
not a single-exe SFX — the packaging scripts under `packaging/windows/` exist
for the SFX variant but are not wired into CI yet).

See [docs/build.md](docs/build.md), [docs/architecture.md](docs/architecture.md),
[docs/performance-budgets.md](docs/performance-budgets.md), and
[docs/security.md](docs/security.md).

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+T | New tab |
| Ctrl+Shift+N | New incognito window |
| Ctrl+W | Close tab |
| Ctrl+Shift+T | Reopen closed tab |
| Ctrl+L | Focus address bar |
| Ctrl+F | Find in page |
| F5 / Ctrl+R | Reload |
| Ctrl+Shift+R | Hard reload (bypass cache) |
| Alt+Left / Alt+Right | Back / Forward |
| Alt+Home | Go home |
| Ctrl+Tab / Ctrl+PageDown | Next tab |
| Ctrl+Shift+Tab / Ctrl+PageUp | Previous tab |
| F11 | Toggle fullscreen |
| Ctrl+Shift+I / F12 | Developer tools |
| Ctrl++ / Ctrl+= | Zoom in |
| Ctrl+- | Zoom out |
| Ctrl+0 | Reset zoom |
| Esc | Close find bar |

## Project structure

```
src/
  app/         entry, profile bootstrap, CEF subprocess dispatch, session restore
  ui/          Qt chrome + theme system + find bar + tab context menu + browser menu
  cef/         CEF lifecycle + CefAdapter (WebViewController impl)
  webview/     WebViewController — abstract engine interface
  tabs/        TabManager
  ui/pages/    native internal pages (history, downloads, bookmarks, settings)
  history/  bookmarks/  downloads/  session/  settings/   services backed by SQLite
  storage/     RAII SQLite wrapper + versioned migrations
  adblock/     AdblockEngine + AdblockService (Aurora Shields)
  extensions/  ExtensionService + ZipExtractor
  core/        Url, TabId, TabState, NavigationEntry, InternalUrl
  util/        Log, Perf, Sha256
tests/         doctest suite (no Qt/CEF required)
cmake/         CefCodecs.cmake
scripts/       fetch_cef.sh, check_contrast.py, uidrive.py, fetch_filter_lists.sh, build_cef_codecs.sh, verify_media_codecs.js
packaging/     Windows self-extracting installer (SFX)
docs/          architecture, build, performance budgets, security, twitch-playback, build-id
third_party/   doctest, sqlite, adblock (Rust FFI crate)
tools/         buildid generator
```

## License

MIT (third-party CEF, Chromium, Qt, doctest, SQLite retain their own licenses).

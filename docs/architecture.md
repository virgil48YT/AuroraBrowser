# Aurora — Architecture

Aurora is a desktop web browser built on Chromium/CEF with a native Qt6 UI.
This document describes the architecture at a level another engineer needs to
navigate the codebase confidently.

## Design principles

1. **Engine and chrome are separate.** Chromium/CEF is the rendering engine;
   Aurora's Qt UI is the browser chrome. The two communicate through a narrow
   `webview::WebViewController` interface so neither leaks into the other.
2. **The core is engine-free.** `aurora_core` (tabs, history, bookmarks,
   session, settings, storage, logging, perf) has **no** dependency on Qt or
   CEF. It compiles and runs on a headless CI box. This is what lets us unit-
   test browser semantics without a display, a GPU, or a gigabyte of CEF.
3. **The UI thread never blocks.** All persistence (history writes, download
   accounting, bookmark edits) runs on a background sequence. The UI thread
   only ever touches the in-memory `TabManager` and renders from it.
4. **No fake features.** Anything not wired end-to-end is labeled as such in
   code and docs. The CEF-disabled build shows a clear placeholder rather than
   pretending to render pages.

## Process model

Aurora respects CEF's multi-process architecture. The browser process owns the
Qt UI and the `TabManager`; renderer, GPU, utility, and network-service
subprocesses are spawned by CEF itself.

```
Browser process (this codebase)
  ├─ Qt UI thread: MainWindow, TabBar, Omnibox, TabManager
  ├─ Worker thread(s): HistoryService, BookmarkService, SessionService, Downloads
  └─ CEF message loop (pumped from the Qt event loop)

CEF subprocesses (spawned by CEF, not us)
  ├─ Renderer (per-site isolation)
  ├─ GPU
  └─ Network service
```

A packaged build has one more process in front of all of this: the bootstrap
launcher (`aurora_bootstrap`, `src/bootstrap/main.cpp`). It is what the
self-extracting installer runs. It resolves the CEF **build ID** — a SHA-256 over
the CEF runtime files — materializes the version-locked resource files into
`%LOCALAPPDATA%\Aurora\cef\<build_id>\`, and only then starts
`aurora_browser.exe` with `AURORA_CEF_DIR` pointing at that directory. The
browser passes it to CEF as `resources_dir_path` / `locales_dir_path`, so a CEF
update can never leave the previous engine's `.pak` files in play. The launcher
links `aurora_core` only, and a developer build (no manifest) skips the whole
mechanism and loads resources from the executable's own directory. See
[build-id.md](build-id.md).

`cef::initialize` sets `multi_threaded_message_loop = false` and the UI thread
pumps CEF via a timer integrated into the Qt event loop. This is the
recommended approach when embedding CEF inside another UI toolkit and keeps
all CEF callbacks on a single, predictable thread.

## Threading rules

- **UI thread**: all `TabManager` mutations, all Qt widget calls, all
  `WebViewController` calls. CEF callbacks arrive here (with our message-loop
  config) or are marshalled here via `CefPostTask(TID_UI, ...)`.
- **Worker thread**: SQLite operations for history/bookmarks/session. Each
  service owns its own `Database` connection to the same WAL-mode file, so
  readers never block writers.
- **Never** call SQLite, the filesystem, or the network on the UI thread.

## Layering

```
src/
  app/         Application entry, profile bootstrap, CEF subprocess dispatch
  ui/          Qt chrome: MainWindow, TabBar, Omnibox, theme, stylesheets
  cef/         CEF integration: lifecycle, CefAdapter (implements WebViewController)
  webview/     WebViewController — the abstract engine interface (no CEF)
  tabs/        TabManager — pure tab semantics, no rendering
  history/     HistoryService — SQLite-backed, FTS5 search
  bookmarks/   BookmarkService — tree model, positional reordering
  session/     SessionService — crash-safe tab persistence
               SessionRestore — bulk-restore sequencing (Qt-free, unit-tested)
  storage/     Database (RAII SQLite), Migrator (versioned schema)
  settings/    Settings — typed key-value preferences
  core/        Value types: Url, TabId, TabState, NavigationEntry
  util/        Log, Perf (startup milestones + scoped timers)
```

Dependency direction is strictly downward: `app` may depend on anything;
`ui` depends on `core`, `tabs`, `webview`; `cef` depends on `core`, `webview`;
`core`/`tabs`/`storage`/`history`/`bookmarks`/`session`/`settings`/`util`
depend on nothing below `core`/`util`/`storage`.

## Data storage

A single SQLite database per profile (`profile.db`), WAL mode, schema versioned
via `PRAGMA user_version` and the `Migrator`. Tables: `history` (+ FTS5
mirror), `bookmarks` (self-referential tree with a deferred FK and a root
anchor at id=0), `downloads`, `sessions`, `session_tabs`, `preferences`.

## Session restore: one visible host, always

Restoring a session is the most crash-prone startup path, because it opens many
tabs in one synchronous burst. Web hosts are therefore created **lazily and
one-shot**: `CefHostWidget` creates its engine (and only then forces a native
window handle) when the window has deliberately made it the *current* content
widget — never on construction, never when `QStackedWidget::addWidget` or a
stray `showEvent` reveals it.

The sequencing lives in `session::run_restore` (core, no Qt, unit-tested in
`tests/test_bulk_restore.cpp`):

1. bulk mode on — `MainWindow`'s `Added`/`ActiveChanged` handlers suppress their
   "make this tab visible" path, and the content stack parks on an inert
   placeholder that owns index 0;
2. every planned tab is opened, and the seed tab is closed, still in bulk mode;
3. bulk mode off, then exactly **one** tab is activated and shown, via an
   explicit callback (activating an already-active tab notifies nothing).

`MainWindow` additionally caps how many hosts may be created during a bulk open
(one) and logs restore progress at Info, so a future startup crash is locatable
in the log. `aurora_browser --seed-session=N --smoke-test` runs the whole path
headlessly and fails if more than one host was shown or given a native window.

See [build.md](build.md) for compiling and [performance-budgets.md](performance-budgets.md)
for the performance targets this architecture is shaped to meet.

## What is implemented vs. scaffolded

**Implemented and tested** (core, no GUI): URL/omnibox parsing, TabManager
(open/close/switch/reorder/pin/mute/crash/reopen/recently-closed/back-forward
history), SQLite wrapper (transactions, prepared statements, WAL), schema
migrations, HistoryService (FTS5 search, upsert, prune, clear), BookmarkService
(tree, move, renumber), SessionService (atomic save, prune, restore),
Settings (typed prefs, cache), logging, perf instrumentation.

**Implemented, not compiled in CI** (require Qt6/CEF on a desktop): Qt chrome
(MainWindow, TabBar with context menus, Omnibox, FindBar, BrowserMenu,
theme system, stylesheets), CEF lifecycle and
the `CefAdapter` WebViewController. UI features include fullscreen mode,
find-in-page, tab context menus, reload/stop toggle, status bar, dark/light
theme toggle, session restore, and 20+ keyboard shortcuts.

**Intentionally deferred** (architecture-ready, not built): tab suspension /
background throttling policy, downloads UI, extensions, incognito profile
isolation, installer/packaging. These are called out in the relevant source as
future work rather than stubbed with fake UI.

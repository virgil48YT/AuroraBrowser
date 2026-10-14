# Aurora — Performance budgets

Performance is a product feature, not an afterthought. These are the targets
the architecture is shaped to meet and that profiling work is measured
against. **Numbers are targets, not measured claims** — every target here must
be validated on the target hardware before a release ships, and a regression
that exceeds a budget blocks the release.

## Startup

| Metric | Target |
| --- | --- |
| Cold start to first window paint | < 800 ms |
| Warm start to first window paint | < 300 ms |
| Time to interactive (omnibox focusable) | within 50 ms of first paint |

Strategy: lazy initialization. The profile database is opened and migrated on
the UI thread only because migrations are fast and idempotent; everything else
(history search index warm-up, theme detection) is deferred or posted to a
worker. `util::startup_milestone` instruments each phase.

## Tabs

| Metric | Target |
| --- | --- |
| New tab (state only) | < 1 ms (no renderer) |
| Switch active tab | < 16 ms (one frame) |
| Close tab | < 1 ms |
| 100 tabs open, UI frame rate | 60 fps during scroll |

Strategy: `TabManager` mutations are O(n) only on structural changes
(add/remove/move) and O(1) on state changes (title/loading/progress). The tab
strip repaints only the affected tab on state changes, not the whole strip.

## Navigation / omnibox

| Metric | Target |
| --- | --- |
| Keystroke to omnibox repaint | < 16 ms |
| Omnibox suggestion query (history+bookmarks) | < 30 ms |
| Commit navigation to first paint | engine-bound (CEF) |

Strategy: the omnibox does zero database work on the UI thread. Suggestion
queries run on a worker thread and results are swapped into a `QStringListModel`
asynchronously; typing never blocks on a query in flight.

## Memory & idle

| Metric | Target |
| --- | --- |
| Idle with 1 tab (browser process) | < 150 MB RSS |
| Idle with 20 background tabs | + < 30 MB vs. 1 tab (before suspension) |
| Per suspended tab | ~ 0 MB renderer |

Strategy (planned, architecture-ready): background tabs idle for N minutes
(configurable via `tabs.suspend_minutes`) are discarded — their navigation
entry is preserved so reloading restores the exact URL. The `WebViewController`
interface already has `set_visible`/`destroy` so suspension can be added
without changing the chrome.

## UI responsiveness

The UI thread must never block on: downloads, history writes, bookmark
operations, navigation, tab operations, filesystem or database work, or
network. Any operation that could exceed 1 ms is posted to a worker thread.

## Profiling

`util::ScopedTimer` records named microsecond samples; `util::record_timing`
emits them under the `perf` log tag. For deep profiling, use Chromium's
`chrome://tracing` via the DevTools Performance tab (exposed through
`WebViewController::open_devtools`). Establish the budgets above as CI gates
before claiming them.

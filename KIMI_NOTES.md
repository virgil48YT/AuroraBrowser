# Kimi K3 UI polish pass — notes

Branch `hybrid`, five commits, UI code only. No workflow / CMake / packaging
files touched. Nothing pushed.

## Bug 1 — omnibox focus ring stuck on (src/ui/Omnibox.{hpp,cpp})
Root cause: clicking the web page moves keyboard focus to the native CEF child
window, which Qt does not track — the inner QLineEdit never receives a
focusOut, so the glow never animates down. Fixed with four independent
mechanisms:
1. `QApplication::focusChanged` treats a nullptr `now` (focus left Qt
   entirely) as focus-out for the omnibox.
2. An app-wide event filter re-evaluates the ring on any `MouseButtonPress` /
   `FocusOut`, deferred one event-loop turn so the press can deliver focus
   first.
3. A 120 ms poll timer runs **only while the ring is lit** and stops at zero.
4. Windows-only `NativeFocusGuard` (QAbstractNativeEventFilter) watches
   WM_KILLFOCUS of the top-level HWND and calls `on_native_focus_lost()`.
All filters are removed in `~Omnibox`. `set_current_url()` performs no focus
work, so load progress cannot relight the ring; the `focusProgress` animation
always runs to exactly 0.0.

## Bug 2 — select-all on focus (Omnibox.cpp, NakedEdit)
`focusInEvent` keeps the deferred select-all (keyboard/tab focus).
`mousePressEvent` now selects all when the click lands in an unfocused edit
(deferred so it survives the click's own caret placement); subsequent clicks
place the cursor normally.

## Bug 3 — bottom status bar removed (src/ui/MainWindow.{hpp,cpp}, StylesheetBuilder.cpp)
No `QStatusBar` anywhere — nothing reserves layout height for transient text.
Replaced by `#StatusBubble`, a small pill (`QLabel`, child of the central
widget, mouse-transparent) pinned to the bottom-left of the content area and
kept above the find bar via the window's event filter. Sticky text is only
hover-link URLs and "Loading… NN%"; the tab title/URL is never shown and load
completion hides the bubble. `flash_status()` (bookmarks, clipboard, clears)
auto-hides on a restartable 3.2 s timer. Bubble hides during content
fullscreen.

## Bug 4 — tab arrangement & idle styling (src/ui/TabBar.{hpp,cpp})
All tab painting moved into `TabStripStyle`, a `QProxyStyle` (base Fusion,
parented to the bar) installed with `setStyle()` — QTabBar's drag-reorder,
slide animations and per-tab buttons keep working because the drag grab pixmap
renders through the same style.
- Active tab: full-bleed folder tab, 8 px top radius, 10 px outward bottom
  fillets, filled with the toolbar plate color → seamless merge, no seam, no
  double borders.
- Inactive tabs: fully transparent; 1 px right-edge divider only between calm
  (unselected, unhovered) neighbors; hover raises an animated rounded plate
  (per-index animation on the style, OutCubic 90/140 ms).
- Adaptive widths: normal tabs share the strip width clamped to 76–240 px,
  recomputed on strip resize and on tab add/remove/(un)pin; pinned tabs are
  fixed 40 px icon-only (no text, no close button).
- Close: 16 px painted "×" shown only on hover/active; middle-click close and
  the context menu are unchanged (menu still disables Close for pinned tabs).

## 10x polish
- Theme: dark palette retuned to the reference — strip `#1A1B1E`, toolbar +
  active tab `#28292A`, recessed omnibox `#1E1F23`, cool charcoal neutrals,
  soft dividers. Light theme untouched. Metrics: omnibox 36 px, nav buttons
  32 px, pinned tab 40 px.
- Omnibox is a true stadium pill (`radius = height/2`) with a gentle two-arc
  alpha glow on focus and zero accent at idle; leading search/security glyph;
  trailing bookmark star (outline/filled, accent when bookmarked).
- Icons: new `Puzzle` (filled Material-style extensions glyph) and `Code`;
  `More` is now three filled dots (the stroked dots smudged at 16 px);
  `IconButton::setIconPx` for optical sizing.
- ExtensionsButton now paints the puzzle icon instead of the `\u26ED` font
  glyph that could render as tofu.
- New chrome buttons: tab-search chevron at the far left of the strip
  (menu of tabs, Ctrl+Shift+A) and a downloads tray button right of the
  omnibox (Ctrl+J).
- StylesheetBuilder: removed the dead `QTabBar::tab` QSS block (the proxy
  style owns tab painting), removed QStatusBar rules, zeroed `#TabStrip` /
  `#ToolbarRow` QSS padding so it can't double up with the layouts, fixed the
  omnibox edit's doubled leading padding.

## Verification (Linux sandbox)
- `cmake -S . -B build-check -G Ninja -DAURORA_BUILD_APP=ON` — clean configure
  (CEF layer OFF, as expected here).
- Full `ninja` build of the Qt app target: **compiles and links with zero
  warnings**; `ctest` passes.
- Offscreen smoke run (`QT_QPA_PLATFORM=offscreen`): app starts, window shown,
  stylesheet applies with no QSS parse warnings, clean shutdown.

Not verified here: the Windows/CEF target (no CEF SDK on this box) and the
Windows-only `NativeFocusGuard` path (compiles only under `Q_OS_WIN` — it uses
`MSG`/`HWND` from `<windows.h>` and Qt's `qintptr` result signature; worth one
`cl` compile on the Windows builder).

Commits: `3131280` theme · `a542918` icons · `5747e89` omnibox · `b80dab6`
tabs · `094b994` window/stylesheet.

## Phase 1

UI correctness pass: tab strip, downloads dropdown, popup placement, keyboard
shortcuts. Qt/UI code only; no CEF/extension/permission code, no Windows
native snap, no packaging.

### Bug 1 — Tab title + favicon lost when a tab loses focus (image-2)
Root causes (two of them, feeding each other):
- `TabBar::strip_available_width()` derived the space for tabs from the tab
  bar's OWN width, but the strip row's layout sizes the bar from the tabs'
  `tabSizeHint()`s (`QSizePolicy::Maximum`). Circular: every relayout shaved
  12px/N off every tab, so within seconds of normal browsing all tabs sat at
  the 76px floor — under `TabStripStyle`'s 96px compact threshold, which hides
  the text. Any title change or tab switch (another relayout) made it worse.
  Fixed by deriving the available width from the PARENT strip's width minus
  the sibling chrome (tab-search chevron, new-tab button, window controls,
  layout margins/spacing), with a 640px fallback before first layout. The
  strip's width comes from the window, not from our hints, so the loop is
  broken. The 96px threshold itself is unchanged (per the brief).
- `refresh_tab()` called `setTabIcon(idx, FaviconCache::icon_for_url(url))`
  unconditionally; the cache answers null while a fetch is in flight AND
  forever for hosts without an icon or after a failed fetch — so every
  ActiveChanged/TitleChanged/refresh overwrote a valid favicon with nothing.
  The bar now keeps the last REAL icon per tab (`last_icons_`), replaces it
  only when a genuine icon arrives, re-asserts it after `refresh_all()`
  rebuilds (which drop QTabBar's stored icons), and prunes entries for closed
  tabs. A legitimately empty title still renders as "New Tab".

### Bug 2 — Tabs over-compact with only 2 tabs and ample space
Same root cause as above (the width feedback loop), so the same fix. Tabs now
hold a comfortable readable width up to the 240px cap when the strip has room
and only compact (icons only under 96px) or scroll when the strip genuinely
cannot fit them — verified headlessly: 3 tabs in a 1280px window stay at
240px through 40 active/title-change relayouts (previously each relayout
shrank them); in a 460px window the same tabs settle at 80px icon-only, which
is the intended compact behavior for that width.

### Bug 3 — Downloads dropdown popover (image-1)
New `src/ui/DownloadsPopover.{hpp,cpp}`: a `Qt::Popup` panel that drops from
the toolbar's downloads button (and Ctrl+J) instead of navigating the tab
away. Header "Recent download history" + close ✕; scrollable list of
`DownloadService::recent(8)` (amber folder glyph, wrapping file name, muted
"size • relative time" subline; in-progress rows read "Downloading… NN%";
cancelled/interrupted say so); the footer "Full download history" (with an
open-page glyph at the far right) is a layout sibling BELOW the list — never
an overlay — so it cannot cover the last row; it closes the popover and opens
`aurora://downloads`. The Downloads page stays the complete history view.
Empty and no-profile states show a centered hint. The panel is a rounded
`WA_StyledBackground` widget styled from theme tokens in
StylesheetBuilder; the soft shadow is painted by `paintEvent` (three
expanding, fading rings from `colors.shadow_color`) because QSS cannot draw
one. New icons `Folder` and `OpenExternal` were appended AFTER `Spinner` in
the enum so no existing icon shifts.

### Bug 4 — Menus/popovers escaping the window/screen
New header-only `src/ui/PopupPlacement.hpp`: `clamped_to_screen()` (keep a
popup inside the anchor screen's `availableGeometry()`), `dropdown_pos()`
(under an anchor, left- or right-edge aligned, flips above when the space
below is too small, then clamps), and `exec_menu_at()` / `exec_menu_under()`
drop-in replacements for open-coded `menu.exec(mapToGlobal(...))` sites.
Applied to the tab context menu (cursor-positioned) and the tab-search menu
(anchored), and the downloads popover shows through `dropdown_pos()`. The
browser-menu and extensions popups are `QToolButton::InstantPopup` menus —
Qt positions and clamps those itself; noted, not changed.

### Bug 5 — Keyboard shortcuts
- The browser menu (`BrowserMenu`) and tab context menu registered REAL
  shortcuts on their QActions (Ctrl+T/N/H/J/D/+/−/0, Ctrl+F, F11,
  Ctrl+Shift+I, Ctrl+W, Ctrl+Shift+T) while `MainWindow::setup_shortcuts()`
  bound the same keys in QShortcuts. Two active bindings for one key are
  ambiguous to Qt's shortcut map, and an ambiguous shortcut fires NEITHER
  handler — the menu showed keycaps for shortcuts that did nothing. Menus now
  keep the accelerator DISPLAY (QMenu still renders the shortcut column) with
  the action's shortcut context narrowed to the menu widget itself
  (`Qt::WidgetWithChildrenShortcut`, inert while the menu is hidden);
  MainWindow's QShortcuts are the single real owner of each key.
- F11 was additionally bound twice in `setup_shortcuts()` (window AND
  application context) — one binding now.
- All bindings are explicit `Qt::WindowShortcut`: they fire wherever focus
  sits inside the window (omnibox, find bar, tab strip, internal pages),
  stay quiet while one of our popups/modal dialogs is active (those own the
  active window and handle their own keys), and cannot go ambiguous when a
  second browser window registers the same keys (the old application-context
  F11/Escape pair would have).
- Ctrl+J now toggles the downloads popover (Chrome behavior) instead of
  navigating to the downloads page; added the missing Ctrl+N (new window) and
  Ctrl+H (history); Ctrl+Shift+A (tab search) already existed.
- Escape is one guarded binding: leaves the omnibox first (the edit's own
  Escape-to-blur is pre-empted by the shortcut), then closes the find bar,
  then exits fullscreen.

### Verification (Linux sandbox, headless)
- `cmake -S . -B build-check -G Ninja -DAURORA_BUILD_APP=ON` — clean
  configure (CEF layer OFF, as expected here). Verified with Qt 6.10.3
  (requirement is >= 6.5; 6.5.x is no longer on the Qt mirrors).
- Full `ninja` build: compiles and links with ZERO errors and zero warnings
  in the touched files (the only warnings are pre-existing: a
  `-Wnull-dereference` inside `tests/test_tab_manager.cpp` and C++-flag
  notices on the third-party C sqlite build).
- `ctest` passes; offscreen smoke run
  (`QT_QPA_PLATFORM=offscreen ./aurora_browser --smoke-test`) passes.
- A scratch harness (NOT in the repo) replicated the strip row + toolbar with
  the real `TabBar`/`DownloadsPopover` under the offscreen platform and saved
  grabs: 3 tabs hold 240px with titles after 40 churn relayouts at 1280px
  window width; at 460px they correctly compact to icon-only; the popover
  renders like image-1 (header, rows, footer) and its position clamps into an
  800x600 screen.

Could NOT be verified headlessly:
- Anything needing the native CEF child window (no CEF SDK here): real
  favicon fetch timing against live pages, and — the known limitation —
  keyboard shortcuts while the NATIVE CEF child window owns the keyboard.
  Qt sees no key events from it at all, so NO Qt-side shortcut context can
  fire; that needs `CefKeyboardHandler` forwarding in the CEF layer (a later
  phase by the brief's rules).
- Windows-only behavior (native snap, `NativeFocusGuard`) — untouched this
  phase.
- `DownloadService` has no change-notification signal, so the popover
  rebuilds from `recent(8)` on every open rather than live-updating while
  open — deliberate Phase-1 scope decision.
- While a menu is OPEN, that menu's own accelerators are inert (they are
  display-only now and the window shortcuts stay quiet while a popup owns the
  active window) — matches Chrome; pressing the key with the menu open does
  nothing instead of activating the item.

Commits: `1e5aea2` tabs · `4cd5426` icons · `9f111c3` popup placement ·
`d28ca8e` downloads popover · `f5eade5` shortcuts · `256c600` popover width.

---

# Phase 2 — CEF integration (2026-08-30)

Four tasks from `PHASE2_BRIEF.md`, each committed separately. UI/core/CEF code
only; no workflow / CMake / packaging files touched (two additions that would
normally be .cpp files — `PermissionStore`, `PermissionPrompt` — are
header-only precisely so the explicitly-listed build files did not need an
edit). The CEF SDK is NOT installed in this sandbox, so the `aurora_cef`
target could not be compiled here; all CEF-side code follows the existing
`CefAdapter.cpp` emitter pattern exactly and must be compile-checked where
the SDK is available. Everything else was verified: `aurora_browser` and
`aurora_tests` build clean with zero new warnings after every task, the full
doctest suite passes, and the offscreen smoke run passes after every commit.

## Task 1 — keyboard forwarding (`3676981` + `b7c11fa`)

While a web page has focus the native CEF child window owns the keyboard and
Qt sees NO key events, so none of the MainWindow QShortcuts fired (the known
limitation called out in Phase 1). `TabClient::OnPreKeyEvent` now offers each
accelerator-looking key — a held modifier (Ctrl/Alt/Cmd), a function key
(F1–F24), or Escape — to the host BEFORE the page sees it, via
`WebViewController::set_key_event_callback` (same emitter wiring as
downloads/fullscreen; no globals; `aurora_cef` stays Qt-free). Plain typing is
never forwarded. The fullscreen Escape/F11 intercept keeps first priority, so
its chaining stays unique. The event is `windows_key_code`-based, which CEF
populates identically on every platform; MainWindow owns the small VK→Qt
table (A–Z, 0–9, F1–F24, Escape/Tab/PgUp/PgDn/Home/arrows, OEM `=`/`-`).

MainWindow's `forward_engine_key` maps the VK code + held modifiers through
an explicit hand table to exactly the handler methods `setup_shortcuts()`
binds (Ctrl+T/W/N/R/L/J/H/D/F/0/Tab/PageUp/PageDown, Ctrl+Shift+T/I/A/R/Tab,
Ctrl++/Ctrl+=/Ctrl+- zooms, Alt+Left/Right/Home, F5/F11/F12, Escape) and
dispatches the matched action on the next event-loop turn via
`QTimer::singleShot(0, this, ...)` — never inline, because Ctrl+T/W/N create
and close browsers inside what is still CEF's key dispatch, and the
tab-search menu runs a nested loop.

Two empirically verified Qt behaviors shaped this (scratch harness against
the sandbox's Qt 6.10.3, harness not committed):
1. A POSTED `QKeyEvent` with no Qt focus widget matches NO window-context
   QShortcut (QShortcutMap resolves via `focusWidget()`), so the action is
   invoked directly instead of synthesizing events. Posting would have looked
   right and silently done nothing.
2. `QKeySequence("Ctrl+PageDown")` parses to `Key_unknown` — the pre-existing
   bindings were dead shortcuts. Qt's spelling is `PgDown`/`PgUp`; fixed.
Shift-leniency is handled in the table itself: Ctrl+Shift+= on a US layout
(VK_OEM_PLUS) claims zoom-in from BOTH the Ctrl and the Ctrl+Shift branches
("Ctrl++" is physically the shifted key), and VK_ADD/VK_SUBTRACT cover the
numpad. Unmodified Escape is consumed only when the chrome chain is
actionable (find bar open or fullscreen); otherwise the page keeps its
stop-loading Escape. Ctrl+Alt and Alt+Shift combos are left to the page
(nothing registered).

## Task 2 — .crx/.crx3 routing (extensions/cef, `909ed41`; ui/extensions, `ea7fd2d`)

A packaged extension is an install attempt, not a file for the Downloads
folder. `OnBeforeDownload` recognizes them (`application/x-chrome-extension`
MIME or a `.crx`/`.crx3` filename — the Web Store does both), downloads the
bytes to a private temp path (`aurora-ext-<id>.crx`), and on completion hands
the file to the host via a new `set_extension_install_callback` instead of
recording a DownloadService row (no stray row, no download-shelf noise).
Cancel/interruption deletes the temp copy; with no handler attached the
download is refused outright.

`ExtensionService::install_from_file` validates honestly: `Cr24` magic,
format version 2/3, header-length bounds (16 MiB cap, must fit the file),
ZIP payload (`PK\x03\x04`). INFEASIBLE and declined clearly: actual
installation. CEF only loads UNPACKED extension directories
(`CefRequestContext::LoadExtension` takes a folder), the Web Store signature
verification that makes a signed .crx trustworthy is Chrome code CEF does not
expose, and this build links no zip/inflate library — so a well-formed
package returns `Unsupported` with a user-facing explanation, and MainWindow
shows "Extension installation is not supported" (the brief's fallback),
always removing the temp file afterwards. The extensions page gained an
"Install .crx…" button driving the same code path so the policy is visible
from the UI. Unit tests cover v2/v3 recognition and every malformed case.

## Task 3 — mic/camera permission prompts (cef/permissions, `de2b004`)

There was no permission handler at all. `TabClient` now implements
`CefPermissionHandler`: `OnRequestMediaAccessPermission` masks requests to
device audio/video capture and forwards them with an exactly-once,
default-deny responder — a shared answer object whose destructor DENIES, so a
tab closing mid-prompt or a dropped request can never grant or hang the page.
Desktop capture is denied outright with a log line (a prompt that cannot
preview the shared surface must not offer it), unknown media bits likewise.
`OnShowPermissionPrompt` forwards only notification requests; everything else
returns false so CEF's default (deny) applies.

The host shows `PermissionPrompt`, a styled popup under the omnibox modeled
on the downloads popover (same elevated panel + painted three-ring shadow +
`dropdown_pos` clamping): Allow / Block + opt-in "Remember this decision".
Policy: default-deny (dismissal = deny, never remembered), per-origin
per-kind memory via the new header-only `permissions::PermissionStore` on the
Settings prefs table (`perm.<kind>.<origin>`, no schema migration), NEVER
auto-grant (a combined mic+camera request grants only if every requested
kind is remembered-allowed; any remembered deny answers deny immediately).
Concurrent requests queue — one popup at a time. `origin_of()` scopes keys to
scheme://host[:port] and makes non-web/host-less URLs unkeyable (they always
prompt-or-deny). Store + origin scoping are unit-tested; new Mic/Camera/Bell
icons and stylesheet rules (`PermissionPromptPanel`, `PageAccentButton`)
match the theme. The engine callback defers one event-loop turn before
opening the popup, per the codebase's CEF-callback rule.

## Task 4 — downloads dir (downloads, `a3609a2`)

Already correct: MainWindow set `downloads_dir` from
`QStandardPaths::DownloadLocation` before any browser exists. Hardened: empty
location → `~/Downloads` fallback + `mkpath()` (first download cannot fail on
a missing folder), Warn log on creation failure; the engine-side CWD fallback
now logs a warning instead of silently scattering files.

## What was infeasible

- Compiling/linking the CEF path: no CEF SDK in the sandbox (build-gated
  behind `AURORA_ENABLE_CEF`/`CEF_ROOT`). All CEF code mirrors existing
  patterns and uses only CEF ≥126 (Alloy) APIs, but it has NOT been
  compiler-verified — that must happen on a machine with the SDK.
- Real .crx installation: needs Chrome's signing machinery (not in CEF) and a
  zip/inflate library (not linkable without CMake changes, which are
  off-limits). Declined honestly instead.
- A runtime click-test of the permission popup against a live getUserMedia
  page, for the same no-SDK reason; the widget itself follows the exact
  patterns of the already-verified downloads popover.

Phase 2 commits: `3676981` + `b7c11fa` keyboard forwarding · `909ed41` .crx routing ·
`de2b004` permission prompts · `a3609a2` downloads dir · `ea7fd2d`
"Install .crx…" button.

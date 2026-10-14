# Kimi K3 — Phase 1: UI correctness pass (branch `hybrid`)

You are continuing the Aurora Browser UI work on branch `hybrid`. This is a
**Qt6 + CEF** desktop browser. The prior Kimi pass (commits 3131280..d68ef0f)
did the chrome redesign. Now fix the concrete bugs below. Work on `hybrid`,
make **local commits only — do NOT push**. Keep commits small and named by area.

## Build / verify (MUST do before finishing)
Linux sandbox, CEF layer OFF (expected):
```
cmake -S . -B build-check -G Ninja -DAURORA_BUILD_APP=ON
cmake --build build-check
```
Must configure clean and compile+link with zero errors. Fix any warning you
introduce. At the end, append a `## Phase 1` section to `KIMI_NOTES.md`
describing each fix + what could NOT be verified headlessly (Windows-only,
CEF-only behavior).

## Bugs to fix (in order)

### 1. Tab title + favicon lost when tab loses focus (image-2)
When a tab becomes inactive, its title text disappears and the favicon/SVG
changes/blank. Root-cause in `src/ui/TabBar.cpp`:
- `TabStripStyle::drawControl` hides text when `compact` is true, where
  `compact = pinned || r.width() < 96.0`. If the tab rect is too narrow, text
  vanishes even for a normal unpinned tab.
- `refresh_tab()` calls `setTabIcon(idx, favicons.icon_for_url(page_url))`
  which can return a **null** icon, overwriting a previously-known good icon
  during a refresh (e.g. on ActiveChanged / TitleChanged). A null icon then
  falls through to the faint dot, so the favicon appears to "change".
Fix:
- Do NOT overwrite a valid favicon with a null/placeholder. Retain the last
  valid icon per tab; only replace when a real icon arrives
  (`FaviconCache::icon_ready` already triggers `refresh_tab`).
- Make sure inactive (unselected) tabs still render their title text and
  favicon. The selected/inactive distinction must affect styling only, never
  erase content.
- Verify the model actually carries a non-empty title (TabState / TabManager);
  if a tab legitimately has no title yet, keep "New Tab" — don't blank it.

### 2. Tabs over-compact with only 2 tabs and ample space
With 1–2 tabs and a wide window, tabs shrink below 96px and lose their text
(see bug 1). `tabSizeHint` clamps to `[tab_min_width, tab_max_width]` and
divides `strip_available_width() / normal_count`. Investigate why rects end up
tiny (is `strip_available_width()` / `width()` returning a stale or wrong
value? is the parent resize not propagating? is `setExpanding(false)` fighting
the layout?). Fix so that with few tabs, each tab is a comfortable readable
width (Chrome caps tab width ~240 and does NOT shrink below a readable min
when space is ample). Tabs should only elide/compact when there genuinely
isn't enough room for all of them at min width.

### 3. Downloads dropdown (image-1)
Clicking the downloads tray button (right of omnibox, `downloads_btn_`) should
open a **dropdown popover** of recent downloads (not navigate to a page),
matching image-1:
- Header "Recent download history" + close ✕.
- Scrollable list from `DownloadService::recent(N)` showing file icon, name,
  size, relative time. Don't let the footer cover the last item.
- Footer bar with "Full download history" action that opens the full
  Downloads page (`aurora://downloads` / DownloadsPage).
- Style consistent with the rest of the chrome (pill/popover, dark theme).
The existing DownloadsPage stays as the full-history view.

### 4. Dropdown / menu overflow outside window (image-1)
Popups/menus (tab search menu, downloads dropdown, browser menu, context
menus) escape the window when not fullscreen. Before `exec()`/`show()`, clamp
the popup position to `QScreen::availableGeometry()` and the containing window
geometry so the popover always stays on-screen and anchored correctly.
Centralize this (a helper) and apply to every `mapToGlobal(...)` + `exec()` /
`show()` site.

### 5. Keyboard shortcuts
Audit all `QShortcut` bindings in `src/ui/MainWindow.cpp` (the `bind`/`app_bind`
helpers around line 670). Fix any that don't fire or conflict with the
omnibox/text inputs (e.g. Ctrl+L, Ctrl+T, Ctrl+W, Ctrl+Shift+T, Ctrl+D,
Ctrl+F, F5/Ctrl+R, Ctrl+Shift+R, Alt+Left/Right, Ctrl+Shift+I/F12, Ctrl+J,
Ctrl+Shift+A, zoom Ctrl++/=/-, F11, Escape). Make sure shortcuts work while a
web page/CEF view has focus (not only when Qt chrome has focus). Add Ctrl+J
(open downloads dropdown) and Ctrl+Shift+A (tab search) wiring if missing.

## Hard rules
- UI/Qt code only this phase. Do NOT touch CEF download/extension/permission
  code, Windows native snap, or packaging/SFX — those are later phases.
- Don't push. Commit locally on `hybrid` with clear messages.
- Don't "fix" the tab text bug by raising the compact threshold to hide more.
  Root-cause and preserve content.
- Reference image-1 (downloads popover) and image-2 (tab text lost) in
  /home/user/workspace/uploaded_attachments/b972867bedd34b1e9ba35d5617f1dfcd/image.jpg
  and image-2.jpg — view them with the image tool to match the target look.

# Tabs, Reload, Internal Pages, and Extensions — Implementation Report

## Fixes

### 1. Accurate tab loading indicators

- Loading now begins and ends only from authoritative navigation-state callbacks (`has_nav_state` / CEF `OnLoadingStateChange`).
- Progress-only callbacks preserve the tab's existing loading bit instead of accidentally starting or stopping it.
- `TabSpinner` keeps loading state separate from Qt widget visibility, starts hidden, pauses while off-screen, and refuses to paint when idle.
- Crash and close paths explicitly clear loading state so failed or reopened tabs cannot retain a stale spinner.

### 2. Reload changes to Stop while loading

- The active tab's toolbar button uses the existing Stop/X icon while that tab is loading and returns to Reload when loading finishes.
- Clicking the loading-state button continues to invoke the webview stop callback; clicking it while idle invokes reload.
- Background-tab loading callbacks update only that tab's spinner and no longer alter the active tab's toolbar button.

### 3. Internal pages and shortcut/bookmark destinations open in new tabs

- `show_page()` now opens every `aurora://` destination in a new foreground tab.
- Links activated from native internal pages (including bookmarks, history, and download sources) use the same new-tab path.
- Internal and `chrome-extension://` URLs are preserved verbatim; search-like input is still resolved through the configured search engine.
- Normal HTTP/HTTPS navigation from web content remains same-tab.

### 4. Extension entries open their own options page

- Extension dropdown rows now open the selected extension rather than toggling it.
- Manifest targets are resolved in this order: `options_ui.page`, `options_page`, action/browser-action/page-action popup.
- Chromium-compatible extension IDs are derived from a manifest public key when present, otherwise from the normalized unpacked-extension path.
- Extensions without a usable options or popup target fall back to `aurora://extensions`; enable/disable controls remain on that management page.

### 5. Tab close button alignment

- The close control now has an explicit 20 px hit target, centered 10 px X glyph, zero inherited margins/padding, and a matching tab-layout slot.
- Local hover and pressed styling prevents broad application QSS rules from clipping or displacing the close icon.

### 6. Download popover auto-presentation

- The new-download branch lazily creates `DownloadsPopover` using the existing connection pattern, then calls `present()` immediately.
- Existing-download progress updates do not repeatedly reopen the popover.

## Files Changed

- `src/ui/MainWindow.hpp`
- `src/ui/MainWindow.cpp`
- `src/ui/TabBar.cpp`
- `src/ui/TabSpinner.hpp`
- `src/ui/TabSpinner.cpp`
- `src/ui/ExtensionsButton.hpp`
- `src/ui/ExtensionsButton.cpp`
- `src/ui/pages/InternalPageWidget.hpp`

No change was required in `Icons.*` because the existing `icons::Name::Stop` glyph already provides the required Stop/X affordance.

## Validation

- Ran `git diff --check` on all permitted UI files.
- Searched the changed paths for unresolved merge markers and stale loading/menu wiring.
- Confirmed the collaborating `DownloadsPopover::present()` declaration and implementation are present.
- Did not compile or run CMake/build tools, as required.

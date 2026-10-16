# Aurora — Chrome extensions

Aurora's engine is Chromium, embedded through CEF, so it uses Chromium's own
extension system. Aurora does **not** implement an extension runtime. What it
adds is a registry of the extensions you installed, plus the startup wiring
that hands the enabled ones to Chromium.

## How an extension gets loaded

1. **Registry.** `aurora://extensions` ("Load unpacked…", or a `.crx` file you
   open or download) registers an unpacked extension directory in the
   profile's `extensions` table (`src/extensions/ExtensionService.cpp`). A
   `.crx` / `.crx3` package is unpacked into `<profile>/extensions/` first.
   Each row has an enabled flag.
2. **Load list.** At startup, before CEF initializes and before any window
   exists, `src/app/main.cpp` reads `ExtensionService::enabled_paths()` and
   runs it through `extensions::build_load_list()`
   (`src/extensions/ExtensionLoadList.cpp`). That step drops:
   - directories that no longer exist or have no `manifest.json` (for
     example a "Load unpacked" source folder you deleted or moved). The row
     stays registered and listed; it just is not loaded;
   - paths containing a comma, which the switch cannot express;
   - empty paths and duplicates.

   Every skipped path is logged with the reason.
3. **Engine.** `cef::initialize` (`src/cef/CefLifecycle.cpp`) appends
   Chromium's `--load-extension=dir1,dir2,...` switch to the browser
   process command line in `OnBeforeCommandLineProcessing`. Chromium loads
   those directories during engine startup, the same way it does for
   `chromium --load-extension=...`. (Chrome-*branded* release builds have
   ignored this switch since M137. CEF is not Chrome-branded, so it still
   works.)

The extension wiring only exists in a CEF build (`AURORA_ENABLE_CEF=ON`). A
build without the engine has no extension runtime at all.

`aurora_browser --load-extension=DIR[,DIR...]` registers those directories
in the profile first, so they appear on `aurora://extensions` and use the
normal enabled/disabled bookkeeping. It is not a way around the registry.

## Restart required

Chromium reads the load list once, while the engine starts. CEF 128+ removed
the old Alloy runtime-loading API (`CefRequestContext::LoadExtension` /
`CefExtensionHandler`), and Aurora uses Alloy-style browser windows, so there
is no supported way to load or unload an extension in a running engine.
Installing, enabling, disabling or removing an extension therefore takes
effect **after a restart**, and the extensions page and toolbar menu say so.
Removing an extension does close its open popup right away.

## Incognito

Private windows never expose extensions. A private window drops its
`ExtensionService` (`core::IncognitoPolicy::enable_extensions()` in
`MainWindow`), so it has no extensions button, no popups or options pages,
and no `.crx` install path. Its tabs use a request context with an empty
`cache_path`, which CEF documents as "incognito mode". Chromium only runs an
extension in incognito if the user turned on "Allow in Incognito", and
Aurora has no control for that setting. This incognito behaviour has not been
verified on a desktop build as part of this change.

## Limits

- **No Chrome Web Store installs.** Aurora cannot install from the store or
  update from it. CEF does not expose Chrome's store install or update
  machinery.
- **No CRX signature verification.** When Aurora installs a `.crx` it checks
  the CRX2/CRX3 framing and unpacks the ZIP payload safely (it rejects
  traversal, links, encrypted entries and oversized expansion). It does
  **not** verify the package signature. Only install packages from a source
  you trust.
- **Only stock Chromium extension APIs.** Extensions get the `chrome.*` APIs
  that stock Chromium provides for this embedding: CEF's Chrome bootstrap
  with Alloy-style browsers. CEF documents the Chrome extension API as
  supported for Chrome-style browsers. In Alloy-style tabs, behaviour that
  depends on Chrome's tab/window UI (for example `chrome.tabs` acting on the
  current tab) is limited. Aurora injects a small compatibility bridge into
  extension popups for the current tab's title and URL (see `CefAdapter`),
  but it does not add new extension APIs.
- **Unpacked directories only.** The engine only loads directories. A `.crx`
  is unpacked before it is registered.
- **No live load or unload.** Changes need a restart, as described above.

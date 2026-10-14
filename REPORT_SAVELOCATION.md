# Save Location + Save Image As

## Files changed

- `src/settings/Settings.hpp`
- `src/settings/Settings.cpp`
- `src/ui/pages/SettingsPage.hpp`
- `src/ui/pages/SettingsPage.cpp`
- `src/cef/CefAdapter.cpp`
- `src/app/main.cpp`

## Settings

- Added `downloads.ask_where` as `PrefKeys::kDownloadsAskWhere`.
- Default is off through `PrefDefaults::kDownloadsAskWhere`.
- Added named `Settings` getters/setters for the ask-where flag and the existing
  `downloads.path` custom-directory preference.
- An empty `downloads.path` continues to mean the platform Downloads directory.

## Settings page

- Added **Ask where to save each file before downloading**.
- Added a **Choose…** button beside the download path. It uses
  `QFileDialog::getExistingDirectory`, starting at the configured path or the
  platform Downloads directory.
- Both controls persist immediately through `Settings`.

## Adapter integration

`main.cpp` installs three callbacks in the CEF adapter before constructing the
main window:

1. A live ask-where predicate backed by `Settings`.
2. A live custom-download-directory provider backed by `Settings`.
3. A save-path chooser that posts `QFileDialog::getSaveFileName` to the Qt UI
   event loop with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.

This callback approach keeps `aurora_cef` free of a direct Qt link while still
using the native Qt save dialog. The CEF `OnBeforeDownload` callback returns
before the blocking dialog opens. Its `CefBeforeDownloadCallback` is retained
in a `CefRefPtr` until the queued dialog finishes. Immutable download metadata
is copied instead of retaining `CefDownloadItem`, which CEF forbids holding
outside `OnBeforeDownload`.

## When `OnBeforeDownload` shows the dialog

The native save dialog is forced when either:

- `downloads.ask_where` is true; or
- the download was started by Aurora's **Save image as…** context-menu command.

The context-menu path records the image source URL before `StartDownload`.
`OnBeforeDownload` consumes that one-shot URL flag, checking both the final and
original download URL so redirects still match.

The dialog starts at:

`effective downloads directory / sanitized suggested filename`

The effective directory is the live `downloads.path` override when non-empty;
otherwise it is the platform directory already supplied through
`set_downloads_dir`.

On selection, the adapter calls `Continue(chosenPath, false)` and records the
chosen path for download progress/history. On cancel, it intentionally does
not call `Continue`; returning `true` from `OnBeforeDownload` leaves CEF to
cancel the uncontinued request.

Packaged `.crx`/`.crx3` extension-install downloads retain their pre-existing
private temporary-file routing and do not show the user save dialog. The
existing download-directory creation, write probe, collision avoidance, and
unusable-directory fallback remain in place.

## Validation

- Reviewed all edited branches and callback lifetimes.
- Confirmed no `CefDownloadItem` is retained asynchronously.
- Ran `git diff --check` successfully.
- Per task constraints, no compilation or build command was run.

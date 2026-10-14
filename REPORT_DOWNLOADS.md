# Downloads UX implementation report

## What changed

- Added OS actions to `DownloadService`:
  - Completed, existing files open with the default application through
    `QDesktopServices::openUrl(QUrl::fromLocalFile(...))`.
  - Windows reveal uses `QProcess::startDetached("explorer.exe",
    {"/select,", nativePath})`; other platforms open the containing folder.
  - Both actions reject missing rows, non-complete downloads, empty paths, and
    paths that do not identify an existing file.
- Added a non-toggling downloads-popover presentation path. It rebuilds the
  recent list, recalculates its size, repositions under the remembered
  downloads button (or discovers the host's downloads button on first use),
  shows, and raises the popup.
- Completed, available files in the popover now open when their row is clicked.
  Every row also has a small `QStyle::SP_DirIcon` reveal button; unavailable
  actions are disabled.
- The full downloads page now opens a completed, available file on one row
  click and has a trailing `QStyle::SP_DirIcon` reveal action for every row.
- The Qt-backed definitions of the two `DownloadService` OS actions are kept in
  `DownloadsPopover.cpp`, which is part of the Qt application target. This
  preserves the existing Qt-free `aurora_core` build without requiring a CMake
  change outside the files assigned to this task.

## Pinned public API

```cpp
// aurora::ui::DownloadsPopover — public slot
void present();

// aurora::downloads::DownloadService
bool open_file(int64_t id);
bool reveal_in_folder(int64_t id);
```

## MainWindow integration

After lazily constructing `downloads_popover_` with the existing constructor,
the new-download callback should call:

```cpp
downloads_popover_->present();
```

Do not call `toggle_under()` from the automatic new-download callback:
`toggle_under()` intentionally closes an already-visible popup, while
`present()` always refreshes and shows it. Existing toolbar/Ctrl+J wiring should
continue to call `toggle_under(downloads_btn_)`; that also records the canonical
anchor for later `present()` calls.

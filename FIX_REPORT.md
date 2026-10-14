# CEF Download Interruption Fix Report

## Root cause

Normal downloads and `.crx` downloads use the same CEF download callback and
both supplied absolute paths, so neither absolute paths in general nor callback
lifetime explains why only normal downloads were interrupted. The meaningful
difference is the destination: `.crx` files go to the process temp directory,
whereas normal files were forced into the configured Downloads directory after
checking only `std::filesystem::is_directory()`.

That check establishes that the path is a directory, not that Chromium can
create a file there. A redirected, protected, or ACL-restricted Windows
Downloads folder can therefore pass the check while rejecting the explicit
target; CEF then reports the transfer as interrupted. The normal path also
round-tripped UTF-8 Aurora/Qt strings through `std::filesystem::path::string()`
on Windows, even though native Windows filesystem paths and `CefString` are
UTF-16. That could corrupt a non-ASCII destination or filename independently of
the access problem.

## Fix

`src/cef/CefAdapter.cpp` now:

- creates, writes, closes, and removes a small probe file before treating the
  configured Downloads directory as usable;
- calls `Continue` with an empty path if that test fails, allowing CEF to use
  the suggested filename in its default destination rather than handing
  Chromium a target it cannot open;
- preserves the existing event flow: `OnDownloadUpdated` obtains the actual
  destination from `CefDownloadItem::GetFullPath()`, and the host updates the
  download record when that path becomes available;
- converts Aurora's UTF-8 path strings to native UTF-16 filesystem paths on
  Windows and passes native UTF-16 paths to CEF, including collision-generated
  filenames and the existing `.crx` temp path;
- logs `CefDownloadItem::GetInterruptReason()` when a download is interrupted,
  so any remaining access-denied / path failures are diagnosable from logs
  instead of a bare "interrupted" row.

The `.crx` routing and installation behavior is otherwise unchanged.

## Why this is correct

CEF 152 documents that `CefBeforeDownloadCallback::Continue` accepts either a
full destination path or a blank path; a blank path uses the suggested name and
default download directory:

https://cef-builds.spotifycdn.com/docs/152.0/classCefBeforeDownloadCallback.html

CEF 152 also documents that `CefDownloadItem::GetFullPath()` returns the full
path to the downloading or downloaded file, which makes it the authoritative
way to populate Aurora's record after CEF selects the fallback destination:

https://cef-builds.spotifycdn.com/docs/152.0/classCefDownloadItem.html

`CefDownloadHandler` documents the callback ordering and confirms that download
updates may occur before or after `OnBeforeDownload`, matching Aurora's existing
ID-based event/update handling:

https://cef-builds.spotifycdn.com/docs/152.0/classCefDownloadHandler.html

## Files changed

- `src/cef/CefAdapter.cpp`
- `FIX_REPORT.md`

## Verification

The affected control flow and final diff were reviewed statically. No build,
compiler, CMake, or platform toolchain was run, as required.

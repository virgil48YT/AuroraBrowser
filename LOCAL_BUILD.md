# Building Aurora locally on Windows (no GitHub Actions)

`build-windows-local.bat` reproduces `.github/workflows/build-windows.yml` on
your own machine. You get the same two builds CI produces:

- **Qt shell** (default) — the Qt6 browser chrome without CEF. Always builds and
  runs; web content shows a placeholder until CEF is wired.
- **Full browser + CEF** (`cef`) — real web content via Chromium/CEF. Best
  effort, like the CI job it mirrors.

## One-time prerequisites

| Tool | Install |
| --- | --- |
| Visual Studio 2022 | Workload: **Desktop development with C++** (provides MSVC + `vswhere`). Install from [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/) — not available via winget as a single C++ workload. |
| Python 3.13 | `winget install -e --id Python.Python.3.13` (the generic `Python.Python.3` ID was removed from winget — you must pick a version) |
| CMake 3.24+ | `winget install Kitware.CMake` |
| Ninja | `winget install Ninja-build.Ninja` |
| Git for Windows | `winget install Git.Git` (also provides the **bash** the helper scripts need) |
| 7-Zip | `winget install 7zip.7zip` (only needed for the self-extracting `.exe`) |
| Qt 6.8 | via **aqtinstall** (below) — there is no `Qt.Qt6` winget package |

Add CMake, Ninja, Git, 7-Zip, and Python to your PATH (the installers above do
this by default — close and reopen your shell after installing).

## Install Qt6 with aqtinstall

There is no Qt6 winget package, but `aqtinstall` downloads the official Qt
modules from the command line (it needs Python, so install Python first):

```bat
pip install aqtinstall
aqt install-qt windows desktop 6.8.0 win64_msvc2022_64 -O C:\Qt
```

This lands Qt at `C:\Qt\6.8.0\msvc2022_64`, which is exactly the `QT_ROOT`
the build script expects. (Prefer the latest 6.8.x? Use `6.8` instead of
`6.8.0` and aqtinstall resolves the newest patch.)

## Build

From the repo root in a plain **Command Prompt** (not "Run as Administrator" —
not needed):

```bat
:: Default: Qt shell, no CEF
build-windows-local.bat

:: Full browser with CEF
build-windows-local.bat cef
```

If Qt is not auto-detected, set it first:

```bat
set QT_ROOT=C:\Qt\6.8.0\msvc2022_64
build-windows-local.bat cef
```

Other overrides (all optional):

```bat
set PYTHON=python        :: if python3 isn't on your PATH
set SEVENZIP=C:\Program Files\7-Zip\7z.exe
```

## What the script does

For both modes it mirrors the workflow exactly: locates MSVC via `vswhere` and
calls `vcvars64`, adds Qt to `PATH`, configures with CMake+Ninja, builds, runs
the unit tests, then runs `windeployqt` to produce a runnable `package\` folder.
For `cef` it additionally runs `scripts/fetch_cef.sh` (via Git Bash) to
download the CEF binary distribution, copies the CEF `Resources` and `Release`
contents into the package, stamps a build ID with `aurora_buildid`, and
finally builds a single self-extracting `.exe` via
`packaging/windows/build_sfx.sh`.

### Output

- Qt shell: `Aurora-QtShell.exe` (or the `package\` folder if 7-Zip is missing)
- Full browser: `Aurora.exe`

Both double-click to run. The bootstrap launcher extracts to
`%LOCALAPPDATA%\Aurora\app` and starts `aurora_browser.exe`; profile data lives
in `%APPDATA%\Aurora\Aurora`.

## Notes

- The CEF build is allowed to fail, just like in CI — if `fetch_cef.sh` can't
  reach the CEF distribution index, fall back to the Qt shell build.
- `fetch_cef.sh` was patched to honor a `PYTHON` env var so it works under Git
  Bash on Windows (where `python3` is often unavailable).
- The SFX step is best-effort: if 7-Zip or the SFX module can't be found, the
  plain `package\` folder is still a runnable browser.

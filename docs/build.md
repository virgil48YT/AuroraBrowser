# Aurora — Build guide

## Prerequisites

### Core library + tests (runs on any OS, headless-friendly)
- C++20 compiler (GCC 13+, Clang 16+, or MSVC 2022)
- CMake 3.24+
- Ninja (recommended) or Make
- SQLite 3 (dev headers) — `libsqlite3-dev` on Debian/Ubuntu

### Full browser (Qt + CEF)
- Qt 6.5+ (modules: Core, Gui, Widgets, Network)
- A CEF binary distribution (see below)

## 1. Build the core and run the tests

This is what CI does. It needs only a C++20 compiler and SQLite:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAURORA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
# or directly: ./build/bin/aurora_tests
```

All 76 test cases (414 assertions) should pass.

### Headless chrome smoke test

With `-DAURORA_BUILD_APP=ON` the executable can exercise the whole chrome
without a display, including a large session restore:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/aurora_browser --seed-session=20 --smoke-test
```

`--seed-session=N` writes a synthetic N-tab session to the profile before the
restore runs, and `--smoke-test` fails (non-zero exit) if the restore showed
more than one web host, gave more than one host a native window, or left the
shown tab out of sync with the active tab. Point `XDG_DATA_HOME` at a scratch
directory to keep it away from a real profile.

## 2. Fetch CEF

CEF is distributed as prebuilt binaries (building Chromium from source is not
practical). A helper script downloads and unpacks a pinned build:

```bash
./scripts/fetch_cef.sh                 # auto-detects platform
# PLATFORM=windows64 ./scripts/fetch_cef.sh   # or linux64 / macosarm64 / macosx64
```

This unpacks CEF into `third_party/cef/`.
This stock distribution does not guarantee H.264/AAC. For Twitch playback,
build or supply a complete codec-enabled distribution and enable the configure
guard:

```bash
CEF_BRANCH=<release-branch> ./scripts/build_cef_codecs.sh
cmake -S . -B build -DAURORA_ENABLE_CEF=ON -DAURORA_BUILD_APP=ON \
  -DAURORA_REQUIRE_H264_AAC=ON \
  -DCEF_ROOT=$PWD/third_party/cef-codec
```

See [Twitch playback setup](twitch-playback.md) for source-build requirements,
licensing cautions, and runtime validation. CMake cannot add codecs to stock
CEF.

## 3. Install Qt6

**Linux (Debian/Ubuntu):** `sudo apt-get install qt6-base-dev`
**macOS (Homebrew):** `brew install qt6`
**Windows:** Install Qt 6.5+ via the Qt online installer, or `winget install
Qt.Qt6`.

## 4. Build the full browser

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_BUILD_TESTS=ON \
  -DAURORA_ENABLE_CEF=ON \
  -DAURORA_BUILD_APP=ON \
  -DCEF_ROOT=$PWD/third_party/cef
cmake --build build
./build/bin/aurora_browser
```

On first launch Aurora creates a profile directory under the platform's
standard app-data location (`~/.local/share/Aurora/` on Linux) and migrates
`profile.db`.

## CMake options

| Option | Default | Description |
| --- | --- | --- |
| `AURORA_BUILD_TESTS` | ON | Build the unit-test suite. |
| `AURORA_ENABLE_CEF` | OFF | Enable the CEF integration layer (needs `CEF_ROOT`). |
| `AURORA_BUILD_APP` | OFF | Build the Qt browser executable (needs Qt6). |
| `AURORA_WERROR` | OFF | Treat compiler warnings as errors (use in CI). |
| `AURORA_BUILD_BOOTSTRAP` | ON | Build `aurora_bootstrap`, the launcher that verifies the build ID and extracts CEF resources per version. |
| `AURORA_BUILD_TOOLS` | ON | Build `aurora_buildid`, the packaging tool that writes `aurora_build.manifest`. |

## Packaging and the build ID

A packaged build does not put CEF's resource files next to the executable: the
bootstrap launcher extracts them into `%LOCALAPPDATA%\Aurora\cef\<build_id>\`,
where `<build_id>` is a SHA-256 over the CEF runtime itself. Run

```bash
aurora_buildid generate --root package --cef-root third_party/cef
```

after assembling the package directory, and ship `aurora_bootstrap.exe` plus the
generated `aurora_build.manifest` inside it. See [build-id.md](build-id.md) for
the layout, the launcher's flags (`--shell`, `--aurora-verify-only`, ...) and the
resolution order the browser uses to find the resources.

## Vendoring SQLite (optional)

If you cannot install system SQLite headers, drop the SQLite amalgamation
(`sqlite3.c` + `sqlite3.h`) into `third_party/sqlite/` and the build will use
it automatically with WAL and thread safety enabled.

## Logging

Set `AURORA_LOG_LEVEL` to one of `trace`, `debug`, `info`, `warn`, `error`,
`fatal` to control verbosity. Default is `info` in release and `debug` in
debug builds. Startup milestones and scoped timings are emitted under the
`startup` and `perf` log tags.

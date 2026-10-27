#!/usr/bin/env bash
# packaging/windows/build_installer.sh
# Build the custom Aurora setup.exe from a staged package directory.
#
#   build_installer.sh <package_dir> <setup_ui.exe> <out_setup.exe> [version]
#
# The UI executable is built by CMake (AURORA_BUILD_INSTALLER=ON). This script
# zips the staged app, appends it to the UI, and writes the footer the
# installer reads at runtime. No external SFX module is involved, so the
# wizard can show real per-file progress and real Win32 errors.
set -euo pipefail

SRC_DIR="$1"; UI_EXE="$2"; OUT_EXE="$3"; VERSION="${4:-unknown}"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for required in aurora_browser.exe aurora_bootstrap.exe aurora_build.manifest; do
    if [ ! -f "$SRC_DIR/$required" ]; then
        echo "ERROR: package is missing $required" >&2
        exit 2
    fi
done
if [ ! -f "$UI_EXE" ]; then
    echo "ERROR: setup UI not found: $UI_EXE" >&2
    exit 2
fi

PYTHON=""
for candidate in python python3; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done
if [ -z "$PYTHON" ]; then
    echo "ERROR: Python is required to build the installer payload" >&2
    exit 2
fi

mkdir -p "$(dirname "$OUT_EXE")"
"$PYTHON" "$HERE/make_payload.py" "$SRC_DIR" "$WORK/payload.zip"
"$PYTHON" "$HERE/make_sfx.py" --ui "$UI_EXE" --payload "$WORK/payload.zip" \
    --out "$OUT_EXE"

echo "Built Aurora setup ${VERSION}: $OUT_EXE ($(wc -c < "$OUT_EXE") bytes)"

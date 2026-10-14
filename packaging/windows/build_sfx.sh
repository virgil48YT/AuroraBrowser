#!/usr/bin/env bash
# packaging/windows/build_sfx.sh
# Build a single self-extracting .exe for Aurora using the *modified* 7-Zip SFX
# module (OlegScherbakov/7zSFX), which supports InstallPath + OverwriteMode.
#
#   double-click  ->  extracts to %LOCALAPPDATA%\Aurora\app  ->  launches
#   aurora_bootstrap.exe, which verifies the build ID, materializes the CEF
#   resources into %LOCALAPPDATA%\Aurora\cef\<build_id>\ on the first launch
#   after an update, and then starts aurora_browser.exe.
#
# The payload therefore MUST contain aurora_bootstrap.exe and, for a CEF build,
# aurora_build.manifest (generate it with `aurora_buildid generate --root <dir>`
# after the CEF files have been copied into the package). Without a manifest the
# launcher simply runs the browser out of the flat directory, which is what the
# Qt-only artifact does.
#
# GUIMode="2" => fully silent: no extraction dialog, the browser just opens.
# OverwriteMode="2+8" => update older files, skip locked ones, so an update over
# a running browser never fails the extraction.
# User data (profile.db, cef_cache) persists in %APPDATA%\Aurora\Aurora
# (see src/app/main.cpp -> QStandardPaths::AppDataLocation), independent of the
# extraction folder.
#
# Usage: build_sfx.sh <src_dir> <out_exe> <icon.ico> [config.txt]
set -euo pipefail

SRC_DIR="$1"; OUT_EXE="$2"; ICON="$3"
CONFIG="${4:-$(cd "$(dirname "$0")" && pwd)/sfx_config.txt}"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# --- locate 7z (ships with 7-Zip on the Windows runner) ---
SEVENZIP=""
for c in "${SEVENZIP:-}" "/c/Program Files/7-Zip/7z.exe" "/c/Program Files (x86)/7-Zip/7z.exe"; do
    [ -n "${c:-}" ] && [ -x "$c" ] && SEVENZIP="$c" && break
done
if [ -z "$SEVENZIP" ]; then SEVENZIP="$(command -v 7z || command -v 7za || true)"; fi
if [ -z "$SEVENZIP" ]; then echo "ERROR: 7z not found" >&2; exit 2; fi

# --- locate or fetch the modified SFX module (runner doesn't ship 7zSD.sfx) ---
SFX=""
for c in "${SFX_MODULE:-}" "/c/Program Files/7-Zip/7zSD.sfx" "/c/Program Files/7-Zip/7zS.sfx"; do
    [ -n "${c:-}" ] && [ -f "$c" ] && SFX="$c" && break
done
if [ -z "$SFX" ]; then
    MOD_URL="https://raw.githubusercontent.com/OlegScherbakov/7zSFX/master/files/7zsd_lzma2_170_3900.7z"
    echo "Fetching modified SFX module from $MOD_URL"
    curl -fsSL -o "$WORK/mod.7z" "$MOD_URL"
    "$SEVENZIP" x -y -o"$WORK/mod" "$WORK/mod.7z" >/dev/null
    SFX="$(find "$WORK/mod" -name '*.sfx' | head -1)"
fi
if [ -z "$SFX" ] || [ ! -f "$SFX" ]; then echo "ERROR: SFX module not found" >&2; exit 3; fi

cp "$SFX" "$WORK/sfx.sfx"

# --- best-effort: stamp the Aurora icon onto the module (before overlay) ---
if [ -f "$ICON" ]; then
    RCEDIT="${RCEDIT:-rcedit-x64.exe}"
    if command -v "$RCEDIT" >/dev/null 2>&1; then
        "$RCEDIT" "$WORK/sfx.sfx" --set-icon "$ICON" || echo "warning: rcedit icon patch skipped"
    fi
fi

# --- build the 7z payload (LZMA2 ultra) ---
# Archive the *contents* of the package dir (not the folder wrapper) so that
# aurora_browser.exe lands at the extraction root and RunProgram finds it.
( cd "$SRC_DIR" && "$SEVENZIP" a -t7z -mx=9 -mfb=273 -r "$WORK/payload.7z" . ) >/dev/null

# --- SFX config (UTF-8, no BOM) ---
cp "$CONFIG" "$WORK/config.txt"

# --- module + config + payload => single self-extracting .exe ---
cat "$WORK/sfx.sfx" "$WORK/config.txt" "$WORK/payload.7z" > "$OUT_EXE"

echo "Built SFX: $OUT_EXE ($(wc -c < "$OUT_EXE") bytes)"

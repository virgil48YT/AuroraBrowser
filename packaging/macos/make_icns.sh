#!/usr/bin/env bash
# packaging/macos/make_icns.sh SRC.png OUT.icns
# Build a macOS .icns from one square PNG with the stock sips + iconutil
# tools (both ship with macOS). Larger sizes are upscaled from the source.
set -euo pipefail
src="$1"; out="$2"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
set_dir="$work/Aurora.iconset"
mkdir -p "$set_dir"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$src" --out "$set_dir/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" "$src" --out "$set_dir/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$set_dir" -o "$out"

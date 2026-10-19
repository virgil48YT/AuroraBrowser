#!/usr/bin/env bash
# packaging/macos/package_macos.sh APP_BUNDLE OUT_DIR VERSION [ARCH]
#
# Turn a built Aurora.app into a distributable disk image:
#   1. macdeployqt copies the Qt frameworks and plugins into the bundle.
#   2. Code signing:
#        - MACOS_SIGN_IDENTITY set ("Developer ID Application: ..."): every
#          nested helper, the CEF framework and the app are signed inside-out
#          with the hardened runtime and packaging/macos/entitlements.plist.
#        - otherwise: ad-hoc signature ("-"). Required for the bundle to run
#          on Apple silicon at all after macdeployqt rewrote it, but Gatekeeper
#          will still warn that the developer cannot be verified (right-click
#          > Open, or `xattr -dr com.apple.quarantine /Applications/Aurora.app`).
#   3. hdiutil builds Aurora-VERSION-macos-ARCH.dmg with an /Applications link.
#   4. Notarization runs only when APPLE_ID, APPLE_TEAM_ID and
#      APPLE_APP_PASSWORD (an app-specific password) are all set *and* a real
#      identity was used; the ticket is stapled to the DMG.
set -euo pipefail

APP="${1:?usage: package_macos.sh APP_BUNDLE OUT_DIR VERSION [ARCH]}"
OUT="${2:?missing OUT_DIR}"
VERSION="${3:?missing VERSION}"
ARCH="${4:-$(uname -m)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENTITLEMENTS="${HERE}/entitlements.plist"
NAME="Aurora"

[ -d "$APP" ] || { echo "error: $APP is not a directory" >&2; exit 1; }
mkdir -p "$OUT"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/${NAME}.app"
BUNDLE="$STAGE/${NAME}.app"

MACDEPLOYQT="${MACDEPLOYQT:-$(command -v macdeployqt || true)}"
[ -n "$MACDEPLOYQT" ] || { echo "error: macdeployqt not found (set MACDEPLOYQT)" >&2; exit 1; }
"$MACDEPLOYQT" "$BUNDLE" -verbose=1

IDENTITY="${MACOS_SIGN_IDENTITY:-}"
sign() {
    if [ -n "$IDENTITY" ]; then
        codesign --force --timestamp --options runtime \
            --entitlements "$ENTITLEMENTS" --sign "$IDENTITY" "$1"
    else
        codesign --force --sign - "$1"
    fi
}

# Inside-out: Qt frameworks/plugins, CEF framework, helpers, then the app.
while IFS= read -r -d '' item; do sign "$item"; done < <(
    find "$BUNDLE/Contents/PlugIns" "$BUNDLE/Contents/Frameworks" \
        \( -name '*.dylib' -o -name '*.so' \) -type f -print0 2>/dev/null)
while IFS= read -r -d '' fw; do sign "$fw"; done < <(
    find "$BUNDLE/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0 2>/dev/null)
while IFS= read -r -d '' helper; do sign "$helper"; done < <(
    find "$BUNDLE/Contents/Frameworks" -maxdepth 1 -name "${NAME} Helper*.app" -print0 2>/dev/null)
sign "$BUNDLE"
codesign --verify --deep --strict --verbose=2 "$BUNDLE"

DMG="$OUT/${NAME}-${VERSION}-macos-${ARCH}.dmg"
DMG_ROOT="$STAGE/dmg"
mkdir -p "$DMG_ROOT"
mv "$BUNDLE" "$DMG_ROOT/"
ln -s /Applications "$DMG_ROOT/Applications"
rm -f "$DMG"
hdiutil create -volname "$NAME" -srcfolder "$DMG_ROOT" -ov -format UDZO "$DMG"
if [ -n "$IDENTITY" ]; then
    codesign --force --timestamp --sign "$IDENTITY" "$DMG"
fi

if [ -n "$IDENTITY" ] && [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] \
        && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
    xcrun notarytool submit "$DMG" --apple-id "$APPLE_ID" \
        --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD" --wait
    xcrun stapler staple "$DMG"
    echo "notarized: $DMG"
else
    echo "note: $DMG is not notarized (needs MACOS_SIGN_IDENTITY + APPLE_ID/APPLE_TEAM_ID/APPLE_APP_PASSWORD)"
fi
( cd "$OUT" && shasum -a 256 "$(basename "$DMG")" > "$(basename "$DMG").sha256" )
echo "$DMG"

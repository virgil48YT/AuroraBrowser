#!/usr/bin/env bash
# packaging/linux/package_linux.sh BUILD_DIR CEF_ROOT OUT_DIR VERSION
#
# Package a CEF-enabled Linux build of Aurora as
#   Aurora-VERSION-linux-x86_64.tar.gz   portable folder (run ./aurora)
#   Aurora-VERSION-linux-x86_64.AppImage single-file (needs FUSE 2 or
#                                         --appimage-extract-and-run)
#
# Layout (AppDir, also the content of the tarball):
#   usr/bin/aurora_browser       the browser, RUNPATH $ORIGIN:$ORIGIN/../lib
#   usr/bin/libcef.so, *.pak, icudtl.dat, v8_context_snapshot.bin, locales/,
#     libvk_swiftshader.so ...   CEF runtime — Chromium resolves its resources
#                                and GL/Vulkan libraries next to libcef.so
#   usr/lib/, usr/plugins/       Qt 6 libraries and plugins (linuxdeploy)
#   usr/share/applications/aurora.desktop, usr/share/icons/.../aurora.png
#   AppRun / aurora              launchers
#
# Tools: linuxdeploy + linuxdeploy-plugin-qt are downloaded unless
# LINUXDEPLOY / LINUXDEPLOY_PLUGIN_QT point at local copies. QMAKE must point
# at the Qt 6 qmake used for the build. patchelf is required.
set -euo pipefail

BUILD="${1:?usage: package_linux.sh BUILD_DIR CEF_ROOT OUT_DIR VERSION}"
CEF="${2:?missing CEF_ROOT}"
OUT="${3:?missing OUT_DIR}"
VERSION="${4:?missing VERSION}"
ARCH="x86_64"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
NAME="Aurora-${VERSION}-linux-${ARCH}"

EXE="${BUILD}/bin/aurora_browser"
[ -x "$EXE" ] || { echo "error: $EXE not found" >&2; exit 1; }
[ -f "${CEF}/Release/libcef.so" ] || { echo "error: ${CEF}/Release/libcef.so not found" >&2; exit 1; }
command -v patchelf >/dev/null || { echo "error: patchelf is required" >&2; exit 1; }

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
APPDIR="${WORK}/AppDir"
mkdir -p "${APPDIR}/usr/bin" "${APPDIR}/usr/share/applications" \
         "${APPDIR}/usr/share/icons/hicolor/256x256/apps"

install -m 0755 "$EXE" "${APPDIR}/usr/bin/aurora_browser"
# CEF runtime. chrome-sandbox IS shipped (not setuid — a package cannot
# grant that): Aurora prefers the user-namespace sandbox, which needs no
# helper, and uses chrome-sandbox only when an administrator has made it
# root-owned 4755 (sudo chown root:root chrome-sandbox && sudo chmod 4755
# chrome-sandbox in an extracted tarball). Inside the AppImage it can never be
# setuid (FUSE mounts are nosuid), so the AppImage relies on user namespaces.
# See docs/security.md and src/cef/SandboxPolicy.hpp.
cp -a "${CEF}/Release/." "${APPDIR}/usr/bin/"
cp -a "${CEF}/Resources/." "${APPDIR}/usr/bin/"
rm -f "${APPDIR}/usr/bin/"*.a
install -m 0644 "${HERE}/aurora.desktop" "${APPDIR}/usr/share/applications/aurora.desktop"
install -m 0644 "${ROOT}/assets/icons/aurora_256.png" \
    "${APPDIR}/usr/share/icons/hicolor/256x256/apps/aurora.png"

# Bundle Qt. libcef.so and its siblings are excluded: they already sit in
# usr/bin and are 100s of MB — a second copy in usr/lib would double the
# package for nothing.
TOOLS="${WORK}/tools"; mkdir -p "$TOOLS"
fetch_tool() {
    local var="$1" url="$2" dest="$3"
    if [ -n "${!var:-}" ]; then echo "${!var}"; return; fi
    curl -fsSL "$url" -o "$dest"; chmod +x "$dest"; echo "$dest"
}
LD_BIN="$(fetch_tool LINUXDEPLOY \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
    "${TOOLS}/linuxdeploy-x86_64.AppImage")"
LDQ_BIN="$(fetch_tool LINUXDEPLOY_PLUGIN_QT \
    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage \
    "${TOOLS}/linuxdeploy-plugin-qt-x86_64.AppImage")"
# linuxdeploy discovers plugins on PATH by name.
ln -sf "$LDQ_BIN" "${TOOLS}/linuxdeploy-plugin-qt"
export PATH="${TOOLS}:${PATH}"
export APPIMAGE_EXTRACT_AND_RUN=1
export LD_LIBRARY_PATH="${APPDIR}/usr/bin${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export EXTRA_PLATFORM_PLUGINS="libqxcb.so;libqoffscreen.so"
export EXTRA_QT_MODULES="svg"
EXCLUDES=()
for lib in "${APPDIR}"/usr/bin/*.so "${APPDIR}"/usr/bin/*.so.*; do
    [ -e "$lib" ] && EXCLUDES+=(--exclude-library "$(basename "$lib")")
done

# System libraries Chromium must take from the host, never from the bundle:
# NSS loads libsoftokn3/libfreebl3 with dlopen from its own directory, so a
# bundled libnss3 without them aborts at startup (FATAL nss_util.cc); GBM/DRM,
# ALSA, CUPS and AT-SPI must match the host's drivers and daemons. They are
# listed in the README as runtime requirements, like for any Chromium build.
HOST_LIBS=(libnss3.so libnssutil3.so libsmime3.so libssl3.so libsoftokn3.so
           libfreebl3.so libnspr4.so libplc4.so libplds4.so libgbm.so.1
           libdrm.so.2 libasound.so.2 libcups.so.2 libatk-1.0.so.0
           libatk-bridge-2.0.so.0 libatspi.so.0 libdbus-1.so.3)
for lib in "${HOST_LIBS[@]}"; do EXCLUDES+=(--exclude-library "${lib}*"); done

"$LD_BIN" --appdir "$APPDIR" \
    --executable "${APPDIR}/usr/bin/aurora_browser" \
    --desktop-file "${APPDIR}/usr/share/applications/aurora.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/aurora.png" \
    "${EXCLUDES[@]}" \
    --plugin qt

for lib in "${HOST_LIBS[@]}"; do rm -f "${APPDIR}/usr/lib/${lib}"*; done

# linuxdeploy rewrites the executable's RUNPATH to $ORIGIN/../lib; CEF also
# needs $ORIGIN (libcef.so lives beside the binary).
patchelf --set-rpath '$ORIGIN:$ORIGIN/../lib' "${APPDIR}/usr/bin/aurora_browser"

# The stock linux64 CEF distribution ships libcef.so with its symbol table
# (~1.4 GB); stripping it brings the runtime to a few hundred MB.
# STRIP_CEF=0 keeps the symbols for debugging crashes.
if [ "${STRIP_CEF:-1}" = "1" ]; then
    strip --strip-unneeded "${APPDIR}/usr/bin/libcef.so"
fi

# Launchers. AppRun is what the AppImage runtime executes; ./aurora is the
# entry point of the extracted tarball. Both just exec the binary with the
# AppDir's Qt plugins.
# linuxdeploy leaves AppRun as a symlink to the binary: remove it first, or
# the redirect below would write the script over aurora_browser itself.
rm -f "${APPDIR}/AppRun" "${APPDIR}/aurora"
cat > "${APPDIR}/AppRun" <<'EOS'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export QT_PLUGIN_PATH="${HERE}/usr/plugins${QT_PLUGIN_PATH:+:${QT_PLUGIN_PATH}}"
exec "${HERE}/usr/bin/aurora_browser" "$@"
EOS
chmod 0755 "${APPDIR}/AppRun"
cp "${APPDIR}/AppRun" "${APPDIR}/aurora"
cat > "${APPDIR}/README.txt" <<EOS
Aurora ${VERSION} for Linux (x86_64)

Run ./aurora from this folder. Requires an X11 or Wayland desktop (Wayland
sessions run Aurora through XWayland), glibc 2.31+ and the usual desktop
libraries (NSS, GTK/ATK, ALSA, CUPS) that every Chromium browser needs.
EOS

# Portable tarball of the AppDir.
TAR_DIR="${WORK}/${NAME}"
cp -a "$APPDIR" "$TAR_DIR"
tar -C "$WORK" -czf "${OUT}/${NAME}.tar.gz" "${NAME}"

# AppImage.
APPIMAGETOOL="${APPIMAGETOOL:-}"
if [ -z "$APPIMAGETOOL" ]; then
    APPIMAGETOOL="${TOOLS}/appimagetool-x86_64.AppImage"
    curl -fsSL https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
        -o "$APPIMAGETOOL"
    chmod +x "$APPIMAGETOOL"
fi
ARCH="$ARCH" "$APPIMAGETOOL" --no-appstream "$APPDIR" "${OUT}/${NAME}.AppImage"

( cd "$OUT" && sha256sum "${NAME}.tar.gz" "${NAME}.AppImage" > "${NAME}.sha256" )
ls -la "$OUT"

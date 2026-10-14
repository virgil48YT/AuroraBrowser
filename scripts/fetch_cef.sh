#!/usr/bin/env bash
# scripts/fetch_cef.sh — download and unpack a CEF binary distribution.
#
# CEF cannot be built from source in a reasonable time; the official workflow is
# to download a prebuilt binary package. This script queries the live CEF
# distribution index, picks the latest *stable* minimal build for the current
# platform, and unpacks it into third_party/.
#
# Usage:
#   ./scripts/fetch_cef.sh                 # auto-detect platform
#   PLATFORM=windows64 ./scripts/fetch_cef.sh
#   PLATFORM=linux64 ./scripts/fetch_cef.sh
#   PLATFORM=macosarm64 ./scripts/fetch_cef.sh
#   CEF_CHANNEL=stable PLATFORM=windows64 ./scripts/fetch_cef.sh   # or beta/dev
#   CEF_VERSION_MAJOR=151 ./scripts/fetch_cef.sh   # pin a Chromium major
#
# This downloads the stock CEF distribution. It may not contain H.264/AAC.
# For those codecs, build a custom distribution with build_cef_codecs.sh or
# supply a compatible third-party build whose licensing you have reviewed.
#
# Channel note: CEF labels a build 'stable' only once Chromium's extended-stable
# line catches up, which lags the Chrome on your desktop by a major or two — CEF
# was still publishing Chromium 151 as 'stable' after Chrome 152 had shipped to
# every desktop. The default channel here is therefore 'auto': take the highest
# Chromium version CEF publishes for the platform, whichever channel it sits on,
# so Aurora tracks the Chromium people actually run. Pass CEF_CHANNEL=stable to
# stay on CEF's conservative line, or CEF_VERSION_MAJOR=NNN to pin a major.
#
# Then configure with:
#   cmake -S . -B build -DAURORA_ENABLE_CEF=ON -DCEF_ROOT=$PWD/third_party/cef
set -euo pipefail

BASE="https://cef-builds.spotifycdn.com"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST="${ROOT}/third_party/cef"
mkdir -p "${DEST}"

if [ -z "${PLATFORM:-}" ]; then
    case "$(uname -s)" in
        Linux)  PLATFORM="linux64" ;;
        Darwin) PLATFORM="macosarm64" ;;   # intel macs: macosx64
        MINGW*|MSYS*|CYGWIN*) PLATFORM="windows64" ;;
        *) echo "Unsupported OS; set PLATFORM=windows64|linux64|macosarm64"; exit 1 ;;
    esac
fi
CHANNEL="${CEF_CHANNEL:-auto}"
MAJOR="${CEF_VERSION_MAJOR:-}"

# Resolve the latest stable minimal build for this platform from the live index.
# Falls back gracefully if the requested channel/platform has no minimal file.
PYTHON_BIN="${PYTHON:-python3}"
read -r FILE_URL CEF_VERSION < <("$PYTHON_BIN" - "${BASE}" "${PLATFORM}" "${CHANNEL}" "${MAJOR}" <<'PYEOF'
import json, sys, urllib.request
base, platform, channel, major = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
with urllib.request.urlopen(f"{base}/index.json", timeout=60) as r:
    idx = json.load(r)
versions = idx.get(platform, {}).get("versions", [])
if channel == "auto":
    # Newest Chromium on any published channel. 'dev' is excluded: it tracks
    # Chromium's canary-ish line and is not something to ship a browser on.
    versions = [v for v in versions if v.get("channel") in ("stable", "beta")]
else:
    versions = [v for v in versions if v.get("channel") == channel]


def chromium_key(v):
    # "152.0.7977.54" -> (152, 0, 7977, 54), compared numerically. The index is
    # ordered by publication date and NOT by version, so taking the first entry
    # picked an arbitrary build - which is how this kept fetching Chromium 144
    # months after 151 had shipped on the same channel.
    try:
        return tuple(int(p) for p in v.get("chromium_version", "0").split("."))
    except ValueError:
        return (0,)


if major:
    versions = [v for v in versions
                if v.get("chromium_version", "").split(".")[0] == major]
for v in sorted(versions, key=chromium_key, reverse=True):
    for f in v.get("files", []):
        if f.get("type") == "minimal":
            print(f"{base}/{f['name']} {v['cef_version']}")  # noqa: T201
            sys.exit(0)
sys.exit(1)
PYEOF
)

if [ -z "${FILE_URL:-}" ]; then
    echo "No ${CHANNEL} minimal CEF build found for ${PLATFORM}${MAJOR:+ (major ${MAJOR})}."
    echo "Check ${BASE}/index.html and pass PLATFORM=<...> CEF_CHANNEL=<stable|dev>."
    exit 1
fi

TARBALL="${DEST}/cef.tarball"
echo "Downloading CEF ${CEF_VERSION} (${PLATFORM}, ${CHANNEL})..."
echo "  ${FILE_URL}"
if ! curl -fL "${FILE_URL}" -o "${TARBALL}"; then
    echo "Download failed. See ${BASE}/index.html"
    rm -f "${TARBALL}"
    exit 1
fi

echo "Unpacking..."
# The index ships both .tar.bz2 and .tar.xz; let tar pick the decompressor.
tar -xf "${TARBALL}" -C "${DEST}" --strip-components=1
rm -f "${TARBALL}"

cat > "${DEST}/VERSION.txt" <<EOF
CEF ${CEF_VERSION} (${PLATFORM}, requested channel: ${CHANNEL})
Source: ${FILE_URL}
EOF

echo "Done. CEF is at: ${DEST}"
echo "NOTE: This stock distribution does not guarantee H.264/AAC support."
echo "      See scripts/build_cef_codecs.sh and docs/twitch-playback.md."
echo "Configure with:"
echo "  cmake -S ${ROOT} -B ${ROOT}/build -DAURORA_ENABLE_CEF=ON -DCEF_ROOT=${DEST}"

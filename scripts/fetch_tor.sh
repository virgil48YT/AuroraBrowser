#!/usr/bin/env bash
# Fetch the Tor Expert Bundle that Aurora ships as its built-in Tor.
#
#   scripts/fetch_tor.sh [platform] [dest]
#
# platform: linux64 | windows64 | macosarm64 | macosx64 (Aurora's platform
#           tags; default: this machine). dest defaults to
#           third_party/tor/<platform>, which CMake picks up automatically
#           (AURORA_TOR_DIR) and copies next to the browser as tor/.
#
# The archive comes from dist.torproject.org (falling back to the archive
# mirror) and is checked against the SHA-256 in that release's
# sha256sums-signed-build.txt. If gpg is available and the Tor Browser
# signing key is in the keyring, the checksum file's signature is verified
# too. Only tor itself (+ its libraries) and the GeoIP data are kept;
# pluggable transports and debug symbols are dropped.
#
# Override the release with TOR_BUNDLE_VERSION=15.0.25.
set -euo pipefail

VERSION="${TOR_BUNDLE_VERSION:-15.0.25}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

platform="${1:-}"
if [[ -z "$platform" ]]; then
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64) platform=linux64 ;;
        Darwin-arm64) platform=macosarm64 ;;
        Darwin-x86_64) platform=macosx64 ;;
        MINGW*|MSYS*|CYGWIN*) platform=windows64 ;;
        *) echo "fetch_tor: unsupported host $(uname -s)-$(uname -m); pass a platform" >&2; exit 2 ;;
    esac
fi
case "$platform" in
    linux64)    bundle=linux-x86_64 ;;
    windows64)  bundle=windows-x86_64 ;;
    macosarm64) bundle=macos-aarch64 ;;
    macosx64)   bundle=macos-x86_64 ;;
    *) echo "fetch_tor: unknown platform '$platform'" >&2; exit 2 ;;
esac
dest="${2:-$ROOT/third_party/tor/$platform}"

name="tor-expert-bundle-${bundle}-${VERSION}.tar.gz"
mirrors=(
    "https://dist.torproject.org/torbrowser/${VERSION}"
    "https://archive.torproject.org/tor-package-archive/torbrowser/${VERSION}"
)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch() { # url out
    curl --fail --location --silent --show-error --retry 3 --connect-timeout 20 -o "$2" "$1"
}

base=""
for m in "${mirrors[@]}"; do
    if fetch "$m/sha256sums-signed-build.txt" "$work/sums.txt" 2>/dev/null; then
        base="$m"
        break
    fi
done
if [[ -z "$base" ]]; then
    echo "fetch_tor: Tor ${VERSION} not found on any mirror" >&2
    exit 1
fi

echo "fetch_tor: downloading ${name} from ${base}"
fetch "$base/$name" "$work/$name"

expected="$(awk -v n="$name" '$2 == n || $2 == "*"n {print $1}' "$work/sums.txt" | head -n1)"
if [[ -z "$expected" ]]; then
    echo "fetch_tor: ${name} is not listed in sha256sums-signed-build.txt" >&2
    exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$work/$name" | awk '{print $1}')"
else
    actual="$(shasum -a 256 "$work/$name" | awk '{print $1}')"
fi
if [[ "$expected" != "$actual" ]]; then
    echo "fetch_tor: checksum mismatch for ${name}: expected ${expected}, got ${actual}" >&2
    exit 1
fi
echo "fetch_tor: sha256 OK (${actual})"

if command -v gpg >/dev/null 2>&1 && fetch "$base/sha256sums-signed-build.txt.asc" "$work/sums.txt.asc" 2>/dev/null; then
    if gpg --batch --verify "$work/sums.txt.asc" "$work/sums.txt" >/dev/null 2>&1; then
        echo "fetch_tor: checksum file signature OK"
    else
        echo "fetch_tor: note: could not verify the checksum file's signature (Tor Browser key not in keyring)"
    fi
fi

mkdir -p "$work/x"
tar -xzf "$work/$name" -C "$work/x"
if [[ ! -d "$work/x/tor" ]]; then
    echo "fetch_tor: unexpected archive layout (no tor/ directory)" >&2
    exit 1
fi

rm -rf "$dest"
mkdir -p "$dest/tor" "$dest/data" "$dest/docs"
# tor + its shared libraries (no pluggable transports, no debug symbols).
find "$work/x/tor" -maxdepth 1 -type f -exec cp {} "$dest/tor/" \;
for f in geoip geoip6; do
    [[ -f "$work/x/data/$f" ]] && cp "$work/x/data/$f" "$dest/data/"
done
[[ -d "$work/x/docs" ]] && cp "$work/x/docs/"*.txt "$dest/docs/" 2>/dev/null || true
chmod -R u+rwX,go+rX "$dest"
chmod +x "$dest/tor/"* 2>/dev/null || true
echo "$VERSION" > "$dest/VERSION"
echo "fetch_tor: Tor Expert Bundle ${VERSION} (${bundle}) -> ${dest}"

#!/usr/bin/env bash
# Aurora — refresh the profile's ad-block filter list.
#
# Downloads EasyList (the standard Chrome/Firefox filter list) into the
# profile directory as adblock-rules.txt, where the browser merges it with
# the built-in starter rules at startup (adblock::load_rules).
#
# EasyList is licensed CC-BY-SA 3.0 (https://easylist.to/pages/licence.html):
# redistribution is fine with attribution, which is why the file is fetched
# by the user rather than committed to the repository.
#
# Usage:
#   scripts/fetch_filter_lists.sh [profile-dir]
#
# Without an argument the default profile directory is used: the SAME directory
# the browser reads, i.e. QStandardPaths::AppDataLocation for organization
# "Aurora" / application "Aurora" (set in src/app/main.cpp):
#
#   Windows  %APPDATA%/Aurora/Aurora        (…/AppData/Roaming/Aurora/Aurora)
#   macOS    ~/Library/Application Support/Aurora
#   Linux    ${XDG_DATA_HOME:-~/.local/share}/Aurora/Aurora
#
# This used to default to ~/.aurora/profile, which is not the profile directory
# on any platform — so the download landed where the browser never looks and
# Aurora kept running on the built-in starter rules only. main.cpp still checks
# that legacy path as a fallback, so an old fetch is not wasted.

set -euo pipefail

# The profile directory the running browser uses on this platform.
default_profile_dir() {
    case "$(uname -s 2>/dev/null || echo unknown)" in
        MINGW*|MSYS*|CYGWIN*|Windows_NT)
            # Git Bash / MSYS: prefer %APPDATA%, and normalize the backslashes
            # Windows hands out so mkdir/curl get a usable path.
            local appdata="${APPDATA:-}"
            if [ -z "$appdata" ]; then
                appdata="${USERPROFILE:-$HOME}/AppData/Roaming"
            fi
            printf '%s\n' "${appdata//\\//}/Aurora/Aurora"
            ;;
        Darwin)
            # Qt uses the application name only on macOS (no org directory).
            printf '%s\n' "$HOME/Library/Application Support/Aurora"
            ;;
        *)
            printf '%s\n' "${XDG_DATA_HOME:-$HOME/.local/share}/Aurora/Aurora"
            ;;
    esac
}

PROFILE_DIR="${1:-$(default_profile_dir)}"
LIST_URL="https://easylist.to/easylist/easylist.txt"
TARGET="$PROFILE_DIR/adblock-rules.txt"

mkdir -p "$PROFILE_DIR"

echo "Downloading EasyList from $LIST_URL ..."
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$LIST_URL" -o "$TARGET.part"
elif command -v wget >/dev/null 2>&1; then
    wget -qO "$TARGET.part" "$LIST_URL"
else
    echo "error: need curl or wget to download the filter list" >&2
    exit 1
fi

# Basic sanity check: a filter list is text and EasyList is never tiny.
if [ ! -s "$TARGET.part" ] || [ "$(wc -c < "$TARGET.part")" -lt 10000 ]; then
    echo "error: downloaded file looks empty or truncated" >&2
    rm -f "$TARGET.part"
    exit 1
fi

mv "$TARGET.part" "$TARGET"
echo "Wrote $TARGET ($(wc -l < "$TARGET") rules)."
echo "Restart Aurora to pick it up."

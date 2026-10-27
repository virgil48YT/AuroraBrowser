// Aurora — release tag comparison.
//
// The updater trusts the release tag to decide whether a download is newer
// than the running build, so this parser is deliberately strict: anything that
// is not a plain semantic version (with an optional v prefix, prerelease and
// build metadata) is rejected instead of being guessed at. Qt-free so the unit
// tests can cover it on every platform.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace aurora::updates {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    // Dot-separated prerelease identifiers, without the leading '-'.
    std::string prerelease;
};

// Parses "0.2.0", "v0.2.0", "0.2.0-rc.1" and "0.2.0+build.7". Returns
// nullopt for non-versions (branch names, "latest", ...).
std::optional<Version> parse_version(std::string_view text);

// Returns <0, 0 or >0 using semantic-version precedence. Build metadata is
// ignored; a prerelease sorts before the corresponding release.
int compare_versions(const Version& a, const Version& b);

// Convenience for the updater: true only when both strings parse and the
// candidate is strictly newer than the installed version.
bool is_newer_version(std::string_view candidate, std::string_view installed);

} // namespace aurora::updates

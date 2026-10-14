// Aurora — build identity for the CEF runtime.
//
// Aurora ships as a single self-extracting executable whose payload contains a
// full CEF binary distribution (libcef.dll, the .pak resource bundles,
// icudtl.dat, the V8 snapshots, the locale packs). Extracting that payload into
// one flat directory is how a browser breaks after an update: Chromium refuses
// to start — or worse, starts and renders wrong — when a resource .pak comes
// from one CEF version and libcef.dll from another. The failure mode is silent
// and the fix ("delete this folder") is not something a user can be expected to
// discover.
//
// The build ID solves that by giving every distinct CEF runtime a name derived
// from its own bytes:
//
//   build_id = SHA-256( canonical manifest text )   truncated to 16 hex chars
//
// The canonical text lists the manifest format version, the Aurora version, the
// CEF version string, the platform, and one line per runtime file with its
// role, size and SHA-256 — sorted by path, so it is byte-for-byte reproducible
// on any machine that has the same files. Change CEF, and every .pak hash and
// therefore the build ID changes; rebuild Aurora against the same CEF and the
// build ID does not move, so the extracted tree is reused.
//
// Files carry a role:
//
//   Resource  the file is *materialized* into the per-version directory
//             (%LOCALAPPDATA%\Aurora\cef\<build_id>\) and handed to CEF via
//             resources_dir_path / locales_dir_path.
//   Identity  the file contributes to the build ID but is loaded from the
//             application directory by the OS loader (libcef.dll and friends).
//             Hashing it is what makes "same resources, different engine" a
//             different build ID.
//
// This header is part of aurora_core: no Qt, no CEF, no OpenSSL. The packaging
// tool that writes the manifest (tools/buildid_main.cpp) and the bootstrap
// launcher that verifies it at runtime (src/bootstrap/main.cpp) both call these
// functions, which is the only reason the two can be trusted to agree.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::core {

// Length of a build ID in hex characters. 16 hex chars = 64 bits of SHA-256,
// which is short enough for a directory name a human can read out over the
// phone and far beyond collision range for the handful of CEF builds a single
// installation will ever see.
inline constexpr size_t kBuildIdLength = 16;

// The on-disk manifest format. Bumped only for incompatible changes; the
// version is part of the hashed text, so a bump deliberately invalidates every
// existing extraction.
inline constexpr int kManifestFormatVersion = 1;

// File name of the manifest shipped next to the executable.
inline constexpr char kManifestFileName[] = "aurora_build.manifest";

// Stamp file written at the root of a fully materialized per-version directory.
// Its presence *and* content (the build ID) is what lets the launcher skip the
// extraction on every subsequent start; it is written last, so a directory
// interrupted halfway through has no stamp and is re-materialized.
inline constexpr char kStampFileName[] = ".aurora-build-id";

enum class FileRole {
    Resource,  // materialized into the per-version directory
    Identity,  // hashed only; loaded from the application directory
};

std::string_view file_role_name(FileRole role);
std::optional<FileRole> file_role_from_name(std::string_view name);

struct BuildFile {
    std::string path;    // relative, forward slashes, e.g. "locales/en-US.pak"
    uint64_t size = 0;
    std::string sha256;  // lowercase hex, 64 chars
    FileRole role = FileRole::Resource;
};

struct BuildManifest {
    int format_version = kManifestFormatVersion;
    std::string app_version;   // "0.1.0"
    std::string cef_version;   // "141.0.5+g1234567+chromium-141.0.7390.55"
    std::string platform;      // "windows64", "linux64", ...
    std::vector<BuildFile> files;
    // Filled by finalize() / parse_manifest(). Never part of the hashed text.
    std::string build_id;
};

// Sorts `files` by path, drops duplicate paths (last wins), and recomputes
// `build_id`. Call this before serializing or comparing.
void finalize(BuildManifest& manifest);

// The exact bytes the build ID is computed over. Deterministic: LF newlines,
// no trailing whitespace, files sorted by path. Does NOT include the build ID.
std::string canonical_manifest_text(const BuildManifest& manifest);

// SHA-256 of canonical_manifest_text(), truncated to kBuildIdLength hex chars.
std::string compute_build_id(const BuildManifest& manifest);

// canonical_manifest_text() plus a trailing "build-id <id>" line. This is what
// is written to disk as aurora_build.manifest.
std::string serialize_manifest(const BuildManifest& manifest);

// Parses a manifest. Returns nullopt on a malformed document or an unsupported
// format version. Unknown keys are ignored so a newer manifest that only adds
// metadata still loads; anything structural is guarded by format_version.
std::optional<BuildManifest> parse_manifest(std::string_view text);

// True for exactly kBuildIdLength lowercase hex characters. Used to validate
// anything that is about to become a directory name.
bool is_valid_build_id(std::string_view id);

// --- Filesystem-facing helpers ----------------------------------------------
// These touch the filesystem through <filesystem>; they live here (rather than
// in the launcher) so they are covered by the unit tests.

// Scans `root` and returns a manifest for the files matching `resource_globs`
// (role Resource) and `identity_globs` (role Identity). Patterns are matched
// against the relative, forward-slash path with `*` and `?` wildcards; `*`
// does not cross '/' unless the pattern contains "**". Files matching neither
// list are ignored, which is what keeps Qt DLLs and the browser executable out
// of the CEF identity.
BuildManifest scan_build_tree(const std::string& root,
                              const std::vector<std::string>& resource_globs,
                              const std::vector<std::string>& identity_globs);

// Glob matcher used by scan_build_tree, exposed for testing.
bool glob_match(std::string_view pattern, std::string_view text);

enum class VerifyMode {
    // Presence and size only: one stat() per file. Used on the hot path (every
    // launch after the first) where the stamp already asserts provenance.
    Quick,
    // Presence, size and SHA-256 of every file. Used when the stamp is missing
    // or mismatched, and by `aurora_buildid verify`.
    Full,
};

struct VerifyReport {
    bool ok = false;
    std::vector<std::string> missing;
    std::vector<std::string> wrong_size;
    std::vector<std::string> wrong_hash;
    size_t checked = 0;

    // A single-line, log-friendly summary.
    std::string summary() const;
};

// Verifies that the Resource files of `manifest` exist under `root` with the
// recorded size (and, in Full mode, the recorded hash). Identity files are not
// checked: they are not materialized there.
VerifyReport verify_resource_tree(const std::string& root,
                                  const BuildManifest& manifest,
                                  VerifyMode mode);

// Reads the stamp file at `root`/kStampFileName. Returns the build ID it
// contains, or nullopt when the file is absent or does not hold a valid ID.
std::optional<std::string> read_stamp(const std::string& root);

// Writes the stamp file. Returns false if it could not be written, in which
// case the directory is deliberately left un-stamped so the next launch
// re-materializes it rather than trusting a half-built tree.
bool write_stamp(const std::string& root, const std::string& build_id);

} // namespace aurora::core

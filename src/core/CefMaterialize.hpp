// Aurora — materializing a per-version CEF resource tree.
//
// The bootstrap launcher calls this once per launch. On the first launch after
// an update it populates %LOCALAPPDATA%\Aurora\cef\<build_id>\ from the files
// that the SFX dropped next to the executable; on every launch after that it
// confirms the tree is intact and returns immediately.
//
// Three properties matter, and they are the reason this is not a shell copy:
//
//   * Cheap. Resource files are hard-linked from the application directory when
//     the filesystem allows it (same volume, NTFS), so a 200 MB CEF payload
//     costs no extra disk space and no copy time. A copy is the fallback.
//   * Atomic per file. Each file is written to "<name>.aurora-tmp" and renamed
//     into place, so a launcher killed mid-way never leaves a truncated .pak
//     that would later pass a presence check.
//   * Self-invalidating. The completion stamp is written last and holds the
//     build ID. A directory without a matching stamp is rebuilt, and because
//     the directory name *is* a hash of the content, a rebuild can never mix
//     files from two CEF versions.
#pragma once

#include "core/BuildId.hpp"

#include <cstddef>
#include <string>

namespace aurora::core {

struct MaterializeOptions {
    // Rebuild even when the stamp matches (used by --aurora-force-extract and
    // after a failed verification).
    bool force = false;
    // Hash every file instead of checking presence and size. Slow (hundreds of
    // MB); used when the stamp is absent or does not match, and by the
    // `aurora_buildid verify` command.
    bool full_verify = false;
    // Allow hard links. Turned off in tests that want to prove the copy path.
    bool allow_hardlinks = true;
};

struct MaterializeResult {
    bool ok = false;
    std::string dir;         // the per-version directory
    bool reused = false;     // true when nothing had to be written
    size_t linked = 0;       // files hard-linked
    size_t copied = 0;       // files copied
    size_t verified = 0;     // files that were already correct
    std::string error;       // human-readable reason when !ok
    VerifyReport report;     // the verification that decided the outcome

    std::string summary() const;
};

// Ensures `target_dir` holds exactly the Resource files of `manifest`, sourced
// from `source_dir`. `manifest.build_id` must be a valid build ID: it is written
// into the completion stamp.
MaterializeResult materialize_cef_resources(const std::string& source_dir,
                                            const std::string& target_dir,
                                            const BuildManifest& manifest,
                                            const MaterializeOptions& options);

// Best-effort recursive delete of an obsolete per-version directory. Returns
// false when something is still locked (a browser from the previous version is
// still running, which is normal during an update) — the caller logs and moves
// on; the directory will be collected on a later launch.
bool remove_cef_dir(const std::string& dir);

} // namespace aurora::core

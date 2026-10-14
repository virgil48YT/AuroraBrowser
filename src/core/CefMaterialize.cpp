#include "core/CefMaterialize.hpp"

#include "core/CefLayout.hpp"
#include "util/Sha256.hpp"

#include <filesystem>
#include <sstream>

namespace aurora::core {
namespace {

namespace fs = std::filesystem;

constexpr char kTempSuffix[] = ".aurora-tmp";

// Places `src` at `dst`, hard-linking when allowed and falling back to a copy.
// Writes through a temporary name in the destination directory and renames, so
// the destination path either does not exist or is complete.
bool place_file(const fs::path& src, const fs::path& dst, bool allow_hardlinks,
                bool& used_link, std::string& error) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    ec.clear();

    fs::path tmp = dst;
    tmp += kTempSuffix;
    fs::remove(tmp, ec);
    ec.clear();

    used_link = false;
    if (allow_hardlinks) {
        fs::create_hard_link(src, tmp, ec);
        used_link = !ec;
    }
    if (!used_link) {
        ec.clear();
        fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "copy " + src.string() + " -> " + dst.string() + ": " +
                    ec.message();
            return false;
        }
    }
    ec.clear();
    // A stale destination (wrong size, half-written) has to go first: rename
    // over an existing file is allowed on POSIX but not universally on Windows.
    fs::remove(dst, ec);
    ec.clear();
    fs::rename(tmp, dst, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        error = "rename into place " + dst.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace

std::string MaterializeResult::summary() const {
    std::ostringstream out;
    out << (ok ? "ok" : "FAILED") << " dir=" << dir;
    if (reused) {
        out << " reused (" << verified << " file(s) verified)";
    } else {
        out << " linked=" << linked << " copied=" << copied
            << " verified=" << verified;
    }
    if (!error.empty()) out << " error: " << error;
    return out.str();
}

MaterializeResult materialize_cef_resources(const std::string& source_dir,
                                           const std::string& target_dir,
                                           const BuildManifest& manifest,
                                           const MaterializeOptions& options) {
    MaterializeResult r;
    r.dir = target_dir;

    if (!is_valid_build_id(manifest.build_id)) {
        r.error = "manifest has no valid build id";
        return r;
    }
    if (target_dir.empty()) {
        r.error = "empty target directory";
        return r;
    }

    const auto stamp = read_stamp(target_dir);
    const bool stamped = stamp.has_value() && *stamp == manifest.build_id;

    // Fast path: a stamped directory only needs presence + size, because the
    // stamp asserts that these exact bytes were written under this build ID and
    // the directory is never written to again.
    if (stamped && !options.force) {
        r.report = verify_resource_tree(
            target_dir, manifest,
            options.full_verify ? VerifyMode::Full : VerifyMode::Quick);
        if (r.report.ok) {
            r.ok = true;
            r.reused = true;
            r.verified = r.report.checked;
            return r;
        }
        // Something removed or truncated a file under us (antivirus, disk
        // cleaner, user). Fall through and repair, but drop the stamp first so
        // a crash halfway through cannot leave the tree looking valid.
    }

    {
        std::error_code ec;
        fs::remove(fs::path(target_dir) / fs::path(kStampFileName), ec);
        ec.clear();
        fs::create_directories(fs::path(target_dir), ec);
        if (ec) {
            r.error = "create " + target_dir + ": " + ec.message();
            return r;
        }
    }

    for (const auto& f : manifest.files) {
        if (f.role != FileRole::Resource) continue;
        const fs::path dst = fs::path(target_dir) / fs::path(f.path);
        const fs::path src = fs::path(source_dir) / fs::path(f.path);

        if (!options.force) {
            // Reuse a file that is already correct. Size is enough here for the
            // same reason as above unless a full verify was asked for.
            std::error_code ec;
            if (fs::is_regular_file(dst, ec) &&
                static_cast<uint64_t>(fs::file_size(dst, ec)) == f.size && !ec) {
                if (!options.full_verify ||
                    util::sha256_file_hex(dst.string()) == f.sha256) {
                    ++r.verified;
                    continue;
                }
            }
        }

        std::error_code ec;
        if (!fs::is_regular_file(src, ec)) {
            r.error = "missing source file " + src.string();
            return r;
        }
        bool used_link = false;
        if (!place_file(src, dst, options.allow_hardlinks, used_link, r.error)) {
            return r;
        }
        if (used_link) {
            ++r.linked;
        } else {
            ++r.copied;
        }
    }

    // Post-condition: the tree must match the manifest before it is stamped.
    // Hashes are checked here on a fresh extraction (the expensive case happens
    // once per update, not per launch) so a corrupt payload is caught now
    // instead of surfacing as a Chromium resource error later.
    const bool wrote_anything = r.linked > 0 || r.copied > 0;
    r.report = verify_resource_tree(
        target_dir, manifest,
        (options.full_verify || wrote_anything) ? VerifyMode::Full
                                                : VerifyMode::Quick);
    if (!r.report.ok) {
        r.error = "verification after extraction: " + r.report.summary();
        return r;
    }
    if (!write_stamp(target_dir, manifest.build_id)) {
        r.error = "could not write the completion stamp in " + target_dir;
        return r;
    }
    r.ok = true;
    return r;
}

bool remove_cef_dir(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    fs::remove_all(fs::path(dir), ec);
    return !ec;
}

} // namespace aurora::core

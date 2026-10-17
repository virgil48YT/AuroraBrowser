#include "adblock/ListUpdater.hpp"

#include "adblock/AdblockEngine.hpp"
#include "settings/Settings.hpp"
#include "util/Log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace aurora::adblock {

namespace fs = std::filesystem;

namespace {

// UTF-8 profile paths: QString::toStdString() hands us UTF-8 (see main.cpp),
// and on Windows a plain std::string path would be read as the ANSI code
// page, so go through u8path-equivalent construction explicitly.
fs::path to_path(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const fs::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    return static_cast<bool>(out);
}

} // namespace

FilterListCheck validate_filter_list(std::string_view content,
                                     std::size_t min_filter_lines) {
    FilterListCheck check;
    if (content.empty()) {
        check.error = "the download was empty";
        return check;
    }
    if (content.size() > kMaxFilterListBytes) {
        check.error = "the download is too large to be a filter list";
        return check;
    }
    // A NUL byte means a binary file (or a compressed body the transport did
    // not decode) — never adblock syntax.
    if (content.find('\0') != std::string_view::npos) {
        check.error = "the download is not a text filter list";
        return check;
    }
    // Captive portals and CDN error pages answer 200 with HTML. Filter lists
    // never start with markup ('<' cannot begin a rule).
    const auto first = content.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first == std::string_view::npos) {
        check.error = "the download contained only whitespace";
        return check;
    }
    if (content[first] == '<') {
        check.error = "the server returned a web page instead of a filter list";
        return check;
    }
    // Count rule lines exactly the way load_rules() reports them: skip blank
    // lines, "!" comments and the "[Adblock Plus 2.0]" header.
    std::size_t pos = 0;
    while (pos < content.size()) {
        auto end = content.find('\n', pos);
        if (end == std::string_view::npos) end = content.size();
        std::string_view line = content.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && line.front() != '!' && line.front() != '[')
            ++check.filter_lines;
        pos = end + 1;
    }
    if (check.filter_lines < min_filter_lines) {
        check.error = "only " + std::to_string(check.filter_lines) +
                      " filter rules were downloaded (expected at least " +
                      std::to_string(min_filter_lines) +
                      "); the download looks truncated";
        return check;
    }
    // When the real engine is linked, let it prove the list parses. The stub
    // build cannot tell, so the structural checks above have to suffice.
    if (engine_available()) {
        AdblockEngine probe(content);
        if (!probe.available()) {
            check.error = "the ad-block engine could not parse the list";
            return check;
        }
    }
    check.ok = true;
    return check;
}

ListUpdater::ListUpdater(std::string profile_dir, std::string source_url,
                         std::size_t min_filter_lines)
    : profile_dir_(std::move(profile_dir)),
      source_url_(std::move(source_url)),
      min_filter_lines_(min_filter_lines) {}

std::string ListUpdater::list_path() const {
    std::string p = profile_dir_;
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
    p += kFilterListFileName;
    return p;
}

std::string ListUpdater::backup_path() const { return list_path() + ".bak"; }

void ListUpdater::update(const Fetcher& fetch, ResultCallback done) {
    if (!fetch) {
        Result r;
        r.message = "No network transport is available for filter updates";
        if (done) done(r);
        return;
    }
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        Result r;
        r.message = "A filter list update is already running";
        if (done) done(r);
        return;
    }
    AURORA_LOG(util::LogLevel::Info, "adblock")
        << "updating filter list from " << source_url_;
    fetch(source_url_, [this, done = std::move(done)](
                           bool ok, std::string body, std::string error) {
        Result r;
        if (!ok) {
            r.message = "Filter list download failed" +
                        (error.empty() ? std::string() : ": " + error);
            r.source_url = source_url_;
        } else {
            r = install(body);
        }
        if (!r.ok) {
            AURORA_LOG(util::LogLevel::Warn, "adblock")
                << "filter list update failed: " << r.message;
        }
        // Clear busy before reporting so the completion handler may start
        // another update (a retry button) without being refused.
        busy_.store(false);
        if (done) done(r);
    });
}

ListUpdater::Result ListUpdater::install(std::string_view content) const {
    Result r;
    r.source_url = source_url_;
    const FilterListCheck check = validate_filter_list(content, min_filter_lines_);
    if (!check.ok) {
        r.message = "Filter list not updated: " + check.error;
        return r;
    }

    std::error_code ec;
    const fs::path target = to_path(list_path());
    const fs::path backup = to_path(backup_path());
    const fs::path part = to_path(list_path() + ".part");
    fs::create_directories(target.parent_path(), ec);

    if (!write_file(part, content)) {
        fs::remove(part, ec);
        r.message = "Filter list not updated: could not write " + list_path() +
                    ".part";
        return r;
    }
    // Keep the outgoing list. Copy (not rename) so the live file never
    // disappears, even for a moment, if the process dies between steps.
    const bool had_previous = fs::exists(target, ec);
    if (had_previous) {
        fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove(part, ec);
            r.message = "Filter list not updated: could not back up the "
                        "current list";
            return r;
        }
    }
    // rename() replaces the destination atomically on POSIX and maps to
    // MoveFileEx(MOVEFILE_REPLACE_EXISTING) on Windows.
    fs::rename(part, target, ec);
    if (ec) {
        std::error_code ignore;
        fs::remove(part, ignore);
        r.message = "Filter list not updated: could not replace the current "
                    "list (" + ec.message() + ")";
        return r;
    }
    // Trust, but verify: what is on disk now must still validate, otherwise
    // put the previous list back.
    std::string installed;
    if (!read_file(target, installed) ||
        !validate_filter_list(installed, min_filter_lines_).ok) {
        const bool restored = had_previous && rollback();
        r.message = restored ? "Filter list not updated: the written file was "
                               "damaged, the previous list was restored"
                             : "Filter list not updated: the written file was "
                               "damaged";
        return r;
    }
    r.ok = true;
    r.filter_lines = check.filter_lines;
    r.message = "Filter list updated: " + std::to_string(check.filter_lines) +
                " rules";
    AURORA_LOG(util::LogLevel::Info, "adblock")
        << "installed filter list (" << check.filter_lines << " rules) at '"
        << list_path() << "' from " << source_url_;
    return r;
}

bool ListUpdater::rollback() const {
    std::error_code ec;
    const fs::path target = to_path(list_path());
    const fs::path backup = to_path(backup_path());
    const fs::path part = to_path(list_path() + ".part");
    if (!fs::exists(backup, ec)) return false;
    // Copy to a side file and rename it in, so the backup survives a second
    // rollback and the live list is replaced in one atomic step, never
    // half-written.
    fs::copy_file(backup, part, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(part, target, ec);
    if (ec) {
        std::error_code ignore;
        fs::remove(part, ignore);
        AURORA_LOG(util::LogLevel::Warn, "adblock")
            << "filter list rollback failed: " << ec.message();
        return false;
    }
    AURORA_LOG(util::LogLevel::Info, "adblock")
        << "restored the previous filter list from '" << backup_path() << "'";
    return true;
}

void ListUpdater::record_success(settings::Settings& prefs, const Result& result,
                                 int64_t now_unix) {
    if (!result.ok) return;
    prefs.set_int(settings::PrefKeys::kAdblockListUpdatedAt, now_unix);
    prefs.set_string(settings::PrefKeys::kAdblockListSource, result.source_url);
}

UpdateRecord ListUpdater::last_update(const settings::Settings& prefs) {
    UpdateRecord rec;
    rec.last_success_unix =
        prefs.get_int(settings::PrefKeys::kAdblockListUpdatedAt, 0);
    rec.source_url = prefs.get_string(settings::PrefKeys::kAdblockListSource, "");
    return rec;
}

} // namespace aurora::adblock

#include "core/BuildId.hpp"

#include "util/Sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aurora::core {
namespace {

namespace fs = std::filesystem;

// Trims ASCII spaces, tabs and CR (so a manifest that survived a text-mode
// round trip through Windows still parses).
std::string_view trim(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool is_hex_lower(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

// Splits a line into at most `max_fields` whitespace-separated fields; the last
// field keeps any embedded spaces, which is how a path with a space in it
// survives (and why the path is always last on a file line).
std::vector<std::string_view> split_fields(std::string_view line,
                                           size_t max_fields) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size() && out.size() + 1 < max_fields) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i == start) break;
        out.push_back(line.substr(start, i - start));
    }
    const std::string_view rest = trim(line.substr(std::min(i, line.size())));
    if (!rest.empty()) out.push_back(rest);
    return out;
}

std::string to_forward_slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

} // namespace

std::string_view file_role_name(FileRole role) {
    return role == FileRole::Identity ? "identity" : "resource";
}

std::optional<FileRole> file_role_from_name(std::string_view name) {
    if (name == "resource") return FileRole::Resource;
    if (name == "identity") return FileRole::Identity;
    return std::nullopt;
}

void finalize(BuildManifest& manifest) {
    for (auto& f : manifest.files) f.path = to_forward_slashes(f.path);
    std::stable_sort(manifest.files.begin(), manifest.files.end(),
                     [](const BuildFile& a, const BuildFile& b) {
                         return a.path < b.path;
                     });
    // Duplicate paths would make the canonical text depend on insertion order,
    // which would make the build ID non-reproducible. Last entry wins.
    auto it = std::unique(manifest.files.begin(), manifest.files.end(),
                          [](const BuildFile& a, const BuildFile& b) {
                              return a.path == b.path;
                          });
    manifest.files.erase(it, manifest.files.end());
    manifest.build_id = compute_build_id(manifest);
}

std::string canonical_manifest_text(const BuildManifest& manifest) {
    // A sorted copy, so callers cannot change the ID by reordering. The cost is
    // one vector of small structs per call and it removes a whole class of
    // "why is the ID different on the build machine" bug.
    std::vector<const BuildFile*> files;
    files.reserve(manifest.files.size());
    for (const auto& f : manifest.files) files.push_back(&f);
    std::stable_sort(files.begin(), files.end(),
                     [](const BuildFile* a, const BuildFile* b) {
                         return a->path < b->path;
                     });

    std::ostringstream out;
    out << "aurora-build-manifest " << manifest.format_version << '\n';
    out << "app-version " << manifest.app_version << '\n';
    out << "cef-version " << manifest.cef_version << '\n';
    out << "platform " << manifest.platform << '\n';
    for (const BuildFile* f : files) {
        out << "file " << file_role_name(f->role) << ' ' << f->size << ' '
            << f->sha256 << ' ' << to_forward_slashes(f->path) << '\n';
    }
    return out.str();
}

std::string compute_build_id(const BuildManifest& manifest) {
    const std::string hex = util::sha256_hex(canonical_manifest_text(manifest));
    return hex.substr(0, kBuildIdLength);
}

std::string serialize_manifest(const BuildManifest& manifest) {
    std::string text = canonical_manifest_text(manifest);
    const std::string id =
        manifest.build_id.empty() ? compute_build_id(manifest)
                                  : manifest.build_id;
    text += "build-id ";
    text += id;
    text += '\n';
    return text;
}

std::optional<BuildManifest> parse_manifest(std::string_view text) {
    BuildManifest m;
    bool saw_header = false;
    std::string stated_id;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                          : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        const std::string_view line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        const auto fields = split_fields(line, 2);
        if (fields.empty()) continue;
        const std::string_view key = fields[0];
        const std::string_view value = fields.size() > 1 ? fields[1] : "";

        if (key == "aurora-build-manifest") {
            int version = 0;
            const auto s = std::string(value);
            try {
                version = std::stoi(s);
            } catch (...) {
                return std::nullopt;
            }
            if (version != kManifestFormatVersion) return std::nullopt;
            m.format_version = version;
            saw_header = true;
        } else if (key == "app-version") {
            m.app_version = std::string(value);
        } else if (key == "cef-version") {
            m.cef_version = std::string(value);
        } else if (key == "platform") {
            m.platform = std::string(value);
        } else if (key == "build-id") {
            stated_id = std::string(value);
        } else if (key == "file") {
            // file <role> <size> <sha256> <path>
            const auto parts = split_fields(line, 5);
            if (parts.size() != 5) return std::nullopt;
            const auto role = file_role_from_name(parts[1]);
            if (!role.has_value()) return std::nullopt;
            BuildFile f;
            f.role = *role;
            try {
                f.size = std::stoull(std::string(parts[2]));
            } catch (...) {
                return std::nullopt;
            }
            f.sha256 = std::string(parts[3]);
            if (f.sha256.size() != util::Sha256::kDigestSize * 2 ||
                !is_hex_lower(f.sha256)) {
                return std::nullopt;
            }
            f.path = to_forward_slashes(std::string(parts[4]));
            if (f.path.empty()) return std::nullopt;
            m.files.push_back(std::move(f));
        }
        // Unknown keys: ignored on purpose (forward compatibility for metadata).
    }
    if (!saw_header) return std::nullopt;

    // The stated ID is informational; the authoritative one is recomputed from
    // the content. A mismatch means the file was edited, and callers that care
    // (the launcher) compare the two.
    m.build_id = stated_id.empty() ? compute_build_id(m) : stated_id;
    return m;
}

bool is_valid_build_id(std::string_view id) {
    return id.size() == kBuildIdLength && is_hex_lower(id);
}

bool glob_match(std::string_view pattern, std::string_view text) {
    // Recursive wildcard match. '*' matches any run of characters except '/';
    // '**' matches across separators; '?' matches one non-'/' character.
    //
    // Recursive rather than the usual single-star iterative form because the
    // iterative version cannot backtrack into an earlier star, so "**/*.pak"
    // failed on "locales/sub/en-US.pak". Patterns here are a handful of
    // characters long, so the pathological cost of backtracking is irrelevant.
    if (pattern.empty()) return text.empty();
    if (pattern.front() == '*') {
        const bool crosses = pattern.size() > 1 && pattern[1] == '*';
        const std::string_view rest = pattern.substr(crosses ? 2 : 1);
        for (size_t i = 0;; ++i) {
            if (glob_match(rest, text.substr(i))) return true;
            if (i == text.size()) return false;
            // A single star stops at a path separator.
            if (!crosses && text[i] == '/') return false;
        }
    }
    if (text.empty()) return false;
    if (pattern.front() == '?') {
        if (text.front() == '/') return false;
        return glob_match(pattern.substr(1), text.substr(1));
    }
    if (pattern.front() != text.front()) return false;
    return glob_match(pattern.substr(1), text.substr(1));
}

BuildManifest scan_build_tree(const std::string& root,
                             const std::vector<std::string>& resource_globs,
                             const std::vector<std::string>& identity_globs) {
    BuildManifest m;
    std::error_code ec;
    const fs::path base = fs::path(root);
    if (!fs::exists(base, ec)) return m;

    const auto matches = [](const std::vector<std::string>& globs,
                            const std::string& rel) {
        for (const auto& g : globs) {
            if (glob_match(g, rel)) return true;
        }
        return false;
    };

    for (fs::recursive_directory_iterator it(base, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string rel =
            to_forward_slashes(fs::relative(it->path(), base, ec).string());
        if (ec || rel.empty()) continue;
        std::optional<FileRole> role;
        // Identity wins over resource: a file that is both (there should be
        // none) must not be materialized twice under different roles.
        if (matches(identity_globs, rel)) {
            role = FileRole::Identity;
        } else if (matches(resource_globs, rel)) {
            role = FileRole::Resource;
        }
        if (!role.has_value()) continue;

        BuildFile f;
        f.path = rel;
        f.role = *role;
        f.size = static_cast<uint64_t>(fs::file_size(it->path(), ec));
        if (ec) continue;
        f.sha256 = util::sha256_file_hex(it->path().string());
        if (f.sha256.empty()) continue;
        m.files.push_back(std::move(f));
    }
    finalize(m);
    return m;
}

std::string VerifyReport::summary() const {
    std::ostringstream out;
    out << (ok ? "ok" : "FAILED") << ": " << checked << " file(s) checked";
    if (!missing.empty()) out << ", " << missing.size() << " missing";
    if (!wrong_size.empty()) out << ", " << wrong_size.size() << " wrong size";
    if (!wrong_hash.empty()) out << ", " << wrong_hash.size() << " wrong hash";
    if (!missing.empty()) out << " (first missing: " << missing.front() << ")";
    else if (!wrong_size.empty()) out << " (first: " << wrong_size.front() << ")";
    else if (!wrong_hash.empty()) out << " (first: " << wrong_hash.front() << ")";
    return out.str();
}

VerifyReport verify_resource_tree(const std::string& root,
                                  const BuildManifest& manifest,
                                  VerifyMode mode) {
    VerifyReport r;
    std::error_code ec;
    for (const auto& f : manifest.files) {
        if (f.role != FileRole::Resource) continue;
        ++r.checked;
        const fs::path p = fs::path(root) / fs::path(f.path);
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
            r.missing.push_back(f.path);
            continue;
        }
        const auto size = static_cast<uint64_t>(fs::file_size(p, ec));
        if (ec || size != f.size) {
            r.wrong_size.push_back(f.path);
            continue;
        }
        if (mode == VerifyMode::Full) {
            if (util::sha256_file_hex(p.string()) != f.sha256) {
                r.wrong_hash.push_back(f.path);
            }
        }
    }
    r.ok = r.missing.empty() && r.wrong_size.empty() && r.wrong_hash.empty();
    return r;
}

std::optional<std::string> read_stamp(const std::string& root) {
    const fs::path p = fs::path(root) / fs::path(kStampFileName);
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string line;
    std::getline(in, line);
    const std::string id(trim(line));
    if (!is_valid_build_id(id)) return std::nullopt;
    return id;
}

bool write_stamp(const std::string& root, const std::string& build_id) {
    if (!is_valid_build_id(build_id)) return false;
    std::error_code ec;
    fs::create_directories(fs::path(root), ec);
    const fs::path p = fs::path(root) / fs::path(kStampFileName);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << build_id << '\n';
    out.flush();
    return static_cast<bool>(out);
}

} // namespace aurora::core

#include "adblock/AdblockService.hpp"

#include "util/Log.hpp"

#include <sqlite3.h>

namespace aurora::adblock {

std::string site_key(std::string_view url) {
    // Skip the scheme. URLs here are engine-reported and normally absolute;
    // a relative fragment yields no host and therefore no site key.
    const bool hierarchical = url.find("://") != std::string_view::npos;
    std::string_view::size_type host_begin;
    if (hierarchical) {
        host_begin = url.find("://") + 3;
    } else {
        // No scheme separator: the input is either a bare, already-normalized
        // host ("example.com", what the exception snapshot stores) or a
        // hostless URL ("about:blank", "data:..."). A bare host contains
        // none of the URL structural characters; anything that does is
        // hostless and has no site key. This is what keeps "about:blank"
        // from becoming a bogus site named "about".
        if (url.find_first_of("/?#:@") != std::string_view::npos) return {};
        host_begin = 0;
    }
    // The host runs to the first '/', '?', or '#'.
    const auto host_end = url.find_first_of("/?#", host_begin);
    const std::string_view host =
        url.substr(host_begin, host_end == std::string_view::npos
                                   ? std::string_view::npos
                                   : host_end - host_begin);
    // Trim userinfo (user:pass@) if present, and the port.
    std::string key(host);
    if (const auto at = key.rfind('@'); at != std::string::npos) {
        key.erase(0, at + 1);
    }
    if (const auto colon = key.rfind(':'); colon != std::string::npos) {
        key.erase(colon);
    }
    // Lower-case + strip one leading "www.".
    for (auto& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (key.starts_with("www.")) key.erase(0, 4);
    return key;
}

bool AdblockService::is_site_allowed(std::string_view url) const {
    const std::string key = site_key(url);
    if (key.empty()) return false;
    auto stmt =
        db_.prepare("SELECT 1 FROM adblock_exceptions WHERE site = ?1;");
    if (!stmt) return false;
    stmt->bind(1, key);
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        return true;
    }
    return false;
}

bool AdblockService::set_site_allowed(std::string_view url, bool allowed) {
    const std::string key = site_key(url);
    if (key.empty()) return false;
    if (allowed) {
        auto stmt = db_.prepare(
            "INSERT OR IGNORE INTO adblock_exceptions(site) VALUES (?1);");
        if (!stmt) return false;
        stmt->bind(1, key);
        auto r = stmt->step();
        return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
    }
    auto stmt = db_.prepare("DELETE FROM adblock_exceptions WHERE site = ?1;");
    if (!stmt) return false;
    stmt->bind(1, key);
    auto r = stmt->step();
    return r.has_value() && !*r && sqlite3_changes(db_.handle()) > 0;
}

bool AdblockService::toggle_site(std::string_view url) {
    const bool now_allowed = !is_site_allowed(url);
    set_site_allowed(url, now_allowed);
    return now_allowed;
}

std::vector<std::string> AdblockService::list_exception_sites() const {
    std::vector<std::string> out;
    auto stmt =
        db_.prepare("SELECT site FROM adblock_exceptions ORDER BY site;");
    if (!stmt) return out;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        out.push_back(stmt->column_text(0));
    }
    return out;
}

} // namespace aurora::adblock

// Aurora — site permission decisions.
//
// Remembers the user's answer to "may this site use your microphone /
// camera?" per origin, persisted in the Settings prefs table (key-value, no
// schema change) under "perm.<kind>.<origin>" = "allow" | "deny".
//
// The policy is deliberately strict and lives HERE so it is unit-testable
// without CEF or Qt:
//   * Default-deny: an unknown origin is Ask — the engine must prompt, and a
//     dismissed prompt counts as deny. Nothing is ever auto-granted.
//   * Decisions are per origin (scheme + host [+ port], no path) and per
//     kind (audio capture / video capture).
//   * A remembered "deny" answers immediately; a remembered "allow" answers
//     immediately; anything else prompts.
//
// Header-only on purpose: the core library lists its translation units
// explicitly in CMake, and a header-only store needs no build-file change.
#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "settings/Settings.hpp"

namespace aurora::permissions {

// The media-capture kinds Aurora understands. Desktop capture (screen share)
// is deliberately absent: it is never auto-granted and never remembered —
// always denied outright by the engine-side handler.
enum class PermissionKind {
    AudioCapture,   // microphone
    VideoCapture,   // camera
    Notification,   // desktop notifications
};

enum class PermissionDecision {
    Ask,    // no stored decision — prompt the user
    Allow,
    Deny,
};

// The scheme://host[:port] part of a URL — the scope a permission decision
// applies to. Lowercased. Returns an empty string for anything without a
// host (data:, file:, about:, malformed), which the store treats as
// unkeyable: an unkeyable origin can never recall a decision, so it always
// prompts.
inline std::string origin_of(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) return {};
    const std::string_view scheme = url.substr(0, scheme_end);
    // Permissions only make sense for web origins.
    if (scheme != "http" && scheme != "https") return {};
    const auto rest = url.substr(scheme_end + 3);
    const auto host_end = rest.find_first_of("/?#");
    std::string host(rest.substr(0, host_end));
    if (host.empty()) return {};
    // Strip any userinfo (user:pass@host) — never part of the origin key.
    if (const auto at = host.rfind('@'); at != std::string::npos)
        host.erase(0, at + 1);
    std::string out(scheme);
    out += "://";
    for (char c : host) out += static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// One remembered answer, as listed on aurora://site-settings.
struct RememberedPermission {
    std::string origin;
    PermissionKind kind = PermissionKind::AudioCapture;
    PermissionDecision decision = PermissionDecision::Ask;
};

// Per-origin, per-kind remembered decisions on top of the Settings store.
class PermissionStore {
public:
    explicit PermissionStore(settings::Settings& prefs) : prefs_(prefs) {}

    PermissionDecision decision(std::string_view origin,
                                PermissionKind kind) const {
        if (origin.empty()) return PermissionDecision::Ask;
        const std::string v = prefs_.get_string(key(origin, kind), "");
        if (v == "allow") return PermissionDecision::Allow;
        if (v == "deny") return PermissionDecision::Deny;
        return PermissionDecision::Ask;
    }

    void remember(std::string_view origin, PermissionKind kind,
                  PermissionDecision d) {
        if (origin.empty()) return;
        if (d == PermissionDecision::Ask) {
            prefs_.set_string(key(origin, kind), "");
            return;
        }
        prefs_.set_string(key(origin, kind),
                          d == PermissionDecision::Allow ? "allow" : "deny");
    }

    // Forget everything remembered for `origin` (all kinds).
    void forget(std::string_view origin) {
        for (auto kind : {PermissionKind::AudioCapture,
                          PermissionKind::VideoCapture,
                          PermissionKind::Notification})
            remember(origin, kind, PermissionDecision::Ask);
    }

    // Every remembered allow/deny, ordered by key (so by kind, then origin).
    // Forgotten entries (stored as "") and values this build does not
    // understand are skipped: listing must never resurrect an Ask as a grant.
    std::vector<RememberedPermission> list() const {
        std::vector<RememberedPermission> out;
        for (const auto& [k, v] : prefs_.entries_with_prefix(kKeyPrefix)) {
            // "perm.<kind>.<origin>": kind names contain no '.', origins do,
            // so the first '.' after the prefix ends the kind.
            const std::string_view rest =
                std::string_view(k).substr(kKeyPrefix.size());
            const auto dot = rest.find('.');
            if (dot == std::string_view::npos || dot + 1 >= rest.size())
                continue;
            RememberedPermission entry;
            if (!kind_from_name(rest.substr(0, dot), entry.kind)) continue;
            if (v == "allow") entry.decision = PermissionDecision::Allow;
            else if (v == "deny") entry.decision = PermissionDecision::Deny;
            else continue;
            entry.origin = std::string(rest.substr(dot + 1));
            out.push_back(std::move(entry));
        }
        return out;
    }

private:
    static constexpr std::string_view kKeyPrefix = "perm.";

    static bool kind_from_name(std::string_view name, PermissionKind& out) {
        for (auto kind : {PermissionKind::AudioCapture,
                          PermissionKind::VideoCapture,
                          PermissionKind::Notification}) {
            if (name == kind_name(kind)) {
                out = kind;
                return true;
            }
        }
        return false;
    }
    static std::string_view kind_name(PermissionKind kind) {
        switch (kind) {
            case PermissionKind::AudioCapture: return "audio";
            case PermissionKind::VideoCapture: return "video";
            case PermissionKind::Notification: return "notification";
        }
        return "unknown";
    }
    static std::string key(std::string_view origin, PermissionKind kind) {
        std::string k(kKeyPrefix);
        k += kind_name(kind);
        k += '.';
        k += origin;
        return k;
    }

    settings::Settings& prefs_;
};

} // namespace aurora::permissions

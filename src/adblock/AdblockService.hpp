// Aurora — per-site ad-block exceptions.
//
// Shields-down decisions are per registrable host (the "site key": host with
// any leading "www." stripped) and live in their own SQLite table, so a
// toggle survives restarts and applies to every window of the profile. The
// set is small and read on every intercepted request, so the CEF layer keeps
// an in-memory snapshot (refreshed from the UI thread on change) instead of
// touching the database from the IO thread.
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "storage/Database.hpp"

namespace aurora::adblock {

// The site key a URL belongs to: its host, lower-cased, with a leading
// "www." stripped. Empty for URLs without a host (about:blank, data:,
// aurora:// pages). Kept deliberately simple — no eTLD+1 guessing, so
// "a.example.com" and "example.com" are different sites, matching what the
// user sees in the omnibox.
std::string site_key(std::string_view url);

class AdblockService {
public:
    explicit AdblockService(storage::Database& db) : db_(db) {}

    // True when ad blocking is disabled for the site `url` belongs to. An
    // empty/invalid URL is never excepted.
    bool is_site_allowed(std::string_view url) const;

    // Set/clear the exception for `url`'s site. Setting an exception that
    // already exists (or clearing one that does not) returns false — nothing
    // is written.
    bool set_site_allowed(std::string_view url, bool allowed);

    // Toggle helper: whatever `url`'s site is now, flip it. Returns the new
    // state (true = allowed / shields down).
    bool toggle_site(std::string_view url);

    // All excepted site keys (for the engine snapshot and the settings UI).
    std::vector<std::string> list_exception_sites() const;

private:
    storage::Database& db_;
};

} // namespace aurora::adblock

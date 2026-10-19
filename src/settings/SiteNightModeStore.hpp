// Aurora — per-site night mode for page content.
//
// Night mode darkens web CONTENT (the adapter injects an invert filter, see
// WebViewController::set_night_mode); the browser chrome follows the normal
// theme. The global default lives in PrefKeys::kNightModeDefault (Settings >
// Appearance); a per-site choice made with the toolbar shortcut overrides it
// and is remembered like per-site zoom and Shields exceptions:
// "nightmode.<origin>" = "on" | "off". A choice equal to the current default
// deletes the key, so the site-settings list only shows real exceptions.
//
// Header-only like SiteZoomStore: no Qt, no CEF, unit-tested against an
// in-memory profile (tests/test_site_prefs.cpp).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "settings/Settings.hpp"

namespace aurora::settings {

class SiteNightModeStore {
public:
    struct Entry {
        std::string origin;
        bool on = false;
    };

    explicit SiteNightModeStore(Settings& prefs) : prefs_(prefs) {}

    // Global default (Settings toggle).
    bool default_on() const {
        return prefs_.get_bool(PrefKeys::kNightModeDefault, false);
    }

    // Effective state for `origin`: the per-site override, else the default.
    // An empty origin (internal pages, about:blank) is never darkened.
    bool enabled(std::string_view origin) const {
        if (origin.empty()) return false;
        const std::string v = prefs_.get_string(key(origin), "");
        if (v == "on") return true;
        if (v == "off") return false;
        return default_on();
    }

    // Remember `on` for `origin`; equal to the default clears the override.
    void set(std::string_view origin, bool on) {
        if (origin.empty()) return;
        if (on == default_on()) {
            prefs_.remove(key(origin));
            return;
        }
        prefs_.set_string(key(origin), on ? "on" : "off");
    }

    void clear(std::string_view origin) {
        if (!origin.empty()) prefs_.remove(key(origin));
    }

    // Every per-site override, ordered by origin.
    std::vector<Entry> list() const {
        std::vector<Entry> out;
        for (const auto& [k, v] : prefs_.entries_with_prefix(PrefKeys::kNightModePrefix)) {
            if (v != "on" && v != "off") continue;
            Entry e;
            e.origin = k.substr(PrefKeys::kNightModePrefix.size());
            if (e.origin.empty()) continue;
            e.on = v == "on";
            out.push_back(std::move(e));
        }
        return out;
    }

private:
    static std::string key(std::string_view origin) {
        return std::string(PrefKeys::kNightModePrefix) + std::string(origin);
    }
    Settings& prefs_;
};

} // namespace aurora::settings

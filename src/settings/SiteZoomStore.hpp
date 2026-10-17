// Aurora — per-site zoom levels.
//
// Remembers the page zoom the user picked for an origin, persisted in the
// Settings prefs table under "zoom.<origin>" = the engine zoom level as text
// (Chromium's logarithmic scale: 0 is 100%, each unit is a factor of 1.2).
// No key means "default zoom", so resetting a site deletes its key instead of
// storing 0 — the site-settings page then has nothing left to list.
//
// The origin is whatever scheme://host[:port] string the engine adapter hands
// in; the adapter derives it with the same parser for reads and writes, so the
// store itself does no URL parsing.
//
// Header-only on purpose, like PermissionStore: no Qt, no CEF, no build-file
// change, and unit-testable against an in-memory profile.
#pragma once

#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "settings/Settings.hpp"

namespace aurora::settings {

class SiteZoomStore {
public:
    struct Entry {
        std::string origin;
        double level = 0.0;
    };

    explicit SiteZoomStore(Settings& prefs) : prefs_(prefs) {}

    // The remembered level for `origin`, or false when the site uses the
    // default zoom (no key, an unparsable value, or an empty origin).
    bool level(std::string_view origin, double& out) const {
        if (origin.empty()) return false;
        return parse(prefs_.get_string(key(origin), ""), out);
    }

    // Remember `level` for `origin`. A level that is (numerically) the
    // default clears the entry rather than storing a redundant 0.
    void set_level(std::string_view origin, double level) {
        if (origin.empty() || !std::isfinite(level)) return;
        if (is_default(level)) {
            clear(origin);
            return;
        }
        // to_chars, not printf/ostream: QCoreApplication calls setlocale()
        // on Unix, and a German locale would otherwise write "1,5" that the
        // next read could not parse.
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof(buf), level);
        if (res.ec != std::errc()) return;
        prefs_.set_string(key(origin),
                          std::string_view(buf, static_cast<size_t>(res.ptr - buf)));
    }

    void clear(std::string_view origin) {
        if (origin.empty()) return;
        prefs_.remove(key(origin));
    }

    // Every non-default remembered zoom, ordered by origin.
    std::vector<Entry> list() const {
        std::vector<Entry> out;
        for (const auto& [k, v] :
             prefs_.entries_with_prefix(PrefKeys::kZoomPrefix)) {
            Entry e;
            if (!parse(v, e.level) || is_default(e.level)) continue;
            e.origin = k.substr(PrefKeys::kZoomPrefix.size());
            if (e.origin.empty()) continue;
            out.push_back(std::move(e));
        }
        return out;
    }

    // Engine level -> the percentage users recognise (Chromium: 1.2^level).
    static int percent(double level) {
        return static_cast<int>(std::lround(100.0 * std::pow(1.2, level)));
    }

    static bool is_default(double level) { return std::fabs(level) < 1e-6; }

private:
    static std::string key(std::string_view origin) {
        std::string k(PrefKeys::kZoomPrefix);
        k += origin;
        return k;
    }

    static bool parse(std::string_view text, double& out) {
        if (text.empty()) return false;
        double v = 0.0;
        const char* end = text.data() + text.size();
        const auto res = std::from_chars(text.data(), end, v);
        if (res.ec != std::errc() || res.ptr != end || !std::isfinite(v))
            return false;
        out = v;
        return true;
    }

    Settings& prefs_;
};

} // namespace aurora::settings

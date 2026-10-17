#include "settings/Settings.hpp"

#include "util/Log.hpp"

#include <sqlite3.h>

namespace aurora::settings {

namespace {
// Minimal JSON value encoders. We do not need a full JSON library for
// preferences; values are primitives and the keys are trusted.
std::string encode_bool(bool v) { return v ? "true" : "false"; }
bool decode_bool(std::string_view v) { return v == "true"; }
std::string encode_int(int64_t v) { return std::to_string(v); }
int64_t decode_int(std::string_view v) {
    try {
        return std::stoll(std::string(v));
    } catch (...) {
        return 0;
    }
}
} // namespace

void Settings::ensure_schema() {
    if (read_only_) return;
    db_.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS preferences (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )SQL");
}

std::unique_ptr<Settings> Settings::read_only_view() const {
    return std::make_unique<Settings>(db_, true);
}

std::string Settings::raw_get(std::string_view key) const {
    // Private views must see changes saved by normal windows, rather than
    // retaining a separate, stale cache for the lifetime of the private window.
    if (!read_only_) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(std::string(key));
        if (it != cache_.end()) return it->second;
    }
    std::string value;
    if (auto s = db_.prepare("SELECT value FROM preferences WHERE key = ?1;")) {
        s->bind(1, key);
        for (auto r = s->step(); r && *r; r = s->step()) {
            value = s->column_text(0);
        }
    }
    if (!read_only_) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_[std::string(key)] = value;
    }
    return value;
}

void Settings::raw_set(std::string_view key, std::string_view value) {
    // Covers every typed setter, download accessor and remembered permission.
    // Do not even populate the cache: a suppressed write is a true no-op.
    if (read_only_) return;
    if (auto s = db_.prepare(
            "INSERT INTO preferences(key, value) VALUES (?1, ?2) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value;")) {
        s->bind(1, key);
        s->bind(2, value);
        s->step();
    }
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[std::string(key)] = std::string(value);
}

bool Settings::get_bool(std::string_view key, bool fallback) const {
    std::string v = raw_get(key);
    if (v.empty()) return fallback;
    return decode_bool(v);
}

int64_t Settings::get_int(std::string_view key, int64_t fallback) const {
    std::string v = raw_get(key);
    if (v.empty()) return fallback;
    return decode_int(v);
}

std::string Settings::get_string(std::string_view key,
                                  std::string_view fallback) const {
    std::string v = raw_get(key);
    if (v.empty()) return std::string(fallback);
    return v;
}

void Settings::set_bool(std::string_view key, bool value) {
    raw_set(key, encode_bool(value));
}
void Settings::set_int(std::string_view key, int64_t value) {
    raw_set(key, encode_int(value));
}
void Settings::set_string(std::string_view key, std::string_view value) {
    raw_set(key, value);
}

std::string Settings::downloads_path() const {
    return get_string(PrefKeys::kDownloadsPath, "");
}

void Settings::set_downloads_path(std::string_view path) {
    set_string(PrefKeys::kDownloadsPath, path);
}

bool Settings::downloads_ask_where() const {
    return get_bool(PrefKeys::kDownloadsAskWhere,
                    PrefDefaults::kDownloadsAskWhere);
}

void Settings::set_downloads_ask_where(bool ask) {
    set_bool(PrefKeys::kDownloadsAskWhere, ask);
}

std::vector<std::pair<std::string, std::string>>
Settings::entries_with_prefix(std::string_view prefix) const {
    std::vector<std::pair<std::string, std::string>> out;
    // substr() rather than LIKE: '_' is legal in hosts and would be a LIKE
    // wildcard, and an exact prefix compare needs no escaping rules.
    if (auto s = db_.prepare(
            "SELECT key, value FROM preferences "
            "WHERE substr(key, 1, ?2) = ?1 ORDER BY key;")) {
        s->bind(1, prefix);
        s->bind(2, static_cast<int64_t>(prefix.size()));
        for (auto r = s->step(); r && *r; r = s->step()) {
            std::string value = s->column_text(1);
            if (value.empty()) continue;
            out.emplace_back(s->column_text(0), std::move(value));
        }
    }
    return out;
}

void Settings::remove(std::string_view key) {
    if (read_only_) return;
    if (auto s = db_.prepare("DELETE FROM preferences WHERE key = ?1;")) {
        s->bind(1, key);
        s->step();
    }
    // Cache the absence too: raw_get() treats "" as "not set", so the next
    // read returns the caller's fallback without a database round trip.
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[std::string(key)] = std::string();
}

std::string_view startup_mode_name(StartupMode mode) {
    switch (mode) {
        case StartupMode::Home: return "home";
        case StartupMode::Continue: return "continue";
        case StartupMode::Specific: return "specific";
    }
    return "home";
}

bool parse_startup_mode(std::string_view text, StartupMode& out) {
    for (auto mode : {StartupMode::Home, StartupMode::Continue,
                      StartupMode::Specific}) {
        if (text == startup_mode_name(mode)) {
            out = mode;
            return true;
        }
    }
    return false;
}

StartupMode resolve_startup_mode(const Settings& prefs) {
    StartupMode mode = StartupMode::Home;
    if (!parse_startup_mode(prefs.get_string(PrefKeys::kStartupMode, ""),
                            mode)) {
        mode = prefs.get_bool(PrefKeys::kRestoreSession,
                              PrefDefaults::kRestoreSession)
                   ? StartupMode::Continue
                   : StartupMode::Home;
    }
    if (mode == StartupMode::Specific &&
        prefs.get_string(PrefKeys::kStartupHomeUrl, "").empty()) {
        mode = StartupMode::Home;
    }
    return mode;
}

} // namespace aurora::settings

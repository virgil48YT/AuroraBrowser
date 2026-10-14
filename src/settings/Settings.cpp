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
    db_.execute(R"SQL(
        CREATE TABLE IF NOT EXISTS preferences (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )SQL");
}

std::string Settings::raw_get(std::string_view key) const {
    {
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
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[std::string(key)] = value;
    return value;
}

void Settings::raw_set(std::string_view key, std::string_view value) {
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

} // namespace aurora::settings

#include "extensions/ExtensionService.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <sstream>

#include "extensions/ZipExtractor.hpp"
#include "util/Log.hpp"
#include "util/Sha256.hpp"

namespace aurora::extensions {

using util::LogLevel;

namespace {

// A manifest.json is untrusted input, so the reader below is deliberately
// small: it walks the document once, captures four known top-level keys, and
// skips everything else without building a tree.
class Reader {
public:
    explicit Reader(std::string_view text) : s_(text) {}

    // Parse a top-level object, filling `out`. Returns false on malformed
    // input (unterminated string, missing braces, ...).
    bool parse_object(Manifest& out) {
        skip_space();
        if (!eat('{')) return false;
        skip_space();
        if (eat('}')) return true;
        for (;;) {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (!eat(':')) return false;
            skip_space();
            if (!capture(key, out)) return false;
            skip_space();
            if (eat(',')) continue;
            return eat('}');
        }
    }

    // Pick the icon to remember: the top-level "icons" set wins over an
    // action's default_icon, matching Chrome's own precedence.
    void apply_icon(Manifest& out) const {
        out.icon_path = !icons_best_.empty() ? icons_best_ : action_icon_;
    }

private:
    bool done() const { return pos_ >= s_.size(); }
    char peek() const { return done() ? '\0' : s_[pos_]; }

    bool eat(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }

    // Whitespace plus the JS-style comments Chrome tolerates in manifests.
    void skip_space() {
        for (;;) {
            while (!done() &&
                   std::isspace(static_cast<unsigned char>(s_[pos_]))) {
                ++pos_;
            }
            if (pos_ + 1 < s_.size() && s_[pos_] == '/' && s_[pos_ + 1] == '/') {
                while (!done() && s_[pos_] != '\n') ++pos_;
                continue;
            }
            if (pos_ + 1 < s_.size() && s_[pos_] == '/' && s_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < s_.size() &&
                       !(s_[pos_] == '*' && s_[pos_ + 1] == '/')) {
                    ++pos_;
                }
                pos_ = pos_ + 1 < s_.size() ? pos_ + 2 : s_.size();
                continue;
            }
            return;
        }
    }

    // Encode one code point as UTF-8 (BMP only; surrogate pairs are emitted as
    // the replacement character, which is all a display name needs).
    static void append_utf8(std::string& out, unsigned int cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(std::string& out) {
        out.clear();
        if (!eat('"')) return false;
        while (!done()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (done()) return false;
            const char esc = s_[pos_++];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return false;
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = s_[pos_ + static_cast<size_t>(i)];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') {
                            cp |= static_cast<unsigned int>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            cp |= static_cast<unsigned int>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            cp |= static_cast<unsigned int>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    pos_ += 4;
                    append_utf8(out, cp);
                    break;
                }
                default: out.push_back(esc); break;  // \" \\ \/ and friends
            }
        }
        return false;  // unterminated
    }

    // Skip any value: primitives by scanning, containers by brace matching
    // (string-aware, so a '}' inside a string cannot end an object early).
    bool skip_value() {
        skip_space();
        const char c = peek();
        if (c == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (c == '{' || c == '[') {
            const char open = c;
            const char close = (open == '{') ? '}' : ']';
            int depth = 0;
            while (!done()) {
                const char d = s_[pos_];
                if (d == '"') {
                    std::string ignored;
                    if (!parse_string(ignored)) return false;
                    continue;
                }
                ++pos_;
                if (d == open) {
                    ++depth;
                } else if (d == close) {
                    if (--depth == 0) return true;
                }
            }
            return false;
        }
        // Number, true, false, null: run to the next structural character.
        while (!done() && s_[pos_] != ',' && s_[pos_] != '}' &&
               s_[pos_] != ']') {
            ++pos_;
        }
        return true;
    }

    bool parse_int(int& out) {
        const size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (!done() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
        if (pos_ == start) return false;
        out = std::atoi(std::string(s_.substr(start, pos_ - start)).c_str());
        return true;
    }

    // Read the value for `key`, storing it when it is one we care about.
    bool capture(const std::string& key, Manifest& out) {
        if (key == "name" && peek() == '"') return parse_string(out.name);
        if (key == "version" && peek() == '"') return parse_string(out.version);
        if (key == "description" && peek() == '"') {
            return parse_string(out.description);
        }
        if (key == "manifest_version" && peek() != '"') {
            return parse_int(out.manifest_version);
        }
        // "icons": { "16": "a.png", "128": "b.png" } — the extension's own
        // artwork, which is what the extensions page should show.
        if (key == "icons" && peek() == '{') {
            return parse_icon_set(icons_best_, icons_best_size_);
        }
        // The toolbar action's default_icon is the fallback Chrome itself uses
        // when an extension ships no top-level "icons" (MV2 browser_action /
        // page_action, MV3 action).
        if ((key == "action" || key == "browser_action" ||
             key == "page_action") && peek() == '{') {
            return parse_action();
        }
        if (key == "key" && peek() == '"') return parse_string(out.public_key);
        if (key == "options_page" && peek() == '"') {
            return parse_string(out.options_page);
        }
        if (key == "options_ui" && peek() == '{') {
            return parse_options_ui(out);
        }
        return skip_value();
    }

    // Parse an icon map (size -> relative path) keeping the largest entry: a
    // 128px icon downscales cleanly, a 16px one shown at 32px does not.
    bool parse_icon_set(std::string& best, int& best_size) {
        if (!eat('{')) return false;
        skip_space();
        if (eat('}')) return true;
        for (;;) {
            skip_space();
            std::string size_key;
            if (!parse_string(size_key)) return false;
            skip_space();
            if (!eat(':')) return false;
            skip_space();
            if (peek() == '"') {
                std::string path;
                if (!parse_string(path)) return false;
                const int size = std::atoi(size_key.c_str());
                if (!path.empty() && size > best_size) {
                    best_size = size;
                    best = std::move(path);
                }
            } else if (!skip_value()) {
                return false;
            }
            skip_space();
            if (eat(',')) continue;
            return eat('}');
        }
    }

    // Parse an action object, keeping only its default_icon (a string, or an
    // icon map like "icons").
    bool parse_action() {
        if (!eat('{')) return false;
        skip_space();
        if (eat('}')) return true;
        for (;;) {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (!eat(':')) return false;
            skip_space();
            if (key == "default_icon") {
                if (peek() == '"') {
                    if (!parse_string(action_icon_)) return false;
                } else if (peek() == '{') {
                    if (!parse_icon_set(action_icon_, action_icon_size_)) {
                        return false;
                    }
                } else if (!skip_value()) {
                    return false;
                }
            } else if (!skip_value()) {
                return false;
            }
            skip_space();
            if (eat(',')) continue;
            return eat('}');
        }
    }

    // options_ui: { "page": "options.html", "open_in_tab": true }
    bool parse_options_ui(Manifest& out) {
        if (!eat('{')) return false;
        skip_space();
        if (eat('}')) return true;
        for (;;) {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            skip_space();
            if (!eat(':')) return false;
            skip_space();
            if (key == "page" && peek() == '"') {
                if (!parse_string(out.options_page)) return false;
            } else if (!skip_value()) {
                return false;
            }
            skip_space();
            if (eat(',')) continue;
            return eat('}');
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string icons_best_;
    int icons_best_size_ = -1;
    std::string action_icon_;
    int action_icon_size_ = -1;
};

// --- Chrome extension i18n: __MSG_key__ resolution -------------------------
// Many Chrome extensions put "__MSG_extensionName__" in manifest.json instead
// of a literal name. The real string lives in
// _locales/<locale>/messages.json under a "message" field. Without this
// resolution the extensions page shows the raw placeholder text.

// Determine the user's language code from the environment. Chrome extensions
// name locale directories by language code (e.g. "en", "fr", "de").
std::string system_locale() {
    // LC_ALL > LC_MESSAGES > LANG, matching POSIX locale resolution.
    const char* env = std::getenv("LC_ALL");
    if (!env || !*env) env = std::getenv("LC_MESSAGES");
    if (!env || !*env) env = std::getenv("LANG");
    if (env && *env) {
        std::string locale(env);
        // "en_US.UTF-8" -> "en": keep only the language part.
        size_t pos = locale.find('_');
        if (pos != std::string::npos) locale = locale.substr(0, pos);
        pos = locale.find('.');
        if (pos != std::string::npos) locale = locale.substr(0, pos);
        if (!locale.empty()) return locale;
    }
    return "en";
}

// Look up a single message key in a messages.json blob. The format is:
//   { "key": { "message": "value", "description": "..." }, ... }
// Returns empty string when the key is not found.
std::string lookup_message(std::string_view json, std::string_view key) {
    // Build the quoted key to search for (e.g. "extensionName").
    std::string needle = "\"";
    needle += key;
    needle += "\"";

    size_t pos = 0;
    while (pos < json.size()) {
        size_t found = json.find(needle, pos);
        if (found == std::string_view::npos) return "";

        // Verify this is a JSON key, not a substring inside a string value:
        // the character before the quote (skipping whitespace) must be '{' or ','.
        size_t check = found;
        while (check > 0 &&
               (json[check - 1] == ' ' || json[check - 1] == '\t' ||
                json[check - 1] == '\n' || json[check - 1] == '\r')) {
            --check;
        }
        if (check == 0 ||
            (json[check - 1] != '{' && json[check - 1] != ',')) {
            pos = found + 1;
            continue;
        }

        // Found the key object. Find "message" within it.
        size_t after_key = found + needle.size();
        size_t msg = json.find("\"message\"", after_key);
        if (msg == std::string_view::npos) return "";

        // Find the colon after "message", then the opening quote of the value.
        size_t colon = json.find(':', msg + 9);
        if (colon == std::string_view::npos) return "";
        size_t val_start = json.find('"', colon);
        if (val_start == std::string_view::npos) return "";

        // Read the string value, handling common escapes.
        std::string value;
        size_t i = val_start + 1;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) {
                ++i;
                switch (json[i]) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case 'r': value.push_back('\r'); break;
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    case '/': value.push_back('/'); break;
                    default: value.push_back(json[i]); break;
                }
            } else {
                value.push_back(json[i]);
            }
            ++i;
        }
        return value;
    }
    return "";
}

// Read _locales/<locale>/messages.json from the extension directory.
// Tries the system locale first, then falls back to "en".
// Returns empty string when no messages file is found.
std::string read_messages_json(std::string_view dir) {
    namespace fs = std::filesystem;
    const fs::path base = fs::path(dir) / "_locales";
    std::error_code ec;

    const std::string locale = system_locale();
    // Try the exact locale, then "en" as the universal fallback.
    for (const auto& candidate : {locale, std::string("en")}) {
        const fs::path msg_file = base / candidate / "messages.json";
        if (!fs::is_regular_file(msg_file, ec)) continue;
        // Cap the read size: a messages.json should never be enormous.
        constexpr std::uintmax_t kMax = 4u * 1024u * 1024u;
        if (fs::file_size(msg_file, ec) > kMax || ec) continue;
        std::ifstream in(msg_file, std::ios::binary);
        if (!in) continue;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
    return "";
}

// Resolve all __MSG_key__ placeholders in `text` using the messages.json
// content. Falls back to the original __MSG_key__ text when a key is not found.
std::string resolve_msg_placeholders(std::string_view text,
                                      std::string_view messages_json) {
    if (messages_json.empty()) return std::string(text);

    std::string result;
    size_t pos = 0;
    while (pos < text.size()) {
        // Look for the __MSG_ prefix.
        size_t start = text.find("__MSG_", pos);
        if (start == std::string_view::npos) {
            result += text.substr(pos);
            break;
        }
        // Append text before the placeholder.
        result += text.substr(pos, start - pos);

        // Extract the key between __MSG_ and the closing __.
        size_t key_start = start + 6;  // length of "__MSG_"
        size_t end = text.find("__", key_start);
        if (end == std::string_view::npos) {
            // No closing __; keep the rest as-is.
            result += text.substr(start);
            break;
        }

        std::string_view key = text.substr(key_start, end - key_start);
        std::string resolved = lookup_message(messages_json, key);
        if (!resolved.empty()) {
            result += resolved;
        } else {
            // Key not found: keep the original placeholder so the issue is visible.
            result += text.substr(start, end + 2 - start);
        }

        pos = end + 2;  // skip past the closing __
    }
    return result;
}

// Resolve __MSG_...__ patterns in a manifest's name and description using the
// extension's own _locales/ directory. No-op when the manifest uses literal
// strings (the common case for simple extensions).
void resolve_manifest_messages(Manifest& m, std::string_view dir) {
    // Only read messages.json if there's actually a placeholder to resolve.
    if (m.name.find("__MSG_") == std::string::npos &&
        m.description.find("__MSG_") == std::string::npos) {
        return;
    }
    std::string messages_json = read_messages_json(dir);
    if (messages_json.empty()) return;  // no _locales, leave placeholders as-is
    m.name = resolve_msg_placeholders(m.name, messages_json);
    m.description = resolve_msg_placeholders(m.description, messages_json);
}


// Map the first 16 bytes of a SHA-256 digest onto Chromium's a-p extension id.
std::string chrome_id_from_bytes(const void* data, size_t len) {
    util::Sha256 hasher;
    hasher.update(data, len);
    const auto digest = hasher.finish();
    std::string id;
    id.reserve(32);
    for (size_t i = 0; i < 16; ++i) {
        const unsigned byte = digest[i];
        id.push_back(static_cast<char>('a' + ((byte >> 4) & 0x0f)));
        id.push_back(static_cast<char>('a' + (byte & 0x0f)));
    }
    return id;
}

std::string b64_decode(std::string_view in) {
    static const int8_t tbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = -8;
    for (char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') {
            if (ch == '=') break;
            continue;
        }
        // Explicit cast: indexing with a plain char is implementation-defined
        // for negative values, and -Werror=sign-conversion rejects the
        // implicit char -> unsigned char narrowing on the loop variable.
        const int d = tbl[static_cast<unsigned char>(ch)];
        if (d < 0) return {};
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::string strip_pem_key(std::string key) {
    auto erase_all = [&](const char* token) {
        for (;;) {
            auto pos = key.find(token);
            if (pos == std::string::npos) break;
            key.erase(pos, std::strlen(token));
        }
    };
    erase_all("-----BEGIN PUBLIC KEY-----");
    erase_all("-----END PUBLIC KEY-----");
    key.erase(std::remove_if(key.begin(), key.end(),
                             [](unsigned char c) { return std::isspace(c); }),
              key.end());
    return key;
}

std::string chrome_extension_id(std::string_view dir, const Manifest& manifest) {
    if (!manifest.public_key.empty()) {
        const std::string der = b64_decode(strip_pem_key(manifest.public_key));
        if (!der.empty()) return chrome_id_from_bytes(der.data(), der.size());
    }
    // Unpacked extensions without a key are identified by their native path,
    // matching Chromium's FilePath::value() hashing on desktop Unix (UTF-8).
    std::string native = std::filesystem::path(dir).make_preferred().string();
    return chrome_id_from_bytes(native.data(), native.size());
}

std::string sanitize_extension_page(std::string page) {
    if (page.empty()) return "options.html";
    for (char& c : page) {
        if (c == '\\') c = '/';
    }
    // A manifest may already spell a full chrome-extension:// URL.
    if (page.size() >= 19) {
        std::string lower = page;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.compare(0, 19, "chrome-extension://") == 0) return page;
        if (lower.find("://") != std::string::npos) return "options.html";
    }
    while (!page.empty() && page.front() == '/') page.erase(page.begin());
    std::string suffix;
    auto cut = page.find_first_of("?#");
    if (cut != std::string::npos) {
        suffix = page.substr(cut);
        page.resize(cut);
    }
    // Refuse path traversal.
    if (page.empty() || page == "." || page == ".." ||
        page.compare(0, 3, "../") == 0) {
        return "options.html";
    }
    return page + suffix;
}

} // namespace

std::optional<Manifest> parse_manifest(std::string_view json) {
    Manifest m;
    Reader reader(json);
    if (!reader.parse_object(m)) return std::nullopt;
    reader.apply_icon(m);
    // Chrome also refuses a manifest without a name; without it there is
    // nothing to show the user in the extensions list.
    if (m.name.empty()) return std::nullopt;
    return m;
}

std::optional<Manifest> read_manifest_dir(std::string_view dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path manifest = fs::path(dir) / "manifest.json";
    if (!fs::is_regular_file(manifest, ec)) return std::nullopt;
    // Extensions are user-chosen, but a huge file here is a mistake, not a
    // manifest; cap the read so a bad pick cannot exhaust memory.
    constexpr std::uintmax_t kMaxManifestBytes = 4u * 1024u * 1024u;
    if (fs::file_size(manifest, ec) > kMaxManifestBytes || ec) {
        return std::nullopt;
    }
    std::ifstream in(manifest, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto m = parse_manifest(buffer.str());
    if (!m) return std::nullopt;
    // Resolve Chrome's __MSG_key__ localization pattern: many extensions put
    // "__MSG_extensionName__" in the manifest and ship the real string in
    // _locales/<locale>/messages.json. Without this, the extensions page
    // shows the raw placeholder text instead of the extension's real name.
    resolve_manifest_messages(*m, dir);
    return m;
}

std::string icon_file_for_dir(std::string_view dir, const Manifest& manifest) {
    namespace fs = std::filesystem;
    if (manifest.icon_path.empty()) return {};
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(dir), ec);
    if (ec) return {};
    const fs::path candidate =
        fs::weakly_canonical(root / fs::path(manifest.icon_path), ec);
    if (ec) return {};
    // Containment check: the manifest is untrusted text, so an icon path that
    // escapes the extension folder is refused rather than read.
    const auto root_str = root.string();
    const auto cand_str = candidate.string();
    if (cand_str.size() <= root_str.size() ||
        cand_str.compare(0, root_str.size(), root_str) != 0) {
        return {};
    }
    if (!fs::is_regular_file(candidate, ec) || ec) return {};
    return cand_str;
}

int64_t ExtensionService::add_unpacked(std::string_view dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(fs::path(dir), ec);
    const std::string path = ec ? std::string(dir) : canonical.string();

    const auto manifest = read_manifest_dir(path);
    if (!manifest) {
        AURORA_LOG(LogLevel::Warn, "extensions")
            << "no readable manifest.json in " << path;
        return 0;
    }

    // A folder that was removed and re-added (or edited in place within one
    // filesystem timestamp tick) must not be described by a stale cache entry.
    forget_cached_manifest(path);

    // Idempotent on path: re-adding a folder refreshes its metadata (the user
    // may have edited the extension) and keeps the row's enabled state.
    auto stmt = db_.prepare(
        "INSERT INTO extensions(path, name, version, description, "
        "manifest_version, enabled) VALUES (?1, ?2, ?3, ?4, ?5, 1) "
        "ON CONFLICT(path) DO UPDATE SET name = ?2, version = ?3, "
        "description = ?4, manifest_version = ?5;");
    if (!stmt) return 0;
    stmt->bind(1, path);
    stmt->bind(2, manifest->name);
    stmt->bind(3, manifest->version);
    stmt->bind(4, manifest->description);
    stmt->bind(5, manifest->manifest_version);
    auto r = stmt->step();
    if (!r.has_value() || *r) {
        AURORA_LOG(LogLevel::Warn, "extensions")
            << "add failed: " << db_.last_error_message();
        return 0;
    }
    // The upsert may have updated an existing row, in which case
    // last_insert_rowid is not this row's id — look it up by path.
    auto find = db_.prepare("SELECT id FROM extensions WHERE path = ?1;");
    if (!find) return 0;
    find->bind(1, path);
    if (auto found = find->step(); found && *found) return find->column_int64(0);
    return 0;
}

CrxInstall ExtensionService::install_from_file(std::string_view path) {
    namespace fs = std::filesystem;
    // CRX2: magic, version, public-key length, signature length, key,
    // signature, ZIP. CRX3: magic, version, protobuf-header length, header,
    // ZIP. The payload is installed as an unpacked directory because that is
    // the form Chromium's --load-extension path accepts.
    CrxInstall out;
    const auto bad = [](std::string why) {
        CrxInstall r;
        r.result = CrxInstallResult::NotACrx;
        r.detail = std::move(why);
        return r;
    };

    std::error_code ec;
    const auto size = fs::file_size(fs::path(path), ec);
    if (ec) return bad("The file could not be read.");

    std::ifstream in{fs::path(path), std::ios::binary | std::ios::ate};
    if (!in) return bad("The file could not be opened.");

    constexpr std::uintmax_t kFixed = 16;
    if (size < kFixed) return bad("The file is too small to be a .crx package.");

    auto read_at = [&in](std::uintmax_t off, char* buf, std::streamsize len) {
        in.seekg(static_cast<std::streamoff>(off));
        in.read(buf, len);
        return in.gcount() == len;
    };
    const auto le32 = [](const unsigned char* p) {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8u) |
               (static_cast<std::uint32_t>(p[2]) << 16u) |
               (static_cast<std::uint32_t>(p[3]) << 24u);
    };

    unsigned char fixed[12];
    if (!read_at(0, reinterpret_cast<char*>(fixed), 12))
        return bad("The .crx header could not be read.");
    if (std::memcmp(fixed, "Cr24", 4) != 0)
        return bad("Not a .crx package (missing the Cr24 magic).");

    const std::uint32_t version = le32(fixed + 4);
    if (version != 2 && version != 3)
        return bad("Unsupported .crx format version " + std::to_string(version) + ".");

    constexpr std::uint32_t kMaxHeader = 16u * 1024u * 1024u;
    std::uintmax_t zip_off = 0;
    if (version == 2) {
        unsigned char lengths[8];
        if (!read_at(8, reinterpret_cast<char*>(lengths), 8))
            return bad("The CRX2 header is truncated.");
        const std::uint32_t key_len = le32(lengths);
        const std::uint32_t signature_len = le32(lengths + 4);
        if (key_len == 0 || signature_len == 0 || key_len > kMaxHeader ||
            signature_len > kMaxHeader ||
            static_cast<std::uint64_t>(key_len) + signature_len > kMaxHeader) {
            // Compatibility with early Aurora test packages, which represented
            // CRX2 like CRX3 with one combined header length.
            const std::uint32_t legacy_len = key_len;
            zip_off = 12u + legacy_len;
        } else {
            zip_off = 16u + static_cast<std::uintmax_t>(key_len) +
                      signature_len;
        }
    } else {
        const std::uint32_t header_len = le32(fixed + 8);
        if (header_len == 0 || header_len > kMaxHeader)
            return bad("The CRX3 header length is implausible.");
        zip_off = 12u + header_len;
    }
    if (zip_off + 4 > size)
        return bad("The .crx is truncated (no ZIP payload).");

    unsigned char zip[4];
    if (!read_at(zip_off, reinterpret_cast<char*>(zip), 4))
        return bad("The .crx payload could not be read.");
    if (std::memcmp(zip, "PK\x03\x04", 4) != 0)
        return bad("The .crx payload is not a ZIP archive.");

    out.format_version = static_cast<int>(version);

    const std::string digest = util::sha256_file_hex(std::string(path));
    if (digest.empty()) return bad("The .crx could not be hashed.");

    // The same directory uninstall() is allowed to delete from: both go
    // through managed_root() so "Aurora owns these files" means exactly one
    // thing in both directions.
    fs::path installs;
    if (const std::string managed = managed_root(); !managed.empty()) {
        installs = fs::path(managed);
    } else {
        // Only tests and embedders with an in-memory registry reach this path.
        installs = fs::temp_directory_path(ec) / "Aurora" / "extensions";
    }
    if (ec) {
        out.result = CrxInstallResult::Unsupported;
        out.detail = "The extension install directory could not be located.";
        return out;
    }

    const fs::path destination = installs / digest.substr(0, 32);
    const fs::path staging = installs / (digest.substr(0, 32) + ".installing");
    fs::create_directories(installs, ec);
    if (ec) {
        out.result = CrxInstallResult::Unsupported;
        out.detail = "The extension install directory could not be created.";
        return out;
    }

    // Reinstalling the same package is idempotent.
    if (read_manifest_dir(destination.string())) {
        if (add_unpacked(destination.string()) == 0) {
            out.result = CrxInstallResult::Unsupported;
            out.detail = "The installed extension could not be registered.";
            return out;
        }
        out.result = CrxInstallResult::Installed;
        out.detail = "Extension installed. Restart Aurora to load it.";
        return out;
    }

    fs::remove_all(staging, ec);
    ec.clear();
    fs::create_directories(staging, ec);
    if (ec) {
        out.result = CrxInstallResult::Unsupported;
        out.detail = "A temporary extension directory could not be created.";
        return out;
    }

    const auto extracted =
        extract_zip_payload(path, zip_off, staging.string());
    if (!extracted.ok) {
        fs::remove_all(staging, ec);
        out.result = CrxInstallResult::Unsupported;
        out.detail = extracted.detail;
        return out;
    }
    const auto manifest = read_manifest_dir(staging.string());
    if (!manifest) {
        fs::remove_all(staging, ec);
        return bad("The CRX payload has no readable manifest.json.");
    }

    fs::remove_all(destination, ec);
    ec.clear();
    fs::rename(staging, destination, ec);
    if (ec) {
        fs::remove_all(staging, ec);
        out.result = CrxInstallResult::Unsupported;
        out.detail = "The unpacked extension could not be moved into place.";
        return out;
    }
    if (add_unpacked(destination.string()) == 0) {
        fs::remove_all(destination, ec);
        out.result = CrxInstallResult::Unsupported;
        out.detail = "The unpacked extension could not be registered.";
        return out;
    }

    out.result = CrxInstallResult::Installed;
    out.detail = "Installed " + manifest->name +
                 ". Restart Aurora to load it.";
    AURORA_LOG(LogLevel::Info, "extensions")
        << "installed CRX" << version << " package at "
        << destination.string();
    return out;
}

std::vector<Extension> ExtensionService::all() const {
    std::vector<Extension> out;
    auto stmt = db_.prepare(
        "SELECT id, path, name, version, description, manifest_version, enabled "
        "FROM extensions ORDER BY name COLLATE NOCASE, id;");
    if (!stmt) return out;
    for (auto r = stmt->step(); r && *r; r = stmt->step()) {
        Extension e;
        e.id = stmt->column_int64(0);
        e.path = stmt->column_text(1);
        e.name = stmt->column_text(2);
        e.version = stmt->column_text(3);
        e.description = stmt->column_text(4);
        e.manifest_version = stmt->column_int(5);
        e.enabled = stmt->column_int(6) != 0;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<Extension> ExtensionService::all_detailed() const {
    auto out = all();
    // Lock the cache for the duration of the refresh: all_detailed() is called
    // on every UI refresh, so avoiding redundant disk reads matters when there
    // are many extensions.
    std::lock_guard<std::mutex> lock(cache_mutex_);
    for (auto& e : out) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path manifest_path = fs::path(e.path) / "manifest.json";
        const auto mtime = fs::last_write_time(manifest_path, ec);

        // Cache hit: the manifest file hasn't changed since we last read it.
        // Skip the disk read entirely — this is the fast path on refresh.
        auto cached = manifest_cache_.find(e.path);
        if (cached != manifest_cache_.end() && !ec &&
            cached->second.mtime == mtime) {
            const auto& m = cached->second.manifest;
            e.name = m.name;
            if (!m.version.empty()) e.version = m.version;
            e.description = m.description;
            if (m.manifest_version > 0) {
                e.manifest_version = m.manifest_version;
            }
            e.icon_file = icon_file_for_dir(e.path, m);
            continue;
        }

        // Cache miss: re-read the manifest from disk. This also resolves
        // __MSG_...__ localization placeholders (Bug 2).
        const auto manifest = read_manifest_dir(e.path);
        if (!manifest) continue;  // folder gone or unreadable: keep the row
        // The manifest on disk is the source of truth for what to display: an
        // extension the user updated in place would otherwise keep showing the
        // name and version it had when it was registered.
        e.name = manifest->name;
        if (!manifest->version.empty()) e.version = manifest->version;
        e.description = manifest->description;
        if (manifest->manifest_version > 0) {
            e.manifest_version = manifest->manifest_version;
        }
        e.icon_file = icon_file_for_dir(e.path, *manifest);

        // Store in the cache so the next refresh is free unless the file changed.
        if (!ec) {
            manifest_cache_[e.path] = {mtime, *manifest};
        }
    }
    return out;
}

std::vector<std::string> ExtensionService::enabled_paths() const {
    std::vector<std::string> out;
    for (const auto& e : all()) {
        if (e.enabled) out.push_back(e.path);
    }
    return out;
}

bool ExtensionService::set_enabled(int64_t id, bool enabled) {
    auto stmt = db_.prepare("UPDATE extensions SET enabled = ?2 WHERE id = ?1;");
    if (!stmt) return false;
    stmt->bind(1, id);
    stmt->bind(2, enabled ? 1 : 0);
    auto r = stmt->step();
    return r.has_value() && !*r;
}

std::optional<Extension> ExtensionService::find(int64_t id) const {
    auto stmt = db_.prepare(
        "SELECT id, path, name, version, description, manifest_version, enabled "
        "FROM extensions WHERE id = ?1;");
    if (!stmt) return std::nullopt;
    stmt->bind(1, id);
    auto r = stmt->step();
    if (!r.has_value() || !*r) return std::nullopt;
    Extension e;
    e.id = stmt->column_int64(0);
    e.path = stmt->column_text(1);
    e.name = stmt->column_text(2);
    e.version = stmt->column_text(3);
    e.description = stmt->column_text(4);
    e.manifest_version = stmt->column_int(5);
    e.enabled = stmt->column_int(6) != 0;
    return e;
}

void ExtensionService::forget_cached_manifest(const std::string& path) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    manifest_cache_.erase(path);
}

std::string ExtensionService::managed_root() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    const char* db_file = sqlite3_db_filename(db_.handle(), "main");
    if (db_file == nullptr || *db_file == '\0' ||
        std::string_view(db_file) == ":memory:") {
        return {};
    }
    const fs::path root =
        fs::absolute(fs::path(db_file), ec).parent_path() / "extensions";
    if (ec) return {};
    const fs::path canonical = fs::weakly_canonical(root, ec);
    return ec ? root.string() : canonical.string();
}

ExtensionService::Removal ExtensionService::uninstall(int64_t id) {
    namespace fs = std::filesystem;
    Removal out;

    // Read the row BEFORE deleting it: the path is what tells us whether the
    // files belong to Aurora, and it is unavailable afterwards.
    if (const auto row = find(id)) {
        out.path = row->path;
        out.name = row->name;
    }

    auto stmt = db_.prepare("DELETE FROM extensions WHERE id = ?1;");
    if (!stmt) return out;
    stmt->bind(1, id);
    auto r = stmt->step();
    if (!r.has_value() || *r) {
        AURORA_LOG(LogLevel::Warn, "extensions")
            << "remove failed: " << db_.last_error_message();
        return out;
    }
    out.ok = true;
    if (out.path.empty()) return out;

    // The cached manifest would otherwise keep describing an extension the
    // registry no longer has (and pin its icon path).
    forget_cached_manifest(out.path);

    // Only Aurora's own unpacked copies are deleted. Containment is checked on
    // canonical paths so neither a symlink nor a ".." segment stored in the
    // registry can point the delete at something outside the managed folder.
    const std::string managed = managed_root();
    if (managed.empty()) return out;
    std::error_code ec;
    const fs::path canonical_path = fs::weakly_canonical(fs::path(out.path), ec);
    if (ec) return out;
    const std::string candidate = canonical_path.string();
    const std::string prefix =
        managed.back() == fs::path::preferred_separator
            ? managed
            : managed + static_cast<char>(fs::path::preferred_separator);
    if (candidate.size() <= prefix.size() ||
        candidate.compare(0, prefix.size(), prefix) != 0) {
        return out;  // a folder the user loaded themselves: leave it alone
    }
    if (!fs::is_directory(canonical_path, ec) || ec) return out;
    ec.clear();
    fs::remove_all(canonical_path, ec);
    if (ec) {
        AURORA_LOG(LogLevel::Warn, "extensions")
            << "could not delete managed extension folder " << candidate
            << ": " << ec.message();
        return out;
    }
    out.files_deleted = true;
    AURORA_LOG(LogLevel::Info, "extensions")
        << "uninstalled " << out.name << " and deleted " << candidate;
    return out;
}


std::string ExtensionService::extension_options_url(int64_t id) const {
    const auto row = find(id);
    if (!row) return {};
    const auto manifest = read_manifest_dir(row->path);
    const std::string chrome_id = chrome_extension_id(
        row->path, manifest ? *manifest : Manifest{});
    if (chrome_id.empty()) return {};
    std::string page = "options.html";
    if (manifest && !manifest->options_page.empty()) {
        page = sanitize_extension_page(manifest->options_page);
    }
    if (page.size() >= 19) {
        std::string lower = page;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.compare(0, 19, "chrome-extension://") == 0) return page;
    }
    if (!page.empty() && page.front() == '/') page.erase(page.begin());
    return "chrome-extension://" + chrome_id + "/" + page;
}

bool ExtensionService::remove(int64_t id) { return uninstall(id).ok; }

} // namespace aurora::extensions

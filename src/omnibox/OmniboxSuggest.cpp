#include "omnibox/OmniboxSuggest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "core/InternalUrl.hpp"
#include "core/Url.hpp"

namespace aurora::omnibox {

namespace {

constexpr size_t kMaxQueryBytes = 128;
constexpr size_t kMaxLocalWithRemote = 4;
constexpr size_t kMaxRemoteBytes = 200;

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lower(c);
    return out;
}

std::string_view trim(std::string_view s) {
    const auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}

std::vector<std::string> tokenize(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
        } else {
            cur.push_back(lower(c));
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

// "https://www.Example.com/a/?x=1#top" -> "example.com/a/?x=1"
// Two URLs that differ only by scheme, "www.", a trailing slash or a fragment
// are the same destination for the purpose of showing one row.
std::string url_key(std::string_view url) {
    std::string s = to_lower(trim(url));
    if (const auto hash = s.find('#'); hash != std::string::npos) s.erase(hash);
    if (const auto scheme = s.find("://"); scheme != std::string::npos) {
        s.erase(0, scheme + 3);
    }
    if (starts_with(s, "www.")) s.erase(0, 4);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// The host part of an already-normalised key.
std::string_view host_of(std::string_view key) {
    const auto end = key.find_first_of("/?:");
    return end == std::string_view::npos ? key : key.substr(0, end);
}

// True when `token` occurs in `hay` at the start of a word (start of string
// or right after a non-alphanumeric byte).
bool word_start_match(std::string_view hay, std::string_view token) {
    size_t pos = 0;
    while ((pos = hay.find(token, pos)) != std::string_view::npos) {
        if (pos == 0) return true;
        const unsigned char prev = static_cast<unsigned char>(hay[pos - 1]);
        const bool alnum = (prev >= '0' && prev <= '9') ||
                           (prev >= 'a' && prev <= 'z') || prev >= 0x80;
        if (!alnum) return true;
        ++pos;
    }
    return false;
}

// How well a page matches every token of the query; -1 when any token is
// missing (a multi-word query must match as a whole, in any order).
int match_score(const std::vector<std::string>& tokens,
                std::string_view key, std::string_view title_lower) {
    const std::string_view host = host_of(key);
    int total = 0;
    for (const auto& t : tokens) {
        int best = -1;
        auto take = [&best](int v) { best = std::max(best, v); };
        if (starts_with(host, t)) take(100);
        if (starts_with(key, t)) take(90);
        if (word_start_match(host, t)) take(70);
        if (word_start_match(title_lower, t)) take(60);
        if (host.find(t) != std::string_view::npos) take(45);
        if (title_lower.find(t) != std::string_view::npos) take(30);
        if (key.find(t) != std::string_view::npos) take(20);
        if (best < 0) return -1;
        total += best;
    }
    return total;
}

int frecency(const HistoryCandidate& h, int64_t now) {
    double score = std::min(30.0, 6.0 * std::log2(1.0 + std::max(0, h.visit_count)));
    score += std::min(24, 8 * std::max(0, h.typed_count));
    const int64_t age = now - h.last_visit_epoch;
    constexpr int64_t day = 24 * 3600;
    if (h.last_visit_epoch > 0 && now > 0) {
        if (age < day) score += 20;
        else if (age < 7 * day) score += 12;
        else if (age < 30 * day) score += 6;
        else if (age < 90 * day) score += 2;
    }
    return static_cast<int>(score);
}

bool is_junk_url(std::string_view url) {
    const std::string l = to_lower(trim(url));
    return l.empty() || starts_with(l, "javascript:") || starts_with(l, "data:") ||
           starts_with(l, "about:blank") || starts_with(l, "blob:");
}

int kind_rank(Kind k) {
    switch (k) {
        case Kind::OpenTab: return 3;
        case Kind::Bookmark: return 2;
        case Kind::History: return 1;
        default: return 0;
    }
}

struct Candidate {
    Suggestion row;
    int score = 0;
    int sources = 0;
};

Suggestion default_row(const std::string& query, const std::string& engine_name) {
    (void)engine_name;  // the UI composes "Search <engine> for ..." itself
    Suggestion s;
    s.fill = query;
    s.title = query;
    const bool internal = core::parse_internal_url(query).has_value();
    const core::Url parsed = core::parse_omnibox_input(query);
    s.kind = (internal || (parsed.is_valid() && !parsed.is_search_query()))
                 ? Kind::Navigate
                 : Kind::Search;
    return s;
}

} // namespace

std::vector<Suggestion> build_suggestions(const Inputs& in, size_t max_rows) {
    std::vector<Suggestion> out;
    const std::string query(trim(in.query));
    if (query.empty() || max_rows == 0) return out;

    out.push_back(default_row(query, in.engine_name));
    if (max_rows == 1) return out;

    const auto tokens = tokenize(query);
    const std::string query_key = url_key(query);
    std::unordered_map<std::string, size_t> index;  // key -> candidates slot
    std::vector<Candidate> cands;

    auto offer = [&](Kind kind, const std::string& url, const std::string& title,
                     uint64_t tab_id, int base_score) {
        if (is_junk_url(url)) return;
        const std::string key = url_key(url);
        if (key.empty()) return;
        // A URL typed in full is already the first row.
        if (out.front().kind == Kind::Navigate && key == query_key) return;
        const int m = match_score(tokens, key, to_lower(title));
        if (m < 0) return;
        const int score = m + base_score;

        Candidate c;
        c.row.kind = kind;
        c.row.fill = url;
        c.row.title = title;
        c.row.url = url;
        c.row.tab_id = tab_id;
        c.score = score;
        c.sources = 1;

        auto [it, inserted] = index.emplace(key, cands.size());
        if (inserted) {
            cands.push_back(std::move(c));
            return;
        }
        // Same destination from another source: merge, keeping the most
        // useful kind, the best title, and a small bonus for being everywhere.
        Candidate& have = cands[it->second];
        have.sources += 1;
        have.score = std::max(have.score, score) + 5;
        if (kind_rank(kind) > kind_rank(have.row.kind)) {
            have.row.kind = kind;
            have.row.tab_id = tab_id;
            have.row.fill = url;
            have.row.url = url;
        }
        if (have.row.title.empty() && !title.empty()) have.row.title = title;
    };

    for (const auto& h : in.history) {
        offer(Kind::History, h.url, h.title, 0, frecency(h, in.now_epoch));
    }
    for (const auto& b : in.bookmarks) {
        offer(Kind::Bookmark, b.url, b.title, 0, 18);
    }
    for (const auto& t : in.tabs) {
        if (t.active) continue;
        offer(Kind::OpenTab, t.url, t.title, t.tab_id, 25);
    }

    std::stable_sort(cands.begin(), cands.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.score > b.score;
                     });

    // Remote completions, cleaned and deduplicated.
    std::vector<std::string> remote;
    {
        std::unordered_set<std::string> seen{to_lower(query)};
        for (const auto& r : in.remote) {
            const std::string_view t = trim(r);
            if (t.empty() || t.size() > kMaxRemoteBytes) continue;
            if (!seen.insert(to_lower(t)).second) continue;
            remote.emplace_back(t);
        }
    }

    const size_t room = max_rows - 1;
    size_t local_cap = room;
    if (!remote.empty()) {
        // Leave at least a few rows for the engine's suggestions, but let
        // strong local matches use whatever the engine does not fill.
        local_cap = std::min(room, kMaxLocalWithRemote);
        if (remote.size() < room - local_cap) {
            local_cap = std::min(cands.size(), room - remote.size());
        }
    }
    const size_t local_n = std::min(local_cap, cands.size());
    for (size_t i = 0; i < local_n; ++i) out.push_back(std::move(cands[i].row));

    for (const auto& r : remote) {
        if (out.size() >= max_rows) break;
        Suggestion s;
        s.kind = Kind::RemoteSearch;
        s.fill = r;
        s.title = r;
        out.push_back(std::move(s));
    }
    return out;
}

bool should_request_remote(std::string_view query_in) {
    const std::string_view q = trim(query_in);
    if (q.size() < 2 || q.size() > kMaxQueryBytes) return false;
    const std::string l = to_lower(q);
    if (l.find("://") != std::string::npos) return false;
    for (const char* p : {"aurora:", "about:", "javascript:", "data:", "view-source:",
                          "file:", "blob:", "chrome:", "chrome-extension:"}) {
        if (starts_with(l, p)) return false;
    }
    if (core::parse_internal_url(std::string(q)).has_value()) return false;
    const core::Url parsed = core::parse_omnibox_input(q);
    if (!parsed.is_valid() || !parsed.is_search_query()) return false;
    return true;
}

std::string suggest_url_template(std::string_view engine_key) {
    if (engine_key == "duckduckgo") return "https://duckduckgo.com/ac/?type=list&q=%s";
    if (engine_key == "google") {
        return "https://suggestqueries.google.com/complete/search?client=firefox&q=%s";
    }
    if (engine_key == "bing") return "https://api.bing.com/osjson.aspx?query=%s";
    return {};
}

namespace {

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool hex4(std::string_view s, size_t at, uint32_t& v) {
    if (at + 4 > s.size()) return false;
    v = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char c = s[at + i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else return false;
    }
    return true;
}

// Reads a JSON string starting at s[i] == '"'. Leaves i just past the closing
// quote. Returns false on malformed input.
bool read_string(std::string_view s, size_t& i, std::string& out) {
    out.clear();
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i >= s.size()) return false;
        const char e = s[i++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(s, i, cp)) return false;
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo = 0;
                    if (i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u' &&
                        hex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        return false;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                append_utf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

void skip_ws(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

} // namespace

std::vector<std::string> parse_remote_suggestions(std::string_view json,
                                                  size_t max_results) {
    std::vector<std::string> out;
    constexpr size_t kMaxPayload = 64 * 1024;
    if (json.empty() || json.size() > kMaxPayload || max_results == 0) return out;

    size_t i = 0;
    skip_ws(json, i);
    if (i >= json.size() || json[i] != '[') return out;
    ++i;
    skip_ws(json, i);

    // Element 0: the echoed query.
    std::string tmp;
    if (!read_string(json, i, tmp)) return out;
    skip_ws(json, i);
    if (i >= json.size() || json[i] != ',') return out;
    ++i;
    skip_ws(json, i);

    // Element 1: the array of completions.
    if (i >= json.size() || json[i] != '[') return out;
    ++i;
    std::vector<std::string> parsed;
    while (true) {
        skip_ws(json, i);
        if (i >= json.size()) return {};
        if (json[i] == ']') break;
        if (json[i] == '"') {
            if (!read_string(json, i, tmp)) return {};
            if (parsed.size() < max_results) parsed.push_back(tmp);
        } else {
            return {};  // a non-string completion: not the shape we expect
        }
        skip_ws(json, i);
        if (i < json.size() && json[i] == ',') ++i;
    }
    return parsed;
}

} // namespace aurora::omnibox

// Aurora — text helpers for the page right-click menu.
//
// Header-only and free of CEF types so the edge cases (multi-byte characters
// cut by a byte cap, whitespace runs, empty selections) can be unit-tested
// without an engine.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace aurora::cef {

// Selected text as a menu label / search query: whitespace runs collapsed to a
// single space, trimmed, and capped at `max_bytes` so selecting a whole page
// cannot become a huge query. Never leaves half a UTF-8 character at the end.
inline std::string tidy_selection(std::string_view in, size_t max_bytes = 300) {
    std::string out;
    out.reserve(std::min(in.size(), max_bytes + 4));
    bool pending_space = false;
    for (const unsigned char c : in) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            pending_space = !out.empty();
            continue;
        }
        if (out.size() + (pending_space ? 2 : 1) > max_bytes) break;
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(c));
    }
    // The byte cap may have cut a multi-byte character in half: drop it.
    size_t lead = out.size();
    while (lead > 0 && (static_cast<unsigned char>(out[lead - 1]) & 0xC0) == 0x80) --lead;
    if (lead > 0) {
        const unsigned char c = static_cast<unsigned char>(out[lead - 1]);
        const size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (out.size() - (lead - 1) < need) out.resize(lead - 1);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The first `max_chars` characters (not bytes) of `s`, with an ellipsis when
// something was cut.
inline std::string menu_snippet(const std::string& s, size_t max_chars) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (chars == max_chars) break;
            ++chars;
        }
        ++i;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "\xE2\x80\xA6";
}

} // namespace aurora::cef

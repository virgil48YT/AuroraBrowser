#include "core/InternalUrl.hpp"

#include <algorithm>
#include <cctype>

namespace aurora::core {

namespace {

std::string lowered_trimmed(std::string_view in) {
    size_t b = 0, e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    std::string out(in.substr(b, e - b));
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    // A single trailing slash is cosmetic.
    if (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

bool has_scheme(std::string_view lowered, std::string_view scheme) {
    if (lowered.size() < scheme.size() + 1) return false;
    if (lowered.compare(0, scheme.size(), scheme) != 0) return false;
    return lowered[scheme.size()] == ':';
}

} // namespace

std::optional<InternalPage> parse_internal_url(std::string_view input) {
    const std::string s = lowered_trimmed(input);
    const std::string prefix(kInternalScheme);
    if (s.size() <= prefix.size() + 1) return std::nullopt;
    if (s.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    if (s[prefix.size()] != ':') return std::nullopt;

    // Skip "aurora:" and an optional "//".
    size_t pos = prefix.size() + 1;
    if (s.compare(pos, 2, "//") == 0) pos += 2;
    const std::string host = s.substr(pos);

    if (host == "history") return InternalPage::History;
    if (host == "downloads") return InternalPage::Downloads;
    if (host == "bookmarks") return InternalPage::Bookmarks;
    if (host == "extensions") return InternalPage::Extensions;
    if (host == "settings" || host == "preferences") return InternalPage::Settings;
    return std::nullopt;
}

bool is_extension_scheme(std::string_view input) {
    return has_scheme(lowered_trimmed(input), kExtensionScheme);
}

bool is_internal_url(std::string_view input) {
    // chrome-extension:// is in-app even though it is not an InternalPage.
    if (is_extension_scheme(input)) return true;
    if (parse_internal_url(input).has_value()) return true;
    // Any aurora: URL (including an unknown host) stays inside the browser.
    return has_scheme(lowered_trimmed(input), kInternalScheme);
}

std::string internal_url(InternalPage page) {
    switch (page) {
        case InternalPage::History:   return "aurora://history";
        case InternalPage::Downloads: return "aurora://downloads";
        case InternalPage::Bookmarks: return "aurora://bookmarks";
        case InternalPage::Extensions: return "aurora://extensions";
        case InternalPage::Settings:  return "aurora://settings";
    }
    return "aurora://settings";
}

std::string internal_page_title(InternalPage page) {
    switch (page) {
        case InternalPage::History:   return "History";
        case InternalPage::Downloads: return "Downloads";
        case InternalPage::Bookmarks: return "Bookmarks";
        case InternalPage::Extensions: return "Extensions";
        case InternalPage::Settings:  return "Settings";
    }
    return "Aurora";
}

} // namespace aurora::core

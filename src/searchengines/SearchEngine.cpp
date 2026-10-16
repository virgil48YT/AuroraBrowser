#include "searchengines/SearchEngine.hpp"

#include <array>
#include <cctype>
#include <charconv>

#include "core/Url.hpp"

namespace aurora::searchengines {

namespace {

constexpr std::array<BuiltinEngine, 4> kBuiltins{{
    {"duckduckgo", "DuckDuckGo"},
    {"google", "Google"},
    {"bing", "Bing"},
    {"startpage", "Startpage"},
}};

constexpr std::string_view kCustomPrefix = "custom:";
constexpr std::string_view kHttpsPrefix = "https://";

bool is_space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool is_control(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return uc < 0x20 || uc == 0x7f;
}

bool is_alnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

bool starts_with_https(std::string_view s) {
    if (s.size() < kHttpsPrefix.size()) return false;
    for (size_t i = 0; i < kHttpsPrefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != kHttpsPrefix[i])
            return false;
    }
    return true;
}

// The authority ("host[:port]") of an https:// template.
std::string_view authority_of(std::string_view url_template) {
    const auto rest = url_template.substr(kHttpsPrefix.size());
    const auto end = rest.find_first_of("/?#");
    return end == std::string_view::npos ? rest : rest.substr(0, end);
}

size_t count_placeholders(std::string_view s) {
    size_t n = 0;
    const auto& ph = core::kSearchTermsPlaceholder;
    for (auto pos = s.find(ph); pos != std::string_view::npos;
         pos = s.find(ph, pos + ph.size())) {
        ++n;
    }
    return n;
}

bool valid_port(std::string_view port) {
    if (port.empty() || port.size() > 5) return false;
    unsigned value = 0;
    const auto* first = port.data();
    const auto* last = port.data() + port.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc() && ptr == last && value > 0 && value <= 65535;
}

bool valid_authority(std::string_view authority) {
    if (authority.empty()) return false;
    if (authority.find('@') != std::string_view::npos) return false;  // creds
    std::string_view host = authority;
    std::string_view port;
    if (authority.front() == '[') {
        // IPv6 literal: [hex:...]
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close == 1) return false;
        for (char c : authority.substr(1, close - 1)) {
            if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == ':' ||
                  c == '.'))
                return false;
        }
        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return false;
            port = after.substr(1);
            if (!valid_port(port)) return false;
        }
        return true;
    }
    const auto colon = authority.find(':');
    if (colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
        if (!valid_port(port)) return false;
    }
    if (host.empty() || host.front() == '.' || host.back() == '.' ||
        host.front() == '-' || host.back() == '-')
        return false;
    for (char c : host) {
        if (!(is_alnum(c) || c == '.' || c == '-')) return false;
    }
    return true;
}

} // namespace

std::span<const BuiltinEngine> builtin_engines() {
    return {kBuiltins.data(), kBuiltins.size()};
}

bool is_builtin_key(std::string_view key) {
    for (const auto& b : kBuiltins) {
        if (b.key == key) return true;
    }
    return false;
}

TemplateError validate_template(std::string_view url_template) {
    if (url_template.empty()) return TemplateError::Empty;
    if (url_template.size() > kMaxTemplateLength) return TemplateError::TooLong;
    for (char c : url_template) {
        if (is_space(c) || is_control(c)) return TemplateError::InvalidCharacter;
    }
    if (!starts_with_https(url_template)) return TemplateError::NotHttps;
    const auto authority = authority_of(url_template);
    if (count_placeholders(authority) > 0) return TemplateError::PlaceholderInHost;
    if (!valid_authority(authority)) return TemplateError::InvalidHost;
    const auto placeholders = count_placeholders(url_template);
    if (placeholders == 0) return TemplateError::MissingPlaceholder;
    if (placeholders > 1) return TemplateError::MultiplePlaceholders;
    return TemplateError::Ok;
}

NameError validate_name(std::string_view name) {
    if (name.empty()) return NameError::Empty;
    if (name.size() > kMaxNameLength) return NameError::TooLong;
    for (char c : name) {
        if (is_control(c)) return NameError::InvalidCharacter;
    }
    return NameError::Ok;
}

std::string_view describe(TemplateError error) {
    switch (error) {
        case TemplateError::Ok: return "";
        case TemplateError::Empty: return "Enter a search URL.";
        case TemplateError::TooLong: return "The search URL is too long.";
        case TemplateError::InvalidCharacter:
            return "The search URL cannot contain spaces or control characters.";
        case TemplateError::NotHttps:
            return "The search URL must start with https://.";
        case TemplateError::InvalidHost:
            return "The search URL needs a valid host name (no user name or "
                   "password).";
        case TemplateError::PlaceholderInHost:
            return "%s must be in the path or query, not in the host name.";
        case TemplateError::MissingPlaceholder:
            return "Put %s where the search terms go, e.g. "
                   "https://example.com/search?q=%s.";
        case TemplateError::MultiplePlaceholders:
            return "Use %s exactly once.";
    }
    return "The search URL is not valid.";
}

std::string_view describe(NameError error) {
    switch (error) {
        case NameError::Ok: return "";
        case NameError::Empty: return "Enter a name.";
        case NameError::TooLong: return "The name is too long (64 bytes max).";
        case NameError::InvalidCharacter:
            return "The name cannot contain control characters.";
    }
    return "The name is not valid.";
}

std::string normalize_input(std::string_view text) {
    size_t b = 0, e = text.size();
    while (b < e && is_space(text[b])) ++b;
    while (e > b && is_space(text[e - 1])) --e;
    return std::string(text.substr(b, e - b));
}

std::string expand_template(std::string_view url_template, std::string_view terms) {
    return core::expand_search_template(url_template, terms);
}

std::string template_home(std::string_view url_template) {
    if (validate_template(url_template) != TemplateError::Ok) return {};
    return std::string(kHttpsPrefix) + std::string(authority_of(url_template)) + "/";
}

std::string custom_engine_key(int64_t id) {
    return std::string(kCustomPrefix) + std::to_string(id);
}

std::optional<int64_t> parse_custom_engine_key(std::string_view key) {
    if (key.substr(0, kCustomPrefix.size()) != kCustomPrefix) return std::nullopt;
    const auto digits = key.substr(kCustomPrefix.size());
    if (digits.empty() || digits.size() > 18) return std::nullopt;
    int64_t id = 0;
    const auto* last = digits.data() + digits.size();
    const auto [ptr, ec] = std::from_chars(digits.data(), last, id);
    if (ec != std::errc() || ptr != last || id <= 0) return std::nullopt;
    return id;
}

ResolvedEngine resolve_default_engine(std::string_view pref_value,
                                      const std::vector<SearchEngine>& custom) {
    if (const auto id = parse_custom_engine_key(pref_value)) {
        for (const auto& engine : custom) {
            if (engine.id != *id) continue;
            // Rows are validated on write; re-check so a hand-edited database
            // can never route searches to a non-HTTPS or malformed URL.
            if (validate_template(engine.url_template) != TemplateError::Ok) break;
            ResolvedEngine r;
            r.key = custom_engine_key(engine.id);
            r.name = engine.name;
            r.url_template = engine.url_template;
            r.home = template_home(engine.url_template);
            r.is_custom = true;
            return r;
        }
    }
    const BuiltinEngine* chosen = &kBuiltins[0];
    bool fell_back = true;
    for (const auto& b : kBuiltins) {
        if (b.key == pref_value) {
            chosen = &b;
            fell_back = false;
            break;
        }
    }
    ResolvedEngine r;
    r.key = std::string(chosen->key);
    r.name = std::string(chosen->name);
    r.url_template = core::search_engine_template(chosen->key) +
                     std::string(core::kSearchTermsPlaceholder);
    r.home = core::search_engine_home(chosen->key);
    r.fell_back = fell_back;
    return r;
}

} // namespace aurora::searchengines

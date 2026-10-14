#include "core/Url.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace aurora::core {

namespace {

bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)); }
bool is_alnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}
char to_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return to_lower(static_cast<char>(c)); });
    return out;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
// The hyphen is load-bearing: without it `chrome-extension` never parses as a
// scheme and falls through to the search-query path, which is what dispatched
// extension URLs to the OS browser.
bool is_scheme_tail(char c) {
    return is_alnum(c) || c == '+' || c == '-' || c == '.';
}

bool looks_like_scheme(std::string_view s, size_t colon) {
    if (colon == 0 || colon == std::string_view::npos) return false;
    if (!is_alpha(s[0])) return false;
    return std::all_of(s.begin() + 1,
                       s.begin() + static_cast<std::ptrdiff_t>(colon),
                       is_scheme_tail);
}

// A token is a plausible domain label: letters/digits and hyphens, not
// leading/trailing hyphen, length 1..63.
bool looks_like_domain(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    size_t start = 0;
    size_t labels = 0;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '.') {
            auto label = host.substr(start, i - start);
            if (label.empty() || label.size() > 63) return false;
            if (label.front() == '-' || label.back() == '-') return false;
            for (char c : label) {
                if (!(is_alnum(c) || c == '-')) return false;
            }
            ++labels;
            start = i + 1;
        }
    }
    return labels >= 2; // require at least "example.com"
}

// True if the string contains a space or looks like a sentence rather than a
// host. We treat anything with a space as a search query.
bool has_space(std::string_view s) {
    return std::any_of(s.begin(), s.end(),
                      [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
}

} // namespace

Url::Url(std::string_view input) : raw_(trim(input)) {
    if (raw_.empty()) {
        valid_ = false;
        return;
    }

    // Split scheme.
    size_t colon = raw_.find(':');
    size_t scheme_end = std::string::npos;
    if (looks_like_scheme(raw_, colon)) {
        scheme_ = to_lower(raw_.substr(0, colon));
        scheme_end = colon;
    }

    // Strip authority "//".
    size_t auth_start = scheme_end == std::string::npos ? std::string::npos
                       : (raw_.compare(scheme_end + 1, 2, "//") == 0
                              ? scheme_end + 3
                              : scheme_end + 1);

    if (scheme_ == "about" || scheme_ == "chrome" || scheme_ == "data" ||
        scheme_ == "aurora") {
        path_ = auth_start == std::string::npos ? raw_.substr(scheme_end + 1)
                                                : raw_.substr(auth_start);
        valid_ = true;
        return;
    }

    if (scheme_ == "http" || scheme_ == "https" || scheme_ == "file" ||
        scheme_ == "chrome-extension") {
        if (scheme_ == "file" && auth_start == std::string::npos) {
            valid_ = false;
            return;
        }
        size_t host_end = raw_.find('/', auth_start);
        if (host_end == std::string::npos) {
            host_ = raw_.substr(auth_start);
            path_ = "/";
        } else {
            host_ = raw_.substr(auth_start, host_end - auth_start);
            path_ = raw_.substr(host_end);
        }
        size_t q = path_.find('?');
        if (q != std::string::npos) {
            query_ = path_.substr(q + 1);
            path_ = path_.substr(0, q);
        }
        host_ = to_lower(host_);
        if (path_.empty()) path_ = "/";
        if (scheme_ == "chrome-extension") {
            // Always an in-app URL: never a search query, never dispatched
            // externally, even if the host/id is missing.
            search_query_ = false;
            valid_ = true;
        } else {
            valid_ = !host_.empty();
        }
        return;
    }

    // No recognized scheme: treat as a URL only if it looks like a bare
    // domain, otherwise it is a search query. This mirrors the omnibox rule
    // "host has a dot and no spaces -> URL".
    if (scheme_.empty() && !has_space(raw_)) {
        std::string candidate = raw_;
        // Strip a trailing slash for the domain check.
        if (!candidate.empty() && candidate.back() == '/') candidate.pop_back();
        size_t slash = candidate.find('/');
        std::string host_part = slash == std::string::npos
                                    ? candidate
                                    : candidate.substr(0, slash);
        if (looks_like_domain(host_part)) {
            scheme_ = "https";
            host_ = to_lower(host_part);
            path_ = slash == std::string::npos ? "/" : candidate.substr(slash);
            valid_ = true;
            return;
        }
    }

    // Otherwise it is a search query.
    search_query_ = true;
    valid_ = true;
    query_ = raw_;
}

std::string Url::display() const {
    if (search_query_) return raw_;
    std::ostringstream os;
    if (!scheme_.empty()) os << scheme_ << "://";
    os << host_ << path_;
    if (!query_.empty()) os << '?' << query_;
    return os.str();
}

std::string Url::normalized() const {
    if (search_query_) return std::string("?q=") + raw_;
    std::string s;
    s.reserve(raw_.size());
    s += scheme_;
    s += "://";
    s += host_;
    s += path_;
    // Collapse a trailing duplicate slash.
    if (s.size() > 1 && s.back() == '/' && path_.size() > 1) {
        // keep root slash, do nothing extra
    }
    if (!query_.empty()) {
        s += '?';
        s += query_;
    }
    return s;
}

std::string Url::origin() const {
    if (search_query_ || host_.empty()) return {};
    std::ostringstream os;
    os << scheme_ << "://" << host_;
    return os.str();
}

Url parse_omnibox_input(std::string_view input) {
    return Url(input);
}

std::string search_engine_template(std::string_view engine_id) {
    if (engine_id == "google") return "https://www.google.com/search?q=";
    if (engine_id == "bing") return "https://www.bing.com/search?q=";
    if (engine_id == "startpage") return "https://www.startpage.com/sp/search?query=";
    return "https://duckduckgo.com/?q=";
}

std::string search_engine_home(std::string_view engine_id) {
    if (engine_id == "google") return "https://www.google.com/";
    if (engine_id == "bing") return "https://www.bing.com/";
    if (engine_id == "startpage") return "https://www.startpage.com/";
    return "https://duckduckgo.com/";
}

std::string resolve_navigation(const Url& url) {
    return resolve_navigation(url, search_engine_template("duckduckgo"));
}

std::string resolve_navigation(const Url& url, std::string_view engine_template) {
    // Extension and other in-app schemes must round-trip as themselves.
    // Routing them through a search engine is what opened Chromium as an
    // external window on install/options clicks.
    if (url.is_extension_url() || url.is_internal_scheme()) return url.display();
    if (!url.is_search_query()) return url.display();
    // Route a bare search query to a search engine, percent-encoding the text.
    const std::string kEngine(engine_template);
    std::string encoded;
    encoded.reserve(url.raw().size() * 3);
    for (char c : url.raw()) {
        auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(c);
        } else if (c == ' ') {
            encoded.push_back('+');
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", uc);
            encoded.append(buf);
        }
    }
    return kEngine + encoded;
}

} // namespace aurora::core

// Aurora — "site" boundaries for third-party cookie blocking.
// See CookieSite.hpp for why this is an approximation of the Public Suffix
// List rather than the real thing.
#include "privacy/CookieSite.hpp"

#include <algorithm>
#include <iterator>
#include <cctype>

namespace aurora::privacy {

namespace {

// Multi-label public suffixes Aurora knows about. NOT the Public Suffix List:
// a deliberately small, hand-picked table of the suffixes users meet most
// often, so that two unrelated owners under one of them are not treated as
// the same site. Entries are lower-case, without leading or trailing dots.
// Anything missing here falls back to the generic ccTLD rule below, and then
// to "last two labels". Keep it sorted by group for review; lookup is linear
// (it runs once per sub-resource request on the IO thread, ~100 compares).
constexpr std::string_view kMultiLabelPublicSuffixes[] = {
    // --- ccTLD second levels (ICANN section of the PSL) ---------------------
    // United Kingdom
    "co.uk", "org.uk", "me.uk", "ltd.uk", "plc.uk", "net.uk", "sch.uk",
    "ac.uk", "gov.uk", "nhs.uk", "police.uk",
    // Australia
    "com.au", "net.au", "org.au", "edu.au", "gov.au", "asn.au", "id.au",
    // New Zealand
    "co.nz", "net.nz", "org.nz", "govt.nz", "ac.nz", "school.nz", "geek.nz",
    // Japan
    "co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp", "ad.jp", "ed.jp", "gr.jp",
    "lg.jp",
    // South Korea
    "co.kr", "or.kr", "ne.kr", "go.kr", "ac.kr", "re.kr",
    // Brazil
    "com.br", "net.br", "org.br", "gov.br", "edu.br", "art.br",
    // China / Hong Kong / Taiwan / Singapore
    "com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn", "com.hk", "org.hk",
    "com.tw", "org.tw", "com.sg", "edu.sg",
    // India
    "co.in", "net.in", "org.in", "firm.in", "gen.in", "ind.in", "ac.in",
    "gov.in",
    // South Africa / Israel / Turkey / Mexico / Argentina / Ukraine
    "co.za", "org.za", "gov.za", "co.il", "org.il", "ac.il", "com.tr",
    "org.tr", "com.mx", "org.mx", "gob.mx", "com.ar", "org.ar", "com.ua",
    // Europe (the handful of EU ccTLDs that use second levels)
    "co.at", "or.at", "com.pl", "net.pl", "org.pl", "com.es", "org.es",
    "com.pt", "com.gr",
    // --- Multi-tenant hosting (PRIVATE section of the PSL) ------------------
    // Every subdomain is a different user's site: a.github.io and b.github.io
    // must not share cookies.
    "github.io", "gitlab.io", "pages.dev", "workers.dev", "netlify.app",
    "vercel.app", "herokuapp.com", "appspot.com", "blogspot.com", "web.app",
    "firebaseapp.com", "azurewebsites.net", "cloudfront.net",
    "readthedocs.io", "onrender.com", "fly.dev", "glitch.me", "surge.sh",
    "codeberg.page", "ngrok.io", "duckdns.org",
};

// Second labels that two-letter ccTLDs commonly reserve as public registries
// ("example.co.xx", "example.com.xx"). Used only when the suffix is not in
// the table above, as a coarse catch-all for ccTLDs Aurora does not list.
constexpr std::string_view kGenericSecondLevel[] = {
    "co", "com", "net", "org", "gov", "edu", "ac", "or", "ne", "gob", "go",
};

bool is_ip_literal(std::string_view host) {
    if (host.empty()) return false;
    if (host.front() == '[' || host.find(':') != std::string_view::npos)
        return true;  // IPv6 (bracketed or bare)
    return host.find_first_not_of("0123456789.") == std::string_view::npos;
}

bool ends_with_label(std::string_view host, std::string_view suffix) {
    if (host.size() <= suffix.size()) return false;
    return host.substr(host.size() - suffix.size()) == suffix &&
           host[host.size() - suffix.size() - 1] == '.';
}

// Length of the public suffix of `host` (normalized, not an IP, at least two
// labels): the longest table match, else the generic ccTLD rule, else the
// last label.
std::size_t public_suffix_length(std::string_view host) {
    std::size_t best = 0;
    for (const auto suffix : kMultiLabelPublicSuffixes) {
        if ((host == suffix || ends_with_label(host, suffix)) &&
            suffix.size() > best)
            best = suffix.size();
    }
    if (best != 0) return best;

    const auto last = host.rfind('.');
    const std::string_view tld = host.substr(last + 1);
    const auto second = last == 0 ? std::string_view::npos
                                  : host.rfind('.', last - 1);
    const std::string_view sld =
        second == std::string_view::npos
            ? host.substr(0, last)
            : host.substr(second + 1, last - second - 1);
    if (tld.size() == 2 &&
        std::find(std::begin(kGenericSecondLevel), std::end(kGenericSecondLevel),
                  sld) != std::end(kGenericSecondLevel))
        return sld.size() + 1 + tld.size();
    return tld.size();
}

} // namespace

std::string normalize_host(std::string_view host) {
    std::string out(host);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

bool is_known_multi_label_public_suffix(std::string_view suffix) {
    const std::string normalized = normalize_host(suffix);
    return std::find(std::begin(kMultiLabelPublicSuffixes),
                     std::end(kMultiLabelPublicSuffixes),
                     std::string_view(normalized)) !=
           std::end(kMultiLabelPublicSuffixes);
}

std::string cookie_site(std::string_view raw_host) {
    std::string host = normalize_host(raw_host);
    if (host.empty() || is_ip_literal(host)) return host;
    if (host.find('.') == std::string::npos || host.front() == '.')
        return host;  // single label ("localhost") or malformed
    const std::size_t suffix_len = public_suffix_length(host);
    if (suffix_len >= host.size()) return host;  // host IS a public suffix
    // host = <labels>.<suffix>; keep exactly one label before the suffix.
    const std::size_t dot_before_suffix = host.size() - suffix_len - 1;
    if (dot_before_suffix == 0) return host;
    const auto start = host.rfind('.', dot_before_suffix - 1);
    return start == std::string::npos ? host : host.substr(start + 1);
}

bool same_cookie_site(std::string_view host_a, std::string_view host_b) {
    const std::string a = cookie_site(host_a);
    return !a.empty() && a == cookie_site(host_b);
}

} // namespace aurora::privacy

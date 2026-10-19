// Aurora — "site" boundaries for third-party cookie blocking.
// See CookieSite.hpp: the bundled Public Suffix List when installed, the
// embedded table below as the fallback.
#include "privacy/CookieSite.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>

namespace aurora::privacy {

namespace {

// FALLBACK table — used only while no Public Suffix List is installed.
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

// Fallback: length of the public suffix of `host` (normalized, not an IP, at
// least two labels): the longest table match, else the generic ccTLD rule,
// else the last label.
std::size_t fallback_public_suffix_length(std::string_view host) {
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


bool sorted_contains(const std::vector<std::string>& v, std::string_view key) {
    return std::binary_search(v.begin(), v.end(), key, std::less<>());
}

void sort_unique(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// --- Punycode (RFC 3492), encoder only ---------------------------------------
constexpr std::uint32_t kBase = 36, kTMin = 1, kTMax = 26, kSkew = 38,
                        kDamp = 700, kInitialBias = 72, kInitialN = 128;

std::uint32_t adapt(std::uint32_t delta, std::uint32_t num_points, bool first) {
    delta = first ? delta / kDamp : delta / 2;
    delta += delta / num_points;
    std::uint32_t k = 0;
    while (delta > ((kBase - kTMin) * kTMax) / 2) {
        delta /= kBase - kTMin;
        k += kBase;
    }
    return k + (kBase - kTMin + 1) * delta / (delta + kSkew);
}

char encode_digit(std::uint32_t d) {
    return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26));
}

// UTF-8 -> code points. False on malformed input.
bool decode_utf8(std::string_view in, std::vector<std::uint32_t>& out) {
    for (std::size_t i = 0; i < in.size();) {
        const auto c = static_cast<unsigned char>(in[i]);
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
        else return false;
        if (i + len > in.size()) return false;
        for (std::size_t j = 1; j < len; ++j) {
            const auto cc = static_cast<unsigned char>(in[i + j]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        out.push_back(cp);
        i += len;
    }
    return true;
}

// Normalize one PSL rule body (no "!" / "*." prefix): lower-case ASCII,
// punycode each non-ASCII label. Empty when the rule is unusable.
std::string normalize_rule(std::string_view rule) {
    std::string out;
    std::size_t start = 0;
    while (start <= rule.size()) {
        const auto dot = rule.find('.', start);
        const auto label = rule.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (label.empty()) return {};
        const std::string encoded = punycode_label(label);
        if (encoded.empty()) return {};
        if (!out.empty()) out.push_back('.');
        out += encoded;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return out;
}

// The installed list. Readers load it with acquire semantics; installs keep
// every list ever installed alive so a reader on CEF's IO thread holding the
// old pointer can never dangle (installs happen once per process in practice).
std::atomic<const PublicSuffixList*> g_installed{nullptr};
std::mutex g_install_mutex;
std::vector<std::unique_ptr<PublicSuffixList>>& installed_lists() {
    static std::vector<std::unique_ptr<PublicSuffixList>> lists;
    return lists;
}

} // namespace

// --- PublicSuffixList ----------------------------------------------------------

PublicSuffixList PublicSuffixList::parse(std::string_view text) {
    PublicSuffixList list;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        // First whitespace-delimited token only (the PSL format rule).
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string_view::npos) continue;
        line = line.substr(begin);
        const auto end = line.find_first_of(" \t\r");
        if (end != std::string_view::npos) line = line.substr(0, end);
        if (line.empty() || line.rfind("//", 0) == 0) continue;

        if (line.front() == '!') {
            const std::string rule = normalize_rule(line.substr(1));
            if (!rule.empty() && rule.find('.') != std::string::npos)
                list.exceptions_.push_back(rule);
        } else if (line.rfind("*.", 0) == 0) {
            const std::string rule = normalize_rule(line.substr(2));
            if (!rule.empty()) list.wildcards_.push_back(rule);
        } else if (line.find('*') == std::string_view::npos) {
            const std::string rule = normalize_rule(line);
            if (!rule.empty()) list.rules_.push_back(rule);
        }
        // Wildcards anywhere but the leftmost label are not part of the
        // format and are ignored.
    }
    sort_unique(list.rules_);
    sort_unique(list.wildcards_);
    sort_unique(list.exceptions_);
    return list;
}

std::size_t PublicSuffixList::rule_count() const {
    return rules_.size() + wildcards_.size() + exceptions_.size();
}

std::size_t PublicSuffixList::public_suffix_length(std::string_view host) const {
    // Label start offsets, leftmost first: "a.b.c" -> {0, 2, 4}.
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < host.size(); ++i)
        if (host[i] == '.' && i + 1 < host.size()) starts.push_back(i + 1);

    // Scan candidate suffixes from longest to shortest. Exception rules
    // prevail over everything (the public suffix is then the exception minus
    // its leftmost label); otherwise the longest matching normal or wildcard
    // rule wins, and with none the implicit "*" rule makes the TLD the suffix.
    std::size_t best = 0;
    for (std::size_t k = 0; k < starts.size(); ++k) {
        const std::string_view candidate = host.substr(starts[k]);
        if (sorted_contains(exceptions_, candidate) && k + 1 < starts.size())
            return host.size() - starts[k + 1];
        if (best != 0) continue;
        const bool wildcard = k + 1 < starts.size() &&
                              sorted_contains(wildcards_, host.substr(starts[k + 1]));
        if (wildcard || sorted_contains(rules_, candidate))
            best = candidate.size();
    }
    return best != 0 ? best : host.size() - starts.back();
}

std::string PublicSuffixList::registrable_domain(std::string_view host) const {
    const std::size_t suffix_len = public_suffix_length(host);
    if (suffix_len >= host.size()) return {};
    const std::size_t dot_before_suffix = host.size() - suffix_len - 1;
    if (dot_before_suffix == 0) return {};
    const auto start = host.rfind('.', dot_before_suffix - 1);
    return std::string(start == std::string_view::npos ? host
                                                       : host.substr(start + 1));
}

bool install_public_suffix_list(PublicSuffixList list) {
    if (list.rule_count() < kMinimumPublicSuffixRules) return false;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    installed_lists().push_back(std::make_unique<PublicSuffixList>(std::move(list)));
    g_installed.store(installed_lists().back().get(), std::memory_order_release);
    return true;
}

bool install_public_suffix_list(std::string_view text) {
    return install_public_suffix_list(PublicSuffixList::parse(text));
}

bool public_suffix_list_installed() {
    return g_installed.load(std::memory_order_acquire) != nullptr;
}

void reset_public_suffix_list_for_testing() {
    g_installed.store(nullptr, std::memory_order_release);
}

std::string punycode_label(std::string_view utf8_label) {
    const bool ascii = std::all_of(utf8_label.begin(), utf8_label.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x80;
    });
    if (ascii) return normalize_host(utf8_label);

    std::vector<std::uint32_t> input;
    if (!decode_utf8(utf8_label, input)) return {};
    std::string out = "xn--";
    std::uint32_t basic = 0;
    for (const auto cp : input) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(cp))));
            ++basic;
        }
    }
    std::uint32_t handled = basic;
    if (basic > 0) out.push_back('-');
    std::uint32_t n = kInitialN, delta = 0, bias = kInitialBias;
    const auto total = static_cast<std::uint32_t>(input.size());
    while (handled < total) {
        std::uint32_t m = UINT32_MAX;
        for (const auto cp : input)
            if (cp >= n && cp < m) m = cp;
        delta += (m - n) * (handled + 1);
        n = m;
        for (const auto cp : input) {
            if (cp < n) ++delta;
            if (cp != n) continue;
            std::uint32_t q = delta;
            for (std::uint32_t k = kBase;; k += kBase) {
                const std::uint32_t t =
                    k <= bias ? kTMin : (k >= bias + kTMax ? kTMax : k - bias);
                if (q < t) break;
                out.push_back(encode_digit(t + (q - t) % (kBase - t)));
                q = (q - t) / (kBase - t);
            }
            out.push_back(encode_digit(q));
            bias = adapt(delta, handled + 1, handled == basic);
            delta = 0;
            ++handled;
        }
        ++delta;
        ++n;
    }
    return out;
}

namespace {
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

namespace {

// Shared shape of both cookie_site() overloads: degenerate hosts are their
// own site, everything else keeps exactly one label before its public suffix.
template <typename SuffixLength>
std::string site_of(std::string_view raw_host, SuffixLength suffix_length) {
    std::string host = normalize_host(raw_host);
    if (host.empty() || is_ip_literal(host)) return host;
    if (host.find('.') == std::string::npos || host.front() == '.')
        return host;  // single label ("localhost") or malformed
    const std::size_t suffix_len = suffix_length(host);
    if (suffix_len >= host.size()) return host;  // host IS a public suffix
    // host = <labels>.<suffix>; keep exactly one label before the suffix.
    const std::size_t dot_before_suffix = host.size() - suffix_len - 1;
    if (dot_before_suffix == 0) return host;
    const auto start = host.rfind('.', dot_before_suffix - 1);
    return start == std::string::npos ? host : host.substr(start + 1);
}

} // namespace

std::string cookie_site(std::string_view raw_host, const PublicSuffixList& list) {
    return site_of(raw_host, [&list](std::string_view h) {
        return list.public_suffix_length(h);
    });
}

std::string cookie_site(std::string_view raw_host) {
    if (const PublicSuffixList* list = g_installed.load(std::memory_order_acquire))
        return cookie_site(raw_host, *list);
    return site_of(raw_host, fallback_public_suffix_length);
}

bool same_cookie_site(std::string_view host_a, std::string_view host_b) {
    const std::string a = cookie_site(host_a);
    return !a.empty() && a == cookie_site(host_b);
}

} // namespace aurora::privacy

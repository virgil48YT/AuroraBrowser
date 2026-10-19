// Aurora — "site" boundaries for third-party cookie blocking.
//
// Chromium decides "third party" by comparing registrable domains (eTLD+1)
// computed from the Public Suffix List (https://publicsuffix.org). Aurora does
// the same: the real list is bundled (third_party/publicsuffix/
// public_suffix_list.dat, compiled into the browser as a Qt resource) and
// installed once at startup with install_public_suffix_list(), before CEF's
// IO thread asks its first question. Both PSL sections are honoured — ICANN
// suffixes (co.uk, com.au, ...) and PRIVATE multi-tenant suffixes (github.io,
// pages.dev, ...), where every subdomain belongs to a different owner — with
// the full rule grammar: normal rules, wildcards ("*.ck") and exceptions
// ("!www.ck"). Unicode rules are converted to their punycode ("xn--") form at
// load time, because the hosts CEF hands in are already ASCII.
//
// Fallback: until a list is installed (or if the bundled one is missing or
// corrupt) cookie_site() uses a small embedded table of the most common
// multi-label suffixes plus a generic rule for two-letter ccTLDs whose second
// label is a well-known generic one (co., com., net., org., ...). Hosts under
// a public suffix missing from that table fall back to "last two labels",
// i.e. they are treated as same-site with their siblings — the permissive
// direction, which can only let a cookie through, never break a genuine
// first-party login.
//
// No Qt, no CEF: the CEF adapter feeds Chromium-parsed hosts in, and the
// logic is unit-tested in tests/test_cookie_site.cpp (against both the
// fallback table and the real bundled list).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aurora::privacy {

// A parsed Public Suffix List. Immutable after parse(); lookups are const and
// safe from any thread.
class PublicSuffixList {
public:
    // Parse the publicsuffix.org text format: one rule per line, "//"
    // comments, blank lines ignored, only the first whitespace-separated
    // token of a line counts. Rules are lower-cased; non-ASCII labels are
    // punycode-encoded. Malformed lines are skipped, never fatal.
    static PublicSuffixList parse(std::string_view text);

    // Number of rules of all three kinds.
    std::size_t rule_count() const;
    bool empty() const { return rule_count() == 0; }

    // Length in characters of the public suffix of `host`, which must be
    // normalized (see normalize_host), non-empty and not an IP literal. Never
    // zero: with no matching rule the PSL's implicit "*" rule makes the last
    // label the suffix. A result equal to host.size() means the host is
    // itself a public suffix.
    std::size_t public_suffix_length(std::string_view host) const;

    // The registrable domain (eTLD+1) of a normalized host, or empty when the
    // host is itself a public suffix.
    std::string registrable_domain(std::string_view host) const;

private:
    // Sorted, unique; looked up with binary search (string_view keys).
    std::vector<std::string> rules_;       // "co.uk"
    std::vector<std::string> wildcards_;   // "*.ck" stored as "ck"
    std::vector<std::string> exceptions_;  // "!www.ck" stored as "www.ck"
};

// Install `list` as the process-wide PSL that cookie_site() consults. Call on
// the UI thread at startup, before CEF starts issuing resource requests; the
// pointer swap itself is atomic, and a previously installed list is kept
// alive (never freed) so a concurrent reader cannot be left dangling.
// Refuses (returns false, keeps the fallback) a list with fewer than
// kMinimumPublicSuffixRules rules — a truncated or wrong file must not
// silently replace the embedded table with something worse.
inline constexpr std::size_t kMinimumPublicSuffixRules = 1000;
bool install_public_suffix_list(PublicSuffixList list);
// Convenience: parse + install.
bool install_public_suffix_list(std::string_view text);
// True once a list has been installed. reset_ is for tests only.
bool public_suffix_list_installed();
void reset_public_suffix_list_for_testing();

// Lower-cased, trailing-dot-stripped host. Exposed for tests.
std::string normalize_host(std::string_view host);

// The registrable-domain "site" of `host`:
//   "www.example.co.uk" -> "example.co.uk"
//   "alice.github.io"   -> "alice.github.io"
//   "cdn.example.com"   -> "example.com"
// IP literals (v4 dotted or bracketed/colon v6), single-label hosts and a host
// that is itself a public suffix ("github.io", "co.uk") are their own site.
// Empty input gives an empty result. Uses the installed PSL, else the
// embedded fallback table.
std::string cookie_site(std::string_view host);
// Same, against an explicit list (tests, tools).
std::string cookie_site(std::string_view host, const PublicSuffixList& list);

// True when both hosts have the same non-empty cookie_site(). Scheme-less, like
// Chromium's classic (non-schemeful) same-site cookie check.
bool same_cookie_site(std::string_view host_a, std::string_view host_b);

// True when `suffix` (normalized) is in Aurora's embedded fallback table of
// multi-label public suffixes. Exposed for tests.
bool is_known_multi_label_public_suffix(std::string_view suffix);

// RFC 3492 punycode of one UTF-8 label, with the "xn--" prefix; an all-ASCII
// label is returned unchanged. Empty on invalid UTF-8. Exposed for tests.
std::string punycode_label(std::string_view utf8_label);

} // namespace aurora::privacy

// Aurora — "site" boundaries for third-party cookie blocking.
//
// Chromium decides "third party" by comparing registrable domains (eTLD+1)
// computed from the Public Suffix List. Aurora links no PSL, so this is an
// APPROXIMATION: a small static table of common multi-label public suffixes
// (ccTLD second levels such as co.uk / com.au, and multi-tenant hosting
// suffixes such as github.io / pages.dev where every subdomain belongs to a
// different owner), plus a generic rule for two-letter ccTLDs whose second
// label is a well-known generic one (co., com., net., org., ...). Hosts under
// a public suffix that is missing from the table fall back to "last two
// labels", i.e. they are treated as same-site with their siblings — the
// permissive direction, which can only let a cookie through, never break a
// genuine first-party login.
//
// No Qt, no CEF: the CEF adapter feeds Chromium-parsed hosts in, and the
// logic is unit-tested in tests/test_cookie_site.cpp.
#pragma once

#include <string>
#include <string_view>

namespace aurora::privacy {

// Lower-cased, trailing-dot-stripped host. Exposed for tests.
std::string normalize_host(std::string_view host);

// The registrable-domain-like "site" of `host`:
//   "www.example.co.uk" -> "example.co.uk"
//   "alice.github.io"   -> "alice.github.io"
//   "cdn.example.com"   -> "example.com"
// IP literals (v4 dotted or bracketed/colon v6), single-label hosts and a host
// that is itself a public suffix ("github.io", "co.uk") are their own site.
// Empty input gives an empty result.
std::string cookie_site(std::string_view host);

// True when both hosts have the same non-empty cookie_site(). Scheme-less, like
// Chromium's classic (non-schemeful) same-site cookie check.
bool same_cookie_site(std::string_view host_a, std::string_view host_b);

// True when `suffix` (normalized) is in Aurora's embedded multi-label
// public-suffix table. Exposed for tests.
bool is_known_multi_label_public_suffix(std::string_view suffix);

} // namespace aurora::privacy

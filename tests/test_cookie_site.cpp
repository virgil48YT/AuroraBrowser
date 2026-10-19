// Aurora — site boundaries used by "Block third-party cookies".
//
// Without an installed list, cookie_site() uses the embedded fallback table
// (see privacy/CookieSite.hpp); the first cases below pin the boundaries that
// matter for the third-party decision: ordinary subdomains are same-site, two different
// users on a shared host are NOT, and ccTLD second levels never collapse
// unrelated registrants into one site.
#include <doctest.h>

#include "privacy/CookieSite.hpp"

using aurora::privacy::cookie_site;
using aurora::privacy::is_known_multi_label_public_suffix;
using aurora::privacy::normalize_host;
using aurora::privacy::same_cookie_site;

TEST_CASE("cookie_site: plain registrable domains") {
    CHECK(cookie_site("example.com") == "example.com");
    CHECK(cookie_site("www.example.com") == "example.com");
    CHECK(cookie_site("a.b.c.example.org") == "example.org");
    CHECK(cookie_site("WWW.Example.COM.") == "example.com");
    CHECK(normalize_host("Example.COM..") == "example.com");
}

TEST_CASE("cookie_site: ccTLD second levels from the table") {
    CHECK(cookie_site("www.bbc.co.uk") == "bbc.co.uk");
    CHECK(cookie_site("shop.example.com.au") == "example.com.au");
    CHECK(cookie_site("a.b.example.co.jp") == "example.co.jp");
    CHECK(cookie_site("news.example.com.br") == "example.com.br");
    CHECK_FALSE(same_cookie_site("alpha.co.uk", "beta.co.uk"));
    CHECK_FALSE(same_cookie_site("www.alpha.com.au", "www.beta.com.au"));
}

TEST_CASE("cookie_site: generic ccTLD rule covers unlisted countries") {
    // "co.ve" / "com.ec" are not in the table; the generic rule still keeps
    // the registrant label.
    CHECK(cookie_site("www.example.co.ve") == "example.co.ve");
    CHECK(cookie_site("www.example.com.ec") == "example.com.ec");
    // A non-generic second label under a ccTLD is a registrable domain.
    CHECK(cookie_site("www.example.de") == "example.de");
    CHECK(cookie_site("cdn.shop.example.fr") == "example.fr");
}

TEST_CASE("cookie_site: users on shared hosting are different sites") {
    CHECK(cookie_site("alice.github.io") == "alice.github.io");
    CHECK(cookie_site("docs.alice.github.io") == "alice.github.io");
    CHECK_FALSE(same_cookie_site("a.github.io", "b.github.io"));
    CHECK(same_cookie_site("a.github.io", "www.a.github.io"));
    CHECK_FALSE(same_cookie_site("app1.herokuapp.com", "app2.herokuapp.com"));
    CHECK_FALSE(same_cookie_site("x.pages.dev", "y.pages.dev"));
    CHECK_FALSE(same_cookie_site("one.blogspot.com", "two.blogspot.com"));
    // The hosting provider's own domain is not a public suffix.
    CHECK(cookie_site("www.github.com") == "github.com");
    CHECK_FALSE(same_cookie_site("github.com", "alice.github.io"));
}

TEST_CASE("cookie_site: longest table match wins") {
    // "herokuapp.com" must win over the plain ".com" fallback.
    CHECK(cookie_site("api.myapp.herokuapp.com") == "myapp.herokuapp.com");
    CHECK(is_known_multi_label_public_suffix("GitHub.IO"));
    CHECK(is_known_multi_label_public_suffix("co.uk"));
    CHECK_FALSE(is_known_multi_label_public_suffix("example.com"));
    CHECK_FALSE(is_known_multi_label_public_suffix("com"));
}

TEST_CASE("cookie_site: degenerate hosts are their own site") {
    CHECK(cookie_site("") == "");
    CHECK(cookie_site("localhost") == "localhost");
    CHECK(cookie_site("192.168.1.10") == "192.168.1.10");
    CHECK(cookie_site("[::1]") == "[::1]");
    CHECK(cookie_site("::1") == "::1");
    // A bare public suffix is its own site, never merged with its children.
    CHECK(cookie_site("github.io") == "github.io");
    CHECK(cookie_site("co.uk") == "co.uk");
    CHECK_FALSE(same_cookie_site("github.io", "alice.github.io"));
    // IPs never collapse to a "last two labels" site.
    CHECK_FALSE(same_cookie_site("10.0.0.1", "20.0.0.1"));
    CHECK_FALSE(same_cookie_site("", ""));
}

// --- The real Public Suffix List ---------------------------------------------
// Parsed from the copy bundled with the browser (third_party/publicsuffix/),
// so these cases also guard the shipped data file.
#include <fstream>
#include <sstream>

using aurora::privacy::install_public_suffix_list;
using aurora::privacy::public_suffix_list_installed;
using aurora::privacy::PublicSuffixList;
using aurora::privacy::punycode_label;
using aurora::privacy::reset_public_suffix_list_for_testing;

namespace {
const PublicSuffixList& bundled_psl() {
    static const PublicSuffixList list = [] {
        std::ifstream in(AURORA_PSL_TEST_FILE, std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        return PublicSuffixList::parse(buf.str());
    }();
    return list;
}
} // namespace

TEST_CASE("psl: bundled list parses") {
    CHECK(bundled_psl().rule_count() > 9000);
}

TEST_CASE("psl: ICANN multi-level suffixes") {
    const auto& psl = bundled_psl();
    CHECK(cookie_site("a.b.c.co.uk", psl) == "c.co.uk");
    CHECK(cookie_site("www.bbc.co.uk", psl) == "bbc.co.uk");
    CHECK(cookie_site("co.uk", psl) == "co.uk");
    CHECK(cookie_site("shop.example.com.au", psl) == "example.com.au");
    // Not in the fallback table: only the real list gets these right.
    CHECK(cookie_site("www.example.kawasaki.jp", psl) == "www.example.kawasaki.jp");
    CHECK(cookie_site("a.b.example.pvt.k12.ma.us", psl) == "example.pvt.k12.ma.us");
    CHECK(psl.registrable_domain("co.uk").empty());
    CHECK(psl.registrable_domain("x.y.example.com") == "example.com");
}

TEST_CASE("psl: private multi-tenant suffixes") {
    const auto& psl = bundled_psl();
    CHECK(cookie_site("user.github.io", psl) == "user.github.io");
    CHECK(cookie_site("docs.user.github.io", psl) == "user.github.io");
    CHECK(cookie_site("github.io", psl) == "github.io");
    CHECK(cookie_site("www.github.com", psl) == "github.com");
    CHECK(cookie_site("api.myapp.herokuapp.com", psl) == "myapp.herokuapp.com");
    CHECK(cookie_site("one.blogspot.com", psl) != cookie_site("two.blogspot.com", psl));
}

TEST_CASE("psl: wildcard and exception rules") {
    // *.ck with !www.ck, *.kawasaki.jp with !city.kawasaki.jp.
    const auto& psl = bundled_psl();
    CHECK(cookie_site("a.b.ck", psl) == "a.b.ck");
    CHECK(cookie_site("b.ck", psl) == "b.ck");  // b.ck is itself a public suffix
    CHECK(cookie_site("www.ck", psl) == "www.ck");
    CHECK(cookie_site("x.www.ck", psl) == "www.ck");
    CHECK(cookie_site("city.kawasaki.jp", psl) == "city.kawasaki.jp");
    CHECK(cookie_site("a.city.kawasaki.jp", psl) == "city.kawasaki.jp");
}

TEST_CASE("psl: unlisted TLD uses the implicit * rule") {
    const auto& psl = bundled_psl();
    CHECK(cookie_site("a.b.example.notarealtld", psl) == "example.notarealtld");
    CHECK(cookie_site("localhost", psl) == "localhost");
    CHECK(cookie_site("192.168.1.10", psl) == "192.168.1.10");
    CHECK(cookie_site("[::1]", psl) == "[::1]");
}

TEST_CASE("psl: punycode for IDN rules") {
    // RFC 3492 / IDNA reference vectors.
    CHECK(punycode_label("example") == "example");
    CHECK(punycode_label("bücher") == "xn--bcher-kva");
    CHECK(punycode_label("公司") == "xn--55qx5d");
    CHECK(punycode_label("рф") == "xn--p1ai");
    CHECK(punycode_label("\xff").empty());
    // 公司.cn is an ICANN suffix; CEF hands the host in as punycode.
    const auto& psl = bundled_psl();
    CHECK(cookie_site("www.example.xn--55qx5d.cn", psl) == "example.xn--55qx5d.cn");
    CHECK(cookie_site("www.example.xn--p1ai", psl) == "example.xn--p1ai");
}

TEST_CASE("psl: parser format details") {
    const auto tiny = PublicSuffixList::parse(
        "// comment\n\n  com  trailing words\r\n*.wild\n!ok.wild\nbad.*.rule\nUPPER.Case\n");
    CHECK(tiny.rule_count() == 4);
    CHECK(cookie_site("a.example.com", tiny) == "example.com");
    CHECK(cookie_site("a.b.wild", tiny) == "a.b.wild");
    CHECK(cookie_site("x.ok.wild", tiny) == "ok.wild");
    CHECK(cookie_site("a.b.upper.case", tiny) == "b.upper.case");
}

TEST_CASE("psl: install swaps cookie_site() from the fallback table") {
    reset_public_suffix_list_for_testing();
    CHECK_FALSE(public_suffix_list_installed());
    // Fallback: kawasaki.jp is unknown, so "last two labels".
    CHECK(cookie_site("www.example.kawasaki.jp") == "kawasaki.jp");
    // A truncated list is refused and the fallback stays.
    CHECK_FALSE(install_public_suffix_list(std::string_view("com\nnet\n")));
    CHECK_FALSE(public_suffix_list_installed());

    CHECK(install_public_suffix_list(PublicSuffixList(bundled_psl())));
    CHECK(public_suffix_list_installed());
    CHECK(cookie_site("www.example.kawasaki.jp") == "www.example.kawasaki.jp");
    CHECK_FALSE(same_cookie_site("alice.github.io", "bob.github.io"));
    CHECK(same_cookie_site("a.example.co.uk", "b.example.co.uk"));
    reset_public_suffix_list_for_testing();
}

TEST_CASE("psl: existing boundary cases hold with the real list") {
    const auto& psl = bundled_psl();
    CHECK(cookie_site("www.example.co.ve", psl) == "example.co.ve");
    CHECK(cookie_site("www.example.com.ec", psl) == "example.com.ec");
    CHECK(cookie_site("cdn.shop.example.fr", psl) == "example.fr");
    CHECK(cookie_site("x.pages.dev", psl) != cookie_site("y.pages.dev", psl));
    CHECK(cookie_site("WWW.Example.COM.", psl) == "example.com");
}

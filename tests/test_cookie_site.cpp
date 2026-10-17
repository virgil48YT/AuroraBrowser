// Aurora — site boundaries used by "Block third-party cookies".
//
// cookie_site() is an approximation of the Public Suffix List (see
// privacy/CookieSite.hpp). These cases pin the boundaries that matter for the
// third-party decision: ordinary subdomains are same-site, two different
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

// Url parsing — the omnibox depends on these heuristics being correct.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/Url.hpp"

using namespace aurora::core;

TEST_CASE("absolute https url is parsed into components") {
    Url u("https://www.example.com/path/to/page?q=1");
    CHECK(u.is_valid());
    CHECK_FALSE(u.is_search_query());
    CHECK(u.scheme() == "https");
    CHECK(u.host() == "www.example.com");
    CHECK(u.path() == "/path/to/page");
    CHECK(u.query() == "q=1");
    CHECK(u.origin() == "https://www.example.com");
}

TEST_CASE("bare domain without scheme is treated as https url") {
    Url u("github.com/repo");
    CHECK(u.is_valid());
    CHECK_FALSE(u.is_search_query());
    CHECK(u.scheme() == "https");
    CHECK(u.host() == "github.com");
    CHECK(u.path() == "/repo");
}

TEST_CASE("bare domain with trailing slash") {
    Url u("example.com/");
    CHECK(u.is_valid());
    CHECK(u.host() == "example.com");
    CHECK(u.path() == "/");
}

TEST_CASE("text with spaces is a search query") {
    Url u("how to bake bread");
    CHECK(u.is_search_query());
    CHECK(u.is_valid());
}

TEST_CASE("localhost with port") {
    Url u("http://localhost:8080/api");
    CHECK(u.is_valid());
    CHECK(u.scheme() == "http");
    CHECK(u.host() == "localhost:8080");
}

TEST_CASE("about urls are valid") {
    Url u("about:blank");
    CHECK(u.is_valid());
    CHECK(u.scheme() == "about");
}

TEST_CASE("normalized form ignores original scheme case") {
    Url a("HTTPS://Example.COM/Path");
    Url b("https://example.com/Path");
    CHECK(a.normalized() == b.normalized());
    CHECK(a == b);
}

TEST_CASE("empty input is invalid") {
    Url u("");
    CHECK_FALSE(u.is_valid());
}

TEST_CASE("a single word with no dot is a search query") {
    Url u("weather");
    CHECK(u.is_search_query());
}

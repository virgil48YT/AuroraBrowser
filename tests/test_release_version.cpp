#include <doctest.h>

#include "updates/ReleaseVersion.hpp"

using aurora::updates::Version;
using aurora::updates::compare_versions;
using aurora::updates::is_newer_version;
using aurora::updates::parse_version;

TEST_CASE("release tags parse with an optional v prefix") {
    const auto plain = parse_version("0.1.2");
    REQUIRE(plain.has_value());
    CHECK(plain->major == 0);
    CHECK(plain->minor == 1);
    CHECK(plain->patch == 2);
    CHECK(plain->prerelease.empty());

    const auto tagged = parse_version("v2.10.3");
    REQUIRE(tagged.has_value());
    CHECK(tagged->major == 2);
    CHECK(tagged->minor == 10);
    CHECK(tagged->patch == 3);

    CHECK(parse_version("android-ci") == std::nullopt);
    CHECK(parse_version("") == std::nullopt);
    CHECK(parse_version("1.2.3.4") == std::nullopt);
    CHECK(parse_version("v1.x.0") == std::nullopt);
    CHECK(parse_version("1.2-") == std::nullopt);
    CHECK(parse_version("1.2+") == std::nullopt);
}

TEST_CASE("build metadata is ignored and prereleases sort first") {
    const auto release = parse_version("0.2.0+build.7");
    const auto prerelease = parse_version("0.2.0-rc.1");
    REQUIRE(release.has_value());
    REQUIRE(prerelease.has_value());
    CHECK(compare_versions(*release, *prerelease) > 0);
    CHECK(compare_versions(*prerelease, *release) < 0);

    const auto dev = parse_version("0.0.0-dev.5");
    const auto stable = parse_version("0.1.0");
    REQUIRE(dev.has_value());
    REQUIRE(stable.has_value());
    CHECK(compare_versions(*stable, *dev) > 0);
}

TEST_CASE("semantic version precedence") {
    CHECK(compare_versions(*parse_version("1.0.0"), *parse_version("1.0.1")) < 0);
    CHECK(compare_versions(*parse_version("1.2.0"), *parse_version("1.10.0")) < 0);
    CHECK(compare_versions(*parse_version("1.0.0-alpha"),
                           *parse_version("1.0.0-alpha.1")) < 0);
    CHECK(compare_versions(*parse_version("1.0.0-alpha.1"),
                           *parse_version("1.0.0-beta")) < 0);
    CHECK(compare_versions(*parse_version("1.0.0-beta.2"),
                           *parse_version("1.0.0-beta.11")) < 0);
    CHECK(compare_versions(*parse_version("1.0.0-1"),
                           *parse_version("1.0.0-alpha")) < 0);
    CHECK(compare_versions(*parse_version("1.0.0"), *parse_version("1.0.0")) == 0);
}

TEST_CASE("is_newer_version rejects unparseable tags") {
    CHECK(is_newer_version("v0.2.0", "0.1.0"));
    CHECK_FALSE(is_newer_version("v0.1.0", "0.1.0"));
    CHECK_FALSE(is_newer_version("v0.0.9", "0.1.0"));
    CHECK_FALSE(is_newer_version("nightly", "0.1.0"));
    CHECK_FALSE(is_newer_version("v0.2.0", "not-a-version"));
}

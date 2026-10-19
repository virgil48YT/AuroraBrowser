#include <doctest.h>

#include "core/InternalUrl.hpp"

using namespace aurora::core;

TEST_CASE("canonical aurora:// urls parse to their page") {
    CHECK(parse_internal_url("aurora://history") == InternalPage::History);
    CHECK(parse_internal_url("aurora://downloads") == InternalPage::Downloads);
    CHECK(parse_internal_url("aurora://bookmarks") == InternalPage::Bookmarks);
    CHECK(parse_internal_url("aurora://settings") == InternalPage::Settings);
    CHECK(parse_internal_url("aurora://extensions") == InternalPage::Extensions);
}

TEST_CASE("parsing tolerates case, whitespace, trailing slash and no //") {
    CHECK(parse_internal_url("AURORA://History") == InternalPage::History);
    CHECK(parse_internal_url("  aurora://settings/  ") == InternalPage::Settings);
    CHECK(parse_internal_url("aurora:downloads") == InternalPage::Downloads);
    CHECK(parse_internal_url("aurora://preferences") == InternalPage::Settings);
}

TEST_CASE("non-internal input is rejected") {
    CHECK_FALSE(parse_internal_url("").has_value());
    CHECK_FALSE(parse_internal_url("aurora://").has_value());
    CHECK_FALSE(parse_internal_url("aurora://nope").has_value());
    CHECK_FALSE(parse_internal_url("https://aurora/history").has_value());
    CHECK_FALSE(parse_internal_url("aurorahistory").has_value());
    CHECK_FALSE(parse_internal_url("chrome://history").has_value());
}

TEST_CASE("internal_url round-trips through the parser") {
    for (auto page : {InternalPage::History, InternalPage::Downloads,
                      InternalPage::Bookmarks, InternalPage::Settings,
                      InternalPage::Extensions, InternalPage::SiteSettings}) {
        const auto url = internal_url(page);
        CHECK(parse_internal_url(url) == page);
        CHECK_FALSE(internal_page_title(page).empty());
    }
}

TEST_CASE("internal url: reader mode page") {
    using aurora::core::InternalPage;
    CHECK(aurora::core::parse_internal_url("aurora://reader") == InternalPage::Reader);
    CHECK(aurora::core::parse_internal_url("AURORA:reader/") == InternalPage::Reader);
    CHECK(aurora::core::internal_url(InternalPage::Reader) == "aurora://reader");
    CHECK(aurora::core::internal_page_title(InternalPage::Reader) == "Reader");
}

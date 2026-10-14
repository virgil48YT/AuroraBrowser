#include "doctest.h"

#include "core/ChromeNav.hpp"

using aurora::core::ChromeNavEntry;
using aurora::core::ChromeNavStack;
using aurora::core::InternalPage;
using aurora::core::Url;

namespace {

ChromeNavEntry web(const char* url) {
    return ChromeNavEntry{std::nullopt, Url(url), ""};
}

ChromeNavEntry internal(InternalPage page) {
    return ChromeNavEntry{page, Url(aurora::core::internal_url(page)),
                          aurora::core::internal_page_title(page)};
}

} // namespace

TEST_CASE("empty chrome stack cannot navigate") {
    ChromeNavStack s;
    CHECK_FALSE(s.can_go_back());
    CHECK_FALSE(s.can_go_forward());
    CHECK(s.current() == nullptr);
    CHECK(s.go_back() == nullptr);
    CHECK(s.go_forward() == nullptr);
}

TEST_CASE("a single entry has nothing to go back to") {
    ChromeNavStack s;
    s.record(web("https://example.com"));
    CHECK_FALSE(s.can_go_back());
    CHECK_FALSE(s.can_go_forward());
}

TEST_CASE("back from an internal page returns to the web page before it") {
    ChromeNavStack s;
    s.record(web("https://example.com"));
    s.record(internal(InternalPage::Bookmarks));
    REQUIRE(s.can_go_back());
    CHECK(s.current()->is_internal());

    const auto* back = s.go_back();
    REQUIRE(back != nullptr);
    CHECK_FALSE(back->is_internal());
    CHECK(back->url.normalized() == Url("https://example.com").normalized());
    CHECK(s.can_go_forward());

    const auto* fwd = s.go_forward();
    REQUIRE(fwd != nullptr);
    REQUIRE(fwd->is_internal());
    CHECK(*fwd->page == InternalPage::Bookmarks);
}

TEST_CASE("internal pages chain and unwind in order") {
    ChromeNavStack s;
    s.record(web("https://example.com"));
    s.record(internal(InternalPage::History));
    s.record(internal(InternalPage::Settings));
    CHECK(s.size() == 3);

    const auto* first_back = s.go_back();
    REQUIRE(first_back != nullptr);
    CHECK(*first_back->page == InternalPage::History);
    const auto* second_back = s.go_back();
    REQUIRE(second_back != nullptr);
    CHECK_FALSE(second_back->is_internal());
    CHECK_FALSE(s.can_go_back());
}

TEST_CASE("re-recording the current page does not grow the stack") {
    ChromeNavStack s;
    s.record(web("https://example.com"));
    s.record(internal(InternalPage::Downloads));
    s.record(internal(InternalPage::Downloads));
    CHECK(s.size() == 2);
    REQUIRE(s.can_go_back());
    CHECK_FALSE(s.go_back()->is_internal());
}

TEST_CASE("a new navigation truncates forward history") {
    ChromeNavStack s;
    s.record(web("https://a.example"));
    s.record(internal(InternalPage::History));
    REQUIRE(s.go_back() != nullptr);
    CHECK(s.can_go_forward());

    s.record(web("https://b.example"));
    CHECK_FALSE(s.can_go_forward());
    CHECK(s.size() == 2);
    CHECK(s.can_go_back());
}

TEST_CASE("engine address changes refine the current web entry") {
    ChromeNavStack s;
    s.record(web("https://a.example"));
    s.note_web_url(Url("https://a.example/deep/link"), "Deep");
    CHECK(s.size() == 1);
    CHECK(s.current()->url.normalized() ==
          Url("https://a.example/deep/link").normalized());
    CHECK(s.current()->title == "Deep");
}

TEST_CASE("engine address changes never overwrite an internal entry") {
    ChromeNavStack s;
    s.record(web("https://a.example"));
    s.record(internal(InternalPage::Settings));
    s.note_web_url(Url("https://a.example/late-report"));
    REQUIRE(s.current()->is_internal());
    CHECK(*s.current()->page == InternalPage::Settings);
    CHECK(s.size() == 2);
}

TEST_CASE("the first address change seeds an empty stack") {
    ChromeNavStack s;
    s.note_web_url(Url("https://seed.example"), "Seed");
    REQUIRE(s.current() != nullptr);
    CHECK_FALSE(s.current()->is_internal());
    CHECK(s.size() == 1);
    CHECK_FALSE(s.can_go_back());
}

TEST_CASE("peeking reports what back and forward would do") {
    ChromeNavStack s;
    s.record(web("https://a.example"));
    s.record(internal(InternalPage::History));
    REQUIRE(s.peek_back() != nullptr);
    CHECK_FALSE(s.peek_back()->is_internal());
    CHECK(s.peek_forward() == nullptr);
    s.go_back();
    REQUIRE(s.peek_forward() != nullptr);
    CHECK(s.peek_forward()->is_internal());
}

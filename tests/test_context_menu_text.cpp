// Right-click menu text helpers (selection tidying, label truncation).
#include <doctest.h>

#include "cef/ContextMenuText.hpp"

using aurora::cef::menu_snippet;
using aurora::cef::tidy_selection;

TEST_CASE("selection whitespace is collapsed and trimmed") {
    CHECK(tidy_selection("  hello \n\t world  ") == "hello world");
    CHECK(tidy_selection("one\r\n\r\ntwo") == "one two");
    CHECK(tidy_selection("") == "");
    CHECK(tidy_selection(" \n\t ") == "");
}

TEST_CASE("selection is capped and never cut mid-character") {
    const std::string long_ascii(1000, 'a');
    CHECK(tidy_selection(long_ascii, 300).size() == 300);

    // 2-byte characters: a cap that lands between the bytes drops the char.
    std::string e;
    for (int i = 0; i < 200; ++i) e += "\xC3\xA9";  // é
    const auto cut = tidy_selection(e, 301);
    CHECK(cut.size() == 300);
    CHECK(cut.size() % 2 == 0);

    // 4-byte characters (emoji).
    std::string smile;
    for (int i = 0; i < 100; ++i) smile += "\xF0\x9F\x98\x80";
    for (size_t cap : {301u, 302u, 303u, 304u}) {
        const auto out = tidy_selection(smile, cap);
        CHECK(out.size() % 4 == 0);
    }
}

TEST_CASE("a trailing space left by the cap is removed") {
    CHECK(tidy_selection("abc def", 4) == "abc");
    CHECK(tidy_selection("abc def", 5) == "abc d");
}

TEST_CASE("menu snippets count characters and add an ellipsis only when cut") {
    CHECK(menu_snippet("short", 32) == "short");
    CHECK(menu_snippet("abcdef", 3) == "abc\xE2\x80\xA6");
    CHECK(menu_snippet("abc", 3) == "abc");
    // 3 é characters is 6 bytes but 3 characters.
    CHECK(menu_snippet("\xC3\xA9\xC3\xA9\xC3\xA9", 3) == "\xC3\xA9\xC3\xA9\xC3\xA9");
    CHECK(menu_snippet("\xC3\xA9\xC3\xA9\xC3\xA9x", 3) ==
          "\xC3\xA9\xC3\xA9\xC3\xA9\xE2\x80\xA6");
}

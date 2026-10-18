// Omnibox suggestions — ranking, deduplication and the remote-suggestion
// payload parser. All pure logic, no Qt.
#include <doctest.h>

#include "omnibox/OmniboxSuggest.hpp"

using namespace aurora::omnibox;

namespace {

constexpr int64_t kNow = 1'800'000'000;
constexpr int64_t kDay = 24 * 3600;

HistoryCandidate hist(std::string url, std::string title, int visits = 1,
                      int typed = 0, int64_t age = 0) {
    HistoryCandidate h;
    h.url = std::move(url);
    h.title = std::move(title);
    h.visit_count = visits;
    h.typed_count = typed;
    h.last_visit_epoch = kNow - age;
    return h;
}

Inputs make(std::string q) {
    Inputs in;
    in.query = std::move(q);
    in.now_epoch = kNow;
    return in;
}

} // namespace

TEST_CASE("blank input yields no rows") {
    CHECK(build_suggestions(make("")).empty());
    CHECK(build_suggestions(make("   \t")).empty());
}

TEST_CASE("first row is the default action for the typed text") {
    auto rows = build_suggestions(make("aurora browser"));
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].kind == Kind::Search);
    CHECK(rows[0].fill == "aurora browser");

    rows = build_suggestions(make("github.com/openai"));
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].kind == Kind::Navigate);

    rows = build_suggestions(make("aurora://settings"));
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].kind == Kind::Navigate);
}

TEST_CASE("input is trimmed before use") {
    auto rows = build_suggestions(make("  hello world  "));
    REQUIRE_FALSE(rows.empty());
    CHECK(rows[0].fill == "hello world");
}

TEST_CASE("history matches by host, title and path; non matches are dropped") {
    auto in = make("git");
    in.history = {
        hist("https://github.com/", "GitHub", 5),
        hist("https://example.com/blog", "Why I love git", 1),
        hist("https://news.ycombinator.com/", "Hacker News", 9),
    };
    auto rows = build_suggestions(in);
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].url == "https://github.com/");  // host prefix beats title word
    CHECK(rows[2].url == "https://example.com/blog");
    for (const auto& r : rows) CHECK(r.url != "https://news.ycombinator.com/");
}

TEST_CASE("every word of a multi-word query must match") {
    auto in = make("rust book");
    in.history = {
        hist("https://doc.rust-lang.org/book/", "The Rust Programming Language"),
        hist("https://rust-lang.org/", "Rust"),
        hist("https://example.com/", "A book about cooking"),
    };
    auto rows = build_suggestions(in);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].url == "https://doc.rust-lang.org/book/");
}

TEST_CASE("matching is case-insensitive") {
    auto in = make("GITHUB");
    in.history = {hist("https://github.com/", "GitHub")};
    CHECK(build_suggestions(in).size() == 2);
}

TEST_CASE("frequently and recently visited pages rank higher") {
    auto in = make("shop");
    in.history = {
        hist("https://shop.old.example/", "Shop", 1, 0, 200 * kDay),
        hist("https://shop.busy.example/", "Shop", 40, 6, 1 * 3600),
    };
    auto rows = build_suggestions(in);
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].url == "https://shop.busy.example/");
}

TEST_CASE("same page from several sources collapses to one row") {
    auto in = make("exam");
    in.history = {hist("https://www.example.com/", "Example Domain", 3)};
    in.bookmarks = {{"http://example.com", "Example"}};
    in.tabs = {{"https://example.com/#top", "Example Domain", 7, false}};
    auto rows = build_suggestions(in);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].kind == Kind::OpenTab);  // the most useful kind wins
    CHECK(rows[1].tab_id == 7);
    CHECK_FALSE(rows[1].title.empty());
}

TEST_CASE("the tab the user is already on is not offered") {
    auto in = make("exam");
    in.tabs = {{"https://example.com/", "Example Domain", 7, true}};
    CHECK(build_suggestions(in).size() == 1);
}

TEST_CASE("a fully typed URL is not repeated as a history row") {
    auto in = make("github.com");
    in.history = {hist("https://github.com/", "GitHub", 10),
                  hist("https://github.com/features", "Features", 2)};
    auto rows = build_suggestions(in);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].kind == Kind::Navigate);
    CHECK(rows[1].url == "https://github.com/features");
}

TEST_CASE("script and data bookmarks are never suggested") {
    auto in = make("x");
    in.bookmarks = {{"javascript:alert(1)", "x tool"}, {"data:text/html,x", "x"}};
    CHECK(build_suggestions(in).size() == 1);
}

TEST_CASE("remote suggestions follow local ones and drop duplicates of the query") {
    auto in = make("aurora br");
    in.history = {hist("https://aurora-browser.example/", "Aurora Browser")};
    in.remote = {"aurora br", "Aurora Brewery", "aurora bridal", "AURORA BREWERY"};
    auto rows = build_suggestions(in);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].kind == Kind::Search);
    CHECK(rows[1].kind == Kind::History);
    CHECK(rows[2].kind == Kind::RemoteSearch);
    CHECK(rows[2].fill == "Aurora Brewery");
    CHECK(rows[3].fill == "aurora bridal");
}

TEST_CASE("row count is capped and remote rows keep room against local ones") {
    auto in = make("a");
    for (int i = 0; i < 20; ++i) {
        in.history.push_back(hist("https://a" + std::to_string(i) + ".example/", "A", i + 1));
    }
    for (int i = 0; i < 10; ++i) in.remote.push_back("a query " + std::to_string(i));
    auto rows = build_suggestions(in, 8);
    REQUIRE(rows.size() == 8);
    size_t local = 0, remote = 0;
    for (const auto& r : rows) {
        local += r.kind == Kind::History;
        remote += r.kind == Kind::RemoteSearch;
    }
    CHECK(local == 4);
    CHECK(remote == 3);
}

TEST_CASE("local rows use the space when the engine returns little") {
    auto in = make("a");
    for (int i = 0; i < 20; ++i) {
        in.history.push_back(hist("https://a" + std::to_string(i) + ".example/", "A", i + 1));
    }
    in.remote = {"a thing"};
    auto rows = build_suggestions(in, 8);
    REQUIRE(rows.size() == 8);
    CHECK(rows.back().kind == Kind::RemoteSearch);
}

TEST_CASE("max_rows of one returns only the default action") {
    auto in = make("git");
    in.history = {hist("https://github.com/", "GitHub")};
    CHECK(build_suggestions(in, 1).size() == 1);
}

TEST_CASE("remote requests are skipped for anything that is not plain search text") {
    CHECK(should_request_remote("aurora browser"));
    CHECK(should_request_remote("how to bake"));
    CHECK_FALSE(should_request_remote("a"));
    CHECK_FALSE(should_request_remote(""));
    CHECK_FALSE(should_request_remote("https://example.com/secret?token=1"));
    CHECK_FALSE(should_request_remote("github.com/openai"));
    CHECK_FALSE(should_request_remote("aurora://settings"));
    CHECK_FALSE(should_request_remote("javascript:alert(1)"));
    CHECK_FALSE(should_request_remote("view-source:example.com"));
    CHECK_FALSE(should_request_remote(std::string(500, 'x')));
}

TEST_CASE("suggestion endpoints exist only for engines with a public one") {
    CHECK(suggest_url_template("duckduckgo").find("%s") != std::string::npos);
    CHECK(suggest_url_template("google").find("%s") != std::string::npos);
    CHECK(suggest_url_template("bing").find("%s") != std::string::npos);
    CHECK(suggest_url_template("startpage").empty());
    CHECK(suggest_url_template("custom:3").empty());
    CHECK(suggest_url_template("duckduckgo").rfind("https://", 0) == 0);
}

TEST_CASE("OpenSearch payloads are parsed") {
    auto r = parse_remote_suggestions(
        R"(["aurora br",["aurora brewery","aurora bridal"],[],{"google:suggestsubtypes":[[512],[512]]}])");
    REQUIRE(r.size() == 2);
    CHECK(r[0] == "aurora brewery");
    CHECK(r[1] == "aurora bridal");

    r = parse_remote_suggestions(R"(["q", ["a", "b" , "c"]])", 2);
    CHECK(r.size() == 2);
}

TEST_CASE("JSON escapes in suggestions are decoded") {
    auto r = parse_remote_suggestions(
        R"(["q",["caf\u00e9","say \"hi\"","tab\there","\ud83d\ude00"]])");
    REQUIRE(r.size() == 4);
    CHECK(r[0] == "caf\xC3\xA9");
    CHECK(r[1] == "say \"hi\"");
    CHECK(r[2] == "tab\there");
    CHECK(r[3] == "\xF0\x9F\x98\x80");
}

TEST_CASE("malformed payloads produce no suggestions") {
    CHECK(parse_remote_suggestions("").empty());
    CHECK(parse_remote_suggestions("not json").empty());
    CHECK(parse_remote_suggestions("{}").empty());
    CHECK(parse_remote_suggestions(R"(["q"])").empty());
    CHECK(parse_remote_suggestions(R"(["q",["a","b")").empty());
    CHECK(parse_remote_suggestions(R"(["q",[1,2,3]])").empty());
    CHECK(parse_remote_suggestions(R"(["q",["\ud800"]])").empty());
    CHECK(parse_remote_suggestions(std::string(100 * 1024, '[')).empty());
}

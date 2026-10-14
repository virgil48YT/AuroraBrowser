#include <doctest.h>

#include "bookmarks/BookmarkIo.hpp"
#include "bookmarks/BookmarkService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"

#include <chrono>
#include <string>

using namespace aurora::bookmarks;
using namespace aurora::storage;

namespace {
Database open_io_database() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

auto timestamp(int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

void check_tree(const BookmarkService& left, int64_t lp,
                const BookmarkService& right, int64_t rp) {
    const auto a = left.children(lp);
    const auto b = right.children(rp);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].title == b[i].title);
        CHECK(a[i].url == b[i].url);
        CHECK(a[i].is_folder == b[i].is_folder);
        CHECK(a[i].position == b[i].position);
        CHECK(a[i].added_at == b[i].added_at);
        if (a[i].is_folder) check_tree(left, a[i].id, right, b[i].id);
    }
}
} // namespace

TEST_CASE("Netscape export round trips the complete ordered tree and dates") {
    auto db = open_io_database();
    auto copy_db = open_io_database();
    BookmarkService source(db), copy(copy_db);
    const auto folder = source.add(0, "Research & <ideas> \"today\" 'notes'", "", true, timestamp(1700000000));
    REQUIRE(folder > 0);
    REQUIRE(source.add(folder, "Δοκιμή 日本語\nsecond line", "https://example.org/?a=1&b=\"two\"", false, timestamp(0)) > 0);
    const auto nested = source.add(folder, "Nested", "", true, timestamp(1700000001));
    REQUIRE(source.add(nested, "Literal &amp; entity", "http://example.net/#one&two", false, timestamp(1700000002)) > 0);
    REQUIRE(source.add(0, "Empty folder", "", true, timestamp(1700000003)) > 0);
    REQUIRE(source.add(0, "Sibling", "https://example.com", false, timestamp(1700000004)) > 0);

    const auto html = export_netscape_html(source);
    CHECK(html.find("<!DOCTYPE NETSCAPE-Bookmark-file-1>") == 0);
    CHECK(html.find("charset=UTF-8") != std::string::npos);
    CHECK(html.find("Research &amp; &lt;ideas&gt; &quot;today&quot; &#39;notes&#39;") != std::string::npos);
    CHECK(html.find("a=1&amp;b=&quot;two&quot;") != std::string::npos);
    const auto result = import_netscape_html(copy, html);
    CHECK(result.folders_imported == 3);
    CHECK(result.bookmarks_imported == 3);
    CHECK(result.skipped == 0);
    CHECK(result.malformed == 0);
    check_tree(source, 0, copy, 0);
    CHECK(export_netscape_html(copy) == html);
}

TEST_CASE("Chrome bookmark bar export ignores icons and preserves nested folders") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto result = import_netscape_html(service, R"html(<!DOCTYPE NETSCAPE-Bookmark-file-1>
<!-- This is an automatically generated file.
     It will be read and overwritten. DO NOT EDIT! -->
<META HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=UTF-8">
<TITLE>Bookmarks</TITLE>
<H1>Bookmarks</H1>
<DL><p>
    <DT><H3 ADD_DATE="1700000000" LAST_MODIFIED="1700000001" PERSONAL_TOOLBAR_FOLDER="true">Bookmarks bar</H3>
    <DL><p>
        <DT><A HREF="https://www.chromium.org/" ADD_DATE="1700000002" ICON="data:image/png;base64,ABC">The Chromium Projects</A>
        <DT><H3 ADD_DATE="1700000003">Work</H3>
        <DL><p>
            <DT><A HREF="https://example.org/?x=1&amp;y=2" ADD_DATE="1700000004">One &amp; Two</A>
        </DL><p>
    </DL><p>
    <DT><A HREF="https://outside.example/" ADD_DATE="1700000005">Outside</A>
</DL><p>)html");
    CHECK(result.folders_imported == 2);
    CHECK(result.bookmarks_imported == 3);
    CHECK(result.skipped == 0);
    CHECK(result.malformed == 0);
    const auto root = service.children(0);
    REQUIRE(root.size() == 2);
    CHECK(root[0].title == "Bookmarks bar");
    CHECK(root[0].added_at == timestamp(1700000000));
    const auto bar = service.children(root[0].id);
    REQUIRE(bar.size() == 2);
    const auto work = service.children(bar[1].id);
    REQUIRE(work.size() == 1);
    CHECK(work[0].url == "https://example.org/?x=1&y=2");
    CHECK(root[1].title == "Outside");
}

TEST_CASE("Firefox export imports folder descriptions without importing their links") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto result = import_netscape_html(service, R"html(<!DOCTYPE NETSCAPE-Bookmark-file-1>
<META HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=UTF-8">
<TITLE>Bookmarks</TITLE><H1>Bookmarks Menu</H1>
<DL><p>
<DT><H3 ADD_DATE="1690000000" LAST_MODIFIED="1690000001" PERSONAL_TOOLBAR_FOLDER="true">Bookmarks Toolbar</H3>
<DD>A folder description with <A HREF="https://ignore.example/">a link</A>
<DL><p>
<DT><A HREF="https://www.mozilla.org/" ADD_DATE="1690000002" LAST_MODIFIED="1690000003" ICON_URI="https://www.mozilla.org/favicon.ico" SHORTCUTURL="moz" TAGS="browser,web">Mozilla</A>
<DD>Mozilla's <b>description</b> <A HREF="https://ignore.example/again">not a bookmark</A>
<DT><A HREF="https://firefox.com/" ADD_DATE="1690000004">Firefox</A>
</DL><p>
<DT><H3 UNFILED_BOOKMARKS_FOLDER="true">Other Bookmarks</H3><DL><p></DL><p>
</DL><p>)html");
    CHECK(result.folders_imported == 2);
    CHECK(result.bookmarks_imported == 2);
    CHECK(result.skipped == 0);
    CHECK(result.malformed == 0);
    REQUIRE(service.children(0).size() == 2);
    CHECK(service.children(service.children(0)[0].id).size() == 2);
}

TEST_CASE("Safari style lowercase tags, attributes and legacy DL wrappers are accepted") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto result = import_netscape_html(service, R"html(<!DOCTYPE NETSCAPE-Bookmark-file-1>
<html><head><meta http-equiv="Content-Type" content="text/html; charset=UTF-8"></head>
<body><h1>Bookmarks</h1><dl><p>
<dt><h3 FOLDED>Favorites</h3>
<dl><p><dt><a HrEf='https://apple.com/?a=1&amp;b=2' AdD_DaTe=1700000000>Apple &gt; Safari</a>
<dt><a href=HTTP://example.org/>Example</a></dl><p>
</dl><p></body></html>)html");
    CHECK(result.folders_imported == 1);
    CHECK(result.bookmarks_imported == 2);
    CHECK(result.malformed == 0);
    REQUIRE(service.children(0).size() == 1);
    const auto children = service.children(service.children(0)[0].id);
    REQUIRE(children.size() == 2);
    CHECK(children[0].title == "Apple > Safari");
    CHECK(children[1].url == "HTTP://example.org/");
}

TEST_CASE("Netscape import decodes entities exactly once including Unicode references") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto result = import_netscape_html(service, R"html(<DL><DT><A HREF="https://example.com/?x=&quot;a&quot;&amp;y=&#39;b&#39;">&lt;tag&gt; &apos;x&apos; &amp;amp; &#169; &#x1F680; &nbsp;&unknown; &#xD800; &eacute;&trade;</A></DL>)html");
    CHECK(result.bookmarks_imported == 1);
    const auto nodes = service.children(0);
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].title == "<tag> 'x' &amp; © \xf0\x9f\x9a\x80 \xc2\xa0&unknown; \xef\xbf\xbd é™");
    CHECK(nodes[0].url == "https://example.com/?x=\"a\"&y='b'");
}

TEST_CASE("Unsafe, empty, relative and obfuscated URL schemes are skipped") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto result = import_netscape_html(service, R"html(<DL>
<DT><A>No href</A><DT><A HREF="">Empty</A>
<DT><A HREF="javascript:alert(1)">JS</A>
<DT><A HREF="&#106;avascript:alert(1)">Encoded JS</A>
<DT><A HREF="data:text/html,bad">Data</A><DT><A HREF="file:///tmp/foo">File</A>
<DT><A HREF="ftp://example.com">FTP</A><DT><A HREF="//example.com">Relative</A>
<DT><A HREF="https://">Missing host</A><DT><A HREF="https:///bad">Missing host</A>
<DT><A HREF="https://example.com/&#10;bad">Control character</A>
<DT><A HREF=" https://safe.example/ ">Safe</A></DL>)html");
    CHECK(result.bookmarks_imported == 1);
    CHECK(result.skipped == 11);
    CHECK(result.malformed == 0);
    REQUIRE(service.children(0).size() == 1);
    CHECK(service.children(0)[0].url == "https://safe.example/");
}

TEST_CASE("Import appends into a selected folder without deduplicating or overwriting") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto parent = service.add(0, "Destination", "", true);
    REQUIRE(service.add(parent, "Existing", "https://existing.example/") > 0);
    const auto fragment = "<DT><H3>Folder</H3><DL><DT><A HREF='https://same.example/'>New</A></DL>";
    CHECK(import_netscape_html(service, fragment, parent).bookmarks_imported == 1);
    CHECK(import_netscape_html(service, fragment, parent).bookmarks_imported == 1);
    const auto nodes = service.children(parent);
    REQUIRE(nodes.size() == 3);
    CHECK(nodes[0].title == "Existing");
    CHECK(nodes[1].position == 1);
    CHECK(nodes[2].position == 2);
    CHECK(service.children(0).size() == 1);
}

TEST_CASE("Invalid import parents reject entries including descendant folders") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto bookmark = service.add(0, "Not a folder", "https://example.com/");
    const auto html = "<DL><DT><H3>Folder</H3><DL><DT><A HREF='https://example.org/'>Link</A></DL></DL>";
    for (int64_t parent : {int64_t(-1), int64_t(999999), bookmark}) {
        const auto result = import_netscape_html(service, html, parent);
        CHECK(result.folders_imported == 0);
        CHECK(result.bookmarks_imported == 0);
        CHECK(result.skipped == 2);
    }
    CHECK(service.children(0).size() == 1);
}

TEST_CASE("Deep bookmark nesting exports and imports without a recursion cutoff") {
    auto db = open_io_database();
    auto copy_db = open_io_database();
    BookmarkService source(db), copy(copy_db);
    int64_t parent = 0;
    constexpr int depth = 128;
    for (int i = 0; i < depth; ++i) {
        parent = source.add(parent, "Level " + std::to_string(i), "", true, timestamp(i));
        REQUIRE(parent > 0);
    }
    REQUIRE(source.add(parent, "Leaf", "https://leaf.example/", false, timestamp(123)) > 0);
    const auto html = export_netscape_html(source);
    const auto result = import_netscape_html(copy, html);
    CHECK(result.folders_imported == depth);
    CHECK(result.bookmarks_imported == 1);
    CHECK(result.malformed == 0);
    CHECK(export_netscape_html(copy) == html);
}

TEST_CASE("Malformed dates and omitted entry closures recover without losing siblings") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto result = import_netscape_html(service, R"html(</DL><DL><p>
<DT><H3 ADD_DATE="nonsense">Recovered folder<DL>
<DT><A HREF="https://one.example/" ADD_DATE="-1">One
<DT><A HREF="https://two.example/" ADD_DATE="9999999999999999999999999">Two</A>
<DT><A HREF="https://three.example/" ADD_DATE="1700000000tail">Three</A>
<DT><A HREF="https://four.example/" ADD_DATE="">Four</A>
<DT><A HREF="https://five.example/" ADD_DATE="1700000000">Five)html");
    CHECK(result.folders_imported == 1);
    CHECK(result.bookmarks_imported == 5);
    CHECK(result.malformed == 6);
    CHECK(result.skipped == 0);
    REQUIRE(service.children(0).size() == 1);
    const auto children = service.children(service.children(0)[0].id);
    REQUIRE(children.size() == 5);
    CHECK(children[4].added_at == timestamp(1700000000));
}

TEST_CASE("Broken quoted tags recover at the next tag and do not consume subsequent entries") {
    auto db = open_io_database();
    BookmarkService service(db);
    const auto result = import_netscape_html(service, R"html(<DL>
<DT><A HREF="https://broken.example/>
<DT><A HREF="https://good.example/?compare=1>0" ADD_DATE="1700000000">Good <b>title</b></A>
<DT><A HREF="https://truncated.example/)html");
    CHECK(result.bookmarks_imported == 1);
    CHECK(result.skipped == 2);
    CHECK(result.malformed == 2);
    REQUIRE(service.children(0).size() == 1);
    CHECK(service.children(0)[0].title == "Good title");
    CHECK(service.children(0)[0].url == "https://good.example/?compare=1>0");
}

TEST_CASE("Comments, scripts, styles and empty input never create phantom bookmarks") {
    auto db = open_io_database();
    BookmarkService service(db);
    CHECK(import_netscape_html(service, "").bookmarks_imported == 0);
    CHECK(import_netscape_html(service, "plain text").skipped == 0);
    const auto result = import_netscape_html(service, R"html(
<!-- <DT><A HREF="https://comment.example/">Ignore</A> -->
<script>var html = '<A HREF="https://script.example/">Ignore</A>';</script>
<style>/* <A HREF="https://style.example/">Ignore</A> */</style>
<DL><DT><A HREF="https://real.example/">Real<!-- hidden --> title</A></DL>
<!-- unfinished comment)html");
    CHECK(result.bookmarks_imported == 1);
    CHECK(result.skipped == 0);
    CHECK(service.children(0)[0].title == "Real title");
    auto empty_db = open_io_database();
    BookmarkService empty(empty_db);
    const auto empty_export = export_netscape_html(empty);
    CHECK(empty_export.find("<DL><p>\n</DL><p>") != std::string::npos);
    CHECK(import_netscape_html(empty, empty_export).folders_imported == 0);
}

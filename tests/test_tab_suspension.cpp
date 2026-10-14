#include <doctest.h>

#include "tabs/TabManager.hpp"

using namespace aurora::core;

TEST_CASE("manual suspend preserves identity, history, favicon and metadata") {
    std::chrono::steady_clock::time_point now{};
    TabManager tabs([&] { return now; });
    const auto id = tabs.open("https://example.test/a");
    tabs.set_title(id, "Page A");
    tabs.set_favicon(id, "https://example.test/icon.png");
    tabs.record_web_navigation(id, "https://example.test/a");
    tabs.set_url(id, "https://example.test/b");
    tabs.record_web_navigation(id, "https://example.test/b");
    tabs.mute(id, true);
    tabs.open("https://active.test");
    now += std::chrono::minutes(2);

    const auto count = tabs.count();
    REQUIRE(tabs.suspend(id));
    CHECK_FALSE(tabs.suspend(id));
    REQUIRE(tabs.find(id));
    CHECK(tabs.find(id)->suspended_at == now);
    CHECK(tabs.find(id)->url.raw() == "https://example.test/b");
    CHECK(tabs.find(id)->favicon_url == "https://example.test/icon.png");
    CHECK(tabs.find(id)->title == "Page A");
    CHECK(tabs.find(id)->muted);
    CHECK(tabs.find(id)->history.size() == 2);
    CHECK(tabs.snapshot().front().url == "https://example.test/b");

    REQUIRE(tabs.restore(id));
    CHECK_FALSE(tabs.restore(id));
    CHECK(tabs.count() == count); // restore does NOT call open()
    CHECK(tabs.find(id)->id == id);
    CHECK_FALSE(tabs.find(id)->suspended);
    CHECK(tabs.find(id)->suspended_at == std::chrono::steady_clock::time_point{});
    CHECK(tabs.find(id)->can_go_back());
    REQUIRE(tabs.go_back(id));
    CHECK(tabs.find(id)->current_entry()->url.raw() == "https://example.test/a");
    REQUIRE(tabs.go_forward(id));
    CHECK(tabs.find(id)->current_entry()->url.raw() == "https://example.test/b");
}

TEST_CASE("manual and automatic suspension honor every protection") {
    std::chrono::steady_clock::time_point now{};
    TabManager tabs([&] { return now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    const auto id = tabs.open("https://example.test");
    CHECK_FALSE(tabs.suspend(id)); // active
    tabs.open("https://active.test");
    SUBCASE("pinned") { tabs.pin(id, true); }
    SUBCASE("audio") { tabs.set_audible(id, true); }
    SUBCASE("download") { tabs.set_pending_downloads(id, 1); }
    SUBCASE("loading") { tabs.set_loading(id, true); }
    SUBCASE("crashed") { tabs.mark_crashed(id, TabCrashReason::RendererGone); }
    SUBCASE("user exclusion") { tabs.set_suspend_excluded(id, true); }
    now += std::chrono::minutes(5);
    CHECK_FALSE(tabs.suspend(id));
    CHECK(tabs.tick().empty());
    CHECK_FALSE(tabs.find(id)->suspended);
}

TEST_CASE("exclusions wake tabs and survive transfer without duplicate entries") {
    std::chrono::steady_clock::time_point now{};
    TabManager first([&] { return now; });
    first.set_suspend_after(std::chrono::minutes(1));
    const auto id = first.open("https://example.test");
    first.open("https://active.test");
    now += std::chrono::minutes(1);
    REQUIRE(first.tick().size() == 1);
    CHECK(first.set_suspend_excluded(id, true));
    CHECK_FALSE(first.find(id)->suspended);
    now += std::chrono::minutes(10);
    CHECK(first.tick().empty());
    auto moved = first.detach(id);
    REQUIRE(moved);
    TabManager second([&] { return now; });
    second.open("https://other.test");
    REQUIRE(second.attach(std::move(*moved), std::nullopt, false));
    CHECK(second.find(id)->exclude_from_suspension);
    CHECK(second.count() == 2);
    CHECK_FALSE(second.attach(*second.find(id)));
    CHECK(second.count() == 2);
}

TEST_CASE("model history records links and preserves forward position") {
    TabManager tabs;
    const auto id = tabs.open("https://example.test/a");
    tabs.record_web_navigation(id, "https://example.test/a");
    tabs.set_url(id, "https://example.test/b");
    tabs.record_web_navigation(id, "https://example.test/b");
    tabs.set_title(id, "Second page");
    REQUIRE(tabs.go_back(id));
    CHECK(tabs.find(id)->current_entry()->url.raw() == "https://example.test/a");
    tabs.set_url(id, "https://example.test/a");
    tabs.record_web_navigation(id, "https://example.test/a"); // reload, no append
    CHECK(tabs.find(id)->history.size() == 2);
    CHECK(tabs.find(id)->history[1].title == "Second page");
    CHECK(tabs.find(id)->can_go_forward());
}

TEST_CASE("muted Twitch tabs remain protected from manual suspension") {
    TabManager tabs;
    const auto stream = tabs.open("https://twitch.tv/videos/123");
    tabs.mute(stream, true);
    tabs.open("https://active.test");
    CHECK_FALSE(tabs.suspend(stream));
}

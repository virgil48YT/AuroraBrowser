#include <doctest.h>

#include "tabs/TabManager.hpp"

using namespace aurora::core;

TEST_CASE("open returns a valid id and tracks count") {
    TabManager m;
    TabId a = m.open("https://a.test");
    CHECK(a.valid());
    CHECK(m.count() == 1);
    CHECK(m.active() == a);
}

TEST_CASE("close switches active to a neighbor") {
    TabManager m;
    TabId a = m.open("https://a.test");
    TabId b = m.open("https://b.test");
    TabId c = m.open("https://c.test");
    CHECK(m.active() == c);

    m.close(c);
    CHECK(m.active() == b);
    m.close(b);
    CHECK(m.active() == a);
    m.close(a);
    CHECK_FALSE(m.active().valid());
}

TEST_CASE("reopen_closed restores the last closed tab") {
    TabManager m;
    m.open("https://a.test");
    TabId b = m.open("https://b.test");
    m.close(b);
    auto reopened = m.reopen_closed();
    REQUIRE(reopened.has_value());
    CHECK(m.count() == 2);
    CHECK(m.active() == *reopened);
}

TEST_CASE("reopen_closed returns nullopt when stack empty") {
    TabManager m;
    CHECK_FALSE(m.reopen_closed().has_value());
}

TEST_CASE("move reorders the flat tab list") {
    TabManager m;
    TabId a = m.open("https://a.test");
    TabId b = m.open("https://b.test");
    TabId c = m.open("https://c.test");

    REQUIRE(m.move(a, 2));  // a to end -> b, c, a
    const auto& tabs = m.tabs();
    CHECK(tabs[0].id == b);
    CHECK(tabs[1].id == c);
    CHECK(tabs[2].id == a);
}

TEST_CASE("pin and mute toggle state and notify") {
    struct Recorder : TabObserver {
        int pinned = 0, muted = 0;
        void on_tab_changed(TabId, TabChange c) override {
            if (c == TabChange::PinnedChanged) ++pinned;
            if (c == TabChange::MutedChanged) ++muted;
        }
    } rec;
    TabManager m;
    m.add_observer(&rec);
    TabId t = m.open("https://a.test");
    CHECK(m.pin(t, true));
    CHECK(m.find(t)->pinned);
    CHECK(m.pin(t, false));
    CHECK_FALSE(m.find(t)->pinned);

    CHECK(m.mute(t, true));
    CHECK(m.find(t)->muted);
    CHECK(rec.pinned == 2);
    CHECK(rec.muted >= 1);
}

TEST_CASE("commit_navigation truncates forward history") {
    TabManager m;
    TabId t = m.open("https://a.test");
    NavigationEntry e1{Url("https://a.test"), "A", "", {}};
    m.commit_navigation(t, e1);
    NavigationEntry e2{Url("https://b.test"), "B", "", {}};
    m.commit_navigation(t, e2);
    CHECK(m.find(t)->history.size() == 2);

    CHECK(m.go_back(t));
    CHECK(m.find(t)->current_index == 0);

    // Navigating to a new entry discards the forward entry (e2).
    NavigationEntry e3{Url("https://c.test"), "C", "", {}};
    m.commit_navigation(t, e3);
    CHECK(m.find(t)->history.size() == 2);
    CHECK(m.find(t)->current_entry()->url.host() == "c.test");
}

TEST_CASE("snapshot captures order, titles, and pin state") {
    TabManager m;
    m.open("https://a.test");
    TabId pinned = m.open("https://b.test");
    m.pin(pinned, true);
    auto snap = m.snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0].url == "https://a.test");
    CHECK(snap[1].pinned);
    CHECK(snap[1].tab_index == 1);
}

TEST_CASE("mark_crashed and mark_recovered toggle crash state") {
    TabManager m;
    TabId t = m.open("https://a.test");
    m.mark_crashed(t, TabCrashReason::RendererGone);
    CHECK(m.find(t)->crashed);
    CHECK(m.find(t)->crash_reason == TabCrashReason::RendererGone);
    m.mark_recovered(t);
    CHECK_FALSE(m.find(t)->crashed);
}

TEST_CASE("navigating to another site drops the previous page's favicon URL") {
    // The engine only reports a favicon for pages that have one, so a page
    // without one used to inherit its predecessor's URL — and the favicon cache
    // then stored that icon under the NEW host, showing (and persisting) the
    // wrong site's icon.
    TabManager tabs;
    const auto id = tabs.open("https://github.com/explore");
    tabs.set_favicon(id, "https://github.com/favicon.ico");
    REQUIRE(tabs.find(id) != nullptr);
    CHECK(tabs.find(id)->favicon_url == "https://github.com/favicon.ico");

    // Same host, different page: the icon is genuinely shared, so it stays.
    tabs.set_url(id, "https://github.com/features");
    CHECK(tabs.find(id)->favicon_url == "https://github.com/favicon.ico");

    // Another host: the inherited icon URL must be gone.
    tabs.set_url(id, "https://example.com/");
    CHECK(tabs.find(id)->favicon_url.empty());
}

namespace {
struct SuspensionClock {
    std::chrono::steady_clock::time_point now{};
    void advance(std::chrono::minutes amount) { now += amount; }
};
} // namespace

TEST_CASE("suspension starts at the configured idle boundary and only once") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(5));
    const auto idle = tabs.open("https://idle.test");
    const auto active = tabs.open("https://active.test");
    clock.advance(std::chrono::minutes(4));
    CHECK(tabs.tick().empty());
    clock.advance(std::chrono::minutes(1));
    const auto suspended = tabs.tick();
    REQUIRE(suspended.size() == 1);
    CHECK(suspended.front() == idle);
    CHECK(tabs.find(idle)->suspended);
    CHECK_FALSE(tabs.find(active)->suspended);
    CHECK(tabs.tick().empty());
}

TEST_CASE("suspension is disabled by default and nonpositive thresholds wake tabs") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    const auto idle = tabs.open("https://idle.test");
    tabs.open("https://active.test");
    clock.advance(std::chrono::minutes(90));
    CHECK(tabs.tick().empty());
    tabs.set_suspend_after(std::chrono::minutes(5));
    CHECK(tabs.tick().size() == 1);
    SUBCASE("zero disables") { tabs.set_suspend_after(std::chrono::minutes(0)); }
    SUBCASE("negative disables") { tabs.set_suspend_after(std::chrono::minutes(-5)); }
    CHECK_FALSE(tabs.find(idle)->suspended);
    clock.advance(std::chrono::minutes(90));
    CHECK(tabs.tick().empty());
}

TEST_CASE("activation wakes before ActiveChanged and resets the idle interval") {
    struct Recorder : TabObserver {
        TabManager* tabs = nullptr;
        std::vector<TabChange> events;
        void on_tab_changed(TabId id, TabChange event) override {
            events.push_back(event);
            if (event == TabChange::ActiveChanged) CHECK_FALSE(tabs->find(id)->suspended);
        }
    } recorder;
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    recorder.tabs = &tabs;
    const auto first = tabs.open("https://first.test");
    const auto second = tabs.open("https://second.test");
    tabs.set_suspend_after(std::chrono::minutes(5));
    clock.advance(std::chrono::minutes(5));
    tabs.tick();
    tabs.add_observer(&recorder);
    REQUIRE(tabs.set_active(first));
    REQUIRE(recorder.events.size() == 2);
    CHECK(recorder.events[0] == TabChange::SuspendedChanged);
    CHECK(recorder.events[1] == TabChange::ActiveChanged);
    CHECK(tabs.find(first)->last_activated == clock.now);
    clock.advance(std::chrono::minutes(60));
    tabs.set_active(second);
    CHECK(tabs.tick().empty()); // sixty foreground minutes do not count as idle
    clock.advance(std::chrono::minutes(5));
    CHECK(tabs.tick().size() == 1);
    CHECK(tabs.find(first)->suspended);
    tabs.remove_observer(&recorder);
}

TEST_CASE("suspension protects pinned audible downloading loading and crashed tabs") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    const auto guarded = tabs.open("https://guarded.test");
    tabs.open("https://active.test");
    SUBCASE("pinned") { tabs.pin(guarded, true); }
    SUBCASE("audible") { tabs.set_audible(guarded, true); }
    SUBCASE("muted but still playing") {
        tabs.set_audible(guarded, true);
        tabs.mute(guarded, true);
    }
    SUBCASE("pending downloads") { tabs.set_pending_downloads(guarded, 2); }
    SUBCASE("loading") { tabs.set_loading(guarded, true); }
    SUBCASE("crashed") { tabs.mark_crashed(guarded, TabCrashReason::OutOfMemory); }
    clock.advance(std::chrono::minutes(60));
    CHECK(tabs.tick().empty());
    CHECK_FALSE(tabs.find(guarded)->suspended);
}

TEST_CASE("new protected activity wakes a suspended background tab") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    const auto idle = tabs.open("https://idle.test");
    tabs.open("https://active.test");
    clock.advance(std::chrono::minutes(1));
    REQUIRE(tabs.tick().size() == 1);
    SUBCASE("pin") { tabs.pin(idle, true); }
    SUBCASE("audio") { tabs.set_audible(idle, true); }
    SUBCASE("download") { tabs.set_pending_downloads(idle, 1); }
    SUBCASE("loading") { tabs.set_loading(idle, true); }
    SUBCASE("crash") { tabs.mark_crashed(idle, TabCrashReason::RendererGone); }
    CHECK_FALSE(tabs.find(idle)->suspended);
    clock.advance(std::chrono::minutes(10));
    CHECK(tabs.tick().empty());
}

TEST_CASE("finishing one of several downloads does not permit suspension") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    const auto idle = tabs.open("https://idle.test");
    tabs.open("https://active.test");
    tabs.set_pending_downloads(idle, 2);
    clock.advance(std::chrono::minutes(2));
    tabs.set_pending_downloads(idle, 1);
    CHECK(tabs.tick().empty());
    tabs.set_pending_downloads(idle, 0);
    CHECK(tabs.tick().size() == 1);
}

TEST_CASE("closing the active tab wakes its suspended successor") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    const auto idle = tabs.open("https://idle.test");
    const auto active = tabs.open("https://active.test");
    clock.advance(std::chrono::minutes(2));
    tabs.tick();
    REQUIRE(tabs.find(idle)->suspended);
    tabs.close(active);
    CHECK(tabs.active() == idle);
    CHECK_FALSE(tabs.find(idle)->suspended);
    CHECK(tabs.find(idle)->last_activated == clock.now);
}

TEST_CASE("reopening and transferring tabs do not leave active tabs suspended") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    const auto idle = tabs.open("https://idle.test");
    tabs.open("https://active.test");
    clock.advance(std::chrono::minutes(2));
    tabs.tick();
    SUBCASE("reopen") {
        tabs.close(idle);
        const auto reopened = tabs.reopen_closed();
        REQUIRE(reopened.has_value());
        CHECK_FALSE(tabs.find(*reopened)->suspended);
        CHECK(tabs.find(*reopened)->pending_downloads == 0);
    }
    SUBCASE("transfer") {
        auto state = tabs.detach(idle);
        REQUIRE(state.has_value());
        TabManager destination([&] { return clock.now; });
        REQUIRE(destination.attach(std::move(*state)));
        CHECK(destination.active() == idle);
        CHECK_FALSE(destination.find(idle)->suspended);
    }
}

TEST_CASE("suspension ignores internal pages and tolerates a clock moving backwards") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    tabs.open("aurora://settings");
    tabs.open("file:///tmp/test.html");
    tabs.open("about:blank");
    const auto http = tabs.open("http://idle.test");
    tabs.open("https://active.test");
    clock.advance(std::chrono::minutes(-1));
    CHECK(tabs.tick().empty());
    clock.advance(std::chrono::minutes(2));
    const auto suspended = tabs.tick();
    REQUIRE(suspended.size() == 1);
    CHECK(suspended.front() == http);
}

TEST_CASE("suspension thresholds can change without recreating the manager") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.open("https://idle.test");
    tabs.open("https://active.test");
    tabs.set_suspend_after(std::chrono::minutes(60));
    clock.advance(std::chrono::minutes(10));
    CHECK(tabs.tick().empty());
    tabs.set_suspend_after(std::chrono::minutes(5));
    CHECK(tabs.tick().size() == 1);
    CHECK_FALSE(tabs.set_active(TabId(0)));
    CHECK_FALSE(tabs.find(tabs.active())->suspended);
}

TEST_CASE("background Twitch playback stays live even if muted") {
    SuspensionClock clock;
    TabManager tabs([&] { return clock.now; });
    tabs.set_suspend_after(std::chrono::minutes(1));
    const auto twitch = tabs.open("https://www.twitch.tv/videos/123");
    tabs.mute(twitch, true);
    const auto other = tabs.open("https://twitch.tv.evil.test/");
    tabs.open("https://active.test/");
    clock.advance(std::chrono::minutes(5));
    const auto suspended = tabs.tick();
    REQUIRE(suspended.size() == 1);
    CHECK(suspended.front() == other);
    CHECK_FALSE(tabs.find(twitch)->suspended);
}

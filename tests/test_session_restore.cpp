// Regression tests for the session-restore crash.
//
// Two independent bugs combined to crash Aurora on startup whenever the
// previous session had been saved:
//
//   1. TabManager::open() called set_active() before notifying Added, and
//      TabManager::close() notified Removed while active() still pointed at
//      the tab it had just erased. MainWindow keeps a map<TabId, widget> that
//      it populates on Added and reads on ActiveChanged, so restore made it
//      look up TabIds it had never seen (or had just destroyed) — a dangling
//      lookup that dereferenced freed memory.
//   2. The restore path trusted whatever was in `session_tabs`, so an empty,
//      truncated or javascript: URL produced a tab the window could not host.
//
// The FakeWindow observer below mimics MainWindow's bookkeeping exactly, so
// these invariants are enforced headlessly.
#include <doctest.h>

#include <map>
#include <string>
#include <vector>

#include "session/SessionService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"
#include "tabs/TabManager.hpp"

using namespace aurora;
using namespace aurora::core;
using namespace aurora::session;
using namespace aurora::storage;

namespace {

// Stands in for MainWindow: owns one "host widget" per tab, created on Added
// and destroyed on Removed, and is required to have a widget for whichever tab
// the manager reports as active.
class FakeWindow : public TabObserver {
public:
    explicit FakeWindow(TabManager& tabs) : tabs_(tabs) { tabs_.add_observer(this); }
    ~FakeWindow() override { tabs_.remove_observer(this); }

    FakeWindow(const FakeWindow&) = delete;
    FakeWindow& operator=(const FakeWindow&) = delete;

    void on_tab_changed(TabId id, TabChange change) override {
        switch (change) {
            case TabChange::Added:
                hosts_[id] = "host:" + std::to_string(id.value());
                break;
            case TabChange::Removed:
                hosts_.erase(id);
                // MainWindow reads the active tab here to re-show its host.
                if (tabs_.active().valid() && hosts_.count(tabs_.active()) == 0) {
                    dangling_active_reads++;
                }
                break;
            case TabChange::ActiveChanged:
                if (hosts_.count(id) == 0) missing_host_activations++;
                shown_ = id;
                break;
            default:
                break;
        }
    }

    size_t host_count() const { return hosts_.size(); }
    TabId shown() const { return shown_; }
    bool has_host(TabId id) const { return hosts_.count(id) != 0; }

    int missing_host_activations = 0;
    int dangling_active_reads = 0;

private:
    TabManager& tabs_;
    std::map<TabId, std::string> hosts_;
    TabId shown_{0};
};

Database open_db() {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    return db;
}

SavedSession make_session(std::vector<SavedTab> tabs) {
    SavedSession s;
    s.id = 1;
    s.created_at = std::chrono::system_clock::now();
    s.tabs = std::move(tabs);
    return s;
}

SavedTab tab(std::string url, int index, std::string title = "t") {
    SavedTab t;
    t.url = std::move(url);
    t.title = std::move(title);
    t.window_index = 0;
    t.tab_index = index;
    return t;
}

} // namespace

TEST_CASE("a tab is announced before it becomes active") {
    TabManager tabs;
    FakeWindow window(tabs);

    auto id = tabs.open("https://a.test");
    CHECK(window.has_host(id));
    CHECK(window.shown() == id);
    CHECK(window.missing_host_activations == 0);
}

TEST_CASE("closing a tab never exposes an active id without a host") {
    TabManager tabs;
    FakeWindow window(tabs);

    auto a = tabs.open("https://a.test");
    auto b = tabs.open("https://b.test");
    auto c = tabs.open("https://c.test");

    tabs.close(c);
    CHECK(window.dangling_active_reads == 0);
    CHECK(window.missing_host_activations == 0);
    CHECK(tabs.active() == b);
    CHECK(window.has_host(tabs.active()));

    tabs.close(a);
    tabs.close(b);
    CHECK(window.dangling_active_reads == 0);
    CHECK(window.missing_host_activations == 0);
    CHECK(window.host_count() == 0);
    CHECK_FALSE(tabs.active().valid());
    CHECK_FALSE(tabs.contains(a));
}

TEST_CASE("closing the last tab does not leave a dangling active id") {
    TabManager tabs;
    FakeWindow window(tabs);
    auto only = tabs.open("https://only.test");
    CHECK(tabs.close(only));
    CHECK_FALSE(tabs.active().valid());
    CHECK(tabs.count() == 0);
    CHECK(window.dangling_active_reads == 0);
    // Closing again is a no-op, not a crash.
    CHECK_FALSE(tabs.close(only));
}

TEST_CASE("restore then closing the seed tab keeps a valid active tab") {
    // This is the exact startup sequence: the window opens a seed tab, the
    // saved tabs are restored, then the seed tab is closed.
    TabManager tabs;
    FakeWindow window(tabs);
    auto seed = tabs.open("https://seed.test");

    auto plan = plan_restore(make_session({tab("https://a.test", 0),
                                           tab("https://b.test", 1)}));
    REQUIRE(plan.tabs.size() == 2);
    for (const auto& t : plan.tabs) tabs.open(t.url);

    CHECK(tabs.close(seed));
    CHECK(tabs.count() == 2);
    CHECK(tabs.active().valid());
    CHECK(window.has_host(tabs.active()));
    CHECK(window.dangling_active_reads == 0);
    CHECK(window.missing_host_activations == 0);
}

TEST_CASE("is_restorable_url accepts navigable urls") {
    CHECK(is_restorable_url("https://example.com/x?y=1"));
    CHECK(is_restorable_url("http://example.com"));
    CHECK(is_restorable_url("file:///tmp/page.html"));
    CHECK(is_restorable_url("aurora://history"));
}

TEST_CASE("is_restorable_url rejects junk and unsafe schemes") {
    CHECK_FALSE(is_restorable_url(""));
    CHECK_FALSE(is_restorable_url("   "));
    CHECK_FALSE(is_restorable_url("javascript:alert(1)"));
    CHECK_FALSE(is_restorable_url("JAVASCRIPT:alert(1)"));
    CHECK_FALSE(is_restorable_url("data:text/html,<b>x"));
    CHECK_FALSE(is_restorable_url("blob:https://a.test/123"));
    CHECK_FALSE(is_restorable_url(std::string(9000, 'a')));
}

TEST_CASE("plan_restore drops bad rows and counts them") {
    auto plan = plan_restore(make_session({
        tab("https://good.test", 0),
        tab("", 1),
        tab("javascript:alert(1)", 2),
        tab("   ", 3),
        tab("https://also-good.test", 4),
    }));
    CHECK(plan.tabs.size() == 2);
    CHECK(plan.skipped == 3);
    CHECK(plan.tabs[0].url == "https://good.test");
    CHECK(plan.tabs[1].url == "https://also-good.test");
}

TEST_CASE("plan_restore orders by window then tab index") {
    SavedTab a = tab("https://a.test", 2);
    SavedTab b = tab("https://b.test", 0);
    SavedTab c = tab("https://c.test", 1);
    c.window_index = 1;
    auto plan = plan_restore(make_session({a, c, b}));
    REQUIRE(plan.tabs.size() == 3);
    CHECK(plan.tabs[0].url == "https://b.test");
    CHECK(plan.tabs[1].url == "https://a.test");
    CHECK(plan.tabs[2].url == "https://c.test");
}

TEST_CASE("plan_restore caps the number of restored tabs") {
    std::vector<SavedTab> many;
    for (int i = 0; i < 40; ++i) {
        many.push_back(tab("https://t" + std::to_string(i) + ".test", i));
    }
    auto plan = plan_restore(make_session(many), 10);
    CHECK(plan.tabs.size() == 10);
    CHECK(plan.skipped == 30);
}

TEST_CASE("an empty saved session yields an empty plan, not a crash") {
    auto plan = plan_restore(make_session({}));
    CHECK(plan.tabs.empty());
    CHECK(plan.skipped == 0);
}

TEST_CASE("a round-trip through SessionService survives corrupt rows") {
    auto db = open_db();
    SessionService service(db);

    std::vector<SavedTab> tabs{tab("https://keep.test", 0),
                               tab("javascript:void(0)", 1),
                               tab("", 2)};
    REQUIRE(service.save(tabs) > 0);

    auto loaded = service.load_last();
    REQUIRE(loaded.has_value());
    auto plan = plan_restore(*loaded);
    CHECK(plan.tabs.size() == 1);
    CHECK(plan.tabs[0].url == "https://keep.test");
    CHECK(plan.skipped == 2);

    // And the plan is safe to feed straight into a manager + observer.
    TabManager manager;
    FakeWindow window(manager);
    for (const auto& t : plan.tabs) manager.open(t.url);
    CHECK(manager.count() == 1);
    CHECK(window.has_host(manager.active()));
}

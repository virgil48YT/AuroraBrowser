// Regression tests for the 11-tab session-restore startup crash.
//
// THE BUG. MainWindow::restore_session() opened every saved tab in a tight
// synchronous loop. Each TabManager::open() notified TabChange::Added, and the
// window's Added handler built a web-content host and then immediately made it
// the *current* widget of the content stack. Being made current shows the host,
// and the host's showEvent created a real web engine browser with a native
// window handle. So an 11-tab session created 11 native browsers before the
// event loop ever turned — which is where the crash reported right after the
// log line "restoring session with 11 tabs" came from.
//
// THE FIX, expressed as invariants these tests enforce:
//   * The restore loop runs in bulk mode (session::run_restore), during which
//     the UI makes NOTHING current, so no host may create its engine.
//   * The seed tab is closed inside bulk mode, so removing the visible tab
//     cannot promote an arbitrary sibling to visible.
//   * Bulk mode ends, and then exactly ONE tab is activated — hence exactly one
//     shown host and exactly one created engine, for any number of saved tabs.
//
// FakeWindow below mirrors MainWindow's bookkeeping (hosts map, "current"
// content widget, engine creation on show) so the invariants are enforced
// headlessly, without Qt. The sequencing under test is the real production
// code: session::run_restore is what MainWindow::restore_session() calls.
#include <doctest.h>

#include <map>
#include <string>
#include <vector>

#include "session/SessionRestore.hpp"
#include "session/SessionService.hpp"
#include "storage/Database.hpp"
#include "storage/Migration.hpp"
#include "tabs/TabManager.hpp"

using namespace aurora;
using namespace aurora::core;
using namespace aurora::session;
using namespace aurora::storage;

namespace {

// A stand-in for one MainWindow host widget: it creates its "engine" the first
// time it is made the current content widget, and never more than once.
struct FakeHost {
    TabId tab;
    bool current = false;
    bool engine_created = false;
    int activations = 0;
    int create_calls = 0;

    void make_current() {
        if (!current) ++activations;
        current = true;
        if (engine_created) return;  // one-shot
        engine_created = true;
        ++create_calls;
    }
    void relinquish() { current = false; }
};

// Mirrors MainWindow: hosts are built on Added, and made current only through
// the single set_current() choke point, which is suppressed in bulk mode.
class FakeWindow : public TabObserver {
public:
    explicit FakeWindow(TabManager& tabs) : tabs_(tabs) { tabs_.add_observer(this); }
    ~FakeWindow() override { tabs_.remove_observer(this); }

    FakeWindow(const FakeWindow&) = delete;
    FakeWindow& operator=(const FakeWindow&) = delete;

    // Reproduces the pre-fix window: the Added handler showed every tab as it
    // arrived, and bulk mode did not exist.
    void use_legacy_show_on_added(bool on) { legacy_ = on; }

    void set_bulk_open(bool on) {
        if (legacy_) return;  // the old window had no bulk mode at all
        bulk_open_ = on;
        if (on) {
            bulk_creates_ = 0;
            // Entering bulk mode parks the content area on its placeholder.
            set_current(TabId(0));
        }
    }

    // The window's "show exactly this tab" entry point, called once by
    // run_restore after bulk mode ends.
    void show(TabId id) { set_current(id); }

    void on_tab_changed(TabId id, TabChange change) override {
        switch (change) {
            case TabChange::Added:
                hosts_[id] = FakeHost{id, false, false, 0, 0};
                // The pre-fix bug in one line: show (and therefore create) the
                // host the instant the tab is announced.
                if (legacy_ || !bulk_open_) set_current(id);
                break;
            case TabChange::Removed:
                hosts_.erase(id);
                if (tabs_.active().valid() && hosts_.count(tabs_.active()) == 0) {
                    ++dangling_active_reads;
                }
                // MainWindow re-asserts the invariant after the stack changes.
                sync_current();
                break;
            case TabChange::ActiveChanged:
                if (hosts_.count(id) == 0) ++missing_host_activations;
                if (!bulk_open_) set_current(id);
                break;
            default:
                break;
        }
    }

    // Counts over live hosts.
    int shown_hosts() const {
        int n = 0;
        for (const auto& [id, h] : hosts_) {
            if (h.current) ++n;
            (void)id;
        }
        return n;
    }
    int created_engines() const {
        int n = 0;
        for (const auto& [id, h] : hosts_) {
            if (h.engine_created) ++n;
            (void)id;
        }
        return n;
    }
    int max_create_calls_per_host() const {
        int n = 0;
        for (const auto& [id, h] : hosts_) {
            if (h.create_calls > n) n = h.create_calls;
            (void)id;
        }
        return n;
    }
    size_t host_count() const { return hosts_.size(); }
    bool has_host(TabId id) const { return hosts_.count(id) != 0; }
    TabId current_tab() const { return current_; }
    // Engines created while bulk mode was on. Must stay at 0.
    int bulk_creates() const { return bulk_creates_; }

    int missing_host_activations = 0;
    int dangling_active_reads = 0;

private:
    void set_current(TabId id) {
        for (auto& [host_tab, host] : hosts_) {
            if (host_tab != id) host.relinquish();
        }
        current_ = id;
        auto it = hosts_.find(id);
        if (it == hosts_.end()) return;
        if (bulk_open_ && !it->second.engine_created) ++bulk_creates_;
        it->second.make_current();
    }
    void sync_current() {
        if (bulk_open_) {
            set_current(TabId(0));
            return;
        }
        set_current(tabs_.active());
    }

    TabManager& tabs_;
    bool legacy_ = false;
    std::map<TabId, FakeHost> hosts_;
    TabId current_{0};
    bool bulk_open_ = false;
    int bulk_creates_ = 0;
};

SavedSession make_bulk_session(int n) {
    SavedSession s;
    s.id = 7;
    s.created_at = std::chrono::system_clock::now();
    for (int i = 0; i < n; ++i) {
        SavedTab t;
        t.url = "https://tab" + std::to_string(i) + ".test/page";
        t.title = "Tab " + std::to_string(i);
        t.window_index = 0;
        t.tab_index = i;
        s.tabs.push_back(t);
    }
    return s;
}

} // namespace

TEST_CASE("a 20-tab bulk restore shows and creates exactly one host") {
    TabManager tabs;
    FakeWindow window(tabs);
    const auto seed = tabs.open("https://seed.test");
    REQUIRE(window.shown_hosts() == 1);  // the seed tab is visible pre-restore

    const auto plan = plan_restore(make_bulk_session(20));
    REQUIRE(plan.tabs.size() == 20);

    const auto outcome = run_restore(
        tabs, plan, seed, [&window](bool on) { window.set_bulk_open(on); },
        [&window](TabId id) { window.show(id); });

    CHECK(outcome.opened == 20);
    CHECK(tabs.count() == 20);
    CHECK_FALSE(tabs.contains(seed));

    // The whole point: one visible host, one engine, one active tab.
    CHECK(window.shown_hosts() == 1);
    CHECK(window.created_engines() == 1);
    CHECK(window.host_count() == 20);
    CHECK(window.bulk_creates() == 0);
    CHECK(outcome.active == tabs.active());
    CHECK(tabs.active().valid());
    CHECK(window.current_tab() == tabs.active());
    CHECK(window.has_host(tabs.active()));

    // And no host was asked to create twice (re-entrancy / duplicate create).
    CHECK(window.max_create_calls_per_host() == 1);
    CHECK(window.missing_host_activations == 0);
    CHECK(window.dangling_active_reads == 0);
}

TEST_CASE("the restored active tab is the first restored tab, in order") {
    TabManager tabs;
    FakeWindow window(tabs);
    const auto seed = tabs.open("https://seed.test");

    const auto plan = plan_restore(make_bulk_session(20));
    const auto outcome = run_restore(
        tabs, plan, seed, [&window](bool on) { window.set_bulk_open(on); },
        [&window](TabId id) { window.show(id); });

    REQUIRE(tabs.count() == 20);
    CHECK(tabs.tabs().front().id == outcome.active);
    CHECK(tabs.tabs().front().url.raw() == "https://tab0.test/page");
    CHECK(tabs.tabs().front().title == "Tab 0");
    CHECK(tabs.tabs().back().url.raw() == "https://tab19.test/page");
}

TEST_CASE("the 11-tab session from the crash report restores without a burst") {
    // The exact reported shape: 11 saved tabs, restored over a seed tab.
    TabManager tabs;
    FakeWindow window(tabs);
    const auto seed = tabs.open("https://seed.test");

    const auto plan = plan_restore(make_bulk_session(11));
    REQUIRE(plan.tabs.size() == 11);

    int max_shown_during_restore = 0;
    const auto outcome = run_restore(
        tabs, plan, seed,
        [&](bool on) { window.set_bulk_open(on); },
        [&](TabId id) { window.show(id); },
        [&](int, int, const std::string&) {
            // Sampled after every single opened tab: nothing may be visible.
            if (window.shown_hosts() > max_shown_during_restore) {
                max_shown_during_restore = window.shown_hosts();
            }
        });

    CHECK(max_shown_during_restore == 0);
    CHECK(outcome.opened == 11);
    CHECK(window.shown_hosts() == 1);
    CHECK(window.created_engines() == 1);
}

TEST_CASE("bulk restore keeps a valid active tab even when nothing is valid") {
    TabManager tabs;
    FakeWindow window(tabs);
    const auto seed = tabs.open("https://seed.test");

    SavedSession junk;
    junk.tabs.push_back(SavedTab{"javascript:alert(1)", "x", false, 0, 0});
    junk.tabs.push_back(SavedTab{"", "y", false, 0, 1});
    const auto plan = plan_restore(junk);
    REQUIRE(plan.tabs.empty());

    const auto outcome = run_restore(
        tabs, plan, seed, [&window](bool on) { window.set_bulk_open(on); },
        [&window](TabId id) { window.show(id); });

    CHECK(outcome.opened == 0);
    CHECK(tabs.count() == 1);
    CHECK(tabs.active() == seed);           // the window is never left empty
    CHECK(window.created_engines() == 1);   // still exactly one host
    CHECK(window.shown_hosts() == 1);
}

TEST_CASE("a restore of one tab still shows exactly one host") {
    TabManager tabs;
    FakeWindow window(tabs);
    const auto seed = tabs.open("https://seed.test");

    const auto plan = plan_restore(make_bulk_session(1));
    const auto outcome = run_restore(
        tabs, plan, seed, [&window](bool on) { window.set_bulk_open(on); },
        [&window](TabId id) { window.show(id); });

    CHECK(outcome.opened == 1);
    CHECK(tabs.count() == 1);
    CHECK(window.shown_hosts() == 1);
    CHECK(window.created_engines() == 1);
    CHECK(window.current_tab() == tabs.active());
}

TEST_CASE("bulk mode is left off even when a restore opens nothing") {
    TabManager tabs;
    FakeWindow window(tabs);
    const auto seed = tabs.open("https://seed.test");
    int edges = 0;
    bool last = false;
    run_restore(tabs, plan_restore(make_bulk_session(3)), seed,
                [&](bool on) {
                    ++edges;
                    last = on;
                    window.set_bulk_open(on);
                },
                [&](TabId id) { window.show(id); });
    CHECK(edges == 2);
    CHECK_FALSE(last);

    // A normal new tab after the restore is shown immediately again.
    const auto extra = tabs.open("https://after.test");
    CHECK(window.current_tab() == extra);
    CHECK(window.shown_hosts() == 1);
    CHECK(window.created_engines() == 2);  // the restored one + this one
}

TEST_CASE("a 20-tab session survives a real save/load round-trip") {
    Database db;
    REQUIRE(db.open(":memory:"));
    REQUIRE(Migrator::migrate(db) == DbError::Ok);
    SessionService service(db);

    const auto session = make_bulk_session(20);
    REQUIRE(service.save(session.tabs) > 0);

    const auto loaded = service.load_last();
    REQUIRE(loaded.has_value());
    const auto plan = plan_restore(*loaded);
    REQUIRE(plan.tabs.size() == 20);

    TabManager tabs;
    FakeWindow window(tabs);
    const auto seed = tabs.open("https://seed.test");
    const auto outcome = run_restore(
        tabs, plan, seed, [&window](bool on) { window.set_bulk_open(on); },
        [&window](TabId id) { window.show(id); });

    CHECK(outcome.opened == 20);
    CHECK(window.created_engines() == 1);
    CHECK(window.shown_hosts() == 1);
    CHECK(window.host_count() == 20);
}

TEST_CASE("root cause: the pre-fix window created one engine per restored tab") {
    // Documents the bug the guard exists for. With the old Added handler every
    // restored tab is made visible in turn, so an 11-tab session creates 11 web
    // engines (each with a native window) inside one synchronous loop, before
    // the event loop turns. That is the reported startup crash.
    TabManager tabs;
    FakeWindow window(tabs);
    window.use_legacy_show_on_added(true);
    const auto seed = tabs.open("https://seed.test");

    const auto plan = plan_restore(make_bulk_session(11));
    const auto outcome = run_restore(
        tabs, plan, seed, [&window](bool on) { window.set_bulk_open(on); },
        [&window](TabId id) { window.show(id); });

    CHECK(outcome.opened == 11);
    // Every single restored tab got an engine — the crash.
    CHECK(window.created_engines() == 11);
    // ...while only one of them is visible in the end, which is the whole
    // point: the other 10 were created for nothing.
    CHECK(window.shown_hosts() == 1);
}

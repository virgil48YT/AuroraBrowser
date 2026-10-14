#include "session/SessionRestore.hpp"

#include "tabs/TabManager.hpp"

namespace aurora::session {

namespace {

// Guarantees the "bulk mode off" edge is delivered exactly once, even if a tab
// observer throws while we are in the middle of the restore loop.
class BulkModeScope {
public:
    explicit BulkModeScope(const BulkModeFn& fn) : fn_(fn) {
        if (fn_) fn_(true);
    }
    ~BulkModeScope() {
        if (fn_) fn_(false);
    }
    BulkModeScope(const BulkModeScope&) = delete;
    BulkModeScope& operator=(const BulkModeScope&) = delete;

private:
    const BulkModeFn& fn_;
};

} // namespace

RestoreOutcome run_restore(core::TabManager& tabs, const RestorePlan& plan,
                           core::TabId seed, const BulkModeFn& set_bulk_mode,
                           const ShowTabFn& show_active,
                           const RestoreProgressFn& progress) {
    RestoreOutcome out;
    out.active = tabs.active();
    if (plan.tabs.empty()) {
        // Nothing valid to restore: the seed tab stays, and stays shown.
        if (show_active && out.active.valid()) show_active(out.active);
        return out;
    }

    const int total = static_cast<int>(plan.tabs.size());
    core::TabId first{0};
    {
        // --- bulk mode: nothing may be made visible in here ------------------
        BulkModeScope bulk(set_bulk_mode);
        for (const auto& saved_tab : plan.tabs) {
            const auto id = tabs.open(saved_tab.url);
            if (!id.valid()) continue;
            if (!first.valid()) first = id;
            if (!saved_tab.title.empty()) tabs.set_title(id, saved_tab.title);
            if (saved_tab.pinned) tabs.pin(id, true);
            ++out.opened;
            if (progress) progress(out.opened, total, saved_tab.url);
        }

        // Close the seed tab while still in bulk mode: the removal of the
        // currently visible tab is precisely when a container is tempted to
        // promote an arbitrary sibling to visible.
        if (out.opened > 0 && seed.valid() && tabs.contains(seed) &&
            tabs.count() > 1) {
            tabs.close(seed);
        }
    }
    // --- bulk mode is over: activate and show exactly one tab, once ----------
    // set_active() is a no-op (and notifies nothing) when the tab is already
    // active, which happens whenever closing the seed tab promoted it. The
    // explicit show below is therefore the contract, not the notification.
    if (first.valid() && tabs.contains(first)) tabs.set_active(first);
    out.active = tabs.active();
    if (show_active && out.active.valid()) show_active(out.active);
    return out;
}

} // namespace aurora::session

// Aurora — tab manager.
//
// Owns the ordered list of TabStates for one browser window and the
// "recently closed" stack used by "reopen closed tab". It is a UI-thread data
// structure: state mutations are cheap and synchronous, and the heavy work
// (renderer lifecycle, network) lives in the CEF layer, which observes tab
// changes through the TabObserver interface.
//
// The manager intentionally knows nothing about rendering. That separation is
// what lets the core be unit-tested without CEF and lets the UI be ported
// (or virtualized) independently of tab semantics.
//
// Tear-off / snap-back (Brave-style) is built on detach()/attach(): a tab
// moving to another window is detached here (NOT closed — it never enters the
// recently-closed stack) and attached to the destination window's manager
// with its identity and history intact.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/TabState.hpp"
#include "session/SessionService.hpp"

namespace aurora::core {

enum class TabChange {
    Added,
    Removed,
    ActiveChanged,
    Moved,
    PinnedChanged,
    MutedChanged,
    LoadingStateChanged,
    TitleChanged,
    ProgressChanged,
    Crashed,
    Recovered,
    SuspendedChanged,
};

// The UI implements this to repaint tab strips, update the omnibox, etc.
class TabObserver {
public:
    virtual ~TabObserver() = default;
    virtual void on_tab_changed(TabId id, TabChange change) = 0;
};

class TabManager {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    explicit TabManager(Clock clock = std::chrono::steady_clock::now);
    ~TabManager();

    // Non-owning. Multiple observers are allowed (e.g. tab strip + status bar).
    void add_observer(TabObserver* o);
    void remove_observer(TabObserver* o);

    // Open a tab at `index` (or at the end). If `url` is empty, the new-tab
    // page is shown. Returns the new tab id.
    TabId open(std::string_view url, std::optional<size_t> index = std::nullopt);

    // Close a tab. The tab is pushed onto the recently-closed stack so it can
    // be reopened. Closing the last tab does not destroy the window — the
    // caller decides window lifecycle.
    bool close(TabId id);

    // Remove a live tab for transfer to another window without treating it as
    // closed (it does NOT enter the recently-closed stack, so Ctrl+Shift+T
    // cannot resurrect a tab that merely moved windows). The complete TabState
    // (URL, model history, pin/mute state, title, favicon and crash/loading
    // metadata) is returned to the caller, and normal Removed/ActiveChanged
    // notifications keep this window's views in sync.
    std::optional<TabState> detach(TabId id);

    // Insert a TabState previously returned by detach(). Its identity and
    // history are preserved. Returns false when the state has an invalid id or
    // that id is already present in this manager.
    bool attach(TabState state, std::optional<size_t> index = std::nullopt,
                bool activate = true);

    // Reopen the most recently closed tab next to the active tab. Returns
    // nullopt if the stack is empty.
    std::optional<TabId> reopen_closed();

    bool set_active(TabId id);
    TabId active() const { return active_; }

    // Zero/negative disables suspension and wakes every suspended tab. tick()
    // marks and returns only newly suspended tabs; observers mirror changes
    // to the renderer through SuspendedChanged, including activation/wakeup.
    void set_suspend_after(std::chrono::minutes threshold);
    std::vector<TabId> tick();
    // Manual suspension has the same safety checks as automatic suspension.
    // Restoration keeps the same TabId and all tab metadata/history.
    bool suspend(TabId id);
    bool restore(TabId id);
    bool set_suspend_excluded(TabId id, bool excluded);
    void set_pending_downloads(TabId id, size_t count);

    // Reorder by moving `id` to `new_index` in the flat list.
    bool move(TabId id, size_t new_index);

    bool pin(TabId id, bool pinned);
    bool mute(TabId id, bool muted);
    void set_audible(TabId id, bool audible);
    void set_loading(TabId id, bool loading, int progress = -1);
    void set_title(TabId id, std::string_view title);
    void set_favicon(TabId id, std::string_view url);
    // Update the tab's current URL without touching its history stack or
    // firing navigation notifications. Used when CEF reports a main-frame
    // address change (link click, redirect) so the saved session and the
    // omnibox-on-tab-switch reflect the real URL. commit_navigation() is the
    // heavier path for omnibox-initiated navigations.
    void set_url(TabId id, std::string_view url);
    // Track committed web addresses (including link clicks) for restoration
    // after the native CEF navigation controller has been destroyed.
    void record_web_navigation(TabId id, std::string_view url);
    // Update CEF's real back/forward capability (from OnLoadingStateChange).
    // The toolbar buttons read these instead of the tab's own history stack.
    void set_nav_state(TabId id, bool can_go_back, bool can_go_forward);
    void mark_crashed(TabId id, TabCrashReason reason);
    void mark_recovered(TabId id);

    // Back/forward navigation within a tab's own history stack.
    bool go_back(TabId id);
    bool go_forward(TabId id);

    // Commit a new navigation in a tab: pushes onto the tab history stack,
    // truncating any forward entries (standard browser behavior).
    void commit_navigation(TabId id, const NavigationEntry& entry);

    // Read-only views.
    // True when `id` names a live tab. Cheap guard for observers that hold a
    // TabId across notifications.
    bool contains(TabId id) const;
    const TabState* find(TabId id) const;
    TabState* find(TabId id);
    const std::vector<TabState>& tabs() const { return tabs_; }
    size_t count() const { return tabs_.size(); }

    // Snapshot for session persistence. Only committed URLs are saved; the
    // new-tab page is saved as an empty URL the caller can interpret.
    std::vector<session::SavedTab> snapshot() const;

private:
    Clock clock_;
    std::chrono::minutes suspend_after_{0};
    std::vector<TabState> tabs_;
    TabId active_;
    std::vector<TabObserver*> observers_;
    std::vector<TabState> recently_closed_;

    void notify(TabId id, TabChange change);
    void wake(TabId id);
    bool can_suspend(const TabState& tab) const;
    size_t index_of(TabId id) const;
};

} // namespace aurora::core

#include "tabs/TabManager.hpp"

#include "cef/MediaPolicy.hpp"
#include "util/Log.hpp"

#include <algorithm>

namespace aurora::core {

namespace {
// std::vector iterators take a signed difference; casting here keeps the
// -Wsign-conversion build clean without scattering static_casts everywhere.
std::ptrdiff_t off(size_t n) { return static_cast<std::ptrdiff_t>(n); }

// Bound the recently-closed stack so memory stays bounded during long
// sessions with heavy tab churn.
constexpr size_t kMaxRecentlyClosed = 32;
} // namespace

TabManager::TabManager(Clock clock)
    : clock_(clock ? std::move(clock) : Clock(std::chrono::steady_clock::now)),
      active_(TabId(0)) {}
TabManager::~TabManager() = default;

void TabManager::add_observer(TabObserver* o) {
    observers_.push_back(o);
}
void TabManager::remove_observer(TabObserver* o) {
    observers_.erase(std::remove(observers_.begin(), observers_.end(), o),
                     observers_.end());
}

size_t TabManager::index_of(TabId id) const {
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].id == id) return i;
    }
    return tabs_.size();
}

void TabManager::notify(TabId id, TabChange change) {
    for (auto* o : observers_) o->on_tab_changed(id, change);
}

TabId TabManager::open(std::string_view url, std::optional<size_t> index) {
    TabState state;
    state.id = TabId::generate();
    state.url = Url(url);
    state.load_progress = -1;
    state.last_activated = state.inactive_since = clock_();

    size_t pos = index.value_or(tabs_.size());
    pos = std::min(pos, tabs_.size());
    tabs_.insert(tabs_.begin() + off(pos), state);
    // Announce the tab BEFORE it becomes active. Observers build their view
    // objects (tab-strip entries, content hosts) on Added; if ActiveChanged
    // arrived first they would be asked to activate a tab they have never
    // seen — the ordering bug behind the session-restore crash.
    notify(state.id, TabChange::Added);
    set_active(state.id);
    return state.id;
}

bool TabManager::close(TabId id) {
    size_t idx = index_of(id);
    if (idx >= tabs_.size()) return false;
    // Snapshot BEFORE detach so "reopen closed tab" restores the full state.
    // detach() itself never records here: a tab that merely moved to another
    // window (tear-off / snap-back) was not closed.
    recently_closed_.push_back(tabs_[idx]);
    if (recently_closed_.size() > kMaxRecentlyClosed) {
        recently_closed_.erase(recently_closed_.begin());
    }
    return detach(id).has_value();
}

std::optional<TabState> TabManager::detach(TabId id) {
    size_t idx = index_of(id);
    if (idx >= tabs_.size()) return std::nullopt;
    TabState state = std::move(tabs_[idx]);
    tabs_.erase(tabs_.begin() + off(idx));
    // Choose the successor BEFORE notifying, so an observer handling Removed
    // never sees active() pointing at the tab that just went away. Observers
    // (and the session-restore path, which closes the seed tab) used to read a
    // dangling TabId here and dereference a destroyed widget.
    bool active_changed = false;
    if (active_ == id) {
        active_ = tabs_.empty() ? TabId(0)
                                : tabs_[std::min(idx, tabs_.size() - 1)].id;
        active_changed = true;
    }
    notify(id, TabChange::Removed);
    if (active_changed && active_.valid()) {
        if (auto* next = find(active_)) next->last_activated = clock_();
        wake(active_);
        notify(active_, TabChange::ActiveChanged);
    }
    return state;
}

bool TabManager::attach(TabState state, std::optional<size_t> index,
                        bool activate) {
    if (!state.id.valid() || contains(state.id)) return false;
    const TabId id = state.id;
    // A transferred tab can come from a manager with a different test clock.
    state.inactive_since = clock_();
    size_t pos = std::min(index.value_or(tabs_.size()), tabs_.size());
    tabs_.insert(tabs_.begin() + off(pos), std::move(state));
    // Match open(): views must construct the tab before it can be activated.
    notify(id, TabChange::Added);
    // A window whose manager is empty must never end up with tabs but no
    // active tab, so activation is forced when nothing was active yet.
    if (activate || !active_.valid()) set_active(id);
    return true;
}

std::optional<TabId> TabManager::reopen_closed() {
    if (recently_closed_.empty()) return std::nullopt;
    TabState state = std::move(recently_closed_.back());
    recently_closed_.pop_back();
    // Give the reopened tab a fresh id so observers don't confuse it with the
    // old closed tab; the committed URL history is preserved.
    state.id = TabId::generate();
    state.crashed = false;
    state.crash_reason = TabCrashReason::None;
    state.suspended = false;
    state.suspended_at = {};
    state.pending_downloads = 0;
    state.audible = false;
    state.loading = false;
    state.last_activated = state.inactive_since = clock_();
    size_t idx = index_of(active_);
    if (idx > tabs_.size()) idx = tabs_.size();
    tabs_.insert(tabs_.begin() + off(idx), state);
    notify(state.id, TabChange::Added);
    set_active(state.id);
    return state.id;
}

bool TabManager::set_active(TabId id) {
    if (index_of(id) >= tabs_.size()) return false;
    if (active_ == id) return true;
    const auto now = clock_();
    if (auto* previous = find(active_)) previous->inactive_since = now;
    find(id)->last_activated = now;
    active_ = id;
    wake(id);
    notify(id, TabChange::ActiveChanged);
    return true;
}

void TabManager::wake(TabId id) {
    auto* tab = find(id);
    if (!tab || !tab->suspended) return;
    tab->suspended = false;
    tab->suspended_at = {};
    tab->inactive_since = clock_();
    notify(id, TabChange::SuspendedChanged);
}

bool TabManager::can_suspend(const TabState& tab) const {
    return tab.id != active_ && !tab.suspended && !tab.pinned &&
           !tab.exclude_from_suspension && !tab.audible &&
           !tab.pending_downloads && !tab.crashed && !tab.loading &&
           // CEF does not detect muted video playback; retain this existing
           // Twitch protection even for a manual suspension request.
           !cef::is_twitch_page(tab.url.display()) &&
           (tab.url.scheme() == "http" || tab.url.scheme() == "https");
}

bool TabManager::suspend(TabId id) {
    auto* tab = find(id);
    if (!tab || !can_suspend(*tab)) return false;
    tab->suspended = true;
    tab->suspended_at = clock_();
    notify(id, TabChange::SuspendedChanged);
    return true;
}

bool TabManager::restore(TabId id) {
    auto* tab = find(id);
    if (!tab || !tab->suspended) return false;
    wake(id);
    return true;
}

bool TabManager::set_suspend_excluded(TabId id, bool excluded) {
    auto* tab = find(id);
    if (!tab || tab->exclude_from_suspension == excluded) return false;
    tab->exclude_from_suspension = excluded;
    if (excluded) wake(id);
    notify(id, TabChange::SuspendedChanged);
    return true;
}

void TabManager::set_suspend_after(std::chrono::minutes threshold) {
    suspend_after_ = std::max(threshold, std::chrono::minutes::zero());
    if (suspend_after_.count() != 0) return;
    // Notifications may change the model; do not keep iterators across them.
    std::vector<TabId> suspended;
    for (const auto& tab : tabs_)
        if (tab.suspended) suspended.push_back(tab.id);
    for (const auto id : suspended) wake(id);
}

std::vector<TabId> TabManager::tick() {
    std::vector<TabId> candidates, suspended;
    if (suspend_after_.count() <= 0) return suspended;
    const auto now = clock_();
    const auto eligible = [this, now](const TabState& tab) {
        return can_suspend(tab) &&
               now >= tab.inactive_since &&
               std::chrono::duration_cast<std::chrono::minutes>(
                   now - tab.inactive_since) >= suspend_after_;
    };
    for (const auto& tab : tabs_)
        if (eligible(tab)) candidates.push_back(tab.id);
    for (const auto id : candidates) {
        // An earlier observer may have activated or removed a later candidate.
        auto* tab = find(id);
        if (!tab || !eligible(*tab)) continue;
        if (suspend(id)) suspended.push_back(id);
    }
    return suspended;
}

void TabManager::set_pending_downloads(TabId id, size_t count) {
    auto* tab = find(id);
    if (!tab) return;
    tab->pending_downloads = count;
    if (count) wake(id);
}

bool TabManager::move(TabId id, size_t new_index) {
    size_t from = index_of(id);
    if (from >= tabs_.size()) return false;
    new_index = std::min(new_index, tabs_.size() - 1);
    if (from == new_index) return true;
    auto state = std::move(tabs_[from]);
    tabs_.erase(tabs_.begin() + off(from));
    tabs_.insert(tabs_.begin() + off(new_index), std::move(state));
    notify(id, TabChange::Moved);
    return true;
}

bool TabManager::pin(TabId id, bool pinned) {
    auto* t = find(id);
    if (!t || t->pinned == pinned) return false;
    t->pinned = pinned;
    if (pinned) wake(id);
    notify(id, TabChange::PinnedChanged);
    return true;
}

bool TabManager::mute(TabId id, bool muted) {
    auto* t = find(id);
    if (!t || t->muted == muted) return false;
    t->muted = muted;
    notify(id, TabChange::MutedChanged);
    return true;
}

void TabManager::set_audible(TabId id, bool audible) {
    auto* t = find(id);
    if (!t) return;
    if (t->audible == audible) return;
    t->audible = audible;
    if (audible) wake(id);
    notify(id, TabChange::MutedChanged);
}

void TabManager::set_loading(TabId id, bool loading, int progress) {
    auto* t = find(id);
    if (!t) return;
    t->loading = loading;
    t->load_progress = progress;
    if (loading) wake(id);
    notify(id, TabChange::LoadingStateChanged);
    if (progress >= 0) notify(id, TabChange::ProgressChanged);
}

void TabManager::set_title(TabId id, std::string_view title) {
    auto* t = find(id);
    if (!t) return;
    t->title = std::string(title);
    if (auto* entry = t->current_index >= t->history.size()
                          ? nullptr : &t->history[t->current_index];
        entry && entry->url.raw() == t->url.raw()) {
        entry->title = t->title;
    }
    notify(id, TabChange::TitleChanged);
}

void TabManager::set_favicon(TabId id, std::string_view url) {
    auto* t = find(id);
    if (!t) return;
    t->favicon_url = std::string(url);
    if (!t->history.empty() && t->current_index < t->history.size() &&
        t->history[t->current_index].url.raw() == t->url.raw())
        t->history[t->current_index].favicon_url = t->favicon_url;
    notify(id, TabChange::TitleChanged);
}

void TabManager::set_url(TabId id, std::string_view url) {
    auto* t = find(id);
    if (!t) return;
    const core::Url next(url);
    // The favicon URL belongs to the page that reported it. Carrying it across
    // a navigation to another site is how a tab ended up showing (and the
    // favicon cache ended up PERSISTING, keyed by the new host) the previous
    // site's icon: the engine only reports a favicon for pages that have one,
    // so a page without one inherited its predecessor's. Cleared on a host
    // change; within one site the icon is genuinely shared.
    if (t->url.host() != next.host()) t->favicon_url.clear();
    t->url = next;
    // Views key a tab's tooltip and its favicon off the URL, so a silent URL
    // change would leave a stale icon on the tab.
    notify(id, TabChange::TitleChanged);
}

void TabManager::record_web_navigation(TabId id, std::string_view url) {
    auto* tab = find(id);
    if (!tab) return;
    const Url next(url);
    if (!next.is_valid()) return;
    // Moving within a restored model history must not append a duplicate.
    if (!tab->history.empty() &&
        tab->current_index < tab->history.size() &&
        tab->history[tab->current_index].url.raw() == next.raw()) return;
    if (tab->current_index > 0 &&
        tab->history[tab->current_index - 1].url.raw() == next.raw()) {
        --tab->current_index;
    } else if (tab->current_index + 1 < tab->history.size() &&
               tab->history[tab->current_index + 1].url.raw() == next.raw()) {
        ++tab->current_index;
    } else {
        if (tab->current_index + 1 < tab->history.size())
            tab->history.erase(tab->history.begin() + off(tab->current_index + 1),
                               tab->history.end());
        tab->history.push_back(NavigationEntry{next, tab->title, "", {}});
        tab->current_index = tab->history.size() - 1;
    }
}

void TabManager::set_nav_state(TabId id, bool can_go_back,
                               bool can_go_forward) {
    auto* t = find(id);
    if (!t) return;
    t->cef_can_go_back = can_go_back;
    t->cef_can_go_forward = can_go_forward;
    notify(id, TabChange::LoadingStateChanged);
}

void TabManager::mark_crashed(TabId id, TabCrashReason reason) {
    auto* t = find(id);
    if (!t) return;
    t->crashed = true;
    t->crash_reason = reason;
    t->loading = false;
    wake(id);
    notify(id, TabChange::Crashed);
}

void TabManager::mark_recovered(TabId id) {
    auto* t = find(id);
    if (!t) return;
    t->crashed = false;
    t->crash_reason = TabCrashReason::None;
    notify(id, TabChange::Recovered);
}

bool TabManager::go_back(TabId id) {
    auto* t = find(id);
    if (!t || !t->can_go_back()) return false;
    t->current_index--;
    notify(id, TabChange::LoadingStateChanged);
    return true;
}

bool TabManager::go_forward(TabId id) {
    auto* t = find(id);
    if (!t || !t->can_go_forward()) return false;
    t->current_index++;
    notify(id, TabChange::LoadingStateChanged);
    return true;
}

void TabManager::commit_navigation(TabId id, const NavigationEntry& entry) {
    auto* t = find(id);
    if (!t) return;
    // Truncate any forward history: navigating to a new URL discards the
    // "future" entries, exactly like every major browser. Guard the erase so
    // we never form an iterator past-the-end on an empty history (UB).
    if (t->current_index + 1 < t->history.size()) {
        t->history.erase(t->history.begin() + off(t->current_index + 1),
                         t->history.end());
    }
    t->history.push_back(entry);
    t->current_index = t->history.size() - 1;
    t->url = entry.url;
    t->title = entry.title;
    notify(id, TabChange::LoadingStateChanged);
    notify(id, TabChange::TitleChanged);
}

bool TabManager::contains(TabId id) const {
    return id.valid() && index_of(id) < tabs_.size();
}

const TabState* TabManager::find(TabId id) const {
    size_t idx = index_of(id);
    if (idx >= tabs_.size()) return nullptr;
    return &tabs_[idx];
}

TabState* TabManager::find(TabId id) {
    size_t idx = index_of(id);
    if (idx >= tabs_.size()) return nullptr;
    return &tabs_[idx];
}

std::vector<session::SavedTab> TabManager::snapshot() const {
    std::vector<session::SavedTab> out;
    out.reserve(tabs_.size());
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const auto& t = tabs_[i];
        session::SavedTab s;
        s.url = t.url.raw();
        s.title = t.title;
        s.pinned = t.pinned;
        s.window_index = 0;
        s.tab_index = static_cast<int>(i);
        out.push_back(s);
    }
    return out;
}

} // namespace aurora::core

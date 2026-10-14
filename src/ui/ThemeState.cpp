#include "ui/ThemeState.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

namespace aurora::ui {

namespace {
Theme g_default = dark_theme();
const Theme* g_current = nullptr;
std::unordered_map<ThemeObserverId, ThemeObserver> g_observers;
ThemeObserverId g_next_observer_id = 1;
}

const Theme& current_theme() {
    return g_current ? *g_current : g_default;
}

void set_current_theme(const Theme& t) {
    g_default = t;
    g_current = &g_default;
    // Copy before dispatch: a callback may destroy its widget and unregister
    // itself. Theme changes are UI-thread-only, so no lock is needed.
    std::vector<ThemeObserver> observers;
    observers.reserve(g_observers.size());
    for (const auto& [id, observer] : g_observers) {
        (void)id;
        observers.push_back(observer);
    }
    for (const auto& observer : observers) {
        if (observer) observer(g_default);
    }
}

ThemeObserverId add_theme_observer(ThemeObserver observer) {
    if (!observer) return 0;
    const ThemeObserverId id = g_next_observer_id++;
    g_observers.emplace(id, std::move(observer));
    return id;
}

void remove_theme_observer(ThemeObserverId id) {
    if (id != 0) g_observers.erase(id);
}

} // namespace aurora::ui

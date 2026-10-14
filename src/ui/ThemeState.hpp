// Aurora — global theme accessor for widgets that need the active palette.
//
// Kept separate from Theme.hpp/.cpp (which the design pass owns) so it never
// collides with token refreshes. MainWindow calls set_current_theme() inside
// apply_theme(); widgets read current_theme() to derive state tints.
#pragma once

#include <cstdint>
#include <functional>

#include "ui/Theme.hpp"

namespace aurora::ui {

const Theme& current_theme();
void set_current_theme(const Theme& t);

// Widgets that bridge into non-Qt surfaces (notably CEF) cannot learn about a
// stylesheet change through Qt's palette alone. They subscribe here and
// receive the initial/current semantic theme directly.
using ThemeObserver = std::function<void(const Theme&)>;
using ThemeObserverId = std::uint64_t;
ThemeObserverId add_theme_observer(ThemeObserver observer);
void remove_theme_observer(ThemeObserverId id);

} // namespace aurora::ui

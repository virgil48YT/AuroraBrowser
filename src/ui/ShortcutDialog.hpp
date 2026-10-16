// Aurora — "add / edit a site" dialog shared by the homepage shortcut tiles
// and the left dock's pinned sites.
#pragma once

#include <QString>

#include "ui/ShortcutList.hpp"

class QWidget;

namespace aurora::ui {

// Modal prompt for a site name (optional) and address. `initial` pre-fills the
// fields. Returns true and fills `out` with a normalized http(s) URL and a
// non-empty title when the user confirms a valid address.
bool prompt_shortcut(QWidget* parent, const QString& heading,
                     const Shortcut& initial, Shortcut* out);

} // namespace aurora::ui

// Aurora — stylesheet builder.
//
// Generates the complete Qt stylesheet from a Theme. The stylesheet is
// intentionally comprehensive: every widget in the chrome is styled here
// so the visual identity is consistent and no default OS widgets leak through.
// Both light and dark themes are hand-tuned, not color-inverted.
#pragma once

#include <QString>
#include "ui/Theme.hpp"

namespace aurora::ui {

class StylesheetBuilder {
public:
    static QString build(const Theme& theme);
};

} // namespace aurora::ui

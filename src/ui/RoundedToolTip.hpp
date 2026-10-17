// Aurora — rounded tool tips.
//
// Qt's own tool tip is a square top-level window and a style sheet cannot
// round its corners. This replaces it app-wide: an event filter answers every
// tool tip request with a translucent, rounded card in the theme's popover
// colors. Tab-strip tool tips (the address under a tab) go through it too.
#pragma once

class QApplication;

namespace aurora::ui {

// Installs the filter once for the whole application.
void install_rounded_tooltips(QApplication* app);

} // namespace aurora::ui

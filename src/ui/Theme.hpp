// Aurora — design tokens.
//
// One source of truth for color, spacing, radius, motion, and typography.
// Light and dark themes are both hand-authored from these semantic roles;
// nothing in the UI hardcodes a hex value and nothing in the stylesheet
// invents a color.
//
// Design intent (the quality bar is Arc / Vivaldi / Brave / Linear / Raycast):
//   * Warm-neutral surfaces, a single restrained teal accent.
//   * Depth comes from LAYERING (background < surface < surface_raised) plus
//     1px hairline borders — not from blur, glow, or heavy drop shadows.
//   * Every interactive element has a designed rest / hover / pressed /
//     focus / disabled / selected state.
//
// Surface hierarchy, outermost to innermost:
//   background      the window / content backdrop
//   surface         the chrome plate (tab strip + toolbar live here)
//   surface_alt     recessed fills inside the chrome (omnibox, find inputs)
//   surface_raised  things that float above everything (menus, tooltips)
#pragma once

#include <QColor>
#include <QString>

namespace aurora::ui {

struct ThemeColors {
    // --- Core surfaces -----------------------------------------------------
    QColor background;      // window + web-content backdrop
    QColor surface;         // chrome plate
    QColor surface_alt;     // recessed fill within the chrome
    QColor surface_raised;  // elevated: menus, popovers, tooltips
    QColor surface_hover;   // generic hover wash on a neutral surface
    QColor surface_selected;// selected row / active menu item

    // --- Lines -------------------------------------------------------------
    QColor border;          // primary hairline (chrome edges, inputs)
    QColor border_subtle;   // softer hairline (menus, internal edges)
    QColor divider;         // separators inside menus / groups

    // --- Text --------------------------------------------------------------
    QColor text;            // primary copy, active tab label
    QColor text_muted;      // secondary copy, inactive tabs, icons at rest
    QColor text_faint;      // placeholders, disabled, shortcut hints

    // --- Accent ------------------------------------------------------------
    QColor accent;          // the brand color: focus, active, primary
    QColor accent_hover;    // pressed / stronger accent
    QColor accent_soft;     // translucent accent wash for hovers (has alpha)

    // Aurora-glass additions. Default-constructed to a neutral purple / card
    // grey so any code path that skips dark_theme() still compiles and renders
    // something sane; the dark theme assigns the real nebula pair.
    QColor accent2          = QColor(QStringLiteral("#A855F7")); // secondary brand: neon purple glow
    QColor accent2_soft;    // translucent accent2 wash (has alpha)
    QColor accent3          = QColor(QStringLiteral("#D946EF")); // gradient endpoint: neon magenta
    QColor card_bg          = QColor(QStringLiteral("#0E1626")); // dark-blue content card
    QColor card_raised      = QColor(QStringLiteral("#111C30")); // one-step-lighter card (homepage tiles)

    // --- Toggle switch ------------------------------------------------------
    // Pill toggles (settings, shields): the track/knob pair for its on and
    // off states. Gradient ends for the ON track are accent / accent3 above.
    QColor toggle_track_off;
    QColor toggle_border_off;
    QColor toggle_knob_on;   // dark core dot on the ON toggle
    QColor toggle_knob_off;  // slate dot on the OFF toggle

    // --- Semantic ----------------------------------------------------------
    QColor danger;          // destructive
    QColor warning;         // caution
    QColor success;         // confirmation
    // `error` is retained as the legacy name for `danger` so existing widget
    // code (IconButton's Danger role, tab close hover) stays drop-in valid.
    QColor error;

    // --- Chrome-specific roles --------------------------------------------
    QColor tab_strip_bg;    // the strip behind the tabs
    QColor toolbar_bg;      // the toolbar row
    QColor tab_inactive_bg;
    QColor tab_active_bg;   // must equal toolbar_bg so the tab "connects"
    QColor tab_hover_bg;

    QColor omnibox_bg;
    QColor omnibox_border;
    QColor omnibox_focus_border;

    // --- Elevation ---------------------------------------------------------
    QColor shadow_color;    // carries alpha; used only for restrained shadows
};

struct Theme {
    QString name;
    bool is_dark = false;
    ThemeColors colors;

    // Typography is system-native (Inter on Linux/macOS, Segoe UI on Windows)
    // so the browser never blocks on a font download. Sizes are deliberately
    // compact: a browser chrome is dense, calm furniture, not a landing page.
    int body_font_px = 13;
    int title_font_px = 14;
    int small_font_px = 11;

    // Geometric sans stack (the design's Inter-like voice). Applied by
    // StylesheetBuilder as the app-wide font-family; the first family
    // present on the host OS wins, so there is nothing to bundle.
    QString font_family = QStringLiteral(
        "Inter, 'Segoe UI', 'Noto Sans', Ubuntu, Cantarell, 'Helvetica Neue', "
        "Arial, sans-serif");
};

// Pixel spacing scale (4px grid). Every layout gap is one of these.
struct Spacing {
    static constexpr int xxs = 2;
    static constexpr int xs = 4;
    static constexpr int sm = 8;
    static constexpr int md = 12;
    static constexpr int lg = 16;
    static constexpr int xl = 24;
};

// Corner radii. Deliberately NOT all pills: toolbar controls are small,
// the omnibox is a capsule, menus are moderate, dialogs are largest.
struct Radius {
    static constexpr int none = 0;
    static constexpr int sm = 4;   // small controls, close buttons, find inputs
    static constexpr int md = 6;   // toolbar buttons, menu items
    static constexpr int lg = 8;   // tab tops, menus
    static constexpr int xl = 12;  // dialogs
    static constexpr int xxl = 18; // window corner, floating content card
    // Dropdown panels that hang off the chrome (downloads popover). Matched
    // to `lg` on purpose: a panel that drops from a toolbar button reads as
    // part of the chrome, and the 12px dialog radius made the short downloads
    // panel look like a pill rather than a card.
    static constexpr int popover = lg;
    static constexpr int pill = 999; // omnibox, scrollbar handles
};

// Metrics for the chrome. Kept here so the stylesheet and the C++ layout
// code agree on the same numbers.
struct Metrics {
    static constexpr int nav_button = 32;      // back/forward/reload/home
    static constexpr int omnibox_height = 38;  // address bar (rounded field)
    static constexpr int tab_height = 36;
    static constexpr int tab_min_width = 76;   // favicon + 1-3 chars + close
    static constexpr int tab_max_width = 240;
    static constexpr int tab_pinned_width = 40; // icon-only compact tab
    static constexpr int close_button = 16;
    static constexpr int scrollbar = 10;
    static constexpr int hairline = 1;
};

// Motion durations (ms) + easing curves. Kept short so transitions never
// delay input. Qt stylesheets cannot animate, so these are consumed by
// QVariantAnimation / QPropertyAnimation in widget code.
struct Motion {
    static constexpr int fast = 90;
    static constexpr int base = 140;
    static constexpr int slow = 220;

    // CSS-style easing strings for any web surfaces (devtools panes,
    // internal pages) that should feel like the native chrome.
    static constexpr const char* ease_standard = "cubic-bezier(0.2, 0.0, 0.0, 1.0)";
    static constexpr const char* ease_out      = "cubic-bezier(0.16, 1.0, 0.3, 1.0)";
    static constexpr const char* ease_in_out   = "cubic-bezier(0.4, 0.0, 0.2, 1.0)";
};

Theme light_theme();
Theme dark_theme();

// Compose the existing dark-theme accent wash over a surface. Kept local to
// private widgets: publishing it as the global theme would tint normal windows.
QColor incognito_tint(const QColor& surface);

// Tor incognito windows (File > New Tor Incognito Window). A third theme
// variant next to light and dark: the dark theme re-keyed to pink/purple
// (magenta accent, violet secondary, plum surfaces) so a window whose
// traffic goes through Tor can never be mistaken for a normal or plain
// incognito one. Like incognito_tint it is applied per window through the
// [tor="true"] stylesheet selectors and tor_tint() in custom-painted
// widgets, never published as the global theme.
Theme tor_theme();
// Compose the Tor theme's plum wash over a surface (the Tor counterpart of
// incognito_tint).
QColor tor_tint(const QColor& surface);

} // namespace aurora::ui

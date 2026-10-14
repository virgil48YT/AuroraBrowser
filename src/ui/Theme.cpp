#include "ui/Theme.hpp"

namespace aurora::ui {

// Aurora palette — warm-neutral surfaces, one restrained teal accent.
//
// Both themes are hand-tuned; dark mode is NOT an inversion of light mode.
// Each has its own surface ladder, its own accent lightness, and its own
// hairline weights so that depth reads correctly at both ends.
//
// The chrome ladder is what makes the tab strip look like a real browser:
//
//   tab_strip_bg   (deepest)   the recessed strip the tabs sit in
//   tab_inactive_bg            barely lifted off the strip
//   tab_hover_bg               one step brighter — the "lift" on hover
//   tab_active_bg == toolbar_bg == surface   the active tab fuses with the
//                              toolbar plate below it, so the selected tab
//                              and the toolbar read as ONE continuous surface
//   background                 the content area behind the web view
//   surface_raised (highest)   menus and tooltips floating over everything
//
// accent_soft and shadow_color intentionally carry alpha: they are washes,
// not fills, so they compose over whatever surface is beneath them.

Theme light_theme() {
    Theme t;
    t.name = "light";
    t.is_dark = false;

    ThemeColors c;

    // Surfaces — warm off-whites. Four close shades, not one flat white:
    // the warmth (a few points of yellow/red) is what keeps it from feeling
    // like a clinical Qt utility dialog.
    c.background       = QColor("#FAF9F6"); // content backdrop
    c.surface          = QColor("#F5F3EE"); // chrome plate
    c.surface_alt      = QColor("#EDEAE3"); // recessed fill (omnibox at rest)
    c.surface_raised   = QColor("#FFFFFF"); // menus, tooltips
    c.surface_hover    = QColor("#E8E5DD");
    c.surface_selected = QColor("#E1ECEC"); // faint teal tint for selection

    // Lines — two weights. `border` defines shape, `border_subtle` only hints.
    c.border           = QColor("#D7D3CA");
    c.border_subtle    = QColor("#E5E1D9");
    c.divider          = QColor("#E5E1D9");

    // Text — near-black warm ink, never #000.
    c.text             = QColor("#1F1D18");
    c.text_muted       = QColor("#6E6B64");
    c.text_faint       = QColor("#A9A59D");

    // Accent — deep teal. 5.5:1 on the chrome plate, so it passes WCAG AA
    // for body-size text and is safe for focus rings and active indicators.
    c.accent           = QColor("#0A6E75");
    c.accent_hover     = QColor("#08585E");
    c.accent_soft      = QColor(10, 110, 117, 26);  // ~10% wash for hovers

    // Semantic
    c.danger           = QColor("#B3261E");
    c.warning          = QColor("#96581A");
    c.success          = QColor("#2F7A3D");
    c.error            = c.danger; // legacy alias

    // Chrome roles
    c.tab_strip_bg     = QColor("#E9E6DF");
    c.toolbar_bg       = c.surface;
    c.tab_inactive_bg  = QColor("#EDEAE3");
    c.tab_active_bg    = c.surface; // fuses with the toolbar below
    c.tab_hover_bg     = QColor("#F1EEE8");

    // Omnibox — recessed at rest, lifts to white-ish on focus.
    c.omnibox_bg           = QColor("#EDEAE3");
    c.omnibox_border       = QColor("#CFCAC0");
    c.omnibox_focus_border = c.accent;

    // Elevation — a soft warm-grey shadow, low alpha. Depth is carried by
    // layering and hairlines; the shadow is only a whisper under menus.
    c.shadow_color     = QColor(31, 29, 24, 38);

    t.colors = c;
    t.body_font_px = 13;
    t.title_font_px = 14;
    t.small_font_px = 11;
    return t;
}

Theme dark_theme() {
    Theme t;
    t.name = "dark";
    t.is_dark = true;

    ThemeColors c;

    // Surfaces — cool charcoal tuned against the reference dark browser:
    // a near-black tab strip (#1A1B1E), a calmer toolbar plate (#28292A),
    // and a recessed omnibox that sits one step BELOW the toolbar. Never
    // pure black: black kills the sense of elevation and hides hairlines.
    c.background       = QColor("#15161A"); // content backdrop
    c.surface          = QColor("#28292A"); // chrome plate (toolbar + active tab)
    c.surface_alt      = QColor("#1F2024"); // recessed fill
    c.surface_raised   = QColor("#313338"); // menus, tooltips, status bubble
    c.surface_hover    = QColor("#35373C");
    c.surface_selected = QColor("#26393C"); // teal-tinted charcoal

    // Lines — dark mode needs a slightly brighter border to survive on
    // charcoal, and a much softer subtle border to avoid a boxy look.
    c.border           = QColor("#3E4046");
    c.border_subtle    = QColor("#303237");
    c.divider          = QColor("#34363C");

    // Text — cool off-white, not #FFF, to cut glare in long sessions.
    c.text             = QColor("#ECEDEF");
    c.text_muted       = QColor("#9DA1A9");
    c.text_faint       = QColor("#686C74");

    // Accent — a brighter, slightly cyan-shifted teal so it pops off
    // charcoal (6.4:1 on the chrome plate).
    c.accent           = QColor("#4FB3BE");
    c.accent_hover     = QColor("#6ECBD4");
    c.accent_soft      = QColor(79, 179, 190, 36); // ~14% wash for hovers

    // Semantic — desaturated slightly so they don't vibrate on dark.
    c.danger           = QColor("#E4695C");
    c.warning          = QColor("#D59A4C");
    c.success          = QColor("#6FC06C");
    c.error            = c.danger; // legacy alias

    // Chrome roles. Inactive tabs are TRANSPARENT over the strip (the strip
    // shows through), so tab_inactive_bg is kept only as a documented
    // reference equal to the strip color.
    c.tab_strip_bg     = QColor("#1A1B1E");
    c.toolbar_bg       = c.surface;
    c.tab_inactive_bg  = c.tab_strip_bg; // inactives let the strip show through
    c.tab_active_bg    = c.surface; // fuses with the toolbar below
    c.tab_hover_bg     = QColor("#26272B"); // subtle lift over the strip

    c.omnibox_bg           = QColor("#1E1F23"); // darker than the toolbar
    c.omnibox_border       = QColor("#33353B");
    c.omnibox_focus_border = c.accent;

    // Elevation — near-black at higher alpha; on dark surfaces a shadow only
    // reads if it is darker than the darkest surface.
    c.shadow_color     = QColor(0, 0, 0, 120);

    t.colors = c;
    t.body_font_px = 13;
    t.title_font_px = 14;
    t.small_font_px = 11;
    return t;
}

QColor incognito_tint(const QColor& surface) {
    static const QColor wash = dark_theme().colors.accent_soft;
    const int alpha = wash.alpha();
    const auto blend = [alpha](int base, int accent) {
        return (base * (255 - alpha) + accent * alpha) / 255;
    };
    return QColor(blend(surface.red(), wash.red()),
                  blend(surface.green(), wash.green()),
                  blend(surface.blue(), wash.blue()), surface.alpha());
}

} // namespace aurora::ui

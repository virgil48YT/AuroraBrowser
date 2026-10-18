#include "ui/Theme.hpp"

namespace aurora::ui {

// Aurora palette — cool blue-grey neutrals with a cyan/violet/magenta accent
// family: neon on navy in dark mode, the same hues darkened for light mode.
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

    // Light is the daylight counterpart of the dark theme, not a separate
    // look: the same cyan -> violet -> magenta accent family, darkened until
    // it reads on white, over cool blue-grey neutrals that echo the dark
    // theme's navy. (An earlier warm-beige ladder fought the teal and violet
    // accents; every accent looked pasted onto the wrong background.)
    //
    // Surfaces — four close cool shades, white reserved for things that float.
    c.background       = QColor("#F2F5FA"); // content backdrop
    c.surface          = QColor("#F8FAFD"); // chrome plate (toolbar + active tab)
    c.surface_alt      = QColor("#E9EEF6"); // recessed fill (omnibox at rest)
    c.surface_raised   = QColor("#FFFFFF"); // menus, tooltips
    c.surface_hover    = QColor("#E3E9F3");
    c.surface_selected = QColor("#DCEAF7"); // faint blue tint for selection

    // Lines — two weights. `border` defines shape, `border_subtle` only hints.
    c.border           = QColor("#CBD5E3");
    c.border_subtle    = QColor("#DDE4EF");
    c.divider          = QColor("#DDE4EF");

    // Text — deep slate ink, never #000. muted is 6:1 on the chrome plate;
    // faint (placeholders, disabled) stays above 3:1 so it is still legible.
    c.text             = QColor("#0F172A");
    c.text_muted       = QColor("#51607A");
    c.text_faint       = QColor("#7A889F");

    // Accent — deep sky blue, the dark theme's electric cyan made readable on
    // light. 5.7:1 on the chrome plate, so it passes WCAG AA for body text and
    // is safe for focus rings and active indicators.
    c.accent           = QColor("#0369A1");
    c.accent_hover     = QColor("#075985");
    c.accent_soft      = QColor(3, 105, 161, 26);  // ~10% wash for hovers

    // Secondary pair (toggle tracks, radio rings, glow edges), the dark
    // theme's neon purple and magenta toned down for white backgrounds.
    c.accent2          = QColor("#6D43E0");         // violet
    c.accent2_soft     = QColor(109, 67, 224, 22);
    c.accent3          = QColor("#B02BBF");         // magenta
    c.card_bg          = QColor("#FFFFFF");
    c.card_raised      = QColor("#FFFFFF");
    c.toggle_track_off = QColor("#CBD5E3");
    c.toggle_border_off= QColor("#B6C2D4");
    c.toggle_knob_on   = QColor("#FFFFFF");
    c.toggle_knob_off  = QColor("#FFFFFF");

    // Semantic — each at least 4.5:1 on the chrome plate.
    c.danger           = QColor("#B3261E");
    c.warning          = QColor("#92540F");
    c.success          = QColor("#1A7233");
    c.error            = c.danger; // legacy alias

    // Chrome roles — the strip is the deepest step, the active tab matches the
    // toolbar so they read as one surface.
    c.tab_strip_bg     = QColor("#DFE5EF");
    c.toolbar_bg       = c.surface;
    c.tab_inactive_bg  = QColor("#E6EBF3");
    c.tab_active_bg    = c.surface; // fuses with the toolbar below
    c.tab_hover_bg     = QColor("#EDF1F8");

    // Omnibox — recessed at rest, lifts on focus.
    c.omnibox_bg           = QColor("#E9EEF6");
    c.omnibox_border       = QColor("#C3CEDF");
    c.omnibox_focus_border = c.accent;

    // Elevation — a soft slate shadow, low alpha. Depth is carried by
    // layering and hairlines; the shadow is only a whisper under menus.
    c.shadow_color     = QColor(15, 23, 42, 38);

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

    // Aurora neon: an ultra-dark navy window (#050811 → #0B1120), dark-blue
    // card containers (#0E1626 / #111C30), hairline slate borders (#1E293B),
    // and the electric-cyan (#00E5FF) / neon-purple (#A855F7 → #D946EF)
    // accent pair that glows over the navy. Never pure black: the glow
    // colors need a hue to shine against.
    c.background       = QColor("#050811"); // window / content backdrop
    c.surface          = QColor("#0B1220"); // chrome plate (toolbar + active tab)
    c.surface_alt      = QColor("#0A1120"); // recessed fill (inputs at rest)
    c.surface_raised   = QColor("#101A2E"); // menus, tooltips, status bubble
    c.surface_hover    = QColor("#16233B");
    c.surface_selected = QColor("#15264A"); // navy-tinted selection wash
    c.card_bg          = QColor("#0E1626"); // dark-blue content card
    c.card_raised      = QColor("#111C30"); // one step lighter (home tiles)

    // Lines — the slate hairline ladder the mockup's cards are drawn with.
    c.border           = QColor("#1E293B");
    c.border_subtle    = QColor("#1A2438");
    c.divider          = QColor("#1E293B");

    // Text — cool slate ladder, muted #94A3B8, never harsh #FFF at rest.
    c.text             = QColor("#F1F5F9");
    c.text_muted       = QColor("#94A3B8");
    c.text_faint       = QColor("#64748B");

    // Accent pair — electric cyan and the neon purple → magenta nebula.
    c.accent           = QColor("#00E5FF");
    c.accent_hover     = QColor("#67F0FF");
    c.accent_soft      = QColor(0, 229, 255, 34);  // ~13% wash for hovers
    c.accent2          = QColor("#A855F7");
    c.accent2_soft     = QColor(168, 85, 247, 36);
    c.accent3          = QColor("#D946EF");

    // Toggle switch: on = cyan→magenta gradient track (StylesheetBuilder)
    // with a navy core dot; off = slate track with a slate dot.
    c.toggle_track_off = QColor("#1E293B");
    c.toggle_border_off= QColor("#33415C");
    c.toggle_knob_on   = QColor("#0B1120");
    c.toggle_knob_off  = QColor("#94A3B8");

    // Semantic — desaturated slightly so they don't vibrate on navy.
    c.danger           = QColor("#F87171");
    c.warning          = QColor("#E0A253");
    c.success          = QColor("#4ADE80");
    c.error            = c.danger; // legacy alias

    // Chrome roles. The tab strip is the darkest layer of the window glass;
    // inactive tabs are TRANSPARENT over it (the strip shows through), so
    // tab_inactive_bg is kept only as a documented reference equal to the
    // strip color.
    c.tab_strip_bg     = QColor("#04060E");
    c.toolbar_bg       = c.surface;
    c.tab_inactive_bg  = c.tab_strip_bg; // inactives let the strip show through
    c.tab_active_bg    = c.surface; // fuses with the toolbar below
    c.tab_hover_bg     = QColor("#131D33"); // subtle lift over the strip

    c.omnibox_bg           = QColor("#070D1A"); // darker than the toolbar
    c.omnibox_border       = QColor("#1E293B");
    c.omnibox_focus_border = c.accent;

    // Elevation — near-black at higher alpha; on navy surfaces a shadow only
    // reads if it is darker than the darkest surface.
    c.shadow_color     = QColor(0, 0, 0, 140);

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

#include "ui/StylesheetBuilder.hpp"

#include <QColor>
#include <QString>

namespace aurora::ui {

// ---------------------------------------------------------------------------
// Color helpers. Every hex in this file comes from a theme token — there are
// no literal colors below, only tokens and alpha washes derived from them.
// ---------------------------------------------------------------------------

// Explicit-alpha form: rgba(token, 0..255).
static QString rgba(const QColor& c, int alpha = 255) {
    return QString("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(alpha / 255.0, 0, 'f', 2);
}

// Uses the token's own alpha channel (accent_soft and shadow_color carry it).
static QString rgba_of(const QColor& c) {
    return rgba(c, c.alpha());
}

// Opaque hex for a token.
static QString hex(const QColor& c) {
    return c.name();
}

// Reusable gradient snippets returning the accent pair as QSS. Kept as
// lambdas inside build() scope would be cleaner, but the functions below
// already exist as statics, so the two tiny helpers follow that layout.
static QString qss_gradient(const QColor& a, const QColor& b) {
    return QString("qlineargradient(x1:0, y1:1, x2:1, y2:0, stop:0 %1, stop:1 %2)")
        .arg(hex(a), hex(b));
}

// Height of the in-progress bar in an aurora://downloads Status cell. Mirrors
// kProgressHeight in DownloadsPage.cpp: the widget sets the fixed height, and
// this re-states it in the sheet because the generic QProgressBar rule below
// caps every bar at a 3px hairline.
static constexpr int kDownloadProgressHeight = 18;

QString StylesheetBuilder::build(const Theme& t) {
    const auto& c = t.colors;
    QString s;

    // A single accent wash reused for every neutral hover so the whole chrome
    // responds with one consistent gesture.
    const QString accent_wash        = rgba_of(c.accent_soft);
    const QString accent_wash_strong = rgba(c.accent, c.accent_soft.alpha() * 2);
    const QString accent2_wash       = rgba_of(c.accent2_soft);
    const QString focus_ring         = rgba(c.accent, 90);
    // Aurora glass edge: hairlines that separate chrome from content carry a
    // faint purple glow, so the boundaries read as light, not as boxes.
    const QString glow_edge          = rgba(c.accent2, 60);
    const QString glow_edge_soft     = rgba(c.accent2, 34);

    // =======================================================================
    // Global
    // =======================================================================
    s += QString("* {"
                 "  font-family: %1;"
                 "  font-size: %2px;"
                 "  outline: none;"
                 "}")
            .arg(t.font_family)
            .arg(t.body_font_px);

    // The whole window is one piece of dark aurora glass with the big rounded
    // top (and bottom) corners from the mockup. Qt clips a stylesheet-painted
    // background to border-radius, and MainWindow is frameless, so the
    // radius is what shapes the window silhouette.
    s += QString("QMainWindow, QWidget#Root {"
                 "  background: %1;"
                 "  color: %2;"
                 "  border-radius: %3px;"
                 "}")
            .arg(hex(c.background), hex(c.text))
            .arg(Radius::xxl);
    // The vertical column beside the dock (chrome + content) rides the same
    // window glass; it must stay transparent or it would box out the rail.
    s += QString("QWidget#MainColumn { background: transparent; border: none; }");

    // The content stack is full-bleed under the toolbar. It used to be a
    // rounded, inset card, but the web page inside is a native window that
    // cannot be clipped to rounded corners, so the card's roundness only
    // showed as odd dark margins around pages and the wallpaper.
    s += QString("QWidget#ContentStack {"
                 "  background: %1;"
                 "  border: none;"
                 "  border-radius: 0px;"
                 "  margin: 0px;"
                 "}")
            .arg(hex(c.card_bg));

    // =======================================================================
    // Chrome surface
    //
    // Aurora glass: the chrome is part of the window glass, not a separate
    // plate, so it stays transparent over Root and keeps only the big rounded
    // top corners. The glow that separates chrome from the content card now
    // comes from the card's own border.
    s += QString("QWidget#ChromeTop {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-top-left-radius: %1px;"
                 "  border-top-right-radius: %1px;"
                 "}")
            .arg(Radius::xxl);

    // Strip and toolbar ride the same transparent glass as ChromeTop; tabs
    // float on it as dark pills (painted by TabStripStyle in C++).
    // Padding is owned by the row's QHBoxLayout; the sheet owns only the
    // fill, so the two can never double up.
    s += QString("QWidget#TabStrip {"
                 "  background: transparent;"
                 "  border: none;"
                 "  padding: 0px;"
                 "  border-top-left-radius: %1px;"
                 "  border-top-right-radius: %1px;"
                 "}")
            .arg(Radius::xxl);

    s += QString("QWidget#ToolbarRow {"
                 "  background: transparent;"
                 "  border: none;"
                 "  padding: 0px;"
                 "}");

    // Per-window selectors keep the accent wash out of normal windows, even
    // though custom-painted widgets share the current global theme.
    s += QString("QWidget#ChromeTop[incognito=\"true\"],"
                 "QWidget#ToolbarRow[incognito=\"true\"] { background: %1; }"
                 "QWidget#TabStrip[incognito=\"true\"] { background: %2; }")
            .arg(hex(incognito_tint(c.surface)),
                 hex(incognito_tint(c.tab_strip_bg)));
    s += QString("QLabel#IncognitoBadge {"
                 "  background: %1;"
                 "  color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "  font-size: %6px;"
                 "  font-weight: 600;"
                 "  padding: %7px %8px;"
                 "}")
            .arg(hex(incognito_tint(c.surface)), hex(c.accent))
            .arg(Metrics::hairline)
            .arg(rgba_of(dark_theme().colors.accent_soft))
            .arg(Radius::sm)
            .arg(t.small_font_px)
            .arg(Spacing::xs)
            .arg(Spacing::sm);

    // The bookmarks bar is the last band of the chrome glass: same
    // transparent fill as the toolbar so the chrome stays one surface.
    s += QString("QWidget#BookmarkBar {"
                 "  background: transparent;"
                 "  border: none;"
                 "  padding: 0px;"
                 "}");

    // Bar items are flat and quiet until hovered — they are chrome, not
    // buttons competing with the omnibox.
    s += QString("QToolButton#BookmarkBarItem,"
                 "QToolButton#BookmarkBarOverflow {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-radius: %1px;"
                 "  color: %2;"
                 "  font-size: %3px;"
                 "  padding: 1px %4px;"
                 "}"
                 "QToolButton#BookmarkBarItem:hover,"
                 "QToolButton#BookmarkBarOverflow:hover {"
                 "  background: %5;"
                 "  color: %6;"
                 "}"
                 "QToolButton#BookmarkBarItem:pressed,"
                 "QToolButton#BookmarkBarOverflow:pressed {"
                 "  background: %7;"
                 "}")
            .arg(Radius::sm)
            .arg(hex(c.text_muted))
            .arg(t.small_font_px)
            .arg(Spacing::xs + Spacing::xxs)
            .arg(hex(c.surface_hover))
            .arg(hex(c.text))
            .arg(hex(c.surface_selected));

    // Placeholder shown while the bar folder is empty.
    s += QString("QLabel#BookmarkBarHint {"
                 "  background: transparent;"
                 "  color: %1;"
                 "  font-size: %2px;"
                 "  padding-left: %3px;"
                 "}")
            .arg(hex(c.text_faint))
            .arg(t.small_font_px)
            .arg(Spacing::xxs);

    // QToolBar may still be used for the toolbar row; keep it borderless and
    // visually identical to the custom chrome widget.
    s += QString("QToolBar {"
                 "  background: %1;"
                 "  border: none;"
                 "  padding: %2px %3px;"
                 "  spacing: %4px;"
                 "}")
            .arg(hex(c.toolbar_bg))
            .arg(Spacing::xs)
            .arg(Spacing::sm)
            .arg(Spacing::xs);
    s += QString("QToolBar::separator {"
                 "  background: %1; width: %2px; margin: %3px %4px;"
                 "}")
            .arg(hex(c.divider))
            .arg(Metrics::hairline)
            .arg(Spacing::sm)
            .arg(Spacing::xs);
    s += QString("QToolBar::handle { image: none; width: 0px; }");

    // =======================================================================
    // Tab strip
    //
    // The TABS THEMSELVES carry no QSS at all: every tab pixel (transparent
    // inactive tabs, hairline dividers, the folder-tab active shape with its
    // outward bottom fillets, per-tab widths) is painted by TabStripStyle —
    // the QProxyStyle the TabBar installs on itself in C++. A custom style
    // detaches that widget from the sheet anyway, so tab rules here would be
    // dead code. What remains are the strip's children (scroll buttons) and
    // the per-tab button widgets, which still use the sheet, plus the QSS
    // hooks QTabBar consults even under a custom base style.
    // =======================================================================
    s += QString("QTabBar {"
                 "  background: transparent;"
                 "  border: none;"
                 "}");
    // No QTabBar::tab rules on purpose. Any rule with a box (padding, margin,
    // radius) makes the style sheet paint the tab itself and bypass
    // TabStripStyle, which is what produced flat, square tabs. TabStripStyle
    // draws the rounded pill tabs.
    s += QString("QTabWidget::pane { border: none; background: %1; }")
            .arg(hex(c.background));
    s += QString("QTabWidget::tab-bar { alignment: left; }");

    // Overflow scroll buttons blend into the strip.
    s += QString("QTabBar QToolButton {"
                 "  background: %1;"
                 "  border: none;"
                 "  border-radius: %2px;"
                 "  color: %3;"
                 "  min-width: 20px; min-height: 20px;"
                 "  margin: %4px;"
                 "  padding: 0px;"
                 "}")
            .arg(hex(c.tab_strip_bg))
            .arg(Radius::sm)
            .arg(hex(c.text_muted))
            .arg(Spacing::xs);
    s += QString("QTabBar QToolButton:hover { background: %1; color: %2; }")
            .arg(accent_wash, hex(c.text));
    s += QString("QTabBar QToolButton:disabled { color: %1; }").arg(hex(c.text_faint));

    // Per-tab close button: chromeless at rest (no background box — the glyph
    // alone reads as the close affordance) with a danger wash only on hover.
    s += QString("QToolButton#TabCloseButton {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-radius: 10px;"
                 "  margin: 0px;"
                 "  padding: 0px;"
                 "  min-width: 20px; max-width: 20px;"
                 "  min-height: 20px; max-height: 20px;"
                 "}");
    s += QString("QToolButton#TabCloseButton:hover { background: %1; }")
            .arg(rgba(c.danger, 40));
    s += QString("QToolButton#TabCloseButton:pressed { background: %1; }")
            .arg(rgba(c.danger, 72));

    // New-tab action lives at the end of the strip; the tab-search chevron
    // opens the strip. Both: neutral at rest, quiet wash on hover. Sizes are
    // owned by the C++ ctor (setFixedSize), the sheet owns the look.
    s += QString("QToolButton#NewTabButton, QToolButton#TabSearchButton {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-radius: %1px;"
                 "  color: %2;"
                 "  margin-left: %3px;"
                 "  padding: 0px;"
                 "}")
            .arg(Radius::md)
            .arg(hex(c.text_muted))
            .arg(Spacing::xxs);
    s += QString("QToolButton#NewTabButton:hover, QToolButton#TabSearchButton:hover {"
                 "  background: %1; color: %2; }")
            .arg(accent_wash, hex(c.text));
    s += QString("QToolButton#NewTabButton:pressed, QToolButton#TabSearchButton:pressed {"
                 "  background: %1; color: %2; }")
            .arg(accent_wash_strong, hex(c.accent_hover));
    s += QString("QToolButton#NewTabButton:focus, QToolButton#TabSearchButton:focus {"
                 "  background: %1; }").arg(accent_wash);

    // Custom window controls (frameless): large rounded buttons that fill the
    // full strip height at the top-right corner. The close button's hover fill
    // runs flush into the corner so the extreme top-right pixel is Close.
    s += QString("QToolButton#WindowButton, QToolButton#WindowCloseButton {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-radius: %1px;"
                 "  color: %2;"
                 "  margin: 4px 2px;"
                 "  padding: 0px;"
                 "}")
            .arg(Radius::xl)
            .arg(hex(c.text_muted));
    s += QString("QToolButton#WindowCloseButton {"
                 "  margin: 0px;"
                 "  border-radius: 0px;"
                 "  border-top-right-radius: %1px;"
                 "  border-bottom-left-radius: %1px;"
                 "}").arg(Radius::xl);
    s += QString("QToolButton#WindowButton:hover { background: %1; color: %2; }")
            .arg(rgba(c.text, 22), hex(c.text));
    s += QString("QToolButton#WindowButton:pressed { background: %1; }")
            .arg(rgba(c.text, 40));
    s += QString("QToolButton#WindowCloseButton:hover { background: %1; color: %2; }")
            .arg(rgba(c.danger, 230), hex(c.text));
    s += QString("QToolButton#WindowCloseButton:pressed { background: %1; }")
            .arg(hex(c.danger));

    // =======================================================================
    // Toolbar controls
    //
    // One shared recipe: transparent at rest, accent wash on hover, deeper
    // wash when pressed, faint when disabled. No borders, ever — borders on
    // icon buttons are what makes Qt apps look like 2003.
    // =======================================================================
    const QString icon_button_base =
        QString("  background: transparent;"
                "  border: none;"
                "  border-radius: %1px;"
                "  color: %2;"
                "  padding: 0px;"
                "  min-width: %3px; min-height: %3px;"
                "  max-width: %3px; max-height: %3px;")
            .arg(Radius::md)
            .arg(hex(c.text_muted))
            .arg(Metrics::nav_button);

    s += QString("QToolButton#NavButton, QToolButton#BrowserMenuButton,"
                 "QToolButton#ActionButton, QToolBar QToolButton { %1 }")
            .arg(icon_button_base);

    s += QString("QToolButton#NavButton:hover, QToolButton#BrowserMenuButton:hover,"
                 "QToolButton#ActionButton:hover, QToolBar QToolButton:hover {"
                 "  background: %1; color: %2;"
                 "}")
            .arg(accent_wash, hex(c.text));

    s += QString("QToolButton#NavButton:pressed, QToolButton#BrowserMenuButton:pressed,"
                 "QToolButton#ActionButton:pressed, QToolBar QToolButton:pressed {"
                 "  background: %1; color: %2;"
                 "}")
            .arg(accent_wash_strong, hex(c.accent));

    s += QString("QToolButton#NavButton:checked, QToolButton#BrowserMenuButton:checked,"
                 "QToolButton#ActionButton:checked {"
                 "  background: %1; color: %2;"
                 "}")
            .arg(hex(c.surface_selected), hex(c.accent));

    s += QString("QToolButton#NavButton:disabled, QToolButton#BrowserMenuButton:disabled,"
                 "QToolButton#ActionButton:disabled, QToolBar QToolButton:disabled {"
                 "  background: transparent; color: %1;"
                 "}")
            .arg(hex(c.text_faint));

    // Keyboard focus is visible without changing size: the wash plus a ring.
    s += QString("QToolButton#NavButton:focus, QToolButton#BrowserMenuButton:focus,"
                 "QToolButton#ActionButton:focus {"
                 "  background: %1; border: %2px solid %3;"
                 "}")
            .arg(accent_wash)
            .arg(Metrics::hairline)
            .arg(focus_ring);

    // The extensions button is an IconButton that paints the puzzle glyph;
    // it shares the chrome-button recipe by object name.
    s += QString("QToolButton#ExtensionsButton {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-radius: %1px;"
                 "  color: %2;"
                 "  padding: 0px;"
                 "}")
            .arg(Radius::md)
            .arg(hex(c.text_muted));
    s += QString("QToolButton#ExtensionsButton:hover { background: %1; color: %2; }")
            .arg(accent_wash, hex(c.text));
    s += QString("QToolButton#ExtensionsButton:pressed { background: %1; color: %2; }")
            .arg(accent_wash_strong, hex(c.accent));
    s += QString("QToolButton#ExtensionsButton:disabled {"
                 "  background: transparent; color: %1; }")
            .arg(hex(c.text_faint));
    s += QString("QToolButton#ExtensionsButton::menu-indicator {"
                 "  image: none; width: 0px; }");

    // Menu-bearing buttons: suppress the Qt arrow, the icon says enough.
    s += QString("QToolButton#BrowserMenuButton::menu-indicator,"
                 "QToolButton#NavButton::menu-indicator,"
                 "QToolButton#ActionButton::menu-indicator { image: none; width: 0px; }");

    // Omnibox — the single most-used control in the product.
    //
    // The Omnibox is a QWidget container that paints its own true capsule,
    // animated focus ring, and leading security/search glyph (see Omnibox.cpp).
    // The inner line edit is therefore chromeless: transparent fill, no border,
    // generous leading padding so the URL text never collides with the glyph,
    // and accent-tinted selection. Generic line edits elsewhere stay recessed.
    // The Omnibox layout already reserves the leading indicator gutter
    // (34px), so the edit needs only a small text inset — a big padding here
    // would double-reserve and shove the URL text far from the glyph.
    s += QString("QLineEdit#OmniboxEdit {"
                 "  background: transparent;"
                 "  border: none;"
                 "  color: %1;"
                 "  padding: 0px %2px 0px %3px;"
                 "  font-size: %4px;"
                 "  selection-background-color: %5;"
                 "  selection-color: %1;"
                 "}")
            .arg(hex(c.text))
            .arg(Spacing::sm)
            .arg(Spacing::xs)
            .arg(t.title_font_px)
            .arg(rgba(c.accent, 72));

    s += QString("QLineEdit#OmniboxEdit:hover { color: %1; }").arg(hex(c.text));
    s += QString("QLineEdit#OmniboxEdit:focus { color: %1; }").arg(hex(c.text));
    s += QString("QLineEdit#OmniboxEdit:disabled { color: %1; }").arg(hex(c.text_faint));

    // Omnibox suggestion popup (QCompleter): reads as an extension of the
    // omnibox, not as a foreign list widget.
    s += QString("QListView#OmniboxPopup, QLineEdit#OmniboxEdit QAbstractItemView {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "  padding: %6px;"
                 "  outline: none;"
                 "}")
            .arg(hex(c.surface_raised), hex(c.text))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(Radius::lg)
            .arg(Spacing::xs);
    s += QString("QListView#OmniboxPopup::item, QLineEdit#OmniboxEdit QAbstractItemView::item {"
                 "  padding: %1px %2px; border-radius: %3px; min-height: 22px;"
                 "}")
            .arg(Spacing::xs)
            .arg(Spacing::sm)
            .arg(Radius::sm);
    s += QString("QListView#OmniboxPopup::item:selected,"
                 "QLineEdit#OmniboxEdit QAbstractItemView::item:selected {"
                 "  background: %1; color: %2;"
                 "}")
            .arg(hex(c.surface_selected), hex(c.accent));

    // =======================================================================
    // Find bar
    // =======================================================================
    s += QString("QWidget#FindBarContainer, QWidget#FindBar {"
                 "  background: %1;"
                 "  border: none;"
                 "  border-top: %2px solid %3;"
                 "  padding: %4px %5px;"
                 "}")
            .arg(hex(c.surface))
            .arg(Metrics::hairline)
            .arg(hex(c.border))
            .arg(Spacing::sm)
            .arg(Spacing::md);

    s += QString("QWidget#FindBarContainer QLineEdit, QWidget#FindBar QLineEdit {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "  padding: %6px %7px;"
                 "  min-height: 24px; min-width: 200px;"
                 "}")
            .arg(hex(c.surface_alt), hex(c.text))
            .arg(Metrics::hairline)
            .arg(hex(c.border))
            .arg(Radius::md)
            .arg(Spacing::xs)
            .arg(Spacing::sm);
    s += QString("QWidget#FindBarContainer QLineEdit:focus, QWidget#FindBar QLineEdit:focus {"
                 "  border-color: %1; background: %2;"
                 "}")
            .arg(hex(c.accent), hex(c.surface_raised));

    s += QString("QWidget#FindBarContainer QLabel, QWidget#FindBar QLabel {"
                 "  color: %1; font-size: %2px; padding: 0px %3px;"
                 "}")
            .arg(hex(c.text_muted))
            .arg(t.small_font_px)
            .arg(Spacing::xs);

    s += QString("QWidget#FindBarContainer QToolButton, QWidget#FindBar QToolButton {"
                 "  background: transparent; border: none;"
                 "  border-radius: %1px; color: %2;"
                 "  min-width: 26px; min-height: 26px;"
                 "  max-width: 26px; max-height: 26px; padding: 0px;"
                 "}")
            .arg(Radius::sm)
            .arg(hex(c.text_muted));
    s += QString("QWidget#FindBarContainer QToolButton:hover, QWidget#FindBar QToolButton:hover {"
                 "  background: %1; color: %2;"
                 "}")
            .arg(accent_wash, hex(c.text));
    s += QString("QWidget#FindBarContainer QToolButton:pressed, QWidget#FindBar QToolButton:pressed {"
                 "  background: %1; color: %2;"
                 "}")
            .arg(accent_wash_strong, hex(c.accent));
    s += QString("QWidget#FindBarContainer QToolButton:disabled, QWidget#FindBar QToolButton:disabled {"
                 "  color: %1;"
                 "}")
            .arg(hex(c.text_faint));

    // =======================================================================
    // Status bubble — transient hover-link / load-progress pill
    //
    // Replaces the old QStatusBar band (which reserved a permanent strip of
    // layout height). The bubble floats over the page corner and exists only
    // while it has text.
    // =======================================================================
    s += QString("QLabel#StatusBubble {"
                 // `background-color` is intentional rather than the broad
                 // background shorthand: the label needs one opaque styled
                 // fill clipped by this rule's radius, with no inherited
                 // brush/image capable of painting a square backing.
                 "  background-color: %1;"
                 "  color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "  padding: %6px %7px;"
                 "  margin: 0px;"
                 "  font-size: %8px;"
                 "}")
            .arg(hex(c.surface_raised), hex(c.text_muted))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(Radius::lg)
            .arg(Spacing::xxs + Spacing::xs)
            .arg(Spacing::sm + Spacing::xxs)
            .arg(t.small_font_px);

    // =======================================================================
    // Downloads popover — the recent-downloads dropdown under the tray button
    //
    // One elevated rounded panel (like the menus, one step up the surface
    // hierarchy) with a header, a scrollable row list and a footer action.
    // The panel is a plain QWidget, so the look is QSS + WA_StyledBackground;
    // the drop shadow behind it is painted by DownloadsPopover::paintEvent.
    // =======================================================================
    // Corner radius of the dropdown panel. Deliberately one step BELOW the
    // menu radius: the popover is a short, wide panel, and anything larger
    // rounds its 4 corners into each other so the whole thing reads as a pill
    // instead of a crisp desktop dropdown. Kept local to the downloads-UI
    // rules (DownloadsPopover.cpp paints its shadow with the same value).
    constexpr int kPopoverRadius = Radius::md;  // 6px
    // The popup widget itself must be fully transparent: it is wider/taller
    // than the panel by the painted-shadow margins, and any background here
    // would show as a sharp rectangle peeking out around the rounded panel.
    s += QString("QWidget#DownloadsPopover { background: transparent; border: none; }");
    s += QString("QWidget#DownloadsPopoverPanel {"
                 "  background: %1;"
                 "  border: %2px solid %3;"
                 "  border-radius: %4px;"
                 "}")
            .arg(hex(c.surface_raised))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(kPopoverRadius);
    // Header band: a hairline under it separates title from list the same way
    // the footer hairline closes the panel, so the dropdown reads as three
    // crisp bands rather than one soft blob.
    s += QString("QWidget#DownloadsPopoverHeader {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-bottom: %1px solid %2;"
                 "}")
            .arg(Metrics::hairline)
            .arg(hex(c.divider));
    s += QString("QLabel#DownloadsPopoverTitle {"
                 "  color: %1; font-size: %2px; font-weight: 600;"
                 "  letter-spacing: -0.1px;"
                 "}")
            .arg(hex(c.text))
            .arg(t.title_font_px);
    s += QString("QToolButton#PopoverCloseButton {"
                 "  background: transparent; border: none;"
                 "  border-radius: 11px; color: %1;"
                 "  margin: 0px; padding: 0px;"
                 "  min-width: 22px; max-width: 22px;"
                 "  min-height: 22px; max-height: 22px;"
                 "}")
            .arg(hex(c.text_muted));
    s += QString("QToolButton#PopoverCloseButton:hover { background: %1; color: %2; }")
            .arg(accent_wash, hex(c.text));
    s += QString("QToolButton#PopoverCloseButton:pressed { background: %1; color: %2; }")
            .arg(accent_wash_strong, hex(c.accent_hover));
    s += QString("QScrollArea#DownloadsPopoverScroll {"
                 "  background: transparent; border: none;"
                 "}");
    s += QString("QWidget#DownloadsPopoverList { background: transparent; }");
    // Rows: quiet at rest, the same single accent wash on hover as the menus.
    s += QString("QWidget#DownloadRow {"
                 "  background: transparent; border: none; border-radius: %1px;"
                 "}")
            .arg(Radius::sm - 1);
    s += QString("QWidget#DownloadRow:hover { background: %1; }").arg(accent_wash);
    s += QString("QLabel#DownloadRowName { color: %1; font-size: %2px; }")
            .arg(hex(c.text))
            .arg(t.body_font_px);
    s += QString("QLabel#DownloadRowSub { color: %1; font-size: %2px; }")
            .arg(hex(c.text_muted))
            .arg(t.small_font_px + 1);
    // The in-progress hairline inside a popover row. The generic QProgressBar
    // rule below already caps the height and hides the text; this one only
    // adds the vertical breathing room the row needs and a track quiet enough
    // that a finished row's disappearance is not a visual jump.
    s += QString("QProgressBar#DownloadRowProgress {"
                 "  background: %1; border: none; border-radius: %2px;"
                 "  margin: %3px 0px;"
                 "}"
                 "QProgressBar#DownloadRowProgress::chunk {"
                 "  background: %4; border-radius: %5px;"
                 "}")
            .arg(hex(c.surface_alt))
            .arg(Radius::sm)
            .arg(Spacing::xxs)
            .arg(hex(c.accent))
            .arg(Radius::sm);
    s += QString("QLabel#DownloadsPopoverEmpty {"
                 "  color: %1; font-size: %2px; padding: %3px %4px;"
                 "}")
            .arg(hex(c.text_muted))
            .arg(t.body_font_px)
            .arg(Spacing::xl)
            .arg(Spacing::lg);
    // Footer: full-width action row with a top hairline. The hover wash keeps
    // the panel's rounded bottom corners instead of squaring them off.
    s += QString("QPushButton#DownloadsPopoverFooter {"
                 "  background: transparent; color: %1;"
                 "  border: none;"
                 "  border-top: %2px solid %3;"
                 "  border-radius: 0px;"
                 "  text-align: left;"
                 "  padding: %4px %5px;"
                 "  font-size: %6px;"
                 "  font-weight: 500;"
                 "  min-height: 16px;"
                 "}")
            .arg(hex(c.text))
            .arg(Metrics::hairline)
            .arg(hex(c.divider))
            .arg(Spacing::sm)
            .arg(Spacing::md)
            .arg(t.body_font_px);
    s += QString("QPushButton#DownloadsPopoverFooter:hover {"
                 "  background: %1;"
                 "  border-bottom-left-radius: %2px;"
                 "  border-bottom-right-radius: %2px;"
                 "}")
            .arg(accent_wash)
            .arg(kPopoverRadius - Metrics::hairline);
    s += QString("QPushButton#DownloadsPopoverFooter:pressed { background: %1; }")
            .arg(accent_wash_strong);
    // Per-row transfer controls (pause / resume / cancel / retry) in the
    // popover and on aurora://downloads: same quiet rest state as the old
    // cancel button, with the accent wash on hover. One rule covers all six
    // object names so the controls cannot drift apart between surfaces.
    s += QString("QToolButton#DownloadRowPause, QToolButton#DownloadRowResume,"
                 "QToolButton#DownloadRowCancel, QToolButton#DownloadRowRetry,"
                 "QToolButton#DownloadPagePause, QToolButton#DownloadPageResume,"
                 "QToolButton#DownloadPageCancel, QToolButton#DownloadPageRetry {"
                 "  background: transparent; border: none; border-radius: %1px;"
                 "  padding: 0px; margin: 0px;"
                 "}")
            .arg(Radius::sm);
    s += QString("QToolButton#DownloadRowPause:hover, QToolButton#DownloadRowResume:hover,"
                 "QToolButton#DownloadRowCancel:hover, QToolButton#DownloadRowRetry:hover,"
                 "QToolButton#DownloadPagePause:hover, QToolButton#DownloadPageResume:hover,"
                 "QToolButton#DownloadPageCancel:hover, QToolButton#DownloadPageRetry:hover {"
                 "  background: %1;"
                 "}")
            .arg(accent_wash);
    s += QString("QToolButton#DownloadRowPause:pressed, QToolButton#DownloadRowResume:pressed,"
                 "QToolButton#DownloadRowCancel:pressed, QToolButton#DownloadRowRetry:pressed,"
                 "QToolButton#DownloadPagePause:pressed, QToolButton#DownloadPageResume:pressed,"
                 "QToolButton#DownloadPageCancel:pressed, QToolButton#DownloadPageRetry:pressed {"
                 "  background: %1;"
                 "}")
            .arg(accent_wash_strong);

    // =======================================================================
    // Aurora Shields (toolbar shield popup)
    // =======================================================================
    // Same elevated-panel look as the downloads popover; ShieldButton paints
    // the badge, the panel here only carries text and the toggle.
    s += QString("QWidget#ShieldPopup { background: transparent; border: none; }");
    s += QString("QWidget#ShieldPanel {"
                 "  background: %1;"
                 "  border: %2px solid %3;"
                 "  border-radius: %4px;"
                 "}")
            .arg(hex(c.surface_raised))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(kPopoverRadius);
    s += QString("QLabel#ShieldTitle {"
                 "  color: %1; font-size: %2px; font-weight: 600;"
                 "  letter-spacing: -0.1px;"
                 "}")
            .arg(hex(c.text))
            .arg(t.title_font_px);
    s += QString("QLabel#ShieldSite {"
                 "  color: %1; font-size: %2px;"
                 "}")
            .arg(hex(c.text_muted))
            .arg(t.small_font_px + 1);
    s += QString("QLabel#ShieldCount {"
                 "  color: %1; font-size: %2px;"
                 "}")
            .arg(hex(c.text))
            .arg(t.body_font_px);
    s += QString("QFrame#ShieldSeparator {"
                 "  background: %1; border: none; max-height: %2px;"
                 "}")
            .arg(hex(c.divider))
            .arg(Metrics::hairline);
    // Same pill-switch recipe as the settings page toggles (see the
    // #PageCheck rules below) so shields read as one switch language.
    s += QString("QCheckBox#ShieldToggle {"
                 "  color: %1; font-size: %2px; spacing: %3px;"
                 "}")
            .arg(hex(c.text))
            .arg(t.body_font_px)
            .arg(Spacing::xs);
    s += QString("QCheckBox#ShieldToggle::indicator {"
                 "  width: 34px; height: 20px; border-radius: 10px;"
                 "  border: 1px solid %1;"
                 "  background: qradialgradient(cx:0.18, cy:0.5, radius:0.75,"
                 "   fx:0.18, fy:0.5,"
                 "   stop:0 %2, stop:0.34 %2, stop:0.42 %6, stop:1 %6);"
                 "  subcontrol-position: left center; left: 2px; top: 1px;}"
                 "QCheckBox#ShieldToggle::indicator:checked {"
                 "  background: qradialgradient(cx:0.82, cy:0.5, radius:0.75,"
                 "   fx:0.82, fy:0.5,"
                 "   stop:0 %3, stop:0.34 %3, stop:0.42 %4, stop:1 %5);"
                 "  border-color: %4;"
                 "  subcontrol-position: right center; right: 2px;}"
                 "QCheckBox#ShieldToggle::indicator:hover {"
                 "  border-color: %4;}")
            .arg(hex(c.toggle_border_off), hex(c.toggle_knob_off),
                 hex(c.toggle_knob_on), hex(c.accent2), hex(c.accent3),
                 hex(c.toggle_track_off));
    s += QString("QLabel#ShieldNote {"
                 "  color: %1; font-size: %2px;"
                 "}")
            .arg(hex(c.text_muted))
            .arg(t.small_font_px + 1);


    // =======================================================================
    // Permission prompt (mic/camera/notifications)
    // =======================================================================
    // Same elevated-panel look as the downloads popover; the shadow behind it
    // is painted by PermissionPrompt::paintEvent.
    s += QString("QWidget#PermissionPromptPanel {"
                 "  background: %1;"
                 "  border: %2px solid %3;"
                 "  border-radius: %4px;"
                 "}")
            .arg(hex(c.surface_raised))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(Radius::xl);
    s += QString("QLabel#PermissionPromptTitle {"
                 "  color: %1; font-size: %2px;"
                 "}")
            .arg(hex(c.text))
            .arg(t.body_font_px);
    s += QString("QCheckBox#PermissionPromptRemember {"
                 "  color: %1; font-size: %2px; background: transparent;"
                 "}")
            .arg(hex(c.text_muted))
            .arg(t.small_font_px + 1);
    // The affirmative action is the accent-filled button; Block stays the
    // quiet secondary. Local names so the generic QPushButton rule and the
    // settings-page PageButton rule keep their own looks.
    s += QString("QPushButton#PageAccentButton {"
                 "  background: %1; color: %2; border: none;"
                 "  border-radius: %3px; padding: %4px %5px; font-weight: 600;"
                 "}")
            .arg(hex(c.accent))
            // Text on the accent: the elevated surface color is light in the
            // light theme and dark in the dark theme — readable on the accent
            // in both (there is no dedicated on-accent token).
            .arg(hex(c.surface_raised))
            .arg(Radius::md)
            .arg(Spacing::xs + 2)
            .arg(Spacing::lg);
    s += QString("QPushButton#PageAccentButton:hover { background: %1; }")
            .arg(hex(c.accent_hover));
    s += QString("QPushButton#PageAccentButton:pressed { background: %1; }")
            .arg(hex(c.accent));

    // Bookmark star inside the omnibox pill: chromeless, quiet wash on hover.
    s += QString("QToolButton#OmniboxStar {"
                 "  background: transparent;"
                 "  border: none;"
                 "  border-radius: %1px;"
                 "  color: %2;"
                 "  padding: 0px;"
                 "}")
            .arg(Radius::md)
            .arg(hex(c.text_muted));
    s += QString("QToolButton#OmniboxStar:hover { background: %1; }").arg(accent_wash);
    s += QString("QToolButton#OmniboxStar:pressed { background: %1; }")
            .arg(accent_wash_strong);

    // Progress indication (page load) reads as a thin accent rule.
    s += QString("QProgressBar {"
                 "  background: %1; border: none; border-radius: %2px;"
                 "  max-height: 3px; text-align: center; color: transparent;"
                 "}")
            .arg(hex(c.surface_alt))
            .arg(Radius::sm);
    s += QString("QProgressBar::chunk { background: %1; border-radius: %2px; }")
            .arg(hex(c.accent))
            .arg(Radius::sm);

    // =======================================================================
    // Menus — the only truly elevated surface
    //
    // surface_raised + one subtle hairline + moderate radius. Item padding
    // leaves a left icon column and trailing room for a shortcut hint.
    // =======================================================================
    s += QString("QMenu {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "  padding: %6px;"
                 "  margin: 2px;"  // room for rounded corners to render
                 "}")
            .arg(hex(c.surface_raised), hex(c.text))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(Radius::lg)
            .arg(Spacing::xs + Spacing::xxs);

    s += QString("QMenu::item {"
                 "  background: transparent;"
                 "  color: %1;"
                 "  padding: %2px %3px %2px %4px;"  // left: icon column
                 "  margin: %5px %6px;"
                 "  border-radius: %7px;"
                 "  min-width: 180px;"
                 "}")
            .arg(hex(c.text))
            .arg(Spacing::xs + Spacing::xxs)
            .arg(Spacing::xl)                       // trailing shortcut room
            .arg(Spacing::xl + Spacing::sm)
            .arg(1)
            .arg(Spacing::xxs)
            .arg(Radius::md);

    s += QString("QMenu::item:selected { background: %1; color: %2; }")
            .arg(hex(c.surface_selected), hex(c.accent));
    s += QString("QMenu::item:pressed { background: %1; color: %2; }")
            .arg(accent_wash_strong, hex(c.accent_hover));
    s += QString("QMenu::item:disabled { color: %1; background: transparent; }")
            .arg(hex(c.text_faint));
    s += QString("QMenu::item:checked { color: %1; font-weight: 500; }")
            .arg(hex(c.accent));

    s += QString("QMenu::icon { left: %1px; width: 16px; height: 16px; }")
            .arg(Spacing::sm);
    s += QString("QMenu::indicator {"
                 "  left: %1px; width: 14px; height: 14px;"
                 "}")
            .arg(Spacing::sm + Spacing::xxs);
    s += QString("QMenu::separator {"
                 "  height: %1px; background: %2;"
                 "  margin: %3px %4px;"
                 "}")
            .arg(Metrics::hairline)
            .arg(hex(c.divider))
            .arg(Spacing::xs)
            .arg(Spacing::sm);
    s += QString("QMenu::right-arrow { width: 12px; height: 12px; right: %1px; }")
            .arg(Spacing::sm);
    // Shortcut hints inherit the faint role wherever the platform style
    // renders them as child text.
    s += QString("QMenu QLabel { color: %1; }").arg(hex(c.text_faint));

    s += QString("QMenuBar { background: %1; color: %2; border: none; padding: %3px; }")
            .arg(hex(c.surface), hex(c.text))
            .arg(Spacing::xxs);
    s += QString("QMenuBar::item { background: transparent; padding: %1px %2px;"
                 "  border-radius: %3px; }")
            .arg(Spacing::xs)
            .arg(Spacing::sm)
            .arg(Radius::sm);
    s += QString("QMenuBar::item:selected { background: %1; color: %2; }")
            .arg(accent_wash, hex(c.text));

    // =======================================================================
    // Scrollbars — slim, no arrows, rounded handle, transparent track
    // =======================================================================
    s += QString("QScrollBar:vertical {"
                 "  background: transparent; width: %1px;"
                 "  margin: 0px; border: none;"
                 "}")
            .arg(Metrics::scrollbar);
    s += QString("QScrollBar::handle:vertical {"
                 "  background: %1; border-radius: %2px;"
                 "  min-height: 28px; margin: %3px;"
                 "}")
            .arg(rgba(c.text_muted, 90))
            .arg(Radius::sm)
            .arg(Spacing::xxs);
    s += QString("QScrollBar::handle:vertical:hover { background: %1; }")
            .arg(rgba(c.text_muted, 160));
    s += QString("QScrollBar::handle:vertical:pressed { background: %1; }")
            .arg(rgba(c.accent, 190));
    s += QString("QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
                 "  height: 0px; background: none; border: none;"
                 "}");
    s += QString("QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
                 "  background: transparent;"
                 "}");

    s += QString("QScrollBar:horizontal {"
                 "  background: transparent; height: %1px;"
                 "  margin: 0px; border: none;"
                 "}")
            .arg(Metrics::scrollbar);
    s += QString("QScrollBar::handle:horizontal {"
                 "  background: %1; border-radius: %2px;"
                 "  min-width: 28px; margin: %3px;"
                 "}")
            .arg(rgba(c.text_muted, 90))
            .arg(Radius::sm)
            .arg(Spacing::xxs);
    s += QString("QScrollBar::handle:horizontal:hover { background: %1; }")
            .arg(rgba(c.text_muted, 160));
    s += QString("QScrollBar::handle:horizontal:pressed { background: %1; }")
            .arg(rgba(c.accent, 190));
    s += QString("QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
                 "  width: 0px; background: none; border: none;"
                 "}");
    s += QString("QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
                 "  background: transparent;"
                 "}");

    // =======================================================================
    // Tooltips — small, elevated, quiet
    // =======================================================================
    s += QString("QToolTip {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "  padding: %6px %7px;"
                 "  font-size: %8px;"
                 "  opacity: 240;"
                 "}")
            .arg(hex(c.surface_raised), hex(c.text))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(Radius::md)
            .arg(Spacing::xs)
            .arg(Spacing::sm)
            .arg(t.small_font_px);

    // =======================================================================
    // Shared primitives (dialogs, push buttons, splitters, frames)
    // Radius grows with surface size: dialogs are the largest.
    // =======================================================================
    s += QString("QDialog, QWidget#Dialog {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "}")
            .arg(hex(c.surface_raised), hex(c.text))
            .arg(Metrics::hairline)
            .arg(hex(c.border_subtle))
            .arg(Radius::xl);

    s += QString("QPushButton {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4;"
                 "  border-radius: %5px;"
                 "  padding: %6px %7px;"
                 "  min-height: 26px;"
                 "}")
            .arg(hex(c.surface_alt), hex(c.text))
            .arg(Metrics::hairline)
            .arg(hex(c.border))
            .arg(Radius::md)
            .arg(Spacing::xs + Spacing::xxs)
            .arg(Spacing::md);
    s += QString("QPushButton:hover { background: %1; border-color: %2; }")
            .arg(hex(c.surface_hover), rgba(c.text_muted, 120));
    s += QString("QPushButton:pressed { background: %1; }").arg(accent_wash_strong);
    s += QString("QPushButton:focus { border-color: %1; }").arg(hex(c.accent));
    s += QString("QPushButton:disabled { color: %1; border-color: %2; background: %3; }")
            .arg(hex(c.text_faint), hex(c.border_subtle), hex(c.surface_alt));
    s += QString("QPushButton[primary=\"true\"] {"
                 "  background: %1; color: %2; border-color: %1; font-weight: 500;"
                 "}")
            .arg(hex(c.accent), hex(c.surface_raised));
    s += QString("QPushButton[primary=\"true\"]:hover { background: %1; border-color: %1; }")
            .arg(hex(c.accent_hover));

    s += QString("QSplitter::handle { background: %1; }").arg(hex(c.divider));
    s += QString("QFrame[frameShape=\"4\"], QFrame[frameShape=\"5\"] {"
                 "  background: %1; border: none; max-height: %2px;"
                 "}")
            .arg(hex(c.divider))
            .arg(Metrics::hairline);
    // QLabel is transparent by default. Do not assign a global background
    // brush: named labels such as #StatusBubble own their fill and clipping,
    // and a late generic background rule can produce a rectangular backing on
    // some native styles.
    s += QString("QLabel { color: %1; }").arg(hex(c.text));
    s += QString("QLabel[muted=\"true\"] { color: %1; }").arg(hex(c.text_muted));
    s += QString("QLabel[faint=\"true\"] { color: %1; font-size: %2px; }")
            .arg(hex(c.text_faint))
            .arg(t.small_font_px);


    // =======================================================================
    // Internal pages (aurora://history, downloads, bookmarks, settings,
    // extensions). Carried over from the feature branch and re-tinted with the
    // redesigned tokens so the native pages match the new chrome.
    // =======================================================================
    // --- Internal pages (history/downloads/bookmarks/settings/extensions) ---
    // One shared skeleton (see ui/pages/InternalPageWidget): a centred content
    // column, a header band (title + subtitle + hairline), a toolbar row, the
    // body, and an empty-state card. Everything below is expressed in the same
    // tokens as the chrome so the pages stop looking like a different app.
    // Internal pages no longer paint a flat plate: the aurora wallpaper in
    // InternalPageWidget::paintEvent fills the whole page, so the widget and
    // translucent surfaces on top must not alpha-fight it with solid fills
    // here. Their backgrounds stay transparent and the glass below is what
    // keeps text readable.
    s += QString("QWidget#InternalPage { background: transparent; }");
    s += QString("QWidget#PageColumn, QWidget#PageHeader, QWidget#PageToolbar {"
                 "  background: transparent; }");
    // Scroll areas and viewports are pure windows onto the parent — any fill
    // here would block the wallpaper under long lists.
    s += QString("QScrollArea, QAbstractScrollArea, QAbstractScrollArea#SettingsScroll,"
                 "QAbstractScrollArea#ExtensionsScroll {"
                 "  background: transparent; border: none; }");
    s += QString("QAbstractScrollArea::corner { background: transparent; border: none; }");
    s += QString("QWidget#qt_scrollarea_viewport {"
                 "  background: transparent; border: none; }");
    // The glass surface shared by cards, rows, panels and inputs on every
    // internal page: a dimmed version of the dark-blue card color with a
    // faint purple hairline, so the aurora image keeps breathing through
    // while text contrast stays comfortable (~60-70% alpha covers a busy
    // aurora pixel under body text).
    s += QString("QWidget#PageCard, QWidget#SettingsCard,"
                 " QFrame#PagePanel, QWidget#SettingsSidebarCard,"
                 " QWidget#SettingsSectionCard {"
                 "  background: %1; border: %2px solid %3;"
                 "  border-radius: %4px; }")
            .arg(rgba(c.card_bg, 168))
            .arg(Metrics::hairline)
            .arg(glow_edge)
            .arg(Radius::lg);
    // Title: one clear step above everything else, tightened tracking so a
    // short word like "History" does not read as spaced-out.
    s += QString("QLabel#PageTitle { color: %1; font-size: %2px; font-weight: 600;"
                 "  letter-spacing: -0.3px; }")
            .arg(c.text.name()).arg(t.body_font_px + 11);
    s += QString("QLabel#PageSubtitle { color: %1; font-size: %2px; }")
            .arg(c.text_muted.name()).arg(t.body_font_px);
    s += QString("QFrame#PageDivider { background: %1; border: none; }")
            .arg(c.divider.name());
    // Empty state: a flat panel that spans the full content column (the pages
    // are wide now), not a small rounded card floating in the middle.
    s += QString("QLabel#PageEmptyState {"
                 "  color: %1; font-size: %2px; background: %3;"
                 "  border: %4px solid %5; border-radius: %6px;"
                 "  padding: %7px; min-height: 160px; }")
            .arg(c.text_muted.name()).arg(t.body_font_px + 1)
            .arg(rgba(c.surface, 150)).arg(Metrics::hairline)
            .arg(glow_edge_soft).arg(Radius::md).arg(Spacing::xl);
    s += QString("QLabel#PageWarning { color: %1; }").arg(c.warning.name());
    s += QString("QWidget#InternalPage QLabel { color: %1; }").arg(c.text.name());

    s += QString("QLineEdit#PageSearch, QLineEdit#PageInput {"
                 "  background: %1; color: %2; border: 1px solid %3;"
                 "  padding: 7px 12px; border-radius: %4px; min-height: 24px;"
                 "  selection-background-color: %5; selection-color: %2; }")
            .arg(rgba(c.surface_alt, 160)).arg(c.text.name())
            .arg(glow_edge_soft)
            .arg(Radius::md).arg(rgba(c.accent, 60));
    s += QString("QLineEdit#PageSearch:focus, QLineEdit#PageInput:focus {"
                 "  border: 1px solid %1; }").arg(c.accent.name());
    s += QString("QLineEdit#PageSearch:disabled, QLineEdit#PageInput:disabled {"
                 "  color: %1; background: %2; }")
            .arg(c.text_faint.name()).arg(c.surface.name());

    s += QString("QPushButton#PageButton, QPushButton#PageDangerButton {"
                 "  background: %1; color: %2; border: 1px solid %3;"
                 "  padding: 7px 14px; border-radius: %4px; min-height: 24px;"
                 "  font-weight: 500; }")
            .arg(rgba(c.surface_alt, 160)).arg(c.text.name())
            .arg(glow_edge_soft)
            .arg(Radius::md);
    s += QString("QPushButton#PageButton:hover { background: %1; border-color: %2;"
                 "  color: %3; }")
            .arg(rgba(c.accent, 40)).arg(c.accent.name()).arg(c.text.name());
    s += QString("QPushButton#PageDangerButton:hover { background: %1;"
                 "  border-color: %2; color: %3; }")
            .arg(rgba(c.error, 50)).arg(c.error.name()).arg(c.text.name());
    s += QString("QPushButton#PageButton:pressed, QPushButton#PageDangerButton:pressed {"
                 "  background: %1; }").arg(rgba(c.accent, 70));
    s += QString("QPushButton#PageButton:disabled, QPushButton#PageDangerButton:disabled {"
                 "  color: %1; border-color: %2; background: %3; }")
            .arg(c.text_faint.name()).arg(c.border.name()).arg(c.surface.name());

    // Explanatory hint text under a page's content ("takes effect after a
    // restart", "home page follows your search engine"): quieter than body
    // text, but still above the faint-text contrast floor.
    s += QString("QLabel#PageHint { color: %1; font-size: 12px; }")
            .arg(c.text_muted.name());

    // Lists and trees on the pages. One card per list: a hairline-bordered
    // surface with generous row padding, no zebra striping (hover + selection
    // carry the row, which is what makes a long list scan cleanly).
    s += QString("QTreeWidget#PageList, QTreeWidget#PageTree {"
                 "  background: %1; alternate-background-color: %1;"
                 "  color: %2; border: %3px solid %4; border-radius: %5px;"
                 "  outline: none; padding: %6px; }")
            .arg(rgba(c.card_bg, 168)).arg(c.text.name()).arg(Metrics::hairline)
            .arg(glow_edge_soft).arg(Radius::lg).arg(Spacing::xxs);
    s += QString("QTreeWidget#PageList::item, QTreeWidget#PageTree::item {"
                 "  color: %1; padding: 7px 8px; border-radius: %2px; }")
            .arg(c.text.name()).arg(Radius::sm);
    s += QString("QTreeWidget#PageList::item:hover, QTreeWidget#PageTree::item:hover {"
                 "  background: %1; color: %2; }")
            .arg(rgba(c.accent, 28)).arg(c.text.name());
    s += QString("QTreeWidget#PageList::item:selected, QTreeWidget#PageTree::item:selected {"
                 "  background: %1; color: %2; }")
            .arg(rgba(c.accent, 70)).arg(c.text.name());
    // Selected rows keep the same text color when the view loses focus, which
    // is where Qt's default palette would otherwise draw dark-on-dark.
    s += QString("QTreeWidget#PageList::item:selected:!active,"
                 " QTreeWidget#PageTree::item:selected:!active {"
                 "  background: %1; color: %2; }")
            .arg(rgba(c.accent, 45)).arg(c.text.name());
    // Column headers read as quiet labels, not as another row of content:
    // muted, small, letter-spaced, on the card's own background.
    s += QString("QHeaderView { background: transparent; }");
    s += QString("QHeaderView::section {"
                 "  background: transparent; color: %1; border: none;"
                 "  border-bottom: %2px solid %3; padding: 8px; "
                 "  font-size: %4px; font-weight: 600; letter-spacing: 0.3px; }")
            .arg(c.text_muted.name()).arg(Metrics::hairline)
            .arg(c.divider.name()).arg(t.small_font_px);
    s += QString("QTreeWidget#PageTree::branch { background: transparent; }");

    // --- History page (aurora://history) ------------------------------------
    // The glass plates, gradient rims and outer glows are painted in C++ by
    // GlowFrame and the page's custom widgets; the sheet owns text color,
    // spacing and the few flat bits (checkbox indicator, hover washes).
    s += QString("QWidget#HistoryHeader, QWidget#HistoryBody,"
                 "QWidget#HistoryGroupsHost, QWidget#HistoryGroupHeader,"
                 "QWidget#HistoryGroupRows, QWidget#HistorySearchWrap,"
                 "QWidget#HistoryClearWrap { background: transparent; border: none; }");
    s += QString("QLabel#HistoryTitle { color: %1; font-size: %2px;"
                 "  font-weight: 600; letter-spacing: -0.3px; }")
            .arg(c.text.name()).arg(t.title_font_px + 9);
    // Search pill content: the field itself is a transparent strip inside
    // the GlowFrame, so the gradient rim on the host reads as uninterrupted.
    s += QString("QLineEdit#HistorySearch, QLineEdit#HistorySiteSearch {"
                 "  background: transparent; color: %1; border: none;"
                 "  selection-background-color: %2; selection-color: %1; }")
            .arg(c.text.name())
            .arg(rgba(c.accent, 60));
    s += QString("QLineEdit#HistorySiteSearch {"
                 "  background: %1; border: 1px solid %2;"
                 "  border-radius: %3px; padding: 6px 10px; min-height: 22px; }")
            .arg(rgba(c.surface_alt, 150))
            .arg(glow_edge_soft)
            .arg(Radius::md);
    // Clear-browsing-data chrome: the label sits inside a GlowFrame whose
    // magenta rim marks the destructive action; the button itself is flat.
    s += QString("QPushButton#HistoryClearButton {"
                 "  background: transparent; color: %1; border: none;"
                 "  font-weight: 500; padding: 0px; }")
            .arg(c.text.name());
    // Group cards: bold header, the count as a blue badge, rows on a hover
    // wash. The chevron glyph is a QLabel pixmap so tint comes from C++.
    s += QString("QLabel#HistoryGroupTitle {"
                 "  color: %1; font-size: %2px; font-weight: 600; }")
            .arg(c.text.name()).arg(t.body_font_px + 6);
    // Count badge: accent number on an accent-tinted capsule. Built from the
    // theme accent (not fixed blues) so it stays readable on light and dark.
    s += QString("QLabel#HistoryGroupCount {"
                 "  color: %2; font-size: %1px; font-weight: 600;"
                 "  background: %3;"
                 "  border: 1px solid %4;"
                 "  border-radius: 11px; padding: 1px 10px; }")
            .arg(t.small_font_px + 2)
            .arg(hex(c.accent))
            .arg(rgba(c.accent, t.is_dark ? 40 : 26))
            .arg(rgba(c.accent, t.is_dark ? 90 : 70));
    s += QString("QWidget#HistoryRow {"
                 "  background: transparent; border-radius: %1px; }")
            .arg(Radius::md);
    s += QString("QWidget#HistoryRow:hover { background: %1; }")
            .arg(rgba(c.accent, 22));
    s += QString("QLabel#HistoryRowTitle { color: %1; font-size: %2px; }")
            .arg(c.text.name()).arg(t.body_font_px + 1);
    s += QString("QLabel#HistoryRowDomain { color: %1; font-size: %2px; }")
            .arg(c.text_muted.name()).arg(t.small_font_px + 2);
    s += QString("QLabel#HistoryRowTime { color: %1; font-size: %2px; }")
            .arg(c.text_muted.name()).arg(t.small_font_px + 2);
    // Filter panel rows: the purple highlight bar and radio dot appear only
    // on the selected row, so the left of the list reads as one column.
    s += QString("QWidget#HistoryFilterItem {"
                 "  background: transparent; border-radius: %1px; color: %2; }")
            .arg(Radius::md).arg(c.text.name());
    s += QString("QWidget#HistoryFilterItem:hover { background: %1; }")
            .arg(rgba(c.accent, 20));
    s += QString("QWidget#HistoryFilterItem[selected=\"true\"] {"
                 "  background: %1; }")
            .arg(rgba(c.accent2, 25));
    s += QString("QFrame#HistoryFilterBar {"
                 "  background: %1; border: none; border-radius: 2px; }")
            .arg(c.accent2.name());
    s += QString("QLabel#HistoryPanelHeading {"
                 "  color: %1; font-size: %2px; font-weight: 600;"
                 "  letter-spacing: 0.2px; }")
            .arg(c.text.name()).arg(t.body_font_px);
    s += QString("QWidget#HistoryActionRow {"
                 "  background: transparent; border-radius: %1px; }")
            .arg(Radius::md);
    s += QString("QWidget#HistoryActionRow:hover { background: %1; }")
            .arg(rgba(c.accent, 20));
    s += QString("QLabel#HistoryActionLabel { color: %1; font-size: %2px; }")
            .arg(c.text.name()).arg(t.body_font_px);
    s += QString("QLabel#HistorySyncNote {"
                 "  color: %1; font-size: %2px; font-style: italic; }")
            .arg(c.text_faint.name()).arg(t.small_font_px + 1);

    // --- Downloads page (aurora://downloads) -------------------------------
    // Same family as the shared page list, but flatter and airier: the list is
    // the whole page here, so it gets a small 6px radius (a large radius on a
    // full-width panel reads as a phone card), roomier rows, and a header row
    // that separates cleanly from the first item.
    s += QString("QTreeWidget#DownloadsList {"
                 "  background: %1; alternate-background-color: %1;"
                 "  color: %2; border: %3px solid %4; border-radius: %5px;"
                 "  outline: none; padding: %6px; }")
            .arg(rgba(c.card_bg, 168)).arg(c.text.name()).arg(Metrics::hairline)
            .arg(glow_edge_soft).arg(Radius::md).arg(Spacing::xs);
    s += QString("QTreeWidget#DownloadsList::item {"
                 "  color: %1; padding: %2px %3px; border-radius: %4px; }")
            .arg(c.text.name()).arg(Spacing::sm).arg(Spacing::md)
            .arg(Radius::sm - 1);
    s += QString("QTreeWidget#DownloadsList::item:hover {"
                 "  background: %1; color: %2; }")
            .arg(rgba(c.accent, 28)).arg(c.text.name());
    s += QString("QTreeWidget#DownloadsList::item:selected {"
                 "  background: %1; color: %2; }")
            .arg(rgba(c.accent, 70)).arg(c.text.name());
    s += QString("QTreeWidget#DownloadsList::item:selected:!active {"
                 "  background: %1; color: %2; }")
            .arg(rgba(c.accent, 45)).arg(c.text.name());
    // The in-progress bar that replaces the Status text on aurora://downloads.
    // The generic QProgressBar rule caps every bar at a 3px hairline and hides
    // its text; this one is a real cell, so it re-states height, text color and
    // font — an ID selector outranks the element selector regardless of order.
    s += QString("QProgressBar#DownloadPageProgress {"
                 "  background: %1; border: %2px solid %3; border-radius: %4px;"
                 "  min-height: %5px; max-height: %5px;"
                 "  color: %6; font-size: %7px; text-align: center;"
                 "}"
                 "QProgressBar#DownloadPageProgress::chunk {"
                 "  background: %8; border-radius: %4px;"
                 "}")
            .arg(rgba(c.surface_alt, 170))
            .arg(Metrics::hairline)
            .arg(glow_edge_soft)
            .arg(Radius::sm)
            .arg(kDownloadProgressHeight)
            .arg(c.text.name())
            .arg(t.small_font_px)
            .arg(rgba(c.accent, 110));
    s += QString("QTreeWidget#DownloadsList QHeaderView::section {"
                 "  background: transparent; color: %1; border: none;"
                 "  border-bottom: %2px solid %3; padding: %4px %5px;"
                 "  font-size: %6px; font-weight: 600; letter-spacing: 0.4px; }")
            .arg(c.text_muted.name()).arg(Metrics::hairline)
            .arg(c.divider.name()).arg(Spacing::sm).arg(Spacing::md)
            .arg(t.small_font_px);
    // Leading label of the actions row: the page's own context line now that
    // the skeleton draws no title.
    s += QString("QWidget#InternalPage QLabel#DownloadsCount {"
                 "  color: %1; font-size: %2px; font-weight: 600;"
                 "  letter-spacing: 0.2px; }")
            .arg(c.text_muted.name()).arg(t.body_font_px);

    // Settings page, chrome://settings style: a fixed section rail, then a
    // section header above each rounded card. Rows are transparent on the
    // card and separated by hairlines; the title leads, the subtitle explains
    // in muted text, and the control rides the right edge (8px rhythm).
    s += QString("QWidget#SettingsSidebar, QWidget#SettingsSection,"
                 "QWidget#SettingsRow, QScrollArea#SettingsScroll,"
                 "QWidget#SettingsContainer {"
                 "  background: transparent; border: none; }");
    s += QString("QWidget#SettingsSidebar { min-width: 190px; max-width: 220px; }");
    s += QString("QToolButton#SettingsNavButton {"
                 "  background: transparent; border: %1px solid transparent; color: %2;"
                 "  font-size: %3px; font-weight: 500; text-align: left;"
                 "  padding: %4px %5px; border-radius: %6px; }")
            .arg(Metrics::hairline).arg(c.text_muted.name())
            .arg(t.body_font_px)
            .arg(Spacing::sm).arg(Spacing::md).arg(Radius::lg);
    s += QString("QToolButton#SettingsNavButton:hover { background: %1; color: %2; }")
            .arg(rgba(c.accent, 14)).arg(c.text.name());
    // Active nav item: the mockup's purple-outlined pill with a soft glow
    // wash and full-brightness text.
    s += QString("QToolButton#SettingsNavButton:checked {"
                 "  background: %1; color: %2; font-weight: 600;"
                 "  border: %3px solid %4; }")
            .arg(rgba(c.accent2, 38)).arg(c.text.name())
            .arg(Metrics::hairline).arg(hex(c.accent2));
    s += QString("QLabel#SettingsSectionHeader {"
                 "  color: %1; font-size: %2px; font-weight: 600;"
                 "  letter-spacing: 0.2px; padding-left: %3px; background: transparent; }")
            .arg(c.text.name()).arg(t.title_font_px).arg(Spacing::sm);
    s += QString("QWidget#SettingsCard {"
                 "  background: %1; border: %2px solid %3;"
                 "  border-radius: %4px; }")
            .arg(rgba(c.card_bg, 165)).arg(Metrics::hairline)
            .arg(glow_edge).arg(Radius::xl);
    s += QString("QWidget#SettingsCard QLabel { color: %1; background: transparent; }")
            .arg(c.text.name());
    s += QString("QLabel#SettingsRowTitle { color: %1; font-size: %2px; }")
            .arg(c.text.name()).arg(t.body_font_px + 1);
    s += QString("QLabel#SettingsRowSubtitle, QLabel#SettingsRowHint {"
                 "  color: %1; font-size: %2px; background: transparent; }")
            .arg(c.text_muted.name()).arg(t.small_font_px + 1);
    s += QString("QFrame#SettingsRowDivider {"
                 "  background: %1; border: none;"
                 "  margin-left: %2px; margin-right: %3px; }")
            .arg(c.divider.name()).arg(Spacing::lg).arg(Spacing::lg);

    // Settings page header block: the section title + one-line explanation
    // that introduces the card column ("General — Customize your browsing
    // experience.").
    s += QString("QLabel#SettingsPageTitle {"
                 "  color: %1; font-size: %2px; font-weight: 600;"
                 "  letter-spacing: -0.2px; background: transparent; }")
            .arg(c.text.name()).arg(t.body_font_px + 9);
    s += QString("QLabel#SettingsPageSubtitle {"
                 "  color: %1; font-size: %2px; background: transparent; }")
            .arg(c.text_muted.name()).arg(t.body_font_px);

    // Startup radio group: circular pill radios matching the toggle pair —
    // a slate ring at rest, the cyan→magenta gradient with a navy core dot
    // when selected.
    s += QString("QRadioButton#PageRadio { color: %1; spacing: %2px; "
                 "  background: transparent; min-height: 32px; }")
            .arg(c.text.name()).arg(Spacing::sm);
    s += QString("QRadioButton#PageRadio::indicator {"
                 "  width: 16px; height: 16px; border-radius: 9px;"
                 "  border: 1px solid %1; background: transparent; }")
            .arg(hex(c.toggle_border_off));
    s += QString("QRadioButton#PageRadio::indicator:hover { border-color: %1; }")
            .arg(hex(c.accent2));
    // Checked: a radial fill whose outer band is the neon purple → magenta
    // ring and whose core is the window navy, so the radio reads as a
    // gradient ring with a dark center (the mockup's pill radios).
    s += QString("QRadioButton#PageRadio::indicator:checked {"
                 "  background: qradialgradient(cx:0.5, cy:0.5, radius:0.5,"
                 "   fx:0.5, fy:0.5,"
                 "   stop:0 %1, stop:0.5 %1, stop:0.72 %2, stop:1 %3);"
                 "  border-color: %2; }")
            .arg(hex(c.background), hex(c.accent2), hex(c.accent3));
    s += QString("QRadioButton#PageRadio:disabled { color: %1; }")
            .arg(c.text_faint.name());

    // Theme preview chips (Dark / Light / Auto): small rounded thumbs whose
    // checked state carries the purple outline + soft glow.
    s += QString("QToolButton#ThemeChip {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4; border-radius: %5px;"
                 "  padding: %6px %7px; font-size: %8px; font-weight: 500; }")
            .arg(rgba(c.surface_alt, 170)).arg(c.text_muted.name())
            .arg(Metrics::hairline).arg(c.border.name())
            .arg(Radius::lg).arg(Spacing::sm).arg(Spacing::md)
            .arg(t.small_font_px + 1);
    s += QString("QToolButton#ThemeChip:hover { color: %1; border-color: %2; }")
            .arg(c.text.name()).arg(rgba(c.accent2, 150));
    s += QString("QToolButton#ThemeChip:checked {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4; }")
            .arg(rgba(c.accent2, 38)).arg(c.text.name())
            .arg(Metrics::hairline + 1).arg(hex(c.accent2));

    // The startup URL capture row: a recessed input next to its outlined
    // action button on the right of the radio option.
    s += QString("QLineEdit#PageInputInline {"
                 "  background: %1; color: %2; border: 1px solid %3;"
                 "  padding: 4px 10px; border-radius: %4px; min-height: 22px;"
                 "  selection-background-color: %5; selection-color: %2; }")
            .arg(rgba(c.surface_alt, 160)).arg(c.text.name())
            .arg(glow_edge_soft).arg(Radius::md).arg(rgba(c.accent, 60));
    s += QString("QLineEdit#PageInputInline:focus { border-color: %1; }")
            .arg(c.accent.name());
    s += QString("QPushButton#PageOutlineButton {"
                 "  background: transparent; color: %1;"
                 "  border: 1px solid %2; border-radius: %3px;"
                 "  padding: 4px 12px; min-height: 24px; font-weight: 500; }")
            .arg(c.accent.name()).arg(rgba(c.accent2, 170)).arg(Radius::md);
    s += QString("QPushButton#PageOutlineButton:hover { background: %1; }")
            .arg(rgba(c.accent2, 34));
    s += QString("QPushButton#PageOutlineButton:pressed { background: %1; }")
            .arg(rgba(c.accent2, 60));

    // --- Extension cards (aurora://extensions) ------------------------------
    // One card per extension: artwork, the manifest's name + version, its
    // description, its folder, and the per-card controls.
    s += QString("QScrollArea#ExtensionsScroll, QWidget#ExtensionsList {"
                 "  background: transparent; border: none; }");
    s += QString("QWidget#ExtensionCard {"
                 "  background: %1; border: %2px solid %3;"
                 "  border-radius: %4px; }")
            .arg(rgba(c.card_bg, 168)).arg(Metrics::hairline)
            .arg(glow_edge_soft).arg(Radius::lg);
    s += QString("QWidget#ExtensionCard:hover { border-color: %1; }")
            .arg(c.border.name());
    s += QString("QLabel#ExtensionCardName {"
                 "  color: %1; font-size: %2px; font-weight: 600; }")
            .arg(c.text.name()).arg(t.body_font_px + 1);
    // The version sits next to the name as a quiet chip, not as body text.
    s += QString("QLabel#ExtensionCardVersion {"
                 "  color: %1; font-size: %2px; background: %3;"
                 "  border-radius: %4px; padding: 1px %5px; }")
            .arg(c.text_muted.name()).arg(t.small_font_px)
            .arg(c.surface_alt.name()).arg(Radius::sm).arg(Spacing::xs + 2);
    s += QString("QLabel#ExtensionCardMeta { color: %1; font-size: %2px; }")
            .arg(c.text_muted.name()).arg(t.body_font_px);
    s += QString("QLabel#ExtensionCardPath { color: %1; font-size: %2px; }")
            .arg(c.text_faint.name()).arg(t.small_font_px);
    s += QString("QLabel#ExtensionCardIcon { background: transparent; }");

    // --- Home page (aurora://home) -------------------------------------------
    // The background is a painted deep-cosmic gradient with the cyan/magenta
    // aurora glows (see HomePage::paintEvent); the sheet owns everything
    // interactive so the C++ stays a layout and a canvas.
    s += QString("QWidget#HomePage { background: transparent; }"
                 "QWidget#HomeColumn, QWidget#HomeCardsRow,"
                 "QWidget#HomeContent, QWidget#HomeHeader, QWidget#HomeScroll,"
                 "QWidget#HomeShortcutTile, QWidget#HomeRow {"
                 "  background: transparent; }");

    // Brand block: "A U R O R A" in a widely-tracked cyan→magenta gradient
    // (painted by HomeBrandLabel in C++, which also draws the glowing ribbon
    // "A" logo above it), with the dot-phrase subtitle underneath.
    s += QString("QLabel#HomeSubtitle {"
                 "  color: %1; font-size: %2px; letter-spacing: 3px;}"
                 "QLabel#HomeClock {"
                 "  color: %3; font-size: %4px; font-weight: 300;}"
                 "QLabel#HomeDate, QLabel#HomeWeatherSub {"
                 "  color: %3; font-size: %5px;}"
                 "QFrame#HomeCardDivider { background: %6; border: none; } ")
            .arg(c.text_muted.name())
            .arg(16)
            .arg(c.text.name())
            .arg(68)
            .arg(17)
            .arg(c.divider.name());

    s += QString("QLabel#HomeWeatherTemp {"
                 "  color: %1; font-size: %2px; font-weight: 600;}"
                 "QLabel#HomeCardTitle {"
                 "  color: %1; font-size: %3px; font-weight: 600;"
                 "  letter-spacing: 0.2px;}"
                 "QLabel#HomeRowTitle { color: %1; font-size: %4px;}"
                 "QLabel#HomeRowSub { color: %5; font-size: %6px;}")
            .arg(c.text.name())
            .arg(28)
            .arg(19)
            .arg(17)
            .arg(c.text_muted.name())
            .arg(14);

    // The big centered search pill: a glowing glass capsule with the
    // cyan→purple gradient border. The border lives on the HOST (the pill's
    // icons sit inside it); the field itself is a transparent strip.
    // The host is transparent: its capsule + cyan→purple border are painted
    // by HomePage's PillHost (QSS cannot gradient-stroke a border).
    s += QString("QWidget#HomeSearchPillHost {"
                 "  background: transparent; border: none; border-radius: %1px;}")
            .arg(Radius::pill);
    s += QString("QLineEdit#HomeSearchPill {"
                 "  background: transparent; color: %1; border: none;"
                 "  padding: 0px %2px;"
                 "  font-size: %3px;"
                 "  selection-background-color: %4; selection-color: %1;}")
            .arg(c.text.name())
            .arg(Spacing::xs)
            .arg(22)
            .arg(rgba(c.accent, 60));

    // In-pill trailing sparkle: chromeless, glows to magenta on hover.
    s += QString("QToolButton#HomeSparkleButton {"
                 "  background: transparent; border: none;"
                 "  border-radius: %1px; padding: 0px; margin: 0px;"
                 "  min-width: 44px; max-width: 44px;"
                 "  min-height: 44px; max-height: 44px;}"
                 "QToolButton#HomeSparkleButton:hover { background: %2;}"
                 "QToolButton#HomeSparkleButton:pressed { background: %3;}")
            .arg(Radius::pill)
            .arg(accent2_wash)
            .arg(rgba(c.accent2, 70));

    // Shortcut tiles: dark-blue cards with slate hairlines; hover lifts the
    // border to the cyan half of the gradient.
    s += QString("QToolButton#HomeShortcut {"
                 "  background: transparent; border: none;"
                 "  border-radius: 14px; color: %1;"
                 "  font-size: 15px; padding: 8px 0px 6px 0px;}"
                 "QToolButton#HomeShortcut:hover {"
                 "  background: %2; color: %3;}"
                 "QToolButton#HomeShortcut:pressed { background: %4;}")
            .arg(hex(c.text_muted))
            .arg(rgba(c.accent2, 34))
            .arg(hex(c.text))
            .arg(hex(c.surface_selected));

    // Bottom three-column cards share the settings-card recipe; the rows
    // inside them are flat until hovered.
    // The card plate, gradient rim and outer glow are painted by GlowFrame
    // (the History page's recipe), so no QSS background or border here.
    s += QString("QWidget#HomeCard { background: transparent; border: none; }");
    s += QString("QWidget#HomeRow { border-radius: %1px; padding: 4px %2px;}"
                 "QWidget#HomeRow:hover { background: %3;}")
            .arg(Radius::md)
            .arg(Spacing::xs)
            .arg(rgba(c.accent, 22));
    // Clickable card rows (quick actions) use the row recipe on a button.
    s += QString("QToolButton#HomeActionRow {"
                 "  background: transparent; border: none; color: %1;"
                 "  font-size: %2px; text-align: left;"
                 "  border-radius: %3px; padding: %4px %5px;}"
                 "QToolButton#HomeActionRow:hover { background: %6; color: %1;}"
                 "QToolButton#HomeActionRow:pressed { background: %7;}"
                 "QToolButton#HomeActionRow[accent=\"true\"] { color: %8; font-size: 15px;}"
                 "QToolButton#HomeActionRow[accent=\"true\"]:hover { color: %9;}")
            .arg(hex(c.text))
            .arg(17)
            .arg(Radius::md)
            .arg(Spacing::sm)
            .arg(Spacing::md)
            .arg(rgba(c.accent, 22))
            .arg(rgba(c.accent, 40))
            .arg(hex(c.accent))
            .arg(hex(c.accent3));
    // Resume progress hairlines: the track is the slate border color so only
    // the colored chunk reads as content.
    s += QString("QProgressBar#HomeRowProgress {"
                 "  background: %1; border: none; border-radius: 2px;"
                 "  min-height: 4px; max-height: 4px;"
                 "  text-align: center; color: transparent;}"
                 "QProgressBar#HomeRowProgress::chunk {"
                 "  background: %2; border-radius: 2px;}")
            .arg(hex(c.border_subtle))
            .arg(hex(c.accent));

    s += QString("QCheckBox#PageCheck { color: %1; spacing: %2px; }")
            .arg(c.text.name()).arg(Spacing::sm);
    s += QString("QCheckBox#PageCheck:disabled { color: %1; }").arg(c.text_faint.name());
    // Settings switches render as pill toggles, not square checkboxes: a
    // 34×20 capsule whose indicator travels left (off) to right (on). The ON
    // track is the cyan→magenta gradient with a dark core dot; OFF is a
    // slate track with a slate dot. Sub-control position only animates via
    // state repaint, but the pill + dot still read as a switch instantly.
    s += QString("QCheckBox#PageCheck { spacing: 0px; min-height: 20px; }");
    // OFF: slate track with the slate knob painted as a radial stop cluster
    // hugging the LEFT end of the pill (mirrors the ON knob).
    s += QString("QCheckBox#PageCheck::indicator {"
                 "  width: 34px; height: 20px; border-radius: 10px;"
                 "  border: 1px solid %1;"
                 "  background: qradialgradient(cx:0.18, cy:0.5, radius:0.75,"
                 "   fx:0.18, fy:0.5,"
                 "   stop:0 %2, stop:0.34 %2, stop:0.42 %3, stop:1 %3);"
                 "  subcontrol-position: left center;"
                 "  subcontrol-origin: padding;"
                 "  left: 2px; top: 1px;}"
                 "QCheckBox#PageCheck::indicator:hover { border-color: %4; }")
            .arg(hex(c.toggle_border_off), hex(c.toggle_knob_off),
                 hex(c.toggle_track_off), hex(c.accent2));
    // ON: gradient track with the navy knob painted as a radial stop cluster
    // hugging the right end of the pill. OFF keeps the slate look above.
    s += QString("QCheckBox#PageCheck::indicator:checked {"
                 "  background: qradialgradient(cx:0.82, cy:0.5, radius:0.75,"
                 "   fx:0.82, fy:0.5,"
                 "   stop:0 %1, stop:0.34 %1, stop:0.42 %2, stop:1 %3);"
                 "  border-color: %2;"
                 "  subcontrol-position: right center; right: 2px;}")
            .arg(hex(c.toggle_knob_on), hex(c.accent2), hex(c.accent3));
    s += QString("QCheckBox#PageCheck::indicator:disabled {"
                 "  border-color: %1; background: %2;}")
            .arg(hex(c.border_subtle), hex(c.surface));

    // Numeric rows (tab suspension minutes): a recessed field with quiet
    // pill arrows, matching the page inputs.
    s += QString("QSpinBox#TabSuspendMinutes {"
                 "  background: %1; color: %2;"
                 "  border: 1px solid %3; border-radius: %4px;"
                 "  padding: 4px 6px; min-height: 22px;"
                 "  selection-background-color: %5; selection-color: %2;}"
                 "QSpinBox#TabSuspendMinutes:focus { border-color: %6; }"
                 "QSpinBox#TabSuspendMinutes::up-button, "
                 "QSpinBox#TabSuspendMinutes::down-button {"
                 "  width: 16px; border: none; background: transparent;}"
                 "QSpinBox#TabSuspendMinutes::up-button:hover, "
                 "QSpinBox#TabSuspendMinutes::down-button:hover {"
                 "  background: %7;}")
            .arg(rgba(c.surface_alt, 165)).arg(c.text.name())
            .arg(glow_edge_soft)
            .arg(Radius::md)
            .arg(rgba(c.accent, 60))
            .arg(c.accent.name())
            .arg(accent_wash);

    s += QString("QComboBox#PageCombo {"
                 "  background: %1; color: %2; border: 1px solid %3;"
                 "  border-radius: %4px; padding: 5px 10px; min-height: 22px; }")
            .arg(rgba(c.surface_alt, 170)).arg(c.text.name())
            .arg(glow_edge_soft)
            .arg(Radius::md);
    s += QString("QComboBox#PageCombo:hover { border-color: %1; }").arg(c.accent.name());
    s += QString("QComboBox#PageCombo:disabled { color: %1; background: %2; }")
            .arg(c.text_faint.name()).arg(c.surface.name());
    s += QString("QComboBox#PageCombo::drop-down { border: none; width: 18px; }");
    // The popup is a separate top-level view: it needs its own explicit colors,
    // otherwise Qt falls back to the system palette (dark text on dark).
    s += QString("QComboBox#PageCombo QAbstractItemView {"
                 "  background: %1; color: %2; border: 1px solid %3;"
                 "  selection-background-color: %4; selection-color: %2; }")
            .arg(c.surface.name()).arg(c.text.name()).arg(c.border.name())
            .arg(rgba(c.accent, 90));

    // Dialogs raised from the pages (confirm delete, rename prompt).
    s += QString("QMessageBox, QInputDialog { background: %1; color: %2; }")
            .arg(c.surface.name()).arg(c.text.name());
    s += QString("QMessageBox QLabel, QInputDialog QLabel { color: %1; }")
            .arg(c.text.name());
    s += QString("QDialog QPushButton {"
                 "  background: %1; color: %2; border: 1px solid %3;"
                 "  padding: 6px 14px; border-radius: %4px; min-width: 72px; }")
            .arg(c.surface_alt.name()).arg(c.text.name()).arg(c.border.name())
            .arg(Radius::md);
    s += QString("QDialog QPushButton:hover { background: %1; border-color: %2; }")
            .arg(rgba(c.accent, 40)).arg(c.accent.name());
    s += QString("QDialog QLineEdit { background: %1; color: %2;"
                 "  border: 1px solid %3; border-radius: %4px; padding: 5px 8px; }")
            .arg(c.surface_alt.name()).arg(c.text.name()).arg(c.border.name())
            .arg(Radius::sm);

    // =======================================================================
    // Left dock (slim vertical launcher rail)
    //
    // Transparent over the window glass; icon-only app shortcuts in square
    // tiles, and the gear pinned at the bottom with a persistent aurora glow
    // ring (cyan→purple gradient border over a soft purple wash).
    // =======================================================================
    // The rail is separated from the content column by a whisper of the
    // purple glow, like the mockup's chrome hairlines.
    s += QString("QWidget#LeftDock {"
                 "  background: transparent; border: none;"
                 "  border-right: %1px solid %2; padding: %3px 0px; }")
            .arg(Metrics::hairline).arg(glow_edge_soft).arg(Spacing::xs);
    s += QString("QToolButton#DockItem {"
                 "  background: transparent; border: %1px solid transparent; color: %2;"
                 "  border-radius: %3px; padding: 0px; margin: 0px;"
                 "  min-width: 34px; max-width: 34px;"
                 "  min-height: 34px; max-height: 34px; }")
            .arg(Metrics::hairline).arg(c.text_muted.name()).arg(Radius::lg);
    s += QString("QToolButton#DockItem:hover {"
                 "  background: %1; color: %2; }")
            .arg(accent_wash).arg(c.text.name());
    s += QString("QToolButton#DockItem:checked {"
                 "  background: %1; color: %2; border: %3px solid %4; }")
            .arg(rgba(c.accent2, 40)).arg(c.accent.name())
            .arg(Metrics::hairline).arg(hex(c.accent2));

    // The aurora dot at the top of the rail: a tiny gradient bead marking
    // the dock, painted by the sheet so it re-tints with the theme.
    s += QString("QLabel#DockHomeDot {"
                 "  background: %1; border: none; border-radius: 5px;"
                 "  min-width: 10px; max-width: 10px;"
                 "  min-height: 10px; max-height: 10px; }")
            .arg(qss_gradient(c.accent, c.accent2));

    // The bottom-pinned glowing Settings gear.
    s += QString("QToolButton#DockSettingsGlow {"
                 "  background: %1; color: %2;"
                 "  border: %3px solid %4; border-radius: %5px;"
                 "  padding: 0px; margin: 0px;"
                 "  min-width: 34px; max-width: 34px;"
                 "  min-height: 34px; max-height: 34px; }")
            .arg(rgba(c.accent2, 46)).arg(c.accent.name())
            .arg(Metrics::hairline).arg(hex(c.accent2))
            .arg(Radius::lg);
    s += QString("QToolButton#DockSettingsGlow:hover { background: %1; }")
            .arg(rgba(c.accent2, 76));
    s += QString("QToolButton#DockSettingsGlow:pressed { background: %1; }")
            .arg(rgba(c.accent2, 100));
    return s;
}

} // namespace aurora::ui

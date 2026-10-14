#include "ui/TabContextMenu.hpp"

#include <QAction>
#include <QApplication>
#include <QFont>
#include <QFontInfo>
#include <QIcon>

#if defined(AURORA_HAS_CEF)
#include <QPointer>
#include <QTimer>
#include "ui/MainWindow.hpp"
#include "ui/PipWindow.hpp"
#endif

#include "ui/Icons.hpp"
#include "cef/MediaPolicy.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

QString color_css(const QColor& color) {
    return popup_detail::css_color(color);
}

// Same intent as BrowserMenu's font pinning: take the application UI font,
// ask for an ordinary sans-serif face with no exotic style strategy, and let
// QFontInfo tell us the family the platform actually resolved. Setting that
// concrete family back on the font keeps the menu off partial fallback faces
// (which rendered 'g' as 'q'). No candidate-family list is consulted.
QFont menu_base_font() {
    static const QFont resolved = [] {
        QFont font(QApplication::font());
        font.setStyleHint(QFont::SansSerif, QFont::PreferDefault);
        font.setStyleStrategy(QFont::PreferDefault);
        const QString family = QFontInfo(font).family();
        if (!family.isEmpty()) font.setFamily(family);
        // Re-assert the hint after the family swap so a later fallback stays
        // sans-serif rather than reverting to the style default.
        font.setStyleHint(QFont::SansSerif, QFont::PreferDefault);
        return font;
    }();
    return resolved;
}

} // namespace

TabContextMenu::TabContextMenu(core::TabManager* manager, int tab_index,
                                 QWidget* parent)
    : QMenu(parent), manager_(manager), index_(tab_index) {
    // Object name scopes the stylesheet below to this menu only, so the rules
    // never leak into submenus of other menus that share this widget tree.
    setObjectName(QStringLiteral("TabContextMenu"));
    build();
    apply_style();
}

// Mirrors BrowserMenu::update_popup_style() / roundedPanelStyle() as closely
// as a native QMenu allows: surface_raised panel, 1px border_subtle hairline,
// popover radius, and rows that use the same accent wash for hover/pressed.
// Set on this widget (not the app stylesheet) with colors resolved from
// current_theme() at construction — the menu is transient per right-click, so
// it always reflects the theme active when it opens.
void TabContextMenu::apply_style() {
    const Theme& theme = current_theme();
    const ThemeColors& c = theme.colors;

    QColor pressed_color(c.accent_soft);
    pressed_color.setAlpha(qMin(255, pressed_color.alpha() + 42));

    // A concrete family in the stylesheet as well, so QMenu's own text and any
    // child labels the platform style creates share the resolved face.
    const QString font_css =
        QStringLiteral(" font-family: \"%1\"; font-size: %2px;")
            .arg(menu_base_font().family())
            .arg(theme.body_font_px);
    setFont(menu_base_font());

    QString style =
        QStringLiteral(
            "QMenu#TabContextMenu {"
            " background: %1; color: %2;"
            " border: %3px solid %4;"
            " border-radius: %5px;"
            " padding: %6px;"
            " margin: 2px;"  // room for the rounded corners to render
            "%15 }"
            "QMenu#TabContextMenu::item {"
            " background: transparent; color: %2;"
            " padding: %6px %7px %6px %8px;"  // left: icon column, right: shortcut
            " margin: 1px %9px;"
            " border-radius: %10px;"
            " min-width: 180px;"
            "%15 }"
            "QMenu#TabContextMenu::item:selected {"
            " background: %11; color: %12; }"
            "QMenu#TabContextMenu::item:pressed {"
            " background: %13; color: %12; }"
            "QMenu#TabContextMenu::item:disabled {"
            " background: transparent; color: %14; }"
            "QMenu#TabContextMenu::icon { left: %9px; width: 16px;"
            " height: 16px; }"
            "QMenu#TabContextMenu::indicator { left: %9px; width: 14px;"
            " height: 14px; }"
            "QMenu#TabContextMenu::separator {"
            " height: %3px; background: %16; margin: %9px %17px; }"
            "QMenu#TabContextMenu::right-arrow { width: 12px; height: 12px;"
            " right: %17px; }"
            "QMenu#TabContextMenu QLabel { color: %14;%15 }")
            .arg(color_css(c.surface_raised), color_css(c.text))
            .arg(Metrics::hairline)
            .arg(color_css(c.border_subtle))
            .arg(Radius::popover)
            .arg(Spacing::xs + Spacing::xxs)
            .arg(Spacing::xl)                     // trailing shortcut room
            .arg(Spacing::xl + Spacing::sm)       // leading icon column
            .arg(Spacing::xs)
            .arg(Radius::md)
            .arg(color_css(c.surface_selected), color_css(c.accent))
            .arg(color_css(pressed_color))
            .arg(color_css(c.text_faint))
            .arg(font_css)
            .arg(color_css(c.divider))
            .arg(Spacing::sm);
    setStyleSheet(style);
}

void TabContextMenu::build() {
    const auto& tabs = manager_->tabs();
    if (index_ < 0 || static_cast<size_t>(index_) >= tabs.size()) return;
    const auto& tab = tabs[static_cast<size_t>(index_)];

    const QColor ic = current_theme().colors.text_muted;
    const auto add = [ic](QMenu* m, const QString& title, icons::Name n) {
        auto* a = m->addAction(title);
        a->setIcon(icons::icon(n, ic, 16, 1.0));
        return a;
    };

    auto* reload = add(this, "Reload Tab", icons::Name::Reload);
    connect(reload, &QAction::triggered, this, [this] { emit reload_tab(index_); });

#if defined(AURORA_HAS_CEF)
    auto* pip = add(this, "Picture in Picture", icons::Name::FullscreenExit);
    pip->setToolTip(QStringLiteral("Move this page to a floating window (reloads the page)"));
    QPointer<MainWindow> source = parentWidget()
        ? qobject_cast<MainWindow*>(parentWidget()->window()) : nullptr;
    const auto pip_tab = tab.id; // IDs survive changes while the menu is open.
    pip->setEnabled(PipWindow::can_open(source.data(), pip_tab));
    connect(pip, &QAction::triggered, this, [source, pip_tab] {
        if (!source) return;
        // Unwind the transient menu before detaching its clicked tab.
        QTimer::singleShot(0, source.data(), [source, pip_tab] {
            if (source) PipWindow::open(source.data(), pip_tab);
        });
    });
#endif

    auto* dup = add(this, "Duplicate Tab", icons::Name::Duplicate);
    connect(dup, &QAction::triggered, this, [this] { emit duplicate_tab(index_); });

    addSeparator();

    if (tab.pinned) {
        auto* unpin = add(this, "Unpin Tab", icons::Name::Unpin);
        connect(unpin, &QAction::triggered, this, [this] { emit pin_tab(index_); });
    } else {
        auto* pin = add(this, "Pin Tab", icons::Name::Pin);
        connect(pin, &QAction::triggered, this, [this] { emit pin_tab(index_); });
    }

    if (tab.muted) {
        auto* unmute = add(this, "Unmute Tab", icons::Name::Unmute);
        connect(unmute, &QAction::triggered, this, [this] { emit mute_tab(index_); });
    } else {
        auto* mute = add(this, "Mute Tab", icons::Name::Mute);
        connect(mute, &QAction::triggered, this, [this] { emit mute_tab(index_); });
    }

    if (tab.suspended) {
        auto* restore = add(this, "Restore Tab", icons::Name::Reload);
        connect(restore, &QAction::triggered, this,
                [this] { emit restore_tab(index_); });
    } else {
        auto* suspend = add(this, "Suspend Tab", icons::Name::Stop);
        suspend->setEnabled(tab.id != manager_->active() && !tab.pinned &&
                            !tab.exclude_from_suspension && !tab.audible &&
                            !tab.pending_downloads && !tab.loading &&
                            !tab.crashed &&
                            !cef::is_twitch_page(tab.url.display()) &&
                            (tab.url.scheme() == "http" ||
                             tab.url.scheme() == "https"));
        connect(suspend, &QAction::triggered, this,
                [this] { emit suspend_tab(index_); });
    }
    auto* exclude = add(this, tab.exclude_from_suspension
                                  ? "Allow Auto-Suspend" : "Never Suspend Tab",
                        icons::Name::Pin);
    connect(exclude, &QAction::triggered, this,
            [this] { emit exclude_tab_from_suspension(index_); });

    addSeparator();

    auto* copy = add(this, "Copy URL", icons::Name::Copy);
    connect(copy, &QAction::triggered, this, [this] { emit copy_url(index_); });

    auto* close = add(this, "Close Tab", icons::Name::Close);
    // Display-only hints (scoped to this transient menu so they never
    // collide with MainWindow's application-wide QShortcuts — see
    // BrowserMenu::build() for why the scoping matters).
    close->setShortcut(QKeySequence("Ctrl+W"));
    close->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(close, &QAction::triggered, this, [this] { emit close_tab(index_); });

    addSeparator();

    auto* reopen = add(this, "Reopen Closed Tab", icons::Name::Plus);
    reopen->setShortcut(QKeySequence("Ctrl+Shift+T"));
    reopen->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(reopen, &QAction::triggered, this, &TabContextMenu::reopen_closed);
}

} // namespace aurora::ui

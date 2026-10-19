#include "ui/BrowserMenu.hpp"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QMargins>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/GlowFrame.hpp"
#include "ui/Icons.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace browser_menu_detail {

// A menu row label. It keeps the full string and paints an elided copy of it
// that fits the width the layout actually gave the label, so no label can
// wrap, grow its row, or bleed past the panel border. Elision is re-applied on
// resize and on any font/style change (the panel stylesheet sets font sizes
// after construction), and the size hints stay derived from the FULL text so
// mutating the visible text can never feed back into the layout.
class RowLabel final : public QLabel {
public:
    RowLabel(const QString& text, Qt::Alignment alignment, QWidget* parent)
        : QLabel(parent), full_(text) {
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setAlignment(alignment | Qt::AlignVCenter);
        setTextInteractionFlags(Qt::NoTextInteraction);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        // Ignored horizontally: the label takes the space the row has left
        // instead of forcing the row (and the panel) wider. Vertically it
        // fills the row so the label's own AlignVCenter centres the text.
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
        QLabel::setText(text);
    }

    void setFullText(const QString& text) {
        if (full_ == text) return;
        full_ = text;
        apply_elide();
        updateGeometry();
    }

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return QSize(fm.horizontalAdvance(full_), fm.height());
    }

    QSize minimumSizeHint() const override {
        return QSize(0, QFontMetrics(font()).height());
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        apply_elide();
    }

    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange ||
            event->type() == QEvent::StyleChange) {
            apply_elide();
        }
    }

private:
    void apply_elide() {
        const QFontMetrics fm(font());
        const QString shown =
            fm.elidedText(full_, Qt::ElideRight, qMax(0, width()));
        if (shown != QLabel::text()) QLabel::setText(shown);
    }

    QString full_;
};

} // namespace browser_menu_detail

namespace {

using browser_menu_detail::RowLabel;

// The panel is a fixed-width card; every label elides into it, so the width
// never has to grow for a long string.
constexpr int kPanelWidth = 340;
// One shared row height. Chrome-dense, and 17 rows + chrome still fit under
// the toolbar on a 720p screen, so compaction is only a fallback.
constexpr int kRowHeight = 34;
constexpr int kRowHeightMin = 28;
constexpr int kHeaderHeight = 36;
constexpr int kFooterHeight = kRowHeight + Spacing::xs;
constexpr int kIconBox = 18;
constexpr int kIconPx = 16;
// Separator frame (1px) plus its vertical stylesheet margins (Spacing::xs).
constexpr int kSeparatorHeight = Metrics::hairline + 2 * Spacing::xs;
// The panel is painted like the History page's side panel (GlowFrame card
// style): gradient rim, outer halo and navy glass. The halo is drawn inside
// the panel's own rect, `kGlowExtent` px deep, and the layout keeps the rows
// one pixel clear of the rim on top of that.
constexpr int kPanelRadius = 14;
constexpr int kGlowExtent = 3;
constexpr int kPanelInset = kGlowExtent + 2;
// Transparent border of the popup window around the glowing panel.
constexpr QMargins kShadowMargins(4, 2, 4, 6);

QString color_css(const QColor& color) {
    return popup_detail::css_color(color);
}

QLabel* glyph_label(QWidget* parent, const QString& object_name, int box) {
    auto* label = new QLabel(parent);
    label->setObjectName(object_name);
    label->setFixedSize(box, box);
    label->setAlignment(Qt::AlignCenter);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    return label;
}

} // namespace

// Robust glyph fix: instead of guessing at a candidate family list (which on
// some Windows systems still resolved to a face that substituted 'g' with 'q'),
// let Qt's own font matcher pick the system sans-serif and then pin whatever it
// actually resolved. QFontInfo reports the real family chosen by the font
// engine, so the family we apply is guaranteed to exist and to carry a complete
// Latin glyph set. No NoFontMerging / bitmap-only strategy is used, so missing
// glyphs can still merge in from fallbacks.
const QFont& BrowserMenu::menu_font() {
    static const QFont resolved = [] {
        QFont font = QApplication::font();
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

BrowserMenu::BrowserMenu(QWidget* parent)
    : IconButton(icons::Name::More, parent) {
    setObjectName(QStringLiteral("BrowserMenuButton"));
    setToolTip(QStringLiteral("Menu"));
    setPopupMode(QToolButton::DelayedPopup);

    build_actions();
    build_popup();
}

void BrowserMenu::build_actions() {
    // Shortcuts here are display hints while the panel is open. MainWindow's
    // application-context shortcuts remain the single owners of the bindings.
    const auto make = [this](const QString& text, const char* shortcut,
                             icons::Name icon, auto signal) {
        auto* action = new QAction(text, this);
        if (shortcut && *shortcut) {
            action->setShortcut(QKeySequence(QString::fromLatin1(shortcut)));
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        }
        connect(action, &QAction::triggered, this, signal);
        entries_.append(Entry{action, icon});
        return action;
    };
    const auto separator = [this] { entries_.append(Entry{}); };

    make(QStringLiteral("New Tab"), "Ctrl+T", icons::Name::NewTab,
         &BrowserMenu::new_tab_requested);
    make(QStringLiteral("New Window"), "Ctrl+N", icons::Name::NewTab,
         &BrowserMenu::new_window_requested);
    make(QStringLiteral("New Incognito Window"), "Ctrl+Shift+N",
         icons::Name::NewTab, &BrowserMenu::new_incognito_window_requested);
    make(QStringLiteral("New Tor Incognito Window"), "Ctrl+Alt+Shift+N",
         icons::Name::Lock, &BrowserMenu::new_tor_window_requested);
    separator();

    make(QStringLiteral("History"), "Ctrl+H", icons::Name::History,
         &BrowserMenu::history_requested);
    make(QStringLiteral("Downloads"), "Ctrl+J", icons::Name::Downloads,
         &BrowserMenu::downloads_requested);
    make(QStringLiteral("Bookmarks"), nullptr, icons::Name::Bookmarks,
         &BrowserMenu::bookmarks_requested);
    make(QStringLiteral("Bookmark This Page"), "Ctrl+D",
         icons::Name::StarOutline, &BrowserMenu::bookmark_page_requested);
    // Label flips with the window's state (see set_bookmark_bar_visible); the
    // default text matches the default-on preference.
    bookmark_bar_act_ =
        make(QStringLiteral("Hide Bookmarks Bar"), nullptr,
             icons::Name::Bookmark, &BrowserMenu::bookmark_bar_toggled);
    separator();

    make(QStringLiteral("Zoom In"), "Ctrl++", icons::Name::ZoomIn,
         &BrowserMenu::zoom_in_requested);
    make(QStringLiteral("Zoom Out"), "Ctrl+-", icons::Name::ZoomOut,
         &BrowserMenu::zoom_out_requested);
    make(QStringLiteral("Reset Zoom"), "Ctrl+0", icons::Name::ZoomOut,
         &BrowserMenu::zoom_reset_requested);
    separator();

    make(QStringLiteral("Find in Page"), "Ctrl+F", icons::Name::Search,
         &BrowserMenu::find_requested);
    make(QStringLiteral("Print…"), "Ctrl+P", icons::Name::FileDocument,
         &BrowserMenu::print_requested);
    make(QStringLiteral("Save as PDF…"), nullptr, icons::Name::FileDocument,
         &BrowserMenu::save_as_pdf_requested);
    make(QStringLiteral("Reader Mode"), "Ctrl+Alt+R", icons::Name::FileDocument,
         &BrowserMenu::reader_mode_requested);
    make(QStringLiteral("Night Mode for This Site"), "Ctrl+Alt+N", icons::Name::Moon,
         &BrowserMenu::night_mode_toggled);
    make(QStringLiteral("Screenshot: Visible Area"), "Ctrl+Shift+S", icons::Name::FileImage,
         &BrowserMenu::screenshot_visible_requested);
    make(QStringLiteral("Screenshot: Full Page"), "Ctrl+Alt+Shift+S", icons::Name::FileImage,
         &BrowserMenu::screenshot_full_page_requested);
    make(QStringLiteral("Screenshot: Select Region…"), "Ctrl+Alt+S", icons::Name::FileImage,
         &BrowserMenu::screenshot_region_requested);
    make(QStringLiteral("Share Page as QR Code…"), nullptr, icons::Name::Copy,
         &BrowserMenu::share_qr_requested);
    make(QStringLiteral("Fullscreen"), "F11", icons::Name::Fullscreen,
         &BrowserMenu::fullscreen_requested);
    make(QStringLiteral("Developer Tools"), "Ctrl+Shift+I", icons::Name::Code,
         &BrowserMenu::devtools_requested);
    make(QStringLiteral("Extensions"), nullptr, icons::Name::Puzzle,
         &BrowserMenu::extensions_requested);
    make(QStringLiteral("Site Settings"), nullptr, icons::Name::Shield,
         &BrowserMenu::site_settings_requested);
    separator();

    theme_act_ = make(QStringLiteral("Switch to Dark Theme"), nullptr,
                      icons::Name::Moon, &BrowserMenu::theme_toggled);
    separator();

    make(QStringLiteral("Clear Browsing Data"), nullptr, icons::Name::Trash,
         &BrowserMenu::clear_browsing_data_requested);

    // Settings is not part of the list: it is the visually separated footer.
    settings_act_ = new QAction(QStringLiteral("Settings"), this);
    connect(settings_act_, &QAction::triggered, this,
            &BrowserMenu::settings_requested);
}

void BrowserMenu::build_popup() {
    // Match DownloadsPopover's proven native-window path: the popup is owned
    // by the browser window and uses Qt::Popup mouse capture. A Qt::Tool owned
    // by this tiny button can paint above CEF while pointer input still lands
    // in CEF, producing the page's I-beam cursor and dead rows.
    popup_ = new QWidget(window());
    popup_->setObjectName(QStringLiteral("BrowserMenuPopup"));
    popup_->setAttribute(Qt::WA_TranslucentBackground);
    popup_->setAttribute(Qt::WA_NoSystemBackground);
    popup_->setAttribute(Qt::WA_StyledBackground, true);
    popup_->setAutoFillBackground(false);
    popup_->setFocusPolicy(Qt::StrongFocus);
    // Children inherit this, and every row/label additionally gets setFont()
    // applied directly, so no widget can fall back to an unresolved face.
    popup_->setFont(menu_font());
    popup_->setFixedWidth(kPanelWidth + kShadowMargins.left() +
                          kShadowMargins.right());

    PopupPlacement placement;
    placement.mode = Qt::Popup;
    placement.clickOutsideToClose = true;
    placement.anchor = this;
    placement.apply(popup_);

    auto* root = new QVBoxLayout(popup_);
    root->setContentsMargins(kShadowMargins);
    root->setSpacing(0);

    // Same glass card as the History page's panel; GlowFrame paints the rim,
    // halo and plate itself, so no stylesheet background or drop shadow.
    auto* glow = new GlowFrame(popup_);
    glow->setObjectName(QStringLiteral("BrowserMenuPanel"));
    glow->apply_menu_style(current_theme().is_dark,
                           current_theme().colors.surface_raised,
                           kPanelRadius, kGlowExtent);
    panel_ = glow;
    root->addWidget(panel_);

    auto* panel_layout = new QVBoxLayout(panel_);
    panel_layout->setContentsMargins(kPanelInset, kPanelInset, kPanelInset,
                                     kPanelInset);
    panel_layout->setSpacing(0);

    // --- Header: title + close ------------------------------------------
    header_ = new QWidget(panel_);
    header_->setObjectName(QStringLiteral("BrowserMenuHeader"));
    header_->setAttribute(Qt::WA_StyledBackground, true);
    header_->setFixedHeight(kHeaderHeight);
    auto* header_layout = new QHBoxLayout(header_);
    header_layout->setContentsMargins(Spacing::md, 0, Spacing::sm, 0);
    header_layout->setSpacing(Spacing::sm);

    auto* title = new RowLabel(QStringLiteral("Menu"), Qt::AlignLeft, header_);
    title->setObjectName(QStringLiteral("BrowserMenuTitle"));
    title->setFont(menu_font());
    header_layout->addWidget(title, 1);

    auto* close_button = new IconButton(icons::Name::Close, header_);
    close_button->setObjectName(QStringLiteral("PopoverCloseButton"));
    close_button->setToolTip(QStringLiteral("Close"));
    close_button->setFixedSize(22, 22);
    close_button->setIconPx(12);
    connect(close_button, &IconButton::clicked, popup_, &QWidget::close);
    header_layout->addWidget(close_button, 0, Qt::AlignVCenter);
    panel_layout->addWidget(header_);

    // --- Body: the action rows -------------------------------------------
    list_host_ = new QWidget(panel_);
    list_host_->setObjectName(QStringLiteral("BrowserMenuList"));
    list_layout_ = new QVBoxLayout(list_host_);
    list_layout_->setContentsMargins(Spacing::xs, Spacing::xs, Spacing::xs,
                                     Spacing::xs);
    list_layout_->setSpacing(0);
    panel_layout->addWidget(list_host_);
    build_rows();

    // --- Footer: Settings -------------------------------------------------
    footer_ = new QPushButton(panel_);
    footer_->setObjectName(QStringLiteral("BrowserMenuFooter"));
    footer_->setCursor(Qt::PointingHandCursor);
    footer_->setFocusPolicy(Qt::NoFocus);
    footer_->setAccessibleName(QStringLiteral("Settings"));
    footer_->setFixedHeight(kFooterHeight);
    footer_->setFont(menu_font());
    footer_->setAttribute(Qt::WA_StyledBackground, true);
    auto* footer_layout = new QHBoxLayout(footer_);
    footer_layout->setContentsMargins(Spacing::sm, 0, Spacing::md, 0);
    footer_layout->setSpacing(Spacing::sm);

    footer_icon_ =
        glyph_label(footer_, QStringLiteral("BrowserMenuFooterIcon"), kIconBox);
    footer_layout->addWidget(footer_icon_, 0, Qt::AlignVCenter);

    auto* footer_text =
        new RowLabel(settings_act_->text(), Qt::AlignLeft, footer_);
    footer_text->setObjectName(QStringLiteral("BrowserMenuFooterText"));
    footer_text->setFont(menu_font());
    footer_layout->addWidget(footer_text, 1);

    footer_arrow_ =
        glyph_label(footer_, QStringLiteral("BrowserMenuFooterArrow"), kIconBox);
    footer_layout->addWidget(footer_arrow_, 0, Qt::AlignVCenter);

    connect(footer_, &QPushButton::clicked, this, [this] {
        popup_->close();
        settings_act_->trigger();
    });
    panel_layout->addWidget(footer_);

    refresh_icons();
    update_popup_style();
}

void BrowserMenu::build_rows() {
    // Rows are built exactly once, so there is no teardown path that could
    // leave a detached child painting a stale fragment over the new list.
    const Theme& theme = current_theme();
    QFont text_font(menu_font());
    if (theme.body_font_px > 0) text_font.setPixelSize(theme.body_font_px);
    QFont shortcut_font(menu_font());
    if (theme.small_font_px > 0)
        shortcut_font.setPixelSize(theme.small_font_px + 1);

    for (const Entry& entry : entries_) {
        if (!entry.action) {
            auto* separator = new QFrame(list_host_);
            separator->setObjectName(QStringLiteral("BrowserMenuSeparator"));
            separator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            list_layout_->addWidget(separator);
            continue;
        }

        QAction* action = entry.action;
        auto* button = new QPushButton(list_host_);
        button->setObjectName(QStringLiteral("BrowserMenuRow"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(action->toolTip());
        button->setAccessibleName(action->text());
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setFixedHeight(kRowHeight);
        button->setFont(menu_font());
        // The row is the clip boundary for its labels: children are clipped to
        // the parent's rect, so nothing can paint outside the row, and the
        // layout may not grow the row beyond its fixed height.
        button->setAttribute(Qt::WA_StyledBackground, true);

        auto* layout = new QHBoxLayout(button);
        layout->setContentsMargins(Spacing::sm, 0, Spacing::md, 0);
        layout->setSpacing(Spacing::sm);
        layout->setSizeConstraint(QLayout::SetNoConstraint);

        auto* icon = glyph_label(
            button, QStringLiteral("BrowserMenuRowIcon"), kIconBox);
        layout->addWidget(icon, 0, Qt::AlignVCenter);

        auto* text = new RowLabel(action->text(), Qt::AlignLeft, button);
        text->setObjectName(QStringLiteral("BrowserMenuRowText"));
        text->setFont(text_font);
        layout->addWidget(text, 1);

        if (!action->shortcut().isEmpty()) {
            auto* shortcut = new RowLabel(
                action->shortcut().toString(QKeySequence::NativeText),
                Qt::AlignRight, button);
            shortcut->setObjectName(QStringLiteral("BrowserMenuShortcut"));
            shortcut->setFont(shortcut_font);
            // Natural width, but never more than a third of the panel: past
            // that the hint elides instead of squeezing the label.
            shortcut->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Preferred);
            shortcut->setMaximumWidth(kPanelWidth / 3);
            layout->addWidget(shortcut, 0, Qt::AlignVCenter);
        }

        connect(button, &QPushButton::clicked, this, [this, action] {
            popup_->close();
            action->trigger();
        });
        // Keeps the row in sync with the action's state (the theme row's label
        // flips between "Switch to Dark/Light Theme"). The row is the context,
        // so the connection dies with the row.
        connect(action, &QAction::changed, button, [action, button, text] {
            text->setFullText(action->text());
            button->setAccessibleName(action->text());
            button->setToolTip(action->toolTip());
        });

        rows_.append(Row{action, entry.icon, button, icon, text});
        list_layout_->addWidget(button);
    }
}

// Row height that lets the whole panel (header + every row + separators +
// footer) fit in the space available beside the button, so no option is ever
// clipped off the bottom. The fixed height is used unless the panel genuinely
// does not fit, in which case rows tighten down to kRowHeightMin.
int BrowserMenu::resolve_row_height() const {
    int rows = 0;
    int separators = 0;
    for (const Entry& entry : entries_) {
        if (entry.action) {
            ++rows;
        } else {
            ++separators;
        }
    }
    if (rows <= 0) return kRowHeight;

    const int chrome = kHeaderHeight + kFooterHeight +
                       separators * kSeparatorHeight +
                       2 * Spacing::xs /* list margins */ +
                       2 * kPanelInset /* glow rim + halo */;
    const int available =
        PopupPlacement::availablePanelHeightFromAnchor(this, Spacing::xs);
    if (available <= 0) return kRowHeight;
    return qBound(kRowHeightMin, (available - chrome) / rows, kRowHeight);
}

void BrowserMenu::apply_row_height(int height) {
    for (const Row& row : rows_) {
        if (row.button) row.button->setFixedHeight(height);
    }
}

void BrowserMenu::refresh_icons() {
    const QColor color = current_theme().colors.text_muted;
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    for (const Row& row : rows_) {
        // The theme row is the only one whose glyph depends on runtime state.
        const icons::Name name =
            row.action == theme_act_
                ? (dark_mode_ ? icons::Name::Sun : icons::Name::Moon)
                : row.icon;
        if (row.action) {
            row.action->setIcon(icons::icon(name, color, kIconPx, dpr));
        }
        if (row.icon_label) {
            row.icon_label->setPixmap(
                icons::pixmap(name, kIconPx, color, dpr));
        }
    }
    if (settings_act_) {
        settings_act_->setIcon(
            icons::icon(icons::Name::Settings, color, kIconPx, dpr));
    }
    if (footer_icon_) {
        footer_icon_->setPixmap(
            icons::pixmap(icons::Name::Settings, kIconPx, color, dpr));
    }
    if (footer_arrow_) {
        footer_arrow_->setPixmap(
            icons::pixmap(icons::Name::OpenExternal, 14, color, dpr));
    }
}

void BrowserMenu::update_popup_style() {
    if (!popup_) return;
    const Theme& theme = current_theme();
    const ThemeColors& c = theme.colors;

    const QString hover = color_css(c.accent_soft);
    QColor pressed_color(c.accent_soft);
    pressed_color.setAlpha(qMin(255, pressed_color.alpha() + 42));
    const QString pressed = color_css(pressed_color);
    const QString divider = color_css(c.divider);
    const QString text_color = color_css(c.text);

    // Same family the widgets already carry via setFont(), stated explicitly so
    // the stylesheet cannot re-resolve text to a different (glyph-substituting)
    // face. QFontInfo gives the family the font engine actually selected.
    const QString family = QFontInfo(menu_font()).family();
    const QString font_css =
        family.isEmpty() ? QString()
                         : QStringLiteral(" font-family: \"%1\";").arg(family);

    QString style = QStringLiteral(
        "QWidget#BrowserMenuPopup { background: transparent; border: none; }");
    // The plate is painted by the GlowFrame (History panel look).
    style += QStringLiteral(
        "QFrame#BrowserMenuPanel { background: transparent; border: none; }");
    if (auto* glow = qobject_cast<GlowFrame*>(panel_)) {
        glow->apply_menu_style(theme.is_dark, c.surface_raised, kPanelRadius,
                               kGlowExtent);
    }
    style += QStringLiteral(
                 "QWidget#BrowserMenuHeader { background: transparent;"
                 " border: none; border-bottom: %1px solid %2; }")
                 .arg(Metrics::hairline)
                 .arg(divider);
    style += QStringLiteral(
                 "QLabel#BrowserMenuTitle { background: transparent;"
                 " color: %1; font-size: %2px; font-weight: 600;%3 }")
                 .arg(text_color)
                 .arg(theme.title_font_px)
                 .arg(font_css);
    style += QStringLiteral(
                 "QToolButton#PopoverCloseButton { background: transparent;"
                 " border: none; border-radius: %1px; padding: 0px; }"
                 "QToolButton#PopoverCloseButton:hover { background: %2; }"
                 "QToolButton#PopoverCloseButton:pressed { background: %3; }")
                 .arg(Radius::sm)
                 .arg(hover, pressed);
    style += QStringLiteral(
                 "QWidget#BrowserMenuList { background: transparent;"
                 " border: none; }"
                 "QPushButton#BrowserMenuRow { background: transparent;"
                 " border: none; border-radius: %1px; padding: 0px;"
                 " text-align: left; }"
                 "QPushButton#BrowserMenuRow:hover { background: %2; }"
                 "QPushButton#BrowserMenuRow:pressed { background: %3; }")
                 .arg(Radius::md)
                 .arg(hover, pressed);
    style += QStringLiteral(
                 "QLabel#BrowserMenuRowText, QLabel#BrowserMenuFooterText {"
                 " background: transparent; color: %1; font-size: %2px;%3 }")
                 .arg(text_color)
                 .arg(theme.body_font_px)
                 .arg(font_css);
    style += QStringLiteral(
                 "QLabel#BrowserMenuShortcut { background: transparent;"
                 " color: %1; font-size: %2px; padding: 0px;%3 }")
                 .arg(color_css(c.text_faint))
                 .arg(theme.small_font_px + 1)
                 .arg(font_css);
    style += QStringLiteral(
                 "QFrame#BrowserMenuSeparator { background: %1; border: none;"
                 " min-height: %2px; max-height: %2px; margin: %3px %4px; }")
                 .arg(divider)
                 .arg(Metrics::hairline)
                 .arg(Spacing::xs)
                 .arg(Spacing::sm);
    style += QStringLiteral(
                 "QPushButton#BrowserMenuFooter { background: transparent;"
                 " border: none; border-top: %1px solid %2;"
                 " border-radius: 0px; padding: 0px; text-align: left; }"
                 "QPushButton#BrowserMenuFooter:hover { background: %3;"
                 " border-bottom-left-radius: %5px;"
                 " border-bottom-right-radius: %5px; }"
                 "QPushButton#BrowserMenuFooter:pressed { background: %4; }")
                 .arg(Metrics::hairline)
                 .arg(divider)
                 .arg(hover, pressed)
                 .arg(kPanelRadius - kPanelInset);
    popup_->setStyleSheet(style);
}

void BrowserMenu::toggle_popup() {
    if (!popup_) return;
    if (popup_->isVisible()) {
        popup_->close();
        return;
    }

    refresh_icons();
    update_popup_style();
    apply_row_height(resolve_row_height());

    int width = kPanelWidth + kShadowMargins.left() + kShadowMargins.right();
    if (QScreen* target_screen = screen()) {
        width = qMin(width, qMax(1, target_screen->availableGeometry().width() -
                                        2 * Spacing::sm));
    }
    popup_->setFixedWidth(width);

    // Ask the layouts for the full content height instead of QWidget::
    // adjustSize(), which caps a top-level window at 2/3 of the screen and
    // therefore cut the bottom rows out of the panel.
    int height = 0;
    if (QLayout* root = popup_->layout()) {
        root->activate();
        height = root->totalSizeHint().height();
    }
    height = qMax(height, popup_->sizeHint().height());
    const QSize popup_size(width, qMax(1, height));
    popup_->resize(popup_size);
    popup_->setGeometry(PopupPlacement::popupGeometryFromAnchor(
        this, popup_size, /*alignRight=*/true, Spacing::xs, kShadowMargins));
    // Same show path as DownloadsPopover: Qt::Popup takes mouse/keyboard
    // ownership; forcing activateWindow() on the former tool window was not a
    // substitute for native popup capture and could race the CEF child.
    popup_->show();
    popup_->raise();
}

void BrowserMenu::set_bookmark_bar_visible(bool visible) {
    if (bookmark_bar_act_) {
        // The row label follows through the QAction::changed connection.
        bookmark_bar_act_->setText(visible
                                       ? QStringLiteral("Hide Bookmarks Bar")
                                       : QStringLiteral("Show Bookmarks Bar"));
    }
}

void BrowserMenu::set_dark_mode(bool dark) {
    dark_mode_ = dark;
    if (theme_act_) {
        // The row label follows through the QAction::changed connection.
        theme_act_->setText(dark ? QStringLiteral("Switch to Light Theme")
                                 : QStringLiteral("Switch to Dark Theme"));
    }
    refresh_icons();
    update_popup_style();
}

void BrowserMenu::showEvent(QShowEvent* event) {
    IconButton::showEvent(event);
    refresh_icons();
}

} // namespace aurora::ui

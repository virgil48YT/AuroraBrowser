#pragma once

#include <QFont>
#include <QList>

#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"

class QAction;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace aurora::ui {

namespace browser_menu_detail {
// Plain-text, non-wrapping label that elides itself to the width the layout
// hands it, so a long label can never wrap or bleed past its row. Defined in
// BrowserMenu.cpp; only pointers to it are needed here.
class RowLabel;
} // namespace browser_menu_detail

// The browser ("hamburger"/more) button and its Chrome-style anchored panel.
//
// Structure: a Qt::Popup window owned by the browser window holds a rounded
// panel with three parts — a small header (title + close), the action list,
// and the visually separated Settings footer. The list is built ONCE from a
// static entry table; opening the panel only refreshes icons, style and the
// shared row height, so there is no teardown/rebuild path that could leave
// stale rows painting.
class BrowserMenu : public IconButton {
    Q_OBJECT

public:
    explicit BrowserMenu(QWidget* parent);

    void set_dark_mode(bool dark);
    // Keeps the bookmarks-bar row's label in sync with the window's state, the
    // same way set_dark_mode() keeps the theme row honest.
    void set_bookmark_bar_visible(bool visible);

public slots:
    // Toggle the panel beneath this button. The visible panel's right edge is
    // aligned with the button's right edge and clamped to the current screen.
    void toggle_popup();

signals:
    void new_tab_requested();
    void new_window_requested();
    void history_requested();
    void downloads_requested();
    void bookmarks_requested();
    // Bookmark (or un-bookmark) the page in the active tab (Ctrl+D).
    void bookmark_page_requested();
    // Show/hide the bookmarks bar under the toolbar.
    void bookmark_bar_toggled();
    // Open aurora://extensions.
    void extensions_requested();
    void zoom_in_requested();
    void zoom_out_requested();
    void zoom_reset_requested();
    void find_requested();
    void print_requested();
    void fullscreen_requested();
    void devtools_requested();
    void theme_toggled();
    void clear_browsing_data_requested();
    void settings_requested();

protected:
    void showEvent(QShowEvent* e) override;

private:
    // The single font used by every widget in the panel. Resolved once through
    // Qt's own font matching (QFont::SansSerif + QFontInfo) so the family is a
    // real installed face with a complete glyph set -- no candidate guessing.
    static const QFont& menu_font();

    // One line of the panel body, in display order. A null action is a
    // separator. This table is the single source of truth for labels,
    // shortcuts, icons and grouping.
    struct Entry {
        QAction* action = nullptr;
        icons::Name icon = icons::Name::Settings;
    };

    // A built body row: the clickable button and the widgets it owns.
    struct Row {
        QAction* action = nullptr;
        icons::Name icon = icons::Name::Settings;
        QPushButton* button = nullptr;
        QLabel* icon_label = nullptr;
        browser_menu_detail::RowLabel* text = nullptr;
    };

    void build_actions();
    void build_popup();
    // Builds every row of `entries_` once, at construction time.
    void build_rows();
    // Row height that lets header + every row + separators + footer fit beside
    // the button; the fixed height unless the panel would run off-screen.
    int resolve_row_height() const;
    void apply_row_height(int height);
    void refresh_icons();
    void update_popup_style();

    QWidget* popup_ = nullptr;
    QWidget* panel_ = nullptr;
    QWidget* header_ = nullptr;
    QPushButton* footer_ = nullptr;
    QLabel* footer_icon_ = nullptr;
    QLabel* footer_arrow_ = nullptr;
    QWidget* list_host_ = nullptr;
    QVBoxLayout* list_layout_ = nullptr;
    QList<Entry> entries_;
    QList<Row> rows_;
    // The actions whose state is read back after construction.
    QAction* theme_act_ = nullptr;
    QAction* bookmark_bar_act_ = nullptr;
    QAction* settings_act_ = nullptr;
    bool dark_mode_ = false;
};

} // namespace aurora::ui

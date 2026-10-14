// Aurora — bookmarks bar.
//
// The compact row of saved pages that sits directly under the toolbar, inside
// the same chrome plate (#ChromeTop) as the tab strip and the omnibox, so it
// reads as the last band of the chrome rather than as content.
//
// Data comes from BookmarkService. The bar shows the children of ONE folder —
// a root-level folder titled "Bookmarks bar", found by title among the root's
// children and seeded on first use. That keeps the bar a normal part of the
// user's tree (visible and editable on aurora://bookmarks) instead of a second,
// hidden storage location.
//
// The bar owns no navigation: it reports what the user picked through
// open_requested() and lets MainWindow decide between the current tab and a new
// one, exactly like the internal pages do.
#pragma once

#include <QString>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "bookmarks/BookmarkService.hpp"
#include "core/Url.hpp"

class QHBoxLayout;
class QLabel;
class QMenu;
class QToolButton;

namespace aurora::ui {

class BookmarkBar : public QWidget {
    Q_OBJECT
public:
    // `service` is borrowed and may be null: a window without a profile still
    // builds the bar, it just stays empty (and says so), mirroring the way the
    // internal pages handle a missing profile.
    explicit BookmarkBar(bookmarks::BookmarkService* service,
                         QWidget* parent = nullptr);

    // Re-read the bar folder and rebuild the buttons. Cheap enough to call
    // after every bookmark mutation; the bar holds a handful of rows.
    void reload();

    // Add a page to the bar folder (used by the empty-area context menu, which
    // asks MainWindow for the active tab's URL/title). Returns false when the
    // URL is empty or the insert failed. Reloads on success.
    bool add_page(const QString& title, const QString& url);

signals:
    // The user picked a bookmark. `new_tab` is true for middle-click and for
    // the "Open in new tab" context-menu action.
    void open_requested(const core::Url& url, bool new_tab);
    // Empty-area context menu: bookmark whatever the active tab is showing.
    // MainWindow owns the current URL/title, so it calls add_page() back.
    void add_current_page_requested();
    // Empty-area context menu: open aurora://bookmarks.
    void manage_requested();
    // One-shot confirmations for the window's status bubble.
    void status_message(const QString& text);

protected:
    // Overflow is width-driven, so the visible set is recomputed whenever the
    // window is resized.
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    // Right-click on bare bar background (the buttons handle their own menus).
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    // A built row: the node it came from and the button showing it.
    struct Entry {
        bookmarks::BookmarkNode node;
        QToolButton* button = nullptr;
    };

    // The id of the "Bookmarks bar" folder, creating it when it does not exist
    // yet. Returns 0 when there is no service (or the insert failed), which
    // every caller treats as "nothing to show".
    int64_t resolve_bar_folder();
    // Drop every button and rebuild from `service_->children(bar_folder_id_)`.
    void rebuild();
    // Show as many buttons as fit and collapse the rest into the trailing
    // overflow menu. Pure layout: never touches the service.
    void apply_overflow();
    // Populate `menu` with `parent_id`'s children (folders recurse as
    // submenus). Depth-capped: a corrupt parent_id cycle must not recurse
    // forever, the same guard aurora://bookmarks uses.
    void fill_menu(QMenu* menu, int64_t parent_id, int depth);
    // The dropdown for a folder button.
    void show_folder_menu(const Entry& entry);
    // Per-item context menu (open / open in new tab / edit / delete).
    void show_item_menu(const Entry& entry, const QPoint& global_pos);
    // Edit + delete, shared by the item menu. Both reload the bar.
    void edit_entry(const Entry& entry);
    void delete_entry(const Entry& entry);
    void activate(const Entry& entry, bool new_tab);

    bookmarks::BookmarkService* service_ = nullptr;
    QHBoxLayout* layout_ = nullptr;
    // Trailing "»" button, shown only while something is hidden.
    QToolButton* overflow_btn_ = nullptr;
    // Shown instead of buttons while the bar folder is empty, so a visible but
    // blank band never looks like a rendering bug.
    QLabel* hint_ = nullptr;
    std::vector<Entry> entries_;
    int64_t bar_folder_id_ = 0;
};

} // namespace aurora::ui

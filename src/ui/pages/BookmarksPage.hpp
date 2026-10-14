// Aurora — bookmarks page (aurora://bookmarks).
//
// Shows the real bookmark tree from BookmarkService (folders expand, order
// follows the stored `position`) with add-folder, rename, delete and open.
// Double-clicking a bookmark navigates the active tab.
#pragma once

#include <QString>
#include <cstdint>

#include "ui/pages/InternalPageWidget.hpp"

class QTreeWidget;
class QTreeWidgetItem;

namespace aurora::bookmarks { class BookmarkService; }

namespace aurora::ui {

class BookmarksPage : public InternalPageWidget {
    Q_OBJECT
public:
    explicit BookmarksPage(bookmarks::BookmarkService* service,
                           QWidget* parent = nullptr);

    void refresh() override;

private:
    // Recursively fill `parent_item` with the children of `parent_id`.
    void populate(QTreeWidgetItem* parent_item, int64_t parent_id, int depth);
    void open_selected();
    void rename_selected();
    void delete_selected();
    void new_folder();
    void import_bookmarks();
    void export_bookmarks();
    // Re-apply the site icon to `item` and its descendants whose host matches
    // `host` (empty host = every row). Used when the shared favicon cache
    // finishes a fetch, so icons appear without rebuilding the tree.
    void apply_icons(QTreeWidgetItem* item, const QString& host);
    // The selected row's node id, or 0 when nothing is selected.
    int64_t selected_id() const;
    bool selected_is_folder() const;

    bookmarks::BookmarkService* service_ = nullptr;
    QTreeWidget* tree_ = nullptr;
};

} // namespace aurora::ui

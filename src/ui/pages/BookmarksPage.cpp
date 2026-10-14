#include "ui/pages/BookmarksPage.hpp"

#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>

#include <QStyle>

#include "bookmarks/BookmarkIo.hpp"
#include "bookmarks/BookmarkService.hpp"
#include "ui/FaviconCache.hpp"

namespace aurora::ui {

namespace {

enum Column { ColTitle = 0, ColUrl = 1 };

constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kUrlRole = Qt::UserRole + 2;
constexpr int kFolderRole = Qt::UserRole + 3;

// The bookmark tree is user-created, but a corrupt parent_id cycle in the
// database must not send us into infinite recursion.
constexpr int kMaxDepth = 32;

// Root folder id in the schema (migration 2 seeds row id = 0).
constexpr int64_t kRootId = 0;

} // namespace

BookmarksPage::BookmarksPage(bookmarks::BookmarkService* service, QWidget* parent)
    : InternalPageWidget(QStringLiteral("Bookmarks"), parent), service_(service) {
    toolbar_row()->addStretch(1);

    auto* import_btn = new QPushButton(QStringLiteral("Import…"), this);
    import_btn->setObjectName("PageButton");
    import_btn->setToolTip(QStringLiteral("Import bookmark HTML into the selected folder or the root"));
    import_btn->setEnabled(service_ != nullptr);
    toolbar_row()->addWidget(import_btn);

    auto* export_btn = new QPushButton(QStringLiteral("Export…"), this);
    export_btn->setObjectName("PageButton");
    export_btn->setToolTip(QStringLiteral("Export all bookmarks as Netscape HTML"));
    export_btn->setEnabled(service_ != nullptr);
    toolbar_row()->addWidget(export_btn);

    auto* open_btn = new QPushButton(QStringLiteral("Open"), this);
    open_btn->setObjectName("PageButton");
    toolbar_row()->addWidget(open_btn);

    auto* folder_btn = new QPushButton(QStringLiteral("New folder"), this);
    folder_btn->setObjectName("PageButton");
    toolbar_row()->addWidget(folder_btn);

    auto* rename_btn = new QPushButton(QStringLiteral("Rename"), this);
    rename_btn->setObjectName("PageButton");
    toolbar_row()->addWidget(rename_btn);

    auto* delete_btn = new QPushButton(QStringLiteral("Delete"), this);
    delete_btn->setObjectName("PageDangerButton");
    toolbar_row()->addWidget(delete_btn);

    set_subtitle(QStringLiteral(
        "Your saved pages and folders. Double-click a bookmark to open it."));

    tree_ = new QTreeWidget(this);
    tree_->setObjectName("PageTree");
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("URL")});
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(ColUrl, QHeaderView::Stretch);
    set_body(tree_);

    connect(import_btn, &QPushButton::clicked, this, &BookmarksPage::import_bookmarks);
    connect(export_btn, &QPushButton::clicked, this, &BookmarksPage::export_bookmarks);
    connect(open_btn, &QPushButton::clicked, this, &BookmarksPage::open_selected);
    connect(folder_btn, &QPushButton::clicked, this, &BookmarksPage::new_folder);
    connect(rename_btn, &QPushButton::clicked, this,
            &BookmarksPage::rename_selected);
    connect(delete_btn, &QPushButton::clicked, this,
            &BookmarksPage::delete_selected);
    // Site icons fill in asynchronously from the shared favicon cache.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString& host) {
                for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
                    apply_icons(tree_->topLevelItem(i), host);
                }
            });
    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item) return;
                if (item->data(ColTitle, kFolderRole).toBool()) {
                    item->setExpanded(!item->isExpanded());
                    return;
                }
                const auto url = item->data(ColTitle, kUrlRole).toString();
                if (!url.isEmpty()) emit open_url_requested(url);
            });
}

void BookmarksPage::refresh() {
    tree_->clear();
    if (!service_) {
        set_empty_state(true,
                        QStringLiteral("No profile is attached to this window, "
                                       "so bookmarks are unavailable."));
        return;
    }
    populate(nullptr, kRootId, 0);
    const bool empty = tree_->topLevelItemCount() == 0;
    set_empty_state(empty,
                    QStringLiteral("No bookmarks yet. Pages you bookmark will "
                                   "appear here, and “New folder” starts a "
                                   "collection."));
    tree_->expandToDepth(1);
}

void BookmarksPage::populate(QTreeWidgetItem* parent_item, int64_t parent_id,
                             int depth) {
    if (depth > kMaxDepth) return;
    for (const auto& node : service_->children(parent_id)) {
        auto* item = parent_item ? new QTreeWidgetItem(parent_item)
                                 : new QTreeWidgetItem(tree_);
        const auto title = node.title.empty()
                               ? (node.is_folder ? std::string("Untitled folder")
                                                 : node.url)
                               : node.title;
        item->setText(ColTitle, QString::fromStdString(title));
        item->setText(ColUrl, QString::fromStdString(node.url));
        item->setToolTip(ColUrl, QString::fromStdString(node.url));
        item->setData(ColTitle, kIdRole, QVariant::fromValue(node.id));
        item->setData(ColTitle, kUrlRole, QString::fromStdString(node.url));
        item->setData(ColTitle, kFolderRole, node.is_folder);
        if (node.is_folder) {
            item->setIcon(ColTitle, style()->standardIcon(QStyle::SP_DirIcon));
            populate(item, node.id, depth + 1);
        } else {
            item->setIcon(ColTitle, FaviconCache::instance().icon_for_url(
                                        QString::fromStdString(node.url)));
        }
    }
}

void BookmarksPage::apply_icons(QTreeWidgetItem* item, const QString& host) {
    if (!item) return;
    if (!item->data(ColTitle, kFolderRole).toBool()) {
        const auto url = item->data(ColTitle, kUrlRole).toString();
        if (host.isEmpty() || FaviconCache::host_of(url) == host) {
            item->setIcon(ColTitle,
                          FaviconCache::instance().icon_for_url(url));
        }
    }
    for (int i = 0; i < item->childCount(); ++i) {
        apply_icons(item->child(i), host);
    }
}

int64_t BookmarksPage::selected_id() const {
    auto* item = tree_->currentItem();
    if (!item) return 0;
    return item->data(ColTitle, kIdRole).toLongLong();
}

bool BookmarksPage::selected_is_folder() const {
    auto* item = tree_->currentItem();
    return item && item->data(ColTitle, kFolderRole).toBool();
}

void BookmarksPage::open_selected() {
    auto* item = tree_->currentItem();
    if (!item) return;
    const auto url = item->data(ColTitle, kUrlRole).toString();
    if (url.isEmpty()) {
        emit status_message(QStringLiteral("That folder has no URL to open"));
        return;
    }
    emit open_url_requested(url);
}

void BookmarksPage::rename_selected() {
    if (!service_) return;
    auto* item = tree_->currentItem();
    const auto id = selected_id();
    if (!item || id <= 0) {
        emit status_message(QStringLiteral("Select a bookmark to rename"));
        return;
    }
    bool ok = false;
    const auto title = QInputDialog::getText(
        this, QStringLiteral("Rename bookmark"), QStringLiteral("Name:"),
        QLineEdit::Normal, item->text(ColTitle), &ok);
    if (!ok) return;
    const auto trimmed = title.trimmed();
    if (trimmed.isEmpty()) {
        emit status_message(QStringLiteral("A bookmark name cannot be empty"));
        return;
    }
    const auto url = item->data(ColTitle, kUrlRole).toString();
    if (service_->update(id, trimmed.toStdString(), url.toStdString())) {
        emit status_message(QStringLiteral("Renamed to “%1”").arg(trimmed));
    }
    refresh();
}

void BookmarksPage::delete_selected() {
    if (!service_) return;
    const auto id = selected_id();
    if (id <= 0) {
        emit status_message(QStringLiteral("Select a bookmark to delete"));
        return;
    }
    const bool folder = selected_is_folder();
    const auto answer = QMessageBox::question(
        this, folder ? QStringLiteral("Delete folder")
                     : QStringLiteral("Delete bookmark"),
        folder ? QStringLiteral("Delete this folder and everything inside it?")
               : QStringLiteral("Delete this bookmark?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    if (service_->remove(id)) {
        emit status_message(folder ? QStringLiteral("Folder deleted")
                                   : QStringLiteral("Bookmark deleted"));
    }
    refresh();
}

void BookmarksPage::new_folder() {
    if (!service_) return;
    bool ok = false;
    const auto name = QInputDialog::getText(
        this, QStringLiteral("New folder"), QStringLiteral("Folder name:"),
        QLineEdit::Normal, QStringLiteral("New folder"), &ok);
    if (!ok) return;
    const auto trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        emit status_message(QStringLiteral("A folder name cannot be empty"));
        return;
    }
    // Create inside the selected folder when one is selected, else at the root.
    const int64_t parent = selected_is_folder() ? selected_id() : kRootId;
    if (service_->add(parent, trimmed.toStdString(), "", true) > 0) {
        emit status_message(QStringLiteral("Created folder “%1”").arg(trimmed));
    }
    refresh();
}

void BookmarksPage::import_bookmarks() {
    if (!service_) return;
    const int64_t parent = selected_is_folder() ? selected_id() : kRootId;
    const auto path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import bookmarks"), QString(),
        QStringLiteral("Bookmark HTML (*.html *.htm);;All files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Import bookmarks"), file.errorString());
        return;
    }
    // Exports can contain embedded favicon data, but an accidentally selected
    // multi-gigabyte file must not exhaust the UI process's memory.
    constexpr qint64 max_import_bytes = 128 * 1024 * 1024;
    if (file.size() > max_import_bytes) {
        QMessageBox::warning(this, QStringLiteral("Import bookmarks"),
                            QStringLiteral("This file exceeds the 128 MB import limit."));
        return;
    }
    const auto bytes = file.read(max_import_bytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > max_import_bytes) {
        QMessageBox::warning(this, QStringLiteral("Import bookmarks"),
                            QStringLiteral("The bookmark file could not be read completely."));
        return;
    }
    const auto result = bookmarks::import_netscape_html(
        *service_, std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())), parent);
    refresh();
    const auto message = QStringLiteral(
        "Imported %1 bookmarks and %2 folders. Skipped %3 entries.\n"
        "Found %4 malformed entries (recoverable entries were still imported).")
        .arg(static_cast<qulonglong>(result.bookmarks_imported))
        .arg(static_cast<qulonglong>(result.folders_imported))
        .arg(static_cast<qulonglong>(result.skipped))
        .arg(static_cast<qulonglong>(result.malformed));
    emit status_message(message);
    QMessageBox::information(this, QStringLiteral("Import complete"), message);
}

void BookmarksPage::export_bookmarks() {
    if (!service_) return;
    QFileDialog dialog(this, QStringLiteral("Export bookmarks"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(QStringLiteral("Bookmark HTML (*.html *.htm)"));
    dialog.setDefaultSuffix(QStringLiteral("html"));
    dialog.selectFile(QStringLiteral("bookmarks.html"));
    // Let the dialog add the suffix BEFORE its overwrite confirmation.
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const auto path = dialog.selectedFiles().front();
    // Atomic replacement leaves an existing export intact on write failure.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Export bookmarks"), file.errorString());
        return;
    }
    const auto html = bookmarks::export_netscape_html(*service_);
    const auto size = static_cast<qint64>(html.size());
    if (file.write(html.data(), size) != size || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Export bookmarks"), file.errorString());
        return;
    }
    const auto message = QStringLiteral("Exported %1 bookmarks to %2")
        .arg(static_cast<qulonglong>(service_->all_bookmarks().size())).arg(path);
    emit status_message(message);
    QMessageBox::information(this, QStringLiteral("Export complete"), message);
}

} // namespace aurora::ui

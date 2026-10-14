#include "ui/BookmarkBar.hpp"

#include <QAction>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMargins>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSize>
#include <QToolButton>
#include <functional>

#include "ui/FaviconCache.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

// Geometry. The bar is a chrome band, not content: short, dense, and quiet.
constexpr int kBarHeight = 30;
constexpr int kItemHeight = 24;
constexpr int kIconPx = 16;
// Longest a single item's label may be before it elides. Wide enough for a
// recognisable site name, narrow enough that one long title cannot eat the
// whole bar.
constexpr int kMaxTextWidth = 148;
// The bookmark tree is user-created, but a corrupt parent_id cycle must not
// send the folder menus into infinite recursion (same guard as the bookmarks
// page).
constexpr int kMaxMenuDepth = 12;
// Root folder id in the schema (migration 2 seeds row id = 0).
constexpr int64_t kRootId = 0;
// The folder whose children the bar shows. Matched by title among the root's
// children so the bar folder stays an ordinary, user-visible bookmark folder.
constexpr const char* kBarFolderTitle = "Bookmarks bar";

// A bar item. QToolButton already gives us the hover/pressed states from the
// stylesheet and the icon+text layout; the only browser behavior it is missing
// is middle-click, which opens a bookmark in a new tab. A std::function
// callback (rather than a signal) keeps this helper free of Q_OBJECT and of a
// second moc pass over this translation unit — the same approach DownloadRow
// takes in DownloadsPopover.cpp.
class BarButton final : public QToolButton {
public:
    explicit BarButton(QWidget* parent) : QToolButton(parent) {}

    std::function<void(bool new_tab)> activate;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        // Swallow the middle press so it never reaches the bar's own context
        // handling; the release below decides what it meant.
        if (event->button() == Qt::MiddleButton) {
            event->accept();
            return;
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::MiddleButton) {
            if (activate && rect().contains(event->position().toPoint())) {
                activate(true);
            }
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }
};

// Display label for a node: its title, falling back to the URL (an untitled
// folder still has to say something).
QString node_label(const bookmarks::BookmarkNode& node) {
    if (!node.title.empty()) return QString::fromStdString(node.title);
    if (!node.url.empty()) return QString::fromStdString(node.url);
    return QStringLiteral("Untitled");
}

qreal display_dpr(const QWidget* w) {
    if (w && w->screen()) return w->screen()->devicePixelRatio();
    return qApp ? qApp->devicePixelRatio() : 1.0;
}

} // namespace

BookmarkBar::BookmarkBar(bookmarks::BookmarkService* service, QWidget* parent)
    : QWidget(parent), service_(service) {
    setObjectName(QStringLiteral("BookmarkBar"));
    // Plain QWidget + QSS: without this the sheet's #BookmarkBar background
    // rule is ignored and the band paints the palette window color.
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(kBarHeight);

    layout_ = new QHBoxLayout(this);
    // Aligned with the toolbar row's leading inset so the first bookmark sits
    // under the Back button rather than floating off-grid.
    layout_->setContentsMargins(Spacing::xs + Spacing::xxs, 0,
                                Spacing::xs + Spacing::xxs, Spacing::xxs);
    layout_->setSpacing(Spacing::xxs);

    hint_ = new QLabel(QStringLiteral("Bookmarks you add to this bar appear here"),
                       this);
    hint_->setObjectName(QStringLiteral("BookmarkBarHint"));
    // The hint is background, not a target: right-clicking it must open the
    // bar's own "Add current page" menu.
    hint_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    hint_->hide();

    overflow_btn_ = new QToolButton(this);
    overflow_btn_->setObjectName(QStringLiteral("BookmarkBarOverflow"));
    overflow_btn_->setText(QStringLiteral("»"));
    overflow_btn_->setToolTip(QStringLiteral("More bookmarks"));
    overflow_btn_->setAutoRaise(true);
    overflow_btn_->setCursor(Qt::PointingHandCursor);
    overflow_btn_->setFixedHeight(kItemHeight);
    overflow_btn_->hide();
    connect(overflow_btn_, &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        menu.setObjectName(QStringLiteral("BookmarkBarMenu"));
        for (const Entry& entry : entries_) {
            if (!entry.button || !entry.button->isHidden()) continue;
            if (entry.node.is_folder) {
                QMenu* sub = menu.addMenu(entry.button->icon(),
                                          node_label(entry.node));
                fill_menu(sub, entry.node.id, 0);
                continue;
            }
            QAction* act = menu.addAction(entry.button->icon(),
                                          node_label(entry.node));
            const QString url = QString::fromStdString(entry.node.url);
            connect(act, &QAction::triggered, this, [this, url] {
                emit open_requested(core::Url(url.toStdString()), false);
            });
        }
        if (menu.isEmpty()) return;
        menu.exec(overflow_btn_->mapToGlobal(
            QPoint(0, overflow_btn_->height())));
    });

    // Site icons arrive asynchronously from the shared cache; re-tint only the
    // items whose host just resolved instead of rebuilding the whole bar.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString& host) {
                for (Entry& entry : entries_) {
                    if (entry.node.is_folder || !entry.button) continue;
                    const QString url = QString::fromStdString(entry.node.url);
                    if (!host.isEmpty() && FaviconCache::host_of(url) != host) {
                        continue;
                    }
                    const QIcon icon =
                        FaviconCache::instance().icon_for_url(url);
                    if (!icon.isNull()) entry.button->setIcon(icon);
                }
            });

    reload();
}

int64_t BookmarkBar::resolve_bar_folder() {
    if (!service_) return 0;
    // Match by title among the root's children: the bar folder is an ordinary
    // folder the user can see and rename on aurora://bookmarks, so its id is
    // never hardcoded. Re-resolved on every reload because that page can
    // delete it while the bar is on screen.
    for (const auto& node : service_->children(kRootId)) {
        if (node.is_folder &&
            QString::fromStdString(node.title)
                    .compare(QLatin1String(kBarFolderTitle),
                             Qt::CaseInsensitive) == 0) {
            return node.id;
        }
    }
    // Seed it on first use so "Add current page" always has a home.
    return service_->add(kRootId, kBarFolderTitle, "", true);
}

void BookmarkBar::reload() {
    bar_folder_id_ = resolve_bar_folder();
    rebuild();
    apply_overflow();
}

void BookmarkBar::rebuild() {
    // Take the persistent children out of the layout first so the generic
    // drain below only ever deletes rows built by a previous rebuild.
    layout_->removeWidget(hint_);
    layout_->removeWidget(overflow_btn_);
    while (QLayoutItem* item = layout_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            // Hide before deleteLater: a still-parented button would keep
            // painting until the event loop gets round to freeing it.
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    entries_.clear();

    std::vector<bookmarks::BookmarkNode> nodes;
    if (service_ && bar_folder_id_ > 0) {
        nodes = service_->children(bar_folder_id_);
    }

    const ThemeColors& c = current_theme().colors;
    const qreal dpr = display_dpr(this);
    const QFontMetrics fm(font());

    entries_.reserve(nodes.size());
    for (const auto& node : nodes) {
        auto* btn = new BarButton(this);
        btn->setObjectName(QStringLiteral("BookmarkBarItem"));
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(kItemHeight);
        btn->setIconSize(QSize(kIconPx, kIconPx));
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setFocusPolicy(Qt::NoFocus);
        const QString label = node_label(node);
        // Elided here (not by the layout) so a long title can never widen the
        // item and push every other bookmark into the overflow menu.
        btn->setText(fm.elidedText(label, Qt::ElideRight, kMaxTextWidth));
        btn->setToolTip(node.is_folder
                            ? label
                            : QStringLiteral("%1\n%2").arg(
                                  label, QString::fromStdString(node.url)));
        if (node.is_folder) {
            btn->setIcon(icons::icon(icons::Name::Folder, c.text_muted, kIconPx,
                                     dpr));
        } else {
            const QString url = QString::fromStdString(node.url);
            QIcon icon = FaviconCache::instance().icon_for_url(url);
            if (icon.isNull()) {
                // Not fetched yet (or the host has none): a neutral globe
                // keeps every item the same width until the real icon lands.
                icon = icons::icon(icons::Name::Globe, c.text_muted, kIconPx,
                                   dpr);
            }
            btn->setIcon(icon);
        }

        const Entry entry{node, btn};
        btn->activate = [this, entry](bool new_tab) { activate(entry, new_tab); };
        connect(btn, &QToolButton::clicked, this,
                [this, entry] { activate(entry, false); });
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QToolButton::customContextMenuRequested, this,
                [this, entry](const QPoint& pos) {
                    show_item_menu(entry, entry.button->mapToGlobal(pos));
                });

        layout_->addWidget(btn);
        entries_.push_back(entry);
    }

    hint_->setVisible(entries_.empty());
    if (entries_.empty()) layout_->addWidget(hint_);
    layout_->addWidget(overflow_btn_);
    layout_->addStretch(1);
}

void BookmarkBar::apply_overflow() {
    if (!overflow_btn_) return;
    if (entries_.empty()) {
        overflow_btn_->hide();
        return;
    }

    const QMargins margins = layout_->contentsMargins();
    const int spacing = layout_->spacing();
    const int available = width() - margins.left() - margins.right();

    // First pass: how many items fit with no overflow button at all.
    int used = 0;
    size_t visible = 0;
    for (; visible < entries_.size(); ++visible) {
        const int item_width = entries_[visible].button->sizeHint().width() +
                               (visible == 0 ? 0 : spacing);
        if (used + item_width > available) break;
        used += item_width;
    }
    // Second pass: if anything is left over the "»" button has to fit too, so
    // give back trailing items until it does.
    if (visible < entries_.size()) {
        const int overflow_width =
            overflow_btn_->sizeHint().width() + spacing;
        while (visible > 0 && used + overflow_width > available) {
            --visible;
            used -= entries_[visible].button->sizeHint().width() +
                    (visible == 0 ? 0 : spacing);
        }
    }

    for (size_t i = 0; i < entries_.size(); ++i) {
        entries_[i].button->setVisible(i < visible);
    }
    overflow_btn_->setVisible(visible < entries_.size());
}

void BookmarkBar::activate(const Entry& entry, bool new_tab) {
    if (entry.node.is_folder) {
        show_folder_menu(entry);
        return;
    }
    if (entry.node.url.empty()) return;
    emit open_requested(core::Url(entry.node.url), new_tab);
}

void BookmarkBar::fill_menu(QMenu* menu, int64_t parent_id, int depth) {
    if (!menu || !service_ || depth > kMaxMenuDepth) return;
    const ThemeColors& c = current_theme().colors;
    const qreal dpr = display_dpr(this);
    const auto children = service_->children(parent_id);
    if (children.empty()) {
        QAction* empty = menu->addAction(QStringLiteral("(empty)"));
        empty->setEnabled(false);
        return;
    }
    for (const auto& node : children) {
        if (node.is_folder) {
            QMenu* sub = menu->addMenu(
                icons::icon(icons::Name::Folder, c.text_muted, kIconPx, dpr),
                node_label(node));
            fill_menu(sub, node.id, depth + 1);
            continue;
        }
        const QString url = QString::fromStdString(node.url);
        QIcon icon = FaviconCache::instance().icon_for_url(url);
        if (icon.isNull()) {
            icon = icons::icon(icons::Name::Globe, c.text_muted, kIconPx, dpr);
        }
        QAction* act = menu->addAction(icon, node_label(node));
        connect(act, &QAction::triggered, this, [this, url] {
            emit open_requested(core::Url(url.toStdString()), false);
        });
    }
}

void BookmarkBar::show_folder_menu(const Entry& entry) {
    if (!entry.button) return;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("BookmarkBarMenu"));
    fill_menu(&menu, entry.node.id, 0);
    menu.exec(entry.button->mapToGlobal(QPoint(0, entry.button->height())));
}

void BookmarkBar::show_item_menu(const Entry& entry, const QPoint& global_pos) {
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("BookmarkBarMenu"));
    if (!entry.node.is_folder) {
        const QString url = QString::fromStdString(entry.node.url);
        QAction* open = menu.addAction(QStringLiteral("Open"));
        connect(open, &QAction::triggered, this, [this, url] {
            emit open_requested(core::Url(url.toStdString()), false);
        });
        QAction* open_new = menu.addAction(QStringLiteral("Open in new tab"));
        connect(open_new, &QAction::triggered, this, [this, url] {
            emit open_requested(core::Url(url.toStdString()), true);
        });
        menu.addSeparator();
    }
    QAction* edit = menu.addAction(entry.node.is_folder
                                       ? QStringLiteral("Rename…")
                                       : QStringLiteral("Edit…"));
    connect(edit, &QAction::triggered, this,
            [this, entry] { edit_entry(entry); });
    QAction* remove = menu.addAction(QStringLiteral("Delete"));
    connect(remove, &QAction::triggered, this,
            [this, entry] { delete_entry(entry); });
    menu.exec(global_pos);
}

void BookmarkBar::edit_entry(const Entry& entry) {
    if (!service_ || entry.node.id <= 0) return;
    bool ok = false;
    const QString title = QInputDialog::getText(
        this,
        entry.node.is_folder ? QStringLiteral("Rename folder")
                             : QStringLiteral("Edit bookmark"),
        QStringLiteral("Name:"), QLineEdit::Normal, node_label(entry.node), &ok);
    if (!ok) return;
    const QString trimmed_title = title.trimmed();
    if (trimmed_title.isEmpty()) {
        emit status_message(QStringLiteral("A bookmark name cannot be empty"));
        return;
    }

    QString url = QString::fromStdString(entry.node.url);
    if (!entry.node.is_folder) {
        url = QInputDialog::getText(this, QStringLiteral("Edit bookmark"),
                                    QStringLiteral("URL:"), QLineEdit::Normal,
                                    url, &ok);
        if (!ok) return;
        url = url.trimmed();
        if (url.isEmpty()) {
            emit status_message(QStringLiteral("A bookmark URL cannot be empty"));
            return;
        }
    }

    if (service_->update(entry.node.id, trimmed_title.toStdString(),
                         url.toStdString())) {
        emit status_message(
            QStringLiteral("Updated “%1”").arg(trimmed_title));
    }
    reload();
}

void BookmarkBar::delete_entry(const Entry& entry) {
    if (!service_ || entry.node.id <= 0) return;
    // The bar folder itself is never an item of the bar, but a future caller
    // must not be able to delete the container the bar reads from.
    if (entry.node.id == bar_folder_id_) {
        emit status_message(
            QStringLiteral("The bookmarks bar folder cannot be deleted here"));
        return;
    }
    if (entry.node.is_folder) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Delete folder"),
            QStringLiteral("Delete this folder and everything inside it?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }
    if (service_->remove(entry.node.id)) {
        emit status_message(entry.node.is_folder
                                ? QStringLiteral("Folder deleted")
                                : QStringLiteral("Bookmark removed from the bar"));
    }
    reload();
}

bool BookmarkBar::add_page(const QString& title, const QString& url) {
    if (!service_ || url.isEmpty()) return false;
    if (bar_folder_id_ <= 0) bar_folder_id_ = resolve_bar_folder();
    if (bar_folder_id_ <= 0) return false;
    const QString name = title.trimmed().isEmpty() ? url : title.trimmed();
    const bool added = service_->add(bar_folder_id_, name.toStdString(),
                                     url.toStdString(), false) > 0;
    if (added) {
        emit status_message(QStringLiteral("Added to the bookmarks bar: %1")
                                .arg(name));
        reload();
    }
    return added;
}

void BookmarkBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    apply_overflow();
}

void BookmarkBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // The bar can be shown again long after it was built (menu toggle, leaving
    // content fullscreen), and bookmarks may have changed on aurora://bookmarks
    // meanwhile.
    reload();
}

void BookmarkBar::contextMenuEvent(QContextMenuEvent* event) {
    // Only bare bar background reaches this: the item buttons handle their own
    // menus through Qt::CustomContextMenu.
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("BookmarkBarMenu"));
    QAction* add = menu.addAction(QStringLiteral("Add current page"));
    add->setEnabled(service_ != nullptr);
    connect(add, &QAction::triggered, this,
            [this] { emit add_current_page_requested(); });
    menu.addSeparator();
    QAction* manage = menu.addAction(QStringLiteral("Bookmark manager"));
    connect(manage, &QAction::triggered, this,
            [this] { emit manage_requested(); });
    menu.exec(event->globalPos());
    event->accept();
}

} // namespace aurora::ui

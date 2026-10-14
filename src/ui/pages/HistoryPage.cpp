#include "ui/pages/HistoryPage.hpp"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>

#include "history/HistoryService.hpp"
#include "ui/FaviconCache.hpp"

namespace aurora::ui {

namespace {

// Column layout of the history list.
enum Column { ColTitle = 0, ColUrl = 1, ColVisits = 2, ColLastVisit = 3 };

// Row id is stashed on the item so deletes hit the right database row even
// after the list has been re-sorted or filtered.
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kUrlRole = Qt::UserRole + 2;

// How many rows the page loads at once. History can be huge; the list is a
// window onto the newest entries and search narrows it further.
constexpr size_t kPageSize = 500;

QString format_time(std::chrono::system_clock::time_point tp) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          tp.time_since_epoch())
                          .count();
    if (secs <= 0) return QStringLiteral("—");
    return QDateTime::fromSecsSinceEpoch(secs).toString("yyyy-MM-dd HH:mm");
}

} // namespace

HistoryPage::HistoryPage(history::HistoryService* service, QWidget* parent)
    : InternalPageWidget(QStringLiteral("History"), parent), service_(service) {
    set_subtitle(QStringLiteral(
        "Every page you visited on this profile. Double-click a row to open "
        "it again."));

    search_ = new QLineEdit(this);
    search_->setObjectName("PageSearch");
    search_->setPlaceholderText(QStringLiteral("Search history"));
    search_->setClearButtonEnabled(true);
    toolbar_row()->addWidget(search_, 1);

    auto* delete_btn = new QPushButton(QStringLiteral("Delete selected"), this);
    delete_btn->setObjectName("PageButton");
    toolbar_row()->addWidget(delete_btn);

    auto* clear_btn = new QPushButton(QStringLiteral("Clear all history"), this);
    clear_btn->setObjectName("PageDangerButton");
    toolbar_row()->addWidget(clear_btn);

    list_ = new QTreeWidget(this);
    list_->setObjectName("PageList");
    list_->setColumnCount(4);
    list_->setHeaderLabels({QStringLiteral("Title"), QStringLiteral("URL"),
                            QStringLiteral("Visits"), QStringLiteral("Last visit")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(false);
    list_->setUniformRowHeights(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setSortingEnabled(true);
    list_->header()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(ColUrl, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(ColVisits, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(ColLastVisit, QHeaderView::ResizeToContents);
    set_body(list_);

    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString& text) { reload(text); });
    connect(delete_btn, &QPushButton::clicked, this,
            &HistoryPage::delete_selected);
    connect(clear_btn, &QPushButton::clicked, this, &HistoryPage::clear_all);
    // Site icons come from the shared cache, which fills in asynchronously:
    // when an icon arrives, apply it to every row on that host in place (no
    // reload, so the user's selection and scroll position survive).
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString& host) {
                for (int i = 0; i < list_->topLevelItemCount(); ++i) {
                    auto* item = list_->topLevelItem(i);
                    const auto url = item->data(ColTitle, kUrlRole).toString();
                    if (FaviconCache::host_of(url) == host) {
                        item->setIcon(ColTitle,
                                      FaviconCache::instance().icon_for_url(url));
                    }
                }
            });
    connect(list_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item) return;
                const auto url = item->data(ColTitle, kUrlRole).toString();
                if (!url.isEmpty()) emit open_url_requested(url);
            });
}

void HistoryPage::refresh() { reload(search_ ? search_->text() : QString()); }

void HistoryPage::reload(const QString& query) {
    list_->clear();
    if (!service_) {
        set_empty_state(true,
                        QStringLiteral("No profile is attached to this window, "
                                       "so history is unavailable."));
        return;
    }

    const auto trimmed = query.trimmed();
    const auto visits = trimmed.isEmpty()
                            ? service_->recent(kPageSize)
                            : service_->search(trimmed.toStdString(), kPageSize);

    // Sorting is disabled during the fill so inserting N rows is O(n), not
    // O(n log n) with a re-sort per insertion.
    const bool was_sorting = list_->isSortingEnabled();
    list_->setSortingEnabled(false);
    for (const auto& v : visits) {
        auto* item = new QTreeWidgetItem(list_);
        const auto title = v.title.empty() ? v.url : v.title;
        item->setText(ColTitle, QString::fromStdString(title));
        item->setText(ColUrl, QString::fromStdString(v.url));
        item->setText(ColVisits, QString::number(v.visit_count));
        item->setText(ColLastVisit, format_time(v.last_visit));
        item->setToolTip(ColUrl, QString::fromStdString(v.url));
        item->setData(ColTitle, kIdRole, QVariant::fromValue(v.id));
        const auto qurl = QString::fromStdString(v.url);
        item->setData(ColTitle, kUrlRole, qurl);
        item->setIcon(ColTitle, FaviconCache::instance().icon_for_url(qurl));
    }
    list_->setSortingEnabled(was_sorting);

    if (visits.empty()) {
        set_empty_state(true, trimmed.isEmpty()
                                  ? QStringLiteral("Nothing here yet. Pages you "
                                                   "visit will appear in this list.")
                                  : QStringLiteral("No history entries match “%1”.")
                                        .arg(trimmed));
    } else {
        set_empty_state(false);
    }
}

void HistoryPage::delete_selected() {
    if (!service_) return;
    const auto selected = list_->selectedItems();
    int removed = 0;
    for (auto* item : selected) {
        // Every column of a row yields the same item, but selectedItems() with
        // an ExtendedSelection returns one entry per row, so no dedupe needed.
        const auto id = item->data(ColTitle, kIdRole).toLongLong();
        if (id > 0 && service_->remove(id)) ++removed;
    }
    if (removed > 0) {
        emit status_message(QStringLiteral("Removed %1 history %2")
                                .arg(removed)
                                .arg(removed == 1 ? "entry" : "entries"));
    }
    refresh();
}

void HistoryPage::clear_all() {
    if (!service_) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Clear all history"),
        QStringLiteral("Delete every entry from browsing history? This cannot "
                       "be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    const int removed = service_->clear();
    emit status_message(
        QStringLiteral("Cleared %1 history entries").arg(removed));
    refresh();
}

} // namespace aurora::ui

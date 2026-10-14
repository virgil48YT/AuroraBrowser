#include "ui/pages/DownloadsPage.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QShowEvent>
#include <QSize>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <unordered_set>
#include <utility>

#include "downloads/DownloadService.hpp"
#include "ui/Icons.hpp"

namespace aurora::ui {

namespace {

enum Column {
    ColFile = 0,
    ColStatus = 1,
    ColSize = 2,
    ColSource = 3,
    ColStarted = 4,
    ColActions = 5,
};

constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kOpenableRole = Qt::UserRole + 2;
constexpr size_t kPageSize = 300;

// Row height for the list. Desktop-dense but not cramped; uniform, so a long
// list scans as a table instead of a stack of differently sized cards.
constexpr int kRowHeight = 34;
constexpr int kActionButtonSize = 24;
// The in-progress bar that replaces the Status text while a file is arriving.
// Short enough to sit inside a uniform row, tall enough to carry its own
// percentage text. Mirrored by kDownloadProgressHeight in StylesheetBuilder,
// which has to re-state it: the generic QProgressBar rule caps every bar at a
// 3px hairline.
constexpr int kProgressHeight = 18;
// Width reserved for the Status column. The column resizes to its contents,
// and an in-progress row has no text for it to measure (the bar is an item
// widget, which the header cannot see), so the row publishes this hint
// instead — otherwise the column would collapse around the header label.
// Wide enough for the bar's "42% • 1.4 MB/s" text (was 148, which clipped the
// rate on a fast transfer).
constexpr int kStatusWidth = 176;
// How often the visible page re-reads DownloadService while a transfer runs.
// Half the dropdown's rate: this is a whole table, and the page is a
// deliberate destination rather than a glance.
constexpr int kTickMs = 500;
// Minimum widths for the two elastic columns, so "File" and "Source" split the
// (now much wider) content column instead of truncating everything.
constexpr int kFileMinWidth = 260;
constexpr int kSourceMinWidth = 240;

QString human_size(int64_t bytes) {
    if (bytes < 0) return QStringLiteral("unknown");
    return QLocale::system().formattedDataSize(bytes);
}

// A transfer rate as "1.4 MB/s" / "340 KB/s". Empty for a non-positive rate so
// callers can drop the segment instead of printing "0 KB/s". Mirrors the
// dropdown's helper (DownloadsPopover.cpp) — the two surfaces must read the
// same, and one tiny function is cheaper than a shared header.
QString format_speed(double bytes_per_sec) {
    if (!(bytes_per_sec > 0.0)) return {};
    constexpr double kKB = 1024.0;
    constexpr double kMB = 1024.0 * 1024.0;
    if (bytes_per_sec >= kMB) {
        return QStringLiteral("%1 MB/s").arg(bytes_per_sec / kMB, 0, 'f', 1);
    }
    return QStringLiteral("%1 KB/s")
        .arg(qMax(1.0, bytes_per_sec / kKB), 0, 'f', 0);
}

QString status_text(const downloads::DownloadItem& d) {
    switch (d.state) {
        case downloads::DownloadState::InProgress: {
            const int pct = d.percent();
            if (pct < 0) return QStringLiteral("Downloading…");
            return QStringLiteral("Downloading… %1%").arg(pct);
        }
        case downloads::DownloadState::Paused: {
            const int pct = d.percent();
            if (pct < 0) return QStringLiteral("Paused");
            return QStringLiteral("Paused — %1%").arg(pct);
        }
        case downloads::DownloadState::Complete:
            return QStringLiteral("Completed");
        case downloads::DownloadState::Cancelled:
            return QStringLiteral("Cancelled");
        case downloads::DownloadState::Interrupted:
            return QStringLiteral("Interrupted");
    }
    return QStringLiteral("Unknown");
}

QString size_text(const downloads::DownloadItem& d) {
    if (d.total_bytes > 0 &&
        (d.state == downloads::DownloadState::InProgress ||
         d.state == downloads::DownloadState::Paused)) {
        return QStringLiteral("%1 of %2")
            .arg(human_size(d.received), human_size(d.total_bytes));
    }
    if (d.total_bytes > 0) return human_size(d.total_bytes);
    return human_size(d.received);
}

QString started_text(const downloads::DownloadItem& d) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          d.started_at.time_since_epoch())
                          .count();
    if (secs <= 0) return QStringLiteral("—");
    return QDateTime::fromSecsSinceEpoch(secs).toString("yyyy-MM-dd HH:mm");
}

} // namespace

DownloadsPage::DownloadsPage(downloads::DownloadService* service,
                             QWidget* parent, Actions actions)
    : InternalPageWidget(QStringLiteral("Downloads"), parent), service_(service),
      actions_(std::move(actions)) {
    // --- Actions row -------------------------------------------------------
    // Left: a live count that tells you what you are looking at (the skeleton
    // no longer draws a page title). Right: the two actions, so the row has a
    // clear leading/trailing structure across the full width.
    count_label_ = new QLabel(this);
    count_label_->setObjectName("DownloadsCount");
    toolbar_row()->addWidget(count_label_, 0, Qt::AlignVCenter);
    toolbar_row()->addStretch(1);

    auto* remove_btn = new QPushButton(QStringLiteral("Remove from list"), this);
    remove_btn->setObjectName("PageButton");
    remove_btn->setCursor(Qt::PointingHandCursor);
    toolbar_row()->addWidget(remove_btn);

    auto* clear_btn = new QPushButton(QStringLiteral("Clear finished"), this);
    clear_btn->setObjectName("PageButton");
    clear_btn->setCursor(Qt::PointingHandCursor);
    toolbar_row()->addWidget(clear_btn);

    list_ = new QTreeWidget(this);
    // Downloads-specific list styling (flatter card, taller uniform rows) on
    // top of the shared page-list look; see StylesheetBuilder.
    list_->setObjectName("DownloadsList");
    list_->setColumnCount(6);
    list_->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Status"),
                            QStringLiteral("Size"), QStringLiteral("Source"),
                            QStringLiteral("Started"), QString()});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(false);
    list_->setUniformRowHeights(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setAllColumnsShowFocus(true);
    list_->setTextElideMode(Qt::ElideMiddle);
    list_->setIndentation(0);
    // The two long text columns share the surplus width; the short ones stay
    // exactly as wide as their content needs.
    auto* head = list_->header();
    head->setSectionResizeMode(ColFile, QHeaderView::Stretch);
    head->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
    head->setSectionResizeMode(ColSize, QHeaderView::ResizeToContents);
    head->setSectionResizeMode(ColSource, QHeaderView::Stretch);
    head->setSectionResizeMode(ColStarted, QHeaderView::ResizeToContents);
    head->setSectionResizeMode(ColActions, QHeaderView::ResizeToContents);
    head->setMinimumSectionSize(72);
    head->setStretchLastSection(false);
    head->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    list_->setColumnWidth(ColFile, kFileMinWidth);
    list_->setColumnWidth(ColSource, kSourceMinWidth);
    set_body(list_);

    connect(remove_btn, &QPushButton::clicked, this,
            &DownloadsPage::remove_selected);
    connect(clear_btn, &QPushButton::clicked, this,
            &DownloadsPage::clear_finished);
    connect(list_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int column) {
                if (!service_ || !item || column == ColActions ||
                    !item->data(ColFile, kOpenableRole).toBool()) {
                    return;
                }
                const auto id = item->data(ColFile, kIdRole).toLongLong();
                if (id > 0) service_->open_file(id);
            });

    // Live progress. The browser's download callback writes DownloadService on
    // this (the Qt main) thread, so a plain timer on the same thread is a
    // complete live view — no extra threads, no service changes.
    tick_ = new QTimer(this);
    tick_->setInterval(kTickMs);
    connect(tick_, &QTimer::timeout, this, &DownloadsPage::refresh_if_active);
}

void DownloadsPage::showEvent(QShowEvent* event) {
    InternalPageWidget::showEvent(event);
    if (tick_) tick_->start();
}

void DownloadsPage::hideEvent(QHideEvent* event) {
    // Switching tabs hides the page without destroying it, so this is the only
    // place that can stop the poll.
    if (tick_) tick_->stop();
    InternalPageWidget::hideEvent(event);
}

bool DownloadsPage::has_active_download() const {
    if (!service_) return false;
    for (const auto& d : service_->recent(kPageSize)) {
        if (d.state == downloads::DownloadState::InProgress) return true;
    }
    return false;
}

void DownloadsPage::refresh_if_active() {
    if (!isVisible() || !has_active_download()) return;
    refresh();
}

void DownloadsPage::refresh() {
    // refresh() destroys every row, so a live tick would silently clear the
    // user's selection (and with it the "Remove from list" target) twice a
    // second. Remember the selected download ids and restore them below.
    QSet<qint64> selected;
    for (const auto* item : list_->selectedItems()) {
        const auto id = item->data(ColFile, kIdRole).toLongLong();
        if (id > 0) selected.insert(id);
    }

    list_->clear();
    if (!service_) {
        set_count(0);
        set_empty_state(true,
                        QStringLiteral("No profile is attached to this window, "
                                       "so downloads are unavailable."));
        return;
    }

    const auto items = service_->recent(kPageSize);
    // Rows still listed, so a rate sample for a download that has been removed
    // (or dropped off this page) is not kept alive by the map.
    std::unordered_set<int64_t> live_ids;
    for (const auto& d : items) {
        live_ids.insert(d.id);
        auto* row = new QTreeWidgetItem(list_);
        row->setText(ColFile, QString::fromStdString(d.file_name()));
        // An in-progress row's status is drawn by the bar below, not by text:
        // leaving the text in place would show through around the widget.
        if (d.state != downloads::DownloadState::InProgress) {
            row->setText(ColStatus, status_text(d));
        }
        row->setToolTip(ColStatus, status_text(d));
        row->setText(ColSize, size_text(d));
        row->setText(ColSource, QString::fromStdString(d.url));
        row->setText(ColStarted, started_text(d));
        row->setToolTip(ColFile, QString::fromStdString(d.target_path));
        row->setToolTip(ColSource, QString::fromStdString(d.url));
        row->setData(ColFile, kIdRole, QVariant::fromValue(d.id));
        const QString target = QString::fromStdString(d.target_path);
        const bool file_available =
            d.state == downloads::DownloadState::Complete &&
            !target.isEmpty() && QFileInfo(target).isFile();
        row->setData(ColFile, kOpenableRole, file_available);
        // Uniform, comfortable row height across the full width.
        row->setSizeHint(ColFile, QSize(0, kRowHeight));

        // Sampled once per row per refresh (it advances the rate sample) and
        // empty for anything that is not running.
        const QString speed = speed_text(d);

        if (d.state == downloads::DownloadState::InProgress ||
            d.state == downloads::DownloadState::Paused) {
            // A real bar in the Status cell, so the page shows the transfer
            // moving instead of a percentage that only changed when the user
            // reopened the tab. Paused rows keep the bar (frozen at their
            // progress) so resuming visibly continues from the same place.
            // A server that sends no Content-Length leaves percent() at -1,
            // which is exactly Qt's indeterminate range (0, 0): a busy sweep
            // rather than a fabricated fill.
            const bool paused =
                d.state == downloads::DownloadState::Paused;
            auto* progress = new QProgressBar(list_);
            progress->setObjectName("DownloadPageProgress");
            progress->setFixedHeight(kProgressHeight);
            progress->setAlignment(Qt::AlignCenter);
            progress->setTextVisible(true);
            const int pct = d.percent();
            if (pct < 0) {
                progress->setRange(0, 0);
                progress->setFormat(
                    paused ? QStringLiteral("Paused")
                           : (speed.isEmpty()
                                  ? QStringLiteral("Downloading…")
                                  : QStringLiteral("Downloading… %1")
                                        .arg(speed)));
            } else {
                progress->setRange(0, 100);
                progress->setValue(pct);
                // The rate rides in the bar's own text, so a live row reads
                // "42% • 1.4 MB/s" without a second widget in the cell.
                progress->setFormat(
                    paused ? QStringLiteral("Paused — %p%")
                           : (speed.isEmpty()
                                  ? QStringLiteral("%p%")
                                  : QStringLiteral("%p% • %1").arg(speed)));
            }
            progress->setToolTip(
                speed.isEmpty()
                    ? status_text(d)
                    : QStringLiteral("%1 • %2").arg(status_text(d), speed));
            row->setSizeHint(ColStatus, QSize(kStatusWidth, kRowHeight));
            list_->setItemWidget(row, ColStatus, progress);
        }

        if (selected.contains(static_cast<qint64>(d.id))) row->setSelected(true);

        // One action per row: while the file is still arriving (running or
        // paused) the useful action is to control the transfer — pause it,
        // resume it or stop it — and once it has arrived the useful action is
        // to show it. A failed row offers Retry, which asks the host to start
        // the transfer again from its stored URL. Every control requires the
        // matching host callback; without an engine to talk to the row keeps
        // the reveal button.
        const bool alive = d.state == downloads::DownloadState::InProgress ||
                           d.state == downloads::DownloadState::Paused;
        if (alive && (actions_.cancel || actions_.pause || actions_.resume)) {
            auto* actions = new QWidget(list_);
            auto* lay = new QHBoxLayout(actions);
            lay->setContentsMargins(0, 0, 0, 0);
            lay->setSpacing(2);
            const auto add_btn = [&](icons::Name icon, const QString& tip,
                                     const char* object_name,
                                     std::function<bool(int64_t)> action,
                                     const QString& done_message) {
                auto* btn = new QToolButton(actions);
                btn->setObjectName(QString::fromLatin1(object_name));
                btn->setAutoRaise(true);
                btn->setFixedSize(kActionButtonSize, kActionButtonSize);
                btn->setIcon(icons::icon(icon, palette().color(QPalette::Text),
                                         14));
                btn->setIconSize(QSize(14, 14));
                btn->setToolTip(tip);
                btn->setCursor(Qt::PointingHandCursor);
                connect(btn, &QToolButton::clicked, this,
                        [this, id = d.id, action = std::move(action),
                         done_message] {
                            if (!action) return;
                            if (action(id) && !done_message.isEmpty()) {
                                emit status_message(done_message);
                            }
                            // The host updates the row as soon as the engine
                            // accepts, so repaint: the ticker only runs
                            // while something is still in flight and may
                            // never fire again for this table. Deferred to
                            // the next event-loop turn because refresh()
                            // destroys every row — including the button
                            // emitting this click.
                            QTimer::singleShot(0, this, [this] { refresh(); });
                        });
                lay->addWidget(btn);
            };
            if (d.state == downloads::DownloadState::InProgress &&
                actions_.pause) {
                add_btn(icons::Name::Pause, QStringLiteral("Pause download"),
                        "DownloadPagePause", actions_.pause,
                        QStringLiteral("Download paused"));
            } else if (d.state == downloads::DownloadState::Paused &&
                       actions_.resume) {
                add_btn(icons::Name::Play, QStringLiteral("Resume download"),
                        "DownloadPageResume", actions_.resume,
                        QStringLiteral("Download resumed"));
            }
            if (actions_.cancel) {
                add_btn(icons::Name::Close, QStringLiteral("Cancel download"),
                        "DownloadPageCancel", actions_.cancel,
                        QStringLiteral("Download cancelled"));
            }
            list_->setItemWidget(row, ColActions, actions);
        } else if (d.is_retryable() && actions_.retry) {
            auto* retry_btn = new QToolButton(list_);
            retry_btn->setObjectName("DownloadPageRetry");
            retry_btn->setAutoRaise(true);
            retry_btn->setFixedSize(kActionButtonSize, kActionButtonSize);
            retry_btn->setIcon(icons::icon(icons::Name::Retry,
                                           palette().color(QPalette::Text), 14));
            retry_btn->setIconSize(QSize(14, 14));
            retry_btn->setToolTip(QStringLiteral("Retry download"));
            retry_btn->setCursor(Qt::PointingHandCursor);
            connect(retry_btn, &QToolButton::clicked, this,
                    [this, id = d.id] {
                        if (!actions_.retry) return;
                        if (actions_.retry(id)) {
                            emit status_message(
                                QStringLiteral("Download restarted"));
                        }
                        // The retried transfer arrives as a NEW row (a fresh
                        // engine id); this failed row stays as history, so a
                        // repaint is purely cosmetic here — but it still makes
                        // the click feel acknowledged.
                        QTimer::singleShot(0, this, [this] { refresh(); });
                    });
            list_->setItemWidget(row, ColActions, retry_btn);
        } else {
            auto* reveal = new QToolButton(list_);
            reveal->setAutoRaise(true);
            reveal->setFixedSize(kActionButtonSize, kActionButtonSize);
            reveal->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
            reveal->setIconSize(QSize(16, 16));
            reveal->setToolTip(QStringLiteral("Show in folder"));
            reveal->setEnabled(file_available);
            if (file_available) reveal->setCursor(Qt::PointingHandCursor);
            connect(reveal, &QToolButton::clicked, this, [this, id = d.id] {
                if (service_) service_->reveal_in_folder(id);
            });
            list_->setItemWidget(row, ColActions, reveal);
        }
    }

    for (auto it = speed_samples_.begin(); it != speed_samples_.end();) {
        if (live_ids.count(it->first) > 0) {
            ++it;
        } else {
            it = speed_samples_.erase(it);
        }
    }

    set_count(static_cast<int>(items.size()));

    if (items.empty()) {
        set_empty_state(true,
                        QStringLiteral("No downloads yet. Files you download "
                                       "will be listed here."));
    } else {
        set_empty_state(false);
    }
}

QString DownloadsPage::speed_text(const downloads::DownloadItem& d) {
    if (d.state != downloads::DownloadState::InProgress) {
        // Nothing is moving any more, and a kept sample would only make the
        // map grow with history.
        speed_samples_.erase(d.id);
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    double bytes_per_sec = 0.0;

    const auto prev = speed_samples_.find(d.id);
    if (prev != speed_samples_.end()) {
        const double elapsed =
            std::chrono::duration<double>(now - prev->second.at).count();
        const int64_t delta = d.received - prev->second.received;
        // A non-positive interval or byte delta says nothing about the rate:
        // fall through to the average instead of dividing by zero.
        if (elapsed > 0.0 && delta > 0) {
            bytes_per_sec = static_cast<double>(delta) / elapsed;
        }
    }

    if (bytes_per_sec <= 0.0) {
        // First refresh for this download (or one in which nothing arrived):
        // the average since it started is the honest fallback, so the bar
        // never shows "0 MB/s" on its first paint.
        const double since_start =
            std::chrono::duration<double>(std::chrono::system_clock::now() -
                                          d.started_at)
                .count();
        if (since_start > 0.0 && d.received > 0) {
            bytes_per_sec = static_cast<double>(d.received) / since_start;
        }
    }

    speed_samples_[d.id] = SpeedSample{d.received, now};
    return format_speed(bytes_per_sec);
}

void DownloadsPage::set_count(int count) {
    if (!count_label_) return;
    count_label_->setText(count == 1 ? QStringLiteral("1 download")
                                    : QStringLiteral("%1 downloads").arg(count));
}

void DownloadsPage::remove_selected() {
    if (!service_) return;
    int removed = 0;
    for (auto* item : list_->selectedItems()) {
        const auto id = item->data(ColFile, kIdRole).toLongLong();
        if (id > 0 && service_->remove(id)) ++removed;
    }
    if (removed > 0) {
        emit status_message(
            QStringLiteral("Removed %1 download(s) from the list").arg(removed));
    }
    refresh();
}

void DownloadsPage::clear_finished() {
    if (!service_) return;
    const int removed = service_->clear_finished();
    emit status_message(
        QStringLiteral("Cleared %1 finished download(s)").arg(removed));
    refresh();
}

} // namespace aurora::ui

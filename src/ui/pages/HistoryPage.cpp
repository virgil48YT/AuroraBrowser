// Aurora — history page (aurora://history).
//
// The UI shape follows the neon reference (glowing date-group cards on the
// left, a glass filter panel on the right, gradient-rimmed search and
// clear-data up top) while the behaviour stays exactly what the history
// service already supports: full-text search narrows the set, date filters
// are real cutoffs on last_visit, deleting removes rows through the service,
// and clear asks before wiping. Nothing invents rows, and the sync block is
// an explicit "not available yet" note instead of a fake sign-in.
#include "ui/pages/HistoryPage.hpp"

#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

#include <chrono>
#include <cstdint>

#include "ui/FaviconCache.hpp"
#include "ui/GlowFrame.hpp"
#include "ui/Icons.hpp"
#include "ui/PageBackground.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"
#include "ui/pages/HistoryWidgets.hpp"

namespace aurora::ui {

namespace {

// How many rows the page loads at once. History can be huge; the page shows
// a window onto the newest entries and search narrows it further. Same cap
// the previous layout used.
constexpr size_t kPageSize = 500;

inline qreal current_dpr(const QWidget* w) {
    return w && w->screen() ? w->screen()->devicePixelRatio() : 1.0;
}

} // namespace

HistoryPage::HistoryPage(history::HistoryService* service, QWidget* parent)
    : InternalPageWidget(QStringLiteral("History"), parent), service_(service) {
    build_ui();
    // Favicon fetches land asynchronously; icon_ready re-stamps the group
    // that owns the host instead of rebuilding everything, so collapse and
    // selection survive the refresh.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString& host) { on_icon_ready(host); });
}

void HistoryPage::build_ui() {
    // Header: a large clock glyph next to the bold "History" word. The
    // skeleton's own title label stays wired but hidden so the page can keep
    // its accessible name while the visual header takes over.
    if (auto* t = page_title_label()) t->hide();
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("HistoryHeader"));
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(14);

    const ThemeColors& c = current_theme().colors;
    auto* clock = new QLabel(header);
    clock->setPixmap(icons::pixmap(icons::Name::History, 34, c.accent,
                                   current_dpr(this)));
    clock->setFixedSize(38, 38);
    header_row->addWidget(clock, 0, Qt::AlignVCenter);

    auto* title = new QLabel(QStringLiteral("History"), header);
    title->setObjectName(QStringLiteral("HistoryTitle"));
    header_row->addWidget(title, 0, Qt::AlignVCenter);
    header_row->addStretch(1);
    root_layout()->insertWidget(0, header);

    // Toolbar: the search field inside a glowing cyan→purple pill (the rim
    // is painted by GlowFrame so it can extend past the field), and the
    // magenta clear-browsing-data action.
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("HistorySearch"));
    search_->setPlaceholderText(QStringLiteral("Search history..."));
    search_->setClearButtonEnabled(true);

    auto* search_wrap = new GlowFrame(this);
    search_wrap->setObjectName(QStringLiteral("HistorySearchWrap"));
    {
        GlowFrame::Edge edge;
        QColor cyan = c.accent;
        cyan.setAlpha(165);
        QColor purple = c.accent2;
        purple.setAlpha(115);
        QColor glow = c.accent2;
        glow.setAlpha(30);
        edge.color1 = cyan;
        edge.color2 = purple;
        edge.glow = glow;
        edge.glow_extent = 3;
        QColor fill = c.card_bg;
        fill.setAlpha(150);
        search_wrap->set_style(edge, fill, 12);
        auto* l = new QHBoxLayout(search_wrap);
        l->setContentsMargins(14, 7, 14, 7);
        l->setSpacing(8);
        auto* mag = new QLabel(search_wrap);
        mag->setPixmap(icons::pixmap(icons::Name::Search, 16, c.text_muted,
                                     current_dpr(this)));
        mag->setFixedSize(18, 18);
        l->addWidget(mag);
        l->addWidget(search_, 1);
    }
    toolbar_row()->addWidget(search_wrap, 1);

    auto* clear_wrap = new GlowFrame(this);
    clear_wrap->setObjectName(QStringLiteral("HistoryClearWrap"));
    clear_wrap->apply_danger_style(10, 3);
    {
        auto* l = new QHBoxLayout(clear_wrap);
        l->setContentsMargins(14, 7, 14, 7);
        l->setSpacing(8);
        auto* trash = new QLabel(clear_wrap);
        trash->setPixmap(icons::pixmap(icons::Name::Trash, 15, c.accent3,
                                       current_dpr(this)));
        trash->setFixedSize(18, 18);
        l->addWidget(trash);
        clear_button_ = new QPushButton(QStringLiteral("Clear browsing data"),
                                        clear_wrap);
        clear_button_->setObjectName(QStringLiteral("HistoryClearButton"));
        clear_button_->setCursor(Qt::PointingHandCursor);
        l->addWidget(clear_button_);
    }
    toolbar_row()->addWidget(clear_wrap);

    // Body: date-group cards on the left, filter/actions panel on the right.
    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("HistoryBody"));
    auto* body_row = new QHBoxLayout(body);
    body_row->setContentsMargins(0, 8, 0, 0);
    body_row->setSpacing(16);

    auto* groups_host = new QWidget(body);
    groups_host->setObjectName(QStringLiteral("HistoryGroupsHost"));
    groups_column_ = new QVBoxLayout(groups_host);
    groups_column_->setContentsMargins(0, 0, 0, 0);
    groups_column_->setSpacing(14);

    const QStringList titles = {
        QStringLiteral("Today"),
        QStringLiteral("Yesterday"),
        QStringLiteral("Earlier this week"),
        QStringLiteral("Older"),
    };
    for (const auto& text : titles) {
        auto* card = new HistoryGroupCard(text, groups_host);
        connect(card, &HistoryGroupCard::row_activated, this,
                [this](const QString& url) { emit open_url_requested(url); });
        groups_column_->addWidget(card);
        group_cards_.push_back(card);
    }
    groups_column_->addStretch(1);

    // The date cards drop the old tree's built-in scroll, so the column
    // gets its own scroller: a real history scrolls; a short one sits at
    // the top. The viewport stays transparent (global sheet rule) so the
    // aurora backdrop shows between the cards.
    auto* scroll = new QScrollArea(body);
    scroll->setObjectName(QStringLiteral("HistoryScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(groups_host);
    body_row->addWidget(scroll, 1);

    // Right panel.
    auto* panel = new GlowFrame(body);
    panel->setObjectName(QStringLiteral("HistoryPanel"));
    panel->apply_card_style(14, 3);
    panel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    panel->setFixedWidth(300);
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(16, 14, 16, 14);
    panel_layout->setSpacing(14);

    auto add_heading = [this, panel, panel_layout](icons::Name icon_name,
                                                   const QColor& tint,
                                                   const QString& text) {
        auto* head = new QHBoxLayout;
        head->setSpacing(10);
        auto* icon = new QLabel(panel);
        icon->setPixmap(icons::pixmap(icon_name, 16, tint, current_dpr(this)));
        icon->setFixedSize(18, 18);
        head->addWidget(icon);
        auto* label = new QLabel(text, panel);
        label->setObjectName(QStringLiteral("HistoryPanelHeading"));
        head->addWidget(label, 1);
        panel_layout->addLayout(head);
    };

    // Filter history.
    add_heading(icons::Name::Funnel, c.accent,
                QStringLiteral("Filter history"));
    {
        auto* list = new QVBoxLayout;
        list->setContentsMargins(0, 0, 0, 0);
        list->setSpacing(6);
        const std::pair<Filter, QString> items[] = {
            {Filter::All,      QStringLiteral("All")},
            {Filter::Today,    QStringLiteral("Today")},
            {Filter::Yesterday,QStringLiteral("Yesterday")},
            {Filter::Last7,    QStringLiteral("Last 7 days")},
            {Filter::Last30,   QStringLiteral("Last 30 days")},
            {Filter::Older,    QStringLiteral("Older")},
        };
        for (const auto& item : items) {
            auto* row = new FilterRow(item.second, panel);
            row->setSelected(item.first == filter_);
            connect(row, &FilterRow::pressed, this,
                    [this, f = item.first] { set_filter(f); });
            list->addWidget(row);
        }
        panel_layout->addLayout(list);
    }

    // Search by site.
    add_heading(icons::Name::Link, c.accent,
                QStringLiteral("Search by site"));
    site_search_ = new QLineEdit(panel);
    site_search_->setObjectName(QStringLiteral("HistorySiteSearch"));
    site_search_->setPlaceholderText(QStringLiteral("Enter website..."));
    site_search_->setClearButtonEnabled(true);
    panel_layout->addWidget(site_search_);

    // Actions. "Manage history settings" routes to aurora://settings through
    // the shared open-url signal; there is no sync stack, so the last row is
    // an honest disabled note rather than a fake sign-in affordance.
    add_heading(icons::Name::Trash, c.accent3, QStringLiteral("Actions"));
    {
        const std::pair<QString, int> actions[] = {
            {QStringLiteral("Clear browsing data"), 0},
            {QStringLiteral("Remove selected items"), 1},
            {QStringLiteral("Manage history settings"), 2},
        };
        for (const auto& a : actions) {
            auto* row = new QWidget(panel);
            row->setObjectName(QStringLiteral("HistoryActionRow"));
            row->setCursor(Qt::PointingHandCursor);
            row->setAttribute(Qt::WA_StyledBackground, true);
            row->setProperty(kHistoryActionProperty, a.second);
            row->installEventFilter(this);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(10, 8, 10, 8);
            rl->setSpacing(10);
            auto* text = new QLabel(a.first, row);
            text->setObjectName(QStringLiteral("HistoryActionLabel"));
            rl->addWidget(text, 1);
            auto* chev = new QLabel(row);
            chev->setPixmap(icons::pixmap(icons::Name::ChevronRight, 14,
                                          c.text_faint, current_dpr(this)));
            chev->setFixedSize(16, 16);
            rl->addWidget(chev);
            panel_layout->addWidget(row);
        }
    }

    // History sync: explicit, disabled text, no fake sign-in.
    {
        auto* sync = new QWidget(panel);
        auto* sl = new QHBoxLayout(sync);
        sl->setContentsMargins(0, 4, 0, 0);
        sl->setSpacing(10);
        auto* icon = new QLabel(sync);
        icon->setPixmap(icons::pixmap(icons::Name::CloudOff, 16, c.text_faint,
                                      current_dpr(this)));
        icon->setFixedSize(18, 18);
        sl->addWidget(icon);
        auto* note = new QLabel(
            QStringLiteral("History sync is not available yet."), sync);
        note->setObjectName(QStringLiteral("HistorySyncNote"));
        note->setWordWrap(true);
        sl->addWidget(note, 1);
        panel_layout->addWidget(sync);
    }
    panel_layout->addStretch(1);

    body_row->addWidget(panel);
    set_body(body);

    // Wiring: search is the service's full-text search (the page re-queries),
    // the site field and date filters re-render the loaded rows, and the two
    // destructive actions confirm then call through.
    connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(site_search_, &QLineEdit::textChanged, this,
            [this] { rebuild_rows(); });
    connect(clear_button_, &QPushButton::clicked, this,
            &HistoryPage::clear_all_history);
}

bool HistoryPage::eventFilter(QObject* watched, QEvent* event) {
    // The three action rows in the right panel dispatch here; each carries a
    // stable "history_action" tag instead of relying on label text.
    if (event->type() == QEvent::MouseButtonRelease) {
        const QVariant action = watched->property(kHistoryActionProperty);
        if (action.isValid()) {
            switch (action.toInt()) {
                case 0: clear_all_history(); return true;
                case 1: delete_selected(); return true;
                case 2:
                    // Hand routing to MainWindow through the shared signal,
                    // exactly how the browser menu reaches aurora:// pages.
                    emit open_url_requested(QStringLiteral("aurora://settings"));
                    return true;
            }
        }
    }
    return InternalPageWidget::eventFilter(watched, event);
}

void HistoryPage::refresh() {
    visits_.clear();
    if (!service_) {
        set_empty_state(true,
                        QStringLiteral("No profile is attached to this window, "
                                       "so history is unavailable."));
        return;
    }
    const QString trimmed = search_ ? search_->text().trimmed() : QString();
    const auto raw = trimmed.isEmpty()
        ? service_->recent(kPageSize)
        : service_->search(trimmed.toStdString(), kPageSize);
    visits_.reserve(static_cast<int>(raw.size()));
    for (const auto& v : raw) visits_.push_back(v);
    rebuild_rows();
}

void HistoryPage::rebuild_rows() {
    if (!service_ || group_cards_.isEmpty()) return;

    const QString site_query =
        site_search_ ? site_search_->text().trimmed().toLower() : QString();
    const QDate today = QDateTime::currentDateTime().date();

    int total = 0;
    for (int i = 0; i < group_cards_.size(); ++i) {
        QVector<history::Visit> bucket;
        for (const auto& v : visits_) {
            const auto secs =
                std::chrono::duration_cast<std::chrono::seconds>(
                    v.last_visit.time_since_epoch())
                    .count();
            const QDate d = QDateTime::fromSecsSinceEpoch(secs).date();
            const int days_ago = static_cast<int>(d.daysTo(today));
            // Days-bucket index: 0 = today, 1 = yesterday, 2 = last 7 days,
            // 3 = older. A group only fills when the visit really falls into
            // it, so counts in the header badges are true per-bucket sizes.
            const int group =
                days_ago <= 0 ? 0 : (days_ago == 1 ? 1 : (days_ago <= 7 ? 2 : 3));
            if (group != i) continue;

            // Date-range filters are real cutoffs on top of the grouping.
            bool passes = true;
            switch (filter_) {
                case Filter::All: break;
                case Filter::Today: passes = days_ago <= 0; break;
                case Filter::Yesterday: passes = days_ago == 1; break;
                case Filter::Last7: passes = days_ago <= 7; break;
                case Filter::Last30: passes = days_ago <= 30; break;
                case Filter::Older: passes = days_ago > 30; break;
            }
            if (!passes) continue;

            // Site filter: lowercase containment against the URL's host.
            if (!site_query.isEmpty()) {
                const QString host = FaviconCache::host_of(
                    QString::fromStdString(v.url)).toLower();
                if (!host.contains(site_query)) continue;
            }
            bucket.push_back(v);
        }
        total += static_cast<int>(bucket.size());
        group_cards_[i]->set_visits(bucket);
    }

    const QString trimmed = search_ ? search_->text().trimmed() : QString();
    if (total > 0) {
        set_empty_state(false);
    } else if (visits_.isEmpty() && trimmed.isEmpty()) {
        set_empty_state(true, QStringLiteral("Nothing here yet. Pages you "
                                             "visit will appear here."));
    } else {
        set_empty_state(true,
            QStringLiteral("No history entries match the current search or "
                           "filters."));
    }
}

void HistoryPage::delete_selected() {
    if (!service_) return;
    QVector<qint64> ids;
    for (const auto* card : group_cards_) ids += card->selected_ids();
    if (ids.isEmpty()) {
        emit status_message(QStringLiteral("No history items are selected."));
        return;
    }
    int removed = 0;
    for (qint64 id : ids)
        if (id > 0 && service_->remove(id)) ++removed;
    if (removed > 0) {
        emit status_message(QStringLiteral("Removed %1 history %2")
                                .arg(removed)
                                .arg(removed == 1 ? "entry" : "entries"));
    }
    refresh();
}

void HistoryPage::clear_all_history() {
    if (!service_) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Clear all history"),
        QStringLiteral("Delete every entry from browsing history? This "
                       "cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    const int removed = service_->clear();
    emit status_message(
        QStringLiteral("Cleared %1 history entries").arg(removed));
    refresh();
}

void HistoryPage::set_filter(Filter filter) {
    if (filter_ == filter) return;
    filter_ = filter;
    rebuild_rows();
    // Reflect the selection on the radio rows. They were added in enum
    // order under the same parent, so findChildren traversal lines up with
    // the filter index.
    int selected = 0;
    switch (filter_) {
        case Filter::All: selected = 0; break;
        case Filter::Today: selected = 1; break;
        case Filter::Yesterday: selected = 2; break;
        case Filter::Last7: selected = 3; break;
        case Filter::Last30: selected = 4; break;
        case Filter::Older: selected = 5; break;
    }
    const auto rows = findChildren<FilterRow*>();
    for (int i = 0; i < rows.size(); ++i) rows[i]->setSelected(i == selected);
}

void HistoryPage::on_icon_ready(const QString& host) {
    for (auto* card : group_cards_) card->refresh_icon(host);
}

QString HistoryPage::format_time(std::chrono::system_clock::time_point tp) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          tp.time_since_epoch())
                          .count();
    return format_history_time(secs);
}

} // namespace aurora::ui

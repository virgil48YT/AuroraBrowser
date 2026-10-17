// Aurora — history page (aurora://history).
//
// Neon-aurora rework of the native history tool: a header with the section
// icon, a glowing search field and a clear-browsing-data action on top; a
// left-hand column of collapsible date-group cards (Today, Yesterday,
// Earlier this week, Older) with per-row favicons, domains and quiet
// timestamps; and a right-hand filter panel with date-range filters, a
// search-by-site field and the destructive/management actions.
//
// Everything shown comes from the real HistoryService: the search field is
// the service's own full-text search, each filter narrows with a cutoff on
// `last_visit`, and clear/delete call the same service functions as the
// previous plain page. The empty-state card explains itself instead of
// showing fake rows.
#pragma once

#include <QVector>

#include <chrono>

#include "history/HistoryService.hpp"
#include "ui/pages/InternalPageWidget.hpp"

class QEvent;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace aurora::ui {

class GlowFrame;
class HistoryGroupCard;

class HistoryPage : public InternalPageWidget {
    Q_OBJECT
public:
    explicit HistoryPage(history::HistoryService* service,
                         QWidget* parent = nullptr);

    void refresh() override;

private:
    // Radio-style filters in the right panel.
    enum class Filter { All, Today, Yesterday, Last7, Last30, Older };

    void build_ui();
    void rebuild_rows();
    void delete_selected();
    void clear_all_history();
    void set_filter(Filter filter);
    void on_icon_ready(const QString& host);

    bool eventFilter(QObject* watched, QEvent* event) override;

    static QString format_time(std::chrono::system_clock::time_point tp);

    history::HistoryService* service_ = nullptr;
    QVector<history::Visit> visits_;

    QLineEdit* search_ = nullptr;
    QLineEdit* site_search_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    QVBoxLayout* groups_column_ = nullptr;
    QVector<HistoryGroupCard*> group_cards_;
    Filter filter_ = Filter::All;
};

} // namespace aurora::ui

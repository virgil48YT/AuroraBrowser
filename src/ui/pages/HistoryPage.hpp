// Aurora — history page (aurora://history).
//
// Renders real rows from HistoryService: newest-first list, incremental
// full-text search, per-row delete and a clear-all action. Double-clicking a
// row navigates the active tab to it.
#pragma once

#include "ui/pages/InternalPageWidget.hpp"

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace aurora::history { class HistoryService; }

namespace aurora::ui {

class HistoryPage : public InternalPageWidget {
    Q_OBJECT
public:
    explicit HistoryPage(history::HistoryService* service,
                         QWidget* parent = nullptr);

    void refresh() override;

private:
    void reload(const QString& query);
    void delete_selected();
    void clear_all();

    history::HistoryService* service_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTreeWidget* list_ = nullptr;
};

} // namespace aurora::ui

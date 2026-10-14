#pragma once

#include <QLineEdit>
#include <QWidget>

class QToolButton;
class QLabel;

namespace aurora::ui {

class IconButton;

// DEPRECATED: This custom find-in-page bar is superseded by CEF/Chromium's
// built-in find-in-page (enabled via the --enable-builtin-find CEF switch).
// The browser menu no longer offers a "Find in Page" entry. The class is
// retained because MainWindow.cpp (not owned by us) still constructs it and
// connects its signals; it simply is never shown via the menu anymore.
//
// Find-in-page bar. Shown on Ctrl+F; provides next/previous match navigation,
// a live match counter, and an escape-to-close affordance.
class FindBar : public QWidget {
    Q_OBJECT

public:
    explicit FindBar(QWidget* parent);

    void focus_search();
    void set_match_count(int current, int total);

signals:
    void search_requested(const QString& query, bool forward);
    void close_requested();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void do_search(bool forward);

    QLineEdit* input_ = nullptr;
    QLabel* count_label_ = nullptr;
    IconButton* prev_ = nullptr;
    IconButton* next_ = nullptr;
    IconButton* close_ = nullptr;
    int current_match_ = 0;
    int total_matches_ = 0;
};

} // namespace aurora::ui

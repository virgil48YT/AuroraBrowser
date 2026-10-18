// Aurora — the suggestion dropdown under the omnibox.
//
// A frameless tool window that never takes keyboard focus: the line edit keeps
// the caret the whole time and forwards Up/Down/Enter to this widget through
// the Omnibox. Rows are painted by hand so they match the rest of the chrome
// (theme tokens, no native styling) and so a row can carry a favicon, a title,
// a dimmed URL and a right-aligned hint without a delegate stack.
#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <vector>

#include "omnibox/OmniboxSuggest.hpp"

namespace aurora::ui {

class OmniboxPopup : public QWidget {
    Q_OBJECT

public:
    explicit OmniboxPopup(QWidget* anchor);

    // Replaces the rows. Keeps the highlighted position when it is still valid.
    void set_rows(std::vector<omnibox::Suggestion> rows, const QString& engine_name);
    void clear_rows();
    bool has_rows() const { return !rows_.empty(); }
    int row_count() const { return static_cast<int>(rows_.size()); }

    // -1 means "nothing highlighted": Enter acts on the typed text.
    int selected() const { return selected_; }
    void set_selected(int index);
    // Moves the highlight by `delta` rows, wrapping through the "nothing" slot
    // so the user can always arrow back to what they typed. Returns the new
    // index.
    int move_selection(int delta);

    const omnibox::Suggestion* row(int index) const;

    // Shows the popup flush under the anchor, matching its width.
    void present();

    QSize sizeHint() const override;

signals:
    // A row was clicked.
    void row_activated(int index);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    int row_at(const QPoint& pos) const;
    int content_height() const;
    QString kind_hint(const omnibox::Suggestion& s) const;

    QPointer<QWidget> anchor_;
    std::vector<omnibox::Suggestion> rows_;
    QString engine_name_;
    int selected_ = -1;
    int hovered_ = -1;
};

} // namespace aurora::ui

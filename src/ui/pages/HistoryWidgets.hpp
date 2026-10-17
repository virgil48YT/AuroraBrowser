// Aurora — small helper widgets for the history page.
//
// The radio indicator, filter row and date-group card are the only pieces
// that need signals or custom painting, so they live in one small header +
// cpp beside HistoryPage instead of growing the page's own translation unit
// into a file-with-everything. Everything in here is still aurora::ui and
// still uses the theme tokens from Theme.hpp/ThemeState.hpp.
#pragma once

#include <QFrame>
#include <QHash>
#include <QVector>
#include <QCheckBox>
#include <QLabel>
#include <QWidget>

#include <chrono>
#include <cstdint>

#include "history/HistoryService.hpp"
#include "ui/GlowFrame.hpp"

class QHBoxLayout;
class QVBoxLayout;

namespace aurora::ui {

class HistoryPage;

// Row id stashed on the row widget so selection/remove hit the right
// database row even after filtering or collapse.
inline constexpr const char* kHistoryIdProperty = "history_id";
inline constexpr const char* kHistoryActionProperty = "history_action";

// Square-ish checkbox with a painted tick. The global #PageCheck sheet turns
// every QCheckBox into a pill switch (right for Settings, wrong for a list
// of rows), so history rows draw their own indicator.
class RoundCheck : public QCheckBox {
public:
    explicit RoundCheck(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {20, 20}; }
    QSize minimumSizeHint() const override { return {20, 20}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& pos) const override {
        return rect().contains(pos);
    }
};

// One-line label that elides with "..." instead of forcing its column wider,
// so the domain and time columns stay aligned from row to row.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent = nullptr);
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// Radio indicator for the filter rows. QRadioButton's ::indicator is hard
// to match to the aurora pair at 16px without turning fuzzy, so the ring
// and the checked core dot are painted directly here.
class FilterRadio : public QWidget {
    Q_OBJECT
public:
    explicit FilterRadio(QWidget* parent = nullptr);

    void setChecked(bool on);
    bool isChecked() const { return checked_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool checked_ = false;
};

// One radio row in the filter panel: highlight bar, indicator dot, label.
// Clickable by mousePress and re-polished from "selected" so the QSS bar
// + wash can restyle it without repolishing the whole sheet per row.
class FilterRow : public QWidget {
    Q_OBJECT
public:
    explicit FilterRow(const QString& label, QWidget* parent = nullptr);
    void setSelected(bool on);

signals:
    void pressed();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    QFrame* bar_ = nullptr;
    FilterRadio* radio_ = nullptr;
};

// A single "real" history row: checkbox + favicon + title + domain + time.
// Double-clicking asks the page to re-open the URL, matching the previous
// list's itemActivated behavior.
class HistoryRow : public QWidget {
    Q_OBJECT
public:
    explicit HistoryRow(QString url, QWidget* parent = nullptr);

    void set_visit(const history::Visit& v);
    void refresh_icon(const QString& host);
    qint64 id() const;
    bool checked() const;
    const QString& url() const { return url_; }

signals:
    void activated(const QString& url);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QString url_;
    RoundCheck* check_ = nullptr;
    QLabel* favicon_ = nullptr;
    ElidedLabel* title_ = nullptr;
    ElidedLabel* domain_ = nullptr;
    QLabel* time_ = nullptr;
};

// One collapsible date-group card. The gradient rim and glow are painted by
// the GlowFrame base; the rows are plain HistoryRow children on top.
class HistoryGroupCard : public GlowFrame {
    Q_OBJECT
public:
    explicit HistoryGroupCard(const QString& title, QWidget* parent = nullptr);

    void set_visits(const QVector<history::Visit>& visits);
    void set_collapsed(bool collapsed);
    bool is_collapsed() const { return collapsed_; }

    QVector<qint64> selected_ids() const;
    void refresh_icon(const QString& host);

    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    void row_activated(const QString& url);

private:
    void update_chevron();
    void toggle();

    QWidget* header_ = nullptr;
    QLabel* count_label_ = nullptr;
    QLabel* chevron_ = nullptr;
    QWidget* rows_host_ = nullptr;
    QVBoxLayout* rows_ = nullptr;
    QVector<HistoryRow*> row_widgets_;
    bool collapsed_ = false;
};

} // namespace aurora::ui

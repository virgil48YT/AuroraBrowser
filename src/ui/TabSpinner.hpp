// Aurora — a tiny animated loading spinner painted on the tab strip.
//
// Draws a fading comet-tail ring whose rotation follows the wall clock (60fps timer). Uses the
// theme accent so it reads as a navigation state, not decoration. Self-stopping
// when not visible to keep the chrome idle and CPU-free.
#pragma once

#include <QElapsedTimer>
#include <QWidget>

class QHideEvent;
class QPaintEvent;
class QShowEvent;

namespace aurora::ui {

class TabSpinner : public QWidget {
    Q_OBJECT

public:
    explicit TabSpinner(QWidget* parent = nullptr);

    void setAccent(const QColor& color);
    QSize sizeHint() const override { return {16, 16}; }

public slots:
    void start();
    void stop();

protected:
    void paintEvent(QPaintEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    void tick();
    QColor accent_;
    QElapsedTimer clock_;   // drives the rotation (time-based, not stepped)
    class QTimer* timer_ = nullptr;
    // Visibility is not loading state. QTabBar may show/hide its button slots
    // while laying out or scrolling tabs, so remember the authoritative state
    // separately and never paint an idle spinner even if Qt exposes the slot.
    bool loading_ = false;
};

} // namespace aurora::ui

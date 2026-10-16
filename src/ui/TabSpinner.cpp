#include "ui/TabSpinner.hpp"

#include <QConicalGradient>
#include <QHideEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QShowEvent>
#include <QTimer>

#include <cmath>

#include "ui/Icons.hpp"

namespace aurora::ui {

TabSpinner::TabSpinner(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    timer_ = new QTimer(this);
    // ~60fps with a precise timer; the angle comes from the wall clock, not a
    // per-tick step, so a late tick never makes the motion stutter.
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &TabSpinner::tick);
    hide();
}

void TabSpinner::setAccent(const QColor& color) {
    if (accent_ != color) {
        accent_ = color;
        update();
    }
}

void TabSpinner::start() {
    if (loading_) return;
    loading_ = true;
    clock_.start();
    show();
    if (isVisible() && !timer_->isActive()) timer_->start();
    update();
}

void TabSpinner::stop() {
    loading_ = false;
    timer_->stop();
    hide();
}

void TabSpinner::tick() {
    update();
}

void TabSpinner::paintEvent(QPaintEvent*) {
    if (!loading_) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // One revolution per 900ms, derived from elapsed time.
    constexpr qreal kRevolutionMs = 900.0;
    const qreal angle =
        std::fmod(static_cast<qreal>(clock_.elapsed()) * 360.0 / kRevolutionMs,
                  360.0);

    const qreal stroke = 2.0;
    const qreal inset = stroke / 2.0 + 1.5;
    const QRectF ring = QRectF(rect()).adjusted(inset, inset, -inset, -inset);

    // A faint full track so the ring reads as a circle at rest between frames.
    QColor track = accent_;
    track.setAlpha(46);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(track, stroke));
    p.drawEllipse(ring);

    // A comet: bright at the leading edge, fading to nothing behind it. The
    // conical gradient rotates with the clock, so the tail is always smooth.
    QConicalGradient grad(ring.center(), -angle);
    QColor head = accent_;
    head.setAlpha(255);
    QColor tail = accent_;
    tail.setAlpha(0);
    grad.setColorAt(0.0, head);
    grad.setColorAt(0.72, tail);
    grad.setColorAt(1.0, tail);
    p.setPen(QPen(QBrush(grad), stroke, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(ring);
}

void TabSpinner::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    // A tab can temporarily leave the visible strip while it is still
    // loading. Pause animation while hidden, but do not turn that into an
    // idle state; showEvent() resumes it if the tab comes back into view.
    timer_->stop();
}

void TabSpinner::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (!loading_) {
        // QTabBar is allowed to show its button slot during relayout. Refuse
        // that request for an idle tab so a static "random loader" can never
        // appear merely because a tab was created, switched, or scrolled.
        hide();
        return;
    }
    if (!timer_->isActive()) timer_->start();
}

} // namespace aurora::ui

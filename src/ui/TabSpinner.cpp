#include "ui/TabSpinner.hpp"

#include <QHideEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QShowEvent>
#include <QTimer>

#include "ui/Icons.hpp"

namespace aurora::ui {

TabSpinner::TabSpinner(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    timer_ = new QTimer(this);
    timer_->setInterval(60);  // ~16fps — smooth enough for a 270° arc
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
    angle_ = (angle_ + 30) % 360;
    update();
}

void TabSpinner::paintEvent(QPaintEvent*) {
    if (!loading_) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(width() / 2.0, height() / 2.0);
    p.rotate(angle_);
    p.translate(-width() / 2.0, -height() / 2.0);
    icons::paint(&p, icons::Name::Spinner, rect(), accent_);
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

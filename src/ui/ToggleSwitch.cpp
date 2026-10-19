#include "ui/ToggleSwitch.hpp"

#include <QEasingCurve>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QVariantAnimation>

#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {
constexpr int kTrackW = 34;
constexpr int kTrackH = 20;
constexpr int kKnob = 14;
constexpr int kKnobInset = 3;
constexpr int kTextGap = 8;
} // namespace

ToggleSwitch::ToggleSwitch(QWidget* parent) : QCheckBox(parent) { init(); }

ToggleSwitch::ToggleSwitch(const QString& text, QWidget* parent)
    : QCheckBox(text, parent) {
    init();
}

void ToggleSwitch::init() {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    anim_ = new QVariantAnimation(this);
    anim_->setDuration(140);
    anim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim_, &QVariantAnimation::valueChanged, this,
            [this] { update(); });
    connect(this, &QCheckBox::toggled, this, [this](bool on) {
        // Slide from wherever the knob is now, so a quick double-click never
        // snaps the knob across the track.
        const qreal from = progress();
        anim_->stop();
        anim_->setStartValue(from);
        anim_->setEndValue(on ? 1.0 : 0.0);
        anim_->start();
    });
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

qreal ToggleSwitch::progress() const {
    if (anim_ && anim_->state() == QAbstractAnimation::Running) {
        return anim_->currentValue().toReal();
    }
    return isChecked() ? 1.0 : 0.0;
}

QSize ToggleSwitch::sizeHint() const {
    int w = kTrackW;
    int h = kTrackH;
    if (!text().isEmpty()) {
        const QFontMetrics fm(font());
        w += kTextGap + fm.horizontalAdvance(text());
        h = qMax(h, fm.height());
    }
    return {w, h};
}

bool ToggleSwitch::hitButton(const QPoint& pos) const {
    return rect().contains(pos);
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    const ThemeColors& c = current_theme().colors;
    const qreal t = progress();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (!isEnabled()) p.setOpacity(0.5);

    const QRectF track(0.5, (height() - kTrackH) / 2.0 + 0.5, kTrackW - 1,
                       kTrackH - 1);
    const qreal radius = track.height() / 2.0;

    // Off track, then the on track faded in over it.
    p.setPen(QPen(c.toggle_border_off, 1.0));
    p.setBrush(c.toggle_track_off);
    p.drawRoundedRect(track, radius, radius);

    if (t > 0.0) {
        // Purple, deep violet -> neon purple: the mockup's switches read as
        // one purple pill rather than a purple -> magenta ramp.
        QLinearGradient grad(track.topLeft(), track.topRight());
        grad.setColorAt(0.0, c.accent2.darker(135));
        grad.setColorAt(1.0, c.accent2);
        p.setOpacity((isEnabled() ? 1.0 : 0.5) * t);
        p.setPen(QPen(c.accent2, 1.0));
        p.setBrush(grad);
        p.drawRoundedRect(track, radius, radius);
        p.setOpacity(isEnabled() ? 1.0 : 0.5);
    }

    // The knob: a true circle that slides between the two ends.
    const qreal travel = kTrackW - 2 * kKnobInset - kKnob;
    const qreal kx = kKnobInset + travel * t;
    const qreal ky = track.center().y() - kKnob / 2.0;
    QColor knob = t < 0.5 ? c.toggle_knob_off : c.toggle_knob_on;
    p.setPen(Qt::NoPen);
    p.setBrush(knob);
    p.drawEllipse(QRectF(kx, ky, kKnob, kKnob));

    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c.accent2, 1.5));
        p.drawRoundedRect(track.adjusted(-1.5, -1.5, 1.5, 1.5), radius + 1.5,
                          radius + 1.5);
    }

    if (!text().isEmpty()) {
        p.setPen(palette().color(QPalette::WindowText));
        p.setFont(font());
        p.drawText(QRectF(kTrackW + kTextGap, 0, width() - kTrackW - kTextGap,
                          height()),
                   Qt::AlignVCenter | Qt::AlignLeft, text());
    }
}

} // namespace aurora::ui

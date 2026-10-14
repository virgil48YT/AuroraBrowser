#include "ui/IconButton.hpp"

#include <QEnterEvent>
#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionToolButton>

#include "ui/ThemeState.hpp"

namespace aurora::ui {

IconButton::IconButton(icons::Name name, QWidget* parent)
    : QToolButton(parent), name_(name) {
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::NoFocus);
    setAutoRaise(false);
    setPopupMode(QToolButton::DelayedPopup);
    // No Qt icon or text: the stylesheet paints the panel; we paint the glyph.
}

IconButton::IconButton(icons::Name name, Role role, QWidget* parent)
    : IconButton(name, parent) {
    setRole(role);
}

void IconButton::setIconName(icons::Name name) {
    if (name_ != name) {
        name_ = name;
        update();
    }
}

void IconButton::setRole(Role role) {
    if (role_ != role) {
        role_ = role;
        update();
    }
}

void IconButton::setIconColor(const QColor& c) {
    override_color_ = c;
    update();
}

QSize IconButton::sizeHint() const {
    return {32, 32};
}

QSize IconButton::minimumSizeHint() const {
    return {28, 28};
}

QRectF IconButton::icon_rect() const {
    // Centre in physical pixels, not just logical coordinates. Fractional
    // display scales can otherwise place an otherwise symmetric small glyph
    // between physical pixels, making one diagonal look softer than the other.
    const qreal dpr = qMax<qreal>(1.0, devicePixelRatioF());
    const int button_w_px = qRound(width() * dpr);
    const int button_h_px = qRound(height() * dpr);
    const int glyph_px = qMax(1, qRound(icon_px_ * dpr));
    const int x_px = qRound((button_w_px - glyph_px) / 2.0);
    const int y_px = qRound((button_h_px - glyph_px) / 2.0);
    return QRectF(x_px / dpr, y_px / dpr, glyph_px / dpr, glyph_px / dpr);
}

QColor IconButton::computed_color() const {
    const auto& c = current_theme().colors;
    if (!isEnabled()) return c.text_faint;
    const bool down = isDown() || isChecked();
    const bool hover = underMouse();
    // The frameless window's close target uses a strong danger background.
    // Keep its X neutral at rest and high-contrast while hovered/pressed.
    if (objectName() == QStringLiteral("WindowCloseButton")) {
        return (down || hover) ? QColor(Qt::white) : c.text_muted;
    }
    // A tab X uses a quiet danger wash, so tinting the glyph (rather than
    // turning it teal through the generic pressed state) reads consistently.
    if (objectName() == QStringLiteral("TabCloseButton")) {
        if (down) return c.danger.darker(115);
        if (hover) return c.danger;
        return c.text_muted;
    }
    switch (role_) {
        case Role::Primary:
            if (down) return c.accent_hover;
            return c.accent;
        case Role::Danger:
            if (down) return c.error.lighter(120);
            if (hover) return c.error;
            return c.text_muted;
        case Role::Neutral:
        default:
            if (down) return c.accent_hover;
            if (hover) return c.text;
            return c.text_muted;
    }
}

void IconButton::paintEvent(QPaintEvent* e) {
    // Let the stylesheet draw the hover/pressed background panel.
    QToolButton::paintEvent(e);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = override_color_.isValid() ? override_color_ : computed_color();
    icons::paint(&p, name_, icon_rect(), color);
}

void IconButton::enterEvent(QEnterEvent* e) {
    QToolButton::enterEvent(e);
    update();
}

void IconButton::leaveEvent(QEvent* e) {
    QToolButton::leaveEvent(e);
    update();
}

void IconButton::changeEvent(QEvent* e) {
    QToolButton::changeEvent(e);
    if (e->type() == QEvent::EnabledChange) update();
}

} // namespace aurora::ui

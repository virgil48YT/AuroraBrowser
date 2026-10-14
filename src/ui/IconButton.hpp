// Aurora — a themed, state-aware icon button.
//
// IconButton owns no Qt icon or text; it lets the stylesheet paint the
// background (hover/pressed/focus) and draws its SVG glyph on top in a color
// derived from the current interaction state. This is what makes the chrome
// feel alive: muted at rest, full-strength on hover, accent when pressed,
// faint when disabled — without ever swapping icon pixmaps at runtime.
#pragma once

#include <QColor>
#include <QToolButton>

#include "ui/Icons.hpp"

namespace aurora::ui {

class IconButton : public QToolButton {
    Q_OBJECT
    Q_PROPERTY(QColor iconColor READ iconColor WRITE setIconColor)

public:
    enum class Role { Neutral, Primary, Danger };

    explicit IconButton(icons::Name name, QWidget* parent = nullptr);
    explicit IconButton(icons::Name name, Role role, QWidget* parent = nullptr);

    void setIconName(icons::Name name);
    icons::Name iconName() const { return name_; }

    // Optical glyph size in logical px (default 18). Smaller controls
    // (window controls, tab close, omnibox star) drop to 14-16 so stroke
    // weight stays visually uniform across the chrome.
    void setIconPx(int px) { if (px > 0 && icon_px_ != px) { icon_px_ = px; update(); } }
    int iconPx() const { return icon_px_; }

    void setRole(Role role);
    Role role() const { return role_; }

    // Override the computed tint. When invalid (default), the tint is derived
    // from state + role + the current theme.
    QColor iconColor() const { return override_color_; }
    void setIconColor(const QColor& c);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    icons::Name name_;
    Role role_ = Role::Neutral;
    QColor override_color_;
    int icon_px_ = 18;

    QRectF icon_rect() const;
    QColor computed_color() const;
};

} // namespace aurora::ui

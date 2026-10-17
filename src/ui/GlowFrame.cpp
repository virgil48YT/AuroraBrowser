#include "ui/GlowFrame.hpp"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

// Default edge for the ordinary cards (history groups, filter panel,
// actions): the cyan end is deliberately faint so the glow reads as a
// blue-purple haze rather than a neon circuit trace. The history mock has
// a small purple/blue rim, not a rainbow loop.
constexpr int kCardCyanAlpha = 130;
constexpr int kCardPurpleAlpha = 80;
// One joint purple->magenta rim for destructive actions (Clear browsing
// data) so they read as "this one is different, be careful".
constexpr int kDangerPurpleAlpha = 170;
constexpr int kDangerMagentaAlpha = 110;
constexpr int kCardFillAlpha = 122;   // ~48% over the aurora background
constexpr int kDangerFillAlpha = 90;  // the button stays a touch lighter

} // namespace

GlowFrame::GlowFrame(QWidget* parent) : QFrame(parent) {
    // No stylesheet background: the glass plate is painted by paintEvent and
    // children sit on top transparent over it.
    setAttribute(Qt::WA_StyledBackground, false);
}

void GlowFrame::apply_card_style(int radius, int glow_extent) {
    const ThemeColors& c = current_theme().colors;
    QColor cyan = c.accent;
    cyan.setAlpha(kCardCyanAlpha);
    QColor purple = c.accent2;
    purple.setAlpha(kCardPurpleAlpha);
    QColor glow = c.accent2;
    glow.setAlpha(28);
    QColor fill = c.card_bg;
    fill.setAlpha(kCardFillAlpha);
    set_style({cyan, purple, glow, glow_extent}, fill, radius);
}

void GlowFrame::apply_danger_style(int radius, int glow_extent) {
    const ThemeColors& c = current_theme().colors;
    QColor purple = c.accent2;
    purple.setAlpha(kDangerPurpleAlpha);
    QColor magenta = c.accent3;
    magenta.setAlpha(kDangerMagentaAlpha);
    QColor glow = c.accent3;
    glow.setAlpha(30);
    // A barely-raised fill; the clear button's content (icon + label) is
    // the emphasis, not its surface.
    QColor fill = c.surface_alt;
    fill.setAlpha(kDangerFillAlpha);
    set_style({purple, magenta, glow, glow_extent}, fill, radius);
}

void GlowFrame::set_style(const Edge& edge, const QColor& fill, int radius) {
    edge_ = edge;
    fill_ = fill;
    radius_ = radius;
    update();
}

void GlowFrame::paintEvent(QPaintEvent* event) {
    // The gradient border and outer aura are the whole reason this class
    // exists; QSS is left to colors, paddings and child widgets.
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Inset the plate by the glow extent so the aura stays inside the widget
    // rect instead of being clipped by it.
    QRectF r = QRectF(rect())
                   .adjusted(edge_.glow_extent, edge_.glow_extent,
                             -edge_.glow_extent, -edge_.glow_extent);
    const qreal inset = 0.75;  // half the 1.5px stroke, for crisp edges
    r.adjust(inset, inset, -inset, -inset);
    const qreal rad = qMax<qreal>(0.0, radius_ - edge_.glow_extent - inset);

    QLinearGradient grad(r.topLeft(), r.bottomRight());
    grad.setColorAt(0.0, edge_.color1);
    grad.setColorAt(1.0, edge_.color2);

    // Outer aura: two soft passes so the edge blooms just a little, like the
    // mock's panel shadows. Skipped when no glow color is set.
    if (edge_.glow.isValid() && edge_.glow_extent > 0) {
        QColor outer = edge_.glow;
        outer.setAlpha(edge_.glow.alpha() / 2);
        p.setPen(Qt::NoPen);
        p.setBrush(outer);
        p.drawRoundedRect(r.adjusted(-edge_.glow_extent, -edge_.glow_extent,
                                     edge_.glow_extent, edge_.glow_extent),
                          rad + edge_.glow_extent, rad + edge_.glow_extent);
        p.setBrush(edge_.glow);
        p.drawRoundedRect(r.adjusted(-1.2, -1.2, 1.2, 1.2),
                          rad + 1.2, rad + 1.2);
    }

    // The glass plate: semi-transparent so the aurora wallpaper shows
    // through, solid enough for text to stay readable.
    if (fill_.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(fill_);
        p.drawRoundedRect(r, rad, rad);
    }

    // Gradient rim on top of the fill so it never dips under the plate.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(grad), 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawRoundedRect(r, rad, rad);
}

} // namespace aurora::ui

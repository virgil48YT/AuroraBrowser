#include "ui/GlowFrame.hpp"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

// Default edge for the ordinary cards (history groups, filter panel,
// actions): the cyan end is deliberately faint so the glow reads as a
// blue-purple haze rather than a neon circuit trace. The history mock has
// a small purple/blue rim, not a rainbow loop.
// The mock's cards are near-black navy plates with a thin electric-blue edge
// that warms to violet toward the lower right, and a tight blue halo. These
// are fixed brand colors rather than theme tokens because the look is the
// point: the plate must stay this dark whatever the surface tokens are.
const QColor kCardEdgeA(54, 112, 255, 215);     // electric blue
const QColor kCardEdgeB(124, 78, 240, 170);     // violet
const QColor kCardHalo(52, 96, 255, 96);
const QColor kCardPlate(5, 9, 30, 196);         // ~77% navy over the aurora
// One joint purple->magenta rim for destructive actions (Clear browsing
// data) so they read as "this one is different, be careful".
const QColor kDangerEdgeA(176, 84, 255, 235);
const QColor kDangerEdgeB(236, 72, 190, 200);
const QColor kDangerHalo(200, 70, 240, 100);
const QColor kDangerPlate(26, 8, 40, 190);

} // namespace

GlowFrame::GlowFrame(QWidget* parent) : QFrame(parent) {
    // No stylesheet background: the glass plate is painted by paintEvent and
    // children sit on top transparent over it.
    setAttribute(Qt::WA_StyledBackground, false);
}

void GlowFrame::apply_card_style(int radius, int glow_extent) {
    set_style({kCardEdgeA, kCardEdgeB, kCardHalo, glow_extent}, kCardPlate,
              radius);
}

void GlowFrame::apply_menu_style(bool dark, const QColor& light_fill,
                                 int radius, int glow_extent) {
    QColor plate = kCardPlate;
    plate.setAlpha(244);
    set_style({kCardEdgeA, kCardEdgeB, kCardHalo, glow_extent},
              dark ? plate : light_fill, radius);
}

void GlowFrame::apply_danger_style(int radius, int glow_extent) {
    set_style({kDangerEdgeA, kDangerEdgeB, kDangerHalo, glow_extent},
              kDangerPlate, radius);
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
    // The radius is the plate's own radius (not reduced by the glow), so the
    // corners are exactly as round as asked and rim, fill and aura agree.
    const qreal rad = qMin<qreal>(radius_, qMin(r.width(), r.height()) / 2.0);

    QPainterPath plate;
    plate.addRoundedRect(r, rad, rad);

    // Outer aura: a few widening, faint strokes along the edge, clipped to
    // the OUTSIDE of the plate. Painting them as filled shapes behind the
    // semi-transparent plate tinted its inside and read as a second outline.
    if (edge_.glow.isValid() && edge_.glow_extent > 0) {
        QPainterPath outside;
        outside.addRect(QRectF(rect()));
        p.save();
        p.setClipPath(outside.subtracted(plate));
        QColor pass = edge_.glow;
        pass.setAlpha(qBound(4, edge_.glow.alpha() / edge_.glow_extent, 255));
        p.setBrush(Qt::NoBrush);
        for (int i = edge_.glow_extent; i >= 1; --i) {
            p.setPen(QPen(pass, 2.0 * i));
            p.drawPath(plate);
        }
        p.restore();
    }

    // The glass plate: semi-transparent so the aurora wallpaper shows
    // through, solid enough for text to stay readable.
    if (fill_.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(fill_);
        p.drawPath(plate);
    }

    // Gradient rim on top of the fill so it never dips under the plate.
    QLinearGradient grad(r.topLeft(), r.bottomRight());
    grad.setColorAt(0.0, edge_.color1);
    grad.setColorAt(1.0, edge_.color2);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(grad), 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawPath(plate);
}

} // namespace aurora::ui

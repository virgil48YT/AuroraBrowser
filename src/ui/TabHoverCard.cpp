#include "ui/TabHoverCard.hpp"

#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

constexpr int kCardWidth = 272;
constexpr int kPadX = 16;
constexpr int kPadTop = 16;
constexpr int kPadBottom = 14;
constexpr int kGapBelowTab = 6;
constexpr int kRadius = 12;
constexpr int kLineGap = 3;      // between title lines
constexpr int kSectionGap = 6;   // title block -> address
constexpr int kRuleGap = 14;     // address -> hairline, hairline -> memory

// Splits `text` over at most two lines of `width`: the first breaks at the
// last space that fits, the second is elided with "..." (Qt only elides a
// single line, so the wrap is done by hand).
void wrap_two_lines(const QString& text, const QFontMetrics& fm, int width,
                    QString* line1, QString* line2) {
    *line2 = QString();
    if (fm.horizontalAdvance(text) <= width) {
        *line1 = text;
        return;
    }
    int cut = -1;
    for (int i = 1; i <= text.size(); ++i) {
        if (fm.horizontalAdvance(text.left(i)) > width) break;
        if (i < text.size() && text.at(i) == QLatin1Char(' ')) cut = i;
    }
    if (cut <= 0) {
        // One long unbroken word: break at the last character that fits.
        cut = 1;
        while (cut < text.size() &&
               fm.horizontalAdvance(text.left(cut + 1)) <= width) ++cut;
    }
    *line1 = text.left(cut).trimmed();
    *line2 = fm.elidedText(text.mid(cut).trimmed(), Qt::ElideRight, width);
}

// The small speedometer glyph in front of "Memory usage".
void paint_gauge(QPainter& p, const QRectF& box, const QColor& color) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
    // Open dial: 240 degrees, gap at the bottom.
    const QRectF dial = box.adjusted(1.5, 1.5, -1.5, -1.5);
    p.drawArc(dial, -30 * 16, 240 * 16);
    // Needle toward the upper right.
    const QPointF c = dial.center();
    p.drawLine(c, QPointF(c.x() + dial.width() * 0.27,
                          c.y() - dial.height() * 0.27));
    p.restore();
}

} // namespace

TabHoverCard::TabHoverCard(QWidget* parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint |
                          Qt::NoDropShadowWindowHint |
                          Qt::WindowDoesNotAcceptFocus) {
    setObjectName(QStringLiteral("TabHoverCard"));
    // Translucent window: the rounded corners show the page behind them
    // instead of a square backing.
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFixedWidth(kCardWidth);
}

QString TabHoverCard::format_memory(qint64 bytes) {
    if (bytes < 0) return {};
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(qRound(mb));
}

void TabHoverCard::set_content(const QString& title, const QString& address,
                               const QString& memory_text) {
    title_ = title.isEmpty() ? QStringLiteral("New Tab") : title;
    address_ = address;
    memory_ = memory_text;
    relayout();
    update();
}

void TabHoverCard::set_memory_text(const QString& memory_text) {
    memory_ = memory_text;
    relayout();
    update();
}

void TabHoverCard::relayout() {
    QFont title_font = font();
    title_font.setWeight(QFont::DemiBold);
    title_font.setPixelSize(current_theme().body_font_px + 1);
    wrap_two_lines(title_, QFontMetrics(title_font), kCardWidth - 2 * kPadX,
                   &title_line1_, &title_line2_);

    const QFontMetrics tfm(title_font);
    QFont small = font();
    small.setPixelSize(current_theme().body_font_px);
    const QFontMetrics sfm(small);

    int h = kPadTop + tfm.height();
    if (!title_line2_.isEmpty()) h += kLineGap + tfm.height();
    if (!address_.isEmpty()) h += kSectionGap + sfm.height();
    if (!memory_.isEmpty()) h += kRuleGap + 1 + kRuleGap + sfm.height();
    h += kPadBottom;
    setFixedHeight(h);
}

void TabHoverCard::paintEvent(QPaintEvent*) {
    const ThemeColors& c = current_theme().colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setBrush(c.surface_raised);
    p.setPen(QPen(c.border_subtle, 1.0));
    p.drawRoundedRect(r, kRadius, kRadius);

    QFont title_font = font();
    title_font.setWeight(QFont::DemiBold);
    title_font.setPixelSize(current_theme().body_font_px + 1);
    const QFontMetrics tfm(title_font);
    QFont small = font();
    small.setPixelSize(current_theme().body_font_px);
    const QFontMetrics sfm(small);

    int y = kPadTop;
    const int w = kCardWidth - 2 * kPadX;

    p.setFont(title_font);
    p.setPen(c.text);
    p.drawText(QRect(kPadX, y, w, tfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               title_line1_);
    y += tfm.height();
    if (!title_line2_.isEmpty()) {
        y += kLineGap;
        p.drawText(QRect(kPadX, y, w, tfm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, title_line2_);
        y += tfm.height();
    }

    p.setFont(small);
    if (!address_.isEmpty()) {
        y += kSectionGap;
        p.setPen(c.text_muted);
        p.drawText(QRect(kPadX, y, w, sfm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   sfm.elidedText(address_, Qt::ElideMiddle, w));
        y += sfm.height();
    }

    if (!memory_.isEmpty()) {
        y += kRuleGap;
        QColor rule = c.divider;
        rule.setAlpha(160);
        p.setPen(QPen(rule, 1.0));
        p.drawLine(QPointF(kPadX, y + 0.5), QPointF(width() - kPadX, y + 0.5));
        y += 1 + kRuleGap;

        const int glyph = 16;
        paint_gauge(p, QRectF(kPadX, y + (sfm.height() - glyph) / 2.0, glyph, glyph),
                    c.text_muted);
        p.setPen(c.text);
        p.drawText(QRect(kPadX + glyph + 10, y, w - glyph - 10, sfm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   sfm.elidedText(memory_, Qt::ElideRight, w - glyph - 10));
    }
}

void TabHoverCard::show_below(const QRect& tab_rect_global) {
    relayout();
    QPoint pos(tab_rect_global.center().x() - width() / 2,
               tab_rect_global.bottom() + kGapBelowTab);
    const QScreen* screen = QGuiApplication::screenAt(tab_rect_global.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        pos.setX(qBound(area.left() + 4, pos.x(), area.right() - width() - 4));
        pos.setY(qMin(pos.y(), area.bottom() - height() - 4));
    }
    move(pos);
    show();
    raise();
}

} // namespace aurora::ui

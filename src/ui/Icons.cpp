#include "ui/Icons.hpp"

#include <QGuiApplication>
#include <QPainter>
#include <QPixmapCache>
#include <QScreen>
#include <QSvgRenderer>

namespace aurora::ui::icons {

namespace {

// Wrap a path payload in a complete 24×24 SVG document. Stroke icons share one
// visual weight; filled icons pass fill="__C__" stroke="none".
QString doc(const QString& body, bool filled = false) {
    QString fill = filled ? QStringLiteral("__C__") : QStringLiteral("none");
    QString stroke = filled ? QStringLiteral("none") : QStringLiteral("__C__");
    return QStringLiteral(
               "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" "
               "fill=\"%1\" stroke=\"%2\" stroke-width=\"1.9\" "
               "stroke-linecap=\"round\" stroke-linejoin=\"round\">")
               .arg(fill, stroke)
        + body + QStringLiteral("</svg>");
}

} // namespace

QString svg_markup(Name n) {
    switch (n) {
        case Name::Back:
            return doc("<path d=\"M19 12H6\"/><path d=\"M11 6l-6 6 6 6\"/>");
        case Name::Forward:
            return doc("<path d=\"M5 12h13\"/><path d=\"M13 6l6 6-6 6\"/>");
        case Name::Reload:
            return doc("<path d=\"M21 12a9 9 0 1 1-2.64-6.36\"/><path d=\"M21 3v6h-6\"/>");
        case Name::Stop:
            return doc("<path d=\"M6 6l12 12\"/><path d=\"M18 6L6 18\"/>");
        case Name::Home:
            return doc("<path d=\"M4 11l8-8 8 8\"/><path d=\"M6 9.5V20h12V9.5\"/>"
                       "<path d=\"M9.5 20v-5h5v5\"/>");
        case Name::Search:
            return doc("<circle cx=\"11\" cy=\"11\" r=\"7\"/><path d=\"M21 21l-4.3-4.3\"/>");
        case Name::Plus:
            return doc("<path d=\"M12 5v14\"/><path d=\"M5 12h14\"/>");
        case Name::Close:
            // The genuine Chrome / Brave tab-close glyph: the exact path from
            // Chromium's ui/views/vector_icons/close.icon (canvas 24, matching
            // our viewBox). It is a FILLED shape (two crossed rounded-capsule
            // bars), not a thin stroke — a 1.9px stroked X with round caps
            // turns into four fuzzy blobs at the 12px tab-close size this glyph
            // is drawn at, while a solid filled shape stays crisp on any DPR.
            // Rounded arm ends match Chrome/Brave; flat-cut Material X did not.
            return doc(
                "<path d=\"M 12 13.4 l -4.9 4.9 a 0.95 0.95 0 0 1 -0.7 0.27 "
                "a 0.95 0.95 0 0 1 -0.7 -0.27 a 0.95 0.95 0 0 1 -0.27 -0.7 "
                "c 0 -0.28 0.09 -0.52 0.27 -0.7 L 10.6 12 L 5.7 7.1 "
                "a 0.95 0.95 0 0 1 -0.27 -0.7 c 0 -0.28 0.09 -0.51 0.27 -0.7 "
                "a 0.95 0.95 0 0 1 0.7 -0.27 c 0.28 0 0.52 0.09 0.7 0.27 "
                "L 12 10.6 l 4.9 -4.9 a 0.95 0.95 0 0 1 0.7 -0.27 "
                "c 0.28 0 0.52 0.09 0.7 0.27 a 0.95 0.95 0 0 1 0.27 0.7 "
                "a 0.95 0.95 0 0 1 -0.27 0.7 L 13.4 12 l 4.9 4.9 "
                "a 0.95 0.95 0 0 1 0.27 0.7 c 0 0.28 -0.09 0.52 -0.27 0.7 "
                "a 0.95 0.95 0 0 1 -0.7 0.27 a 0.95 0.95 0 0 1 -0.7 -0.27 Z\"/>",
                true);
        case Name::NewTab:
            return doc("<rect x=\"4\" y=\"5\" width=\"16\" height=\"15\" rx=\"3\"/>"
                       "<path d=\"M12 9v7\"/><path d=\"M8.5 12.5h7\"/>");
        case Name::StarOutline:
            return doc("<path d=\"M12 4.5l2.6 5.3 5.9.9-4.3 4.1 1 5.8L12 18l-5.2 2.6 1-5.8-4.3-4.1 5.9-.9z\"/>");
        case Name::StarFilled:
            return doc("<path d=\"M12 4.5l2.6 5.3 5.9.9-4.3 4.1 1 5.8L12 18l-5.2 2.6 1-5.8-4.3-4.1 5.9-.9z\"/>",
                       true);
        case Name::Bookmarks:
            return doc("<path d=\"M6 3h12a1 1 0 0 1 1 1v16.5l-7-4-7 4V4a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M9 8h6\"/>");
        case Name::Bookmark:
            return doc("<path d=\"M6 3h12a1 1 0 0 1 1 1v16.5l-7-4-7 4V4a1 1 0 0 1 1-1z\"/>");
        case Name::History:
            return doc("<path d=\"M3 12a9 9 0 1 0 3-6.7\"/><path d=\"M3 4v3h3\"/>"
                       "<path d=\"M12 8v4l3 2\"/>");
        case Name::Downloads:
            return doc("<path d=\"M12 3v8\"/><path d=\"M8 9l4 4 4-4\"/>"
                       "<path d=\"M4 17v2a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2\"/>");
        case Name::Settings:
            return doc("<path d=\"M4 7h9\"/><path d=\"M19 7h1\"/><circle cx=\"16\" cy=\"7\" r=\"2\"/>"
                       "<path d=\"M4 12h1\"/><path d=\"M11 12h9\"/><circle cx=\"8\" cy=\"12\" r=\"2\"/>"
                       "<path d=\"M4 17h7\"/><path d=\"M17 17h3\"/><circle cx=\"14\" cy=\"17\" r=\"2\"/>");
        case Name::More:
            // Filled dots: a hairline-stroked near-zero-radius circle rendered
            // as a smudge at small sizes; solid dots stay crisp at 16px.
            return doc("<circle cx=\"12\" cy=\"5.5\" r=\"1.6\" fill=\"__C__\" stroke=\"none\"/>"
                       "<circle cx=\"12\" cy=\"12\" r=\"1.6\" fill=\"__C__\" stroke=\"none\"/>"
                       "<circle cx=\"12\" cy=\"18.5\" r=\"1.6\" fill=\"__C__\" stroke=\"none\"/>");
        case Name::Puzzle:
            // Material-style extension glyph (filled) — the browser-canonical
            // extensions affordance.
            return doc("<path d=\"M20.5 11H19V6c0-1.1-.9-2-2-2h-4V3.5C13 2.12 11.88 1 10.5 1S8 2.12 8 3.5V4H4c-1.1 0-1.99.9-1.99 2v3.8H3.5c1.49 0 2.7 1.21 2.7 2.7s-1.21 2.7-2.7 2.7H2V20c0 1.1.9 2 2 2h3.8v-1.5c0-1.49 1.21-2.7 2.7-2.7 1.49 0 2.7 1.21 2.7 2.7V22H17c1.1 0 2-.9 2-2v-4h1.5c1.38 0 2.5-1.12 2.5-2.5S21.88 11 20.5 11z\"/>",
                       true);
        case Name::Code:
            return doc("<path d=\"M8 6l-6 6 6 6\"/><path d=\"M16 6l6 6-6 6\"/>");
        case Name::ZoomIn:
            return doc("<circle cx=\"11\" cy=\"11\" r=\"7\"/><path d=\"M21 21l-4.3-4.3\"/>"
                       "<path d=\"M11 8v6\"/><path d=\"M8 11h6\"/>");
        case Name::ZoomOut:
            return doc("<circle cx=\"11\" cy=\"11\" r=\"7\"/><path d=\"M21 21l-4.3-4.3\"/>"
                       "<path d=\"M8 11h6\"/>");
        case Name::Fullscreen:
            return doc("<path d=\"M5 9V6a1 1 0 0 1 1-1h3\"/><path d=\"M19 9V6a1 1 0 0 0-1-1h-3\"/>"
                       "<path d=\"M5 15v3a1 1 0 0 0 1 1h3\"/><path d=\"M19 15v3a1 1 0 0 1-1 1h-3\"/>");
        case Name::FullscreenExit:
            return doc("<path d=\"M9 5v3a1 1 0 0 1-1 1H5\"/><path d=\"M15 5v3a1 1 0 0 0 1 1h3\"/>"
                       "<path d=\"M9 19v-3a1 1 0 0 0-1-1H5\"/><path d=\"M15 19v-3a1 1 0 0 1 1-1h3\"/>");
        case Name::ChevronLeft:
            return doc("<path d=\"M15 6l-6 6 6 6\"/>");
        case Name::ChevronRight:
            return doc("<path d=\"M9 6l6 6-6 6\"/>");
        case Name::ChevronUp:
            return doc("<path d=\"M6 15l6-6 6 6\"/>");
        case Name::ChevronDown:
            return doc("<path d=\"M6 9l6 6 6-6\"/>");
        case Name::Pin:
            return doc("<path d=\"M9 4h6l-1 5 3 4H7l3-4-1-5z\"/><path d=\"M12 17v3\"/>");
        case Name::Unpin:
            return doc("<path d=\"M9 4h6l-1 5 3 4H7l3-4-1-5z\"/><path d=\"M5 5l14 14\"/>");
        case Name::Mute:
            return doc("<path d=\"M4 9v6h3l5 4V5L7 9H4z\"/><path d=\"M16 9l5 6\"/><path d=\"M21 9l-5 6\"/>");
        case Name::Unmute:
            return doc("<path d=\"M4 9v6h3l5 4V5L7 9H4z\"/><path d=\"M15.5 9a3.5 3.5 0 0 1 0 6\"/>"
                       "<path d=\"M18.5 6.5a6.5 6.5 0 0 1 0 11\"/>");
        case Name::Copy:
            return doc("<rect x=\"9\" y=\"9\" width=\"11\" height=\"11\" rx=\"2\"/>"
                       "<path d=\"M5 15V5a2 2 0 0 1 2-2h8\"/>");
        case Name::Duplicate:
            return doc("<rect x=\"8\" y=\"8\" width=\"12\" height=\"12\" rx=\"2\"/>"
                       "<path d=\"M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2\"/>");
        case Name::Sun:
            return doc("<circle cx=\"12\" cy=\"12\" r=\"4\"/>"
                       "<path d=\"M12 2v2\"/><path d=\"M12 20v2\"/><path d=\"M2 12h2\"/><path d=\"M20 12h2\"/>"
                       "<path d=\"M5 5l1.5 1.5\"/><path d=\"M17.5 17.5L19 19\"/>"
                       "<path d=\"M19 5l-1.5 1.5\"/><path d=\"M6.5 17.5L5 19\"/>");
        case Name::Moon:
            return doc("<path d=\"M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z\"/>");
        case Name::Shield:
            return doc("<path d=\"M12 3l7 3v5c0 4.2-2.9 7.6-7 9-4.1-1.4-7-4.8-7-9V6z\"/>"
                       "<path d=\"M9 12l2 2 4-4\"/>");
        case Name::Globe:
            return doc("<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M4 12h16\"/>"
                       "<path d=\"M12 4a10 10 0 0 1 0 16 10 10 0 0 1 0-16z\"/>");
        case Name::Info:
            return doc("<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M12 11v4\"/><circle cx=\"12\" cy=\"8\" r=\"0.6\" fill=\"__C__\" stroke=\"none\"/>");
        case Name::Lock:
            return doc("<rect x=\"5\" y=\"11\" width=\"14\" height=\"9\" rx=\"2\"/><path d=\"M8 11V8a4 4 0 0 1 8 0v3\"/>");
        case Name::Trash:
            return doc("<path d=\"M4 7h16\"/><path d=\"M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2\"/>"
                       "<path d=\"M6 7l1 13a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1l1-13\"/>");
        case Name::Minimize:
            return doc("<path d=\"M5 12h14\"/>");
        case Name::Maximize:
            return doc("<rect x=\"5\" y=\"5\" width=\"14\" height=\"14\" rx=\"1.5\"/>");
        case Name::Restore:
            return doc("<rect x=\"5\" y=\"8\" width=\"11\" height=\"11\" rx=\"1.5\"/>"
                       "<path d=\"M8 8V6a1 1 0 0 1 1-1h8a1 1 0 0 1 1 1v8a1 1 0 0 1-1 1h-2\"/>");
        case Name::Spinner:
            // A 270° arc; TabSpinner rotates this via painter transform.
            return doc("<path d=\"M12 3a9 9 0 1 1-6.2 15.5\"/>");
        case Name::Folder:
            return doc("<path d=\"M3 7a2 2 0 0 1 2-2h4.2a2 2 0 0 1 1.6.8L12.6 8H19a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z\"/>");
        case Name::FolderOpen:
            // Open folder: an upright folder whose front flap is hinged open
            // at the top-left and swings out to the RIGHT, so the contents
            // read as visible. Used on the downloads popover "Show in folder"
            // button instead of the generic QStyle::SP_DirIcon.
            //
            // One continuous outline (Material/Feather "folder-open"): the
            // flap's top edge runs x 9.24 -> 21.94 at y ~10-12.5 while its
            // bottom edge runs x 4 -> 20.4 at y 20, i.e. the trapezoid flares
            // outward as it rises. The previous two-path version had that
            // taper the other way round, which is what read as upside-down.
            return doc("<path d=\"M6 14l1.5-2.9A2 2 0 0 1 9.24 10H20"
                       "a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4"
                       "a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9"
                       "l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2\"/>");
        case Name::OpenExternal:
            return doc("<path d=\"M14 4h6v6\"/><path d=\"M20 4L11 13\"/>"
                       "<path d=\"M19 14v5a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h5\"/>");
        case Name::Mic:
            return doc("<rect x=\"9\" y=\"3\" width=\"6\" height=\"11\" rx=\"3\"/>"
                       "<path d=\"M5 11a7 7 0 0 0 14 0\"/><path d=\"M12 18v3\"/>");
        case Name::Camera:
            return doc("<path d=\"M4 8a2 2 0 0 1 2-2h2l1.2-1.6A1 1 0 0 1 10 4h4a1 1 0 0 1 .8.4L16 6h2a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2z\"/>"
                       "<circle cx=\"12\" cy=\"12\" r=\"3\"/>");
        case Name::Bell:
            return doc("<path d=\"M6 9a6 6 0 1 1 12 0c0 4 1.5 5.5 2 6H4c.5-.5 2-2 2-6z\"/>"
                       "<path d=\"M10 19a2 2 0 0 0 4 0\"/>");
        // --- File-type icons (downloads popover) ---
        // Each is a document outline with a folded corner; the glyph inside
        // distinguishes the type so the user can tell a PNG from a ZIP at a
        // glance instead of every row showing the same folder icon.
        case Name::FileImage:
            return doc("<path d=\"M6 2h8l4 4v14a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M14 2v4h4\"/>"
                       "<circle cx=\"9\" cy=\"12\" r=\"1.2\"/>"
                       "<path d=\"M7 17l3-3 2 2 3-3\"/>");
        case Name::FileVideo:
            return doc("<path d=\"M6 2h8l4 4v14a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M14 2v4h4\"/>"
                       "<path d=\"M9 10l5 3-5 3z\"/>");
        case Name::FileAudio:
            return doc("<path d=\"M6 2h8l4 4v14a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M14 2v4h4\"/>"
                       "<path d=\"M10 16V9l5-1v7\"/><circle cx=\"9\" cy=\"16\" r=\"1.5\"/>"
                       "<circle cx=\"14\" cy=\"15\" r=\"1.5\"/>");
        case Name::FileArchive:
            return doc("<path d=\"M6 2h8l4 4v14a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M14 2v4h4\"/>"
                       "<path d=\"M10 5v2M10 9v2M10 13v2\"/>"
                       "<path d=\"M8.5 16h3l-.5 2h-2z\"/>");
        case Name::FileDocument:
            return doc("<path d=\"M6 2h8l4 4v14a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M14 2v4h4\"/>"
                       "<path d=\"M8 12h8\"/><path d=\"M8 15h8\"/><path d=\"M8 9h5\"/>");
        case Name::FileCode:
            return doc("<path d=\"M6 2h8l4 4v14a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M14 2v4h4\"/>"
                       "<path d=\"M9 12l-2 2 2 2\"/><path d=\"M14 10l2 4-2 2\"/>");
        case Name::FileGeneric:
            return doc("<path d=\"M6 2h8l4 4v14a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V3a1 1 0 0 1 1-1z\"/>"
                       "<path d=\"M14 2v4h4\"/>");
        case Name::Pause:
            return doc("<path d=\"M9 5v14\"/><path d=\"M15 5v14\"/>");
        case Name::Play:
            return doc("<path d=\"M8 5l11 7-11 7z\" fill=\"__C__\" stroke=\"none\"/>");
        case Name::Retry:
            return doc("<path d=\"M20 12a8 8 0 1 1-2.34-5.66\"/><path d=\"M20 4v4.5h-4.5\"/>");
        case Name::ShieldOff:
            return doc("<path d=\"M12 3l7 3v5c0 4.2-2.9 7.6-7 9-4.1-1.4-7-4.8-7-9V6z\"/>"
                       "<path d=\"M4.5 4.5l15 15\"/>");
        case Name::Block:
            return doc("<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M6.5 6.5l11 11\"/>");
        default:
            return doc("<rect x=\"5\" y=\"5\" width=\"14\" height=\"14\" rx=\"3\"/>");
    }
}

static QPixmap render(Name name, int px, const QColor& color, qreal dpr) {
    QString svg = svg_markup(name);
    svg.replace(QStringLiteral("__C__"), color.name(QColor::HexRgb));
    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(0, 0, px, px));
    return pm;
}

static QString cache_key(Name name, int px, const QColor& color, qreal dpr) {
    return QStringLiteral("aurora_ic_%1_%2_%3_%4")
        .arg(static_cast<int>(name))
        .arg(color.rgba())
        .arg(px)
        .arg(static_cast<int>(dpr * 100));
}

QPixmap pixmap(Name name, int px, const QColor& color, qreal dpr) {
    const QString key = cache_key(name, px, color, dpr);
    QPixmap pm;
    if (QPixmapCache::find(key, &pm)) return pm;
    pm = render(name, px, color, dpr);
    QPixmapCache::insert(key, pm);
    return pm;
}

void paint(QPainter* p, Name name, const QRectF& rect, const QColor& color) {
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
    const int px = qMax(4, qRound(rect.width()));
    const QPixmap pm = pixmap(name, px, color, dpr);
    // Use explicit target and device-independent source rectangles. This
    // avoids QPointF/natural-size ambiguity on high-DPI pixmaps and keeps the
    // cached SVG canvas exactly inside the caller's centred target.
    p->drawPixmap(rect, pm, QRectF(QPointF(0, 0), pm.deviceIndependentSize()));
}

QIcon icon(Name name, const QColor& color, int px, qreal dpr) {
    QIcon ic;
    ic.addPixmap(pixmap(name, px, color, dpr), QIcon::Normal, QIcon::Off);
    ic.addPixmap(pixmap(name, px, color.darker(150), dpr), QIcon::Disabled, QIcon::Off);
    return ic;
}

} // namespace aurora::ui::icons

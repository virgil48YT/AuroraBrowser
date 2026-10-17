// Aurora — shared page backdrop helpers.
#include "ui/PageBackground.hpp"

#include <QDateTime>
#include <QImageReader>
#include <QSize>
#include <QtGlobal>

namespace aurora::ui {

QPixmap load_page_wallpaper() {
    QPixmap pm;
    // The resource is bundled by src/app's qt_add_resources, so the page is
    // independent of the install layout. Smooth transform is done at scale
    // time, not per repaint, so the wallpaper is effectively free to draw.
    pm.load(QStringLiteral(":/brand/aurora_bg.jpg"));
    return pm;
}

QPixmap scale_page_wallpaper(const QPixmap& source, const QSize& widget_size,
                             qreal dpr) {
    if (source.isNull() || widget_size.isEmpty()) return QPixmap();
    const QSize target = widget_size * dpr;
    QPixmap scaled = source.scaled(target, Qt::KeepAspectRatioByExpanding,
                                   Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    return scaled;
}

QString format_history_time(qint64 epoch_seconds) {
    if (epoch_seconds <= 0) return QStringLiteral("—");
    const QDateTime when = QDateTime::fromSecsSinceEpoch(epoch_seconds);
    const QDate today = QDateTime::currentDateTime().date();
    const QDate d = when.date();
    if (d == today || d == today.addDays(-1)) return when.toString("HH:mm");
    if (d >= today.addDays(-7)) return when.toString("ddd HH:mm");
    return when.toString("dd MMM");
}

} // namespace aurora::ui

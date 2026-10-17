// Aurora — shared page backdrop helpers.
//
// Both InternalPageWidget and HomePage paint the aurora wallpaper as a
// full-bleed cover (KeepAspectRatioByExpanding, centre-crop), cached so it
// is re-scaled only on resize, and the history row timestamps share one
// precise format. The pieces are small enough to sit in one helper instead
// of being duplicated in HistoryPage, HistoryWidgets and the page bases.
#pragma once

#include <QPixmap>
#include <QString>

class QSize;

namespace aurora::ui {

// Scale `source` for a cover-crop over `widget_size * dpr`. Centre-crops
// by returning a pixmap that may be larger than `widget_size` on the long
// axis; callers draw it with centred offsets.
QPixmap scale_page_wallpaper(const QPixmap& source, const QSize& widget_size,
                             qreal dpr);

// Load the wallpaper pixmap once per page. The resource path lives here so
// the app-level qrc (assets/brand/aurora_bg.jpg, listed in
// src/app/CMakeLists.txt) is only named in one place. On failure the
// returned pixmap is null and callers fall back to the window background
// color, so the page never paints uninitialized pixels.
QPixmap load_page_wallpaper();

// The time-column format used on aurora://history rows. Today and yesterday
// keep "HH:mm" (matching the mock), the last week becomes "Wed 14:32" and
// anything older becomes "dd MMM". Local timezone on purpose: history is a
// local record.
QString format_history_time(qint64 epoch_seconds);

} // namespace aurora::ui

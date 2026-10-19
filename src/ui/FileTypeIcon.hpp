// Aurora — per-file-type icons for downloads (page + toolbar popover).
//
// Mirrors what a desktop file manager shows: a finished file that exists on
// disk gets the operating system's own icon for it (QFileIconProvider — the
// WinRAR logo for a .rar, the embedded icon of a .exe, the default PDF
// viewer's icon for a .pdf, ...). Anything that has no system icon yet — a
// transfer still arriving, a file that was moved, a platform without an icon
// theme — falls back to a painted type tile chosen from the file extension:
// archive (.rar/.zip/.7z/...), app/setup (.exe/.msi/...), music (.mp3/...),
// image (.jpg/.png/...), PDF, video (.mkv/.mp4/...), documents, sheets,
// slides, code, disk images, and a generic file for everything else.
//
// The browser's temporary suffixes (.crdownload, .part, ...) are stripped
// before classifying, so an in-flight "movie.mkv.crdownload" is a video.
#pragma once

#include <QIcon>
#include <QPixmap>
#include <QString>

namespace aurora::ui::file_icons {

enum class Kind {
    Archive,
    Application,
    DiskImage,
    Audio,
    Image,
    Video,
    Pdf,
    Document,
    Spreadsheet,
    Presentation,
    Code,
    Generic,
};

// Classify by extension (case-insensitive) after stripping temporary
// download suffixes; `mime` is only consulted when the extension is unknown.
Kind classify(const QString& file_name, const QString& mime = QString());

// Short user-facing label ("Archive", "App / setup", "Music", ...), also used
// by the downloads page's type filter.
QString kind_label(Kind kind);

// The painted fallback tile for a type at logical size `px`.
QPixmap badge(Kind kind, int px, qreal dpr);

// The best icon for a download: the system icon when `file_exists`, then
// (Linux) the desktop icon theme's MIME icon, then the painted type tile.
// `path` may be a full path or just a file name.
QIcon icon_for(const QString& path, const QString& mime, bool file_exists,
               int px, qreal dpr);

} // namespace aurora::ui::file_icons

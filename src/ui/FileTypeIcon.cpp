#include "ui/FileTypeIcon.hpp"

#include <QColor>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLinearGradient>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QStringList>

#include <initializer_list>

#include "ui/Icons.hpp"

namespace aurora::ui::file_icons {

namespace {

// Suffixes browsers (and download managers) give a file while it is still
// arriving. The real type is the extension underneath.
constexpr const char* kTemporarySuffixes[] = {
    "crdownload", "part", "partial", "download", "opdownload", "tmp",
};

bool in(const QString& ext, std::initializer_list<const char*> list) {
    for (const char* item : list) {
        if (ext == QLatin1String(item)) return true;
    }
    return false;
}

QString real_extension(const QString& file_name) {
    QString name = QFileInfo(file_name).fileName().toLower();
    for (int pass = 0; pass < 2; ++pass) {
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot < 0) return {};
        const QString ext = name.mid(dot + 1);
        bool temporary = false;
        for (const char* suffix : kTemporarySuffixes) {
            if (ext == QLatin1String(suffix)) temporary = true;
        }
        if (!temporary) {
            // "archive.tar.gz" reads as an archive either way.
            return ext;
        }
        name.truncate(dot);
    }
    return {};
}

Kind classify_extension(const QString& ext) {
    if (ext.isEmpty()) return Kind::Generic;
    if (in(ext, {"rar", "zip", "7z", "tar", "gz", "tgz", "bz2", "tbz", "tbz2",
                 "xz", "txz", "zst", "lz", "lzma", "lz4", "cab", "arj", "z",
                 "ace", "jar", "war"}))
        return Kind::Archive;
    if (in(ext, {"exe", "msi", "msix", "msixbundle", "appx", "appxbundle",
                 "bat", "cmd", "com", "scr", "apk", "aab", "deb", "rpm", "pkg",
                 "mpkg", "appimage", "run", "flatpak", "snap"}))
        return Kind::Application;
    if (in(ext, {"iso", "img", "dmg", "vhd", "vhdx", "vmdk", "bin", "cue"}))
        return Kind::DiskImage;
    if (in(ext, {"mp3", "wav", "ogg", "oga", "opus", "flac", "m4a", "aac",
                 "wma", "aiff", "aif", "alac", "mid", "midi", "amr"}))
        return Kind::Audio;
    if (in(ext, {"jpg", "jpeg", "png", "gif", "bmp", "webp", "svg", "ico",
                 "tif", "tiff", "heic", "heif", "avif", "jfif", "psd", "raw",
                 "cr2", "nef", "dng"}))
        return Kind::Image;
    if (in(ext, {"mp4", "mkv", "avi", "mov", "wmv", "webm", "flv", "m4v",
                 "mpg", "mpeg", "3gp", "ts", "m2ts", "vob", "ogv"}))
        return Kind::Video;
    if (ext == QLatin1String("pdf")) return Kind::Pdf;
    if (in(ext, {"doc", "docx", "odt", "rtf", "txt", "md", "epub", "pages",
                 "tex", "log"}))
        return Kind::Document;
    if (in(ext, {"xls", "xlsx", "xlsm", "ods", "csv", "tsv", "numbers"}))
        return Kind::Spreadsheet;
    if (in(ext, {"ppt", "pptx", "odp", "key"})) return Kind::Presentation;
    if (in(ext, {"js", "mjs", "ts", "tsx", "jsx", "html", "htm", "css",
                 "json", "xml", "yml", "yaml", "toml", "ini", "py", "c", "cc",
                 "cpp", "h", "hpp", "java", "kt", "rs", "go", "rb", "php",
                 "sh", "ps1", "lua", "sql"}))
        return Kind::Code;
    return Kind::Generic;
}

Kind classify_mime(const QString& mime) {
    if (mime.isEmpty()) return Kind::Generic;
    if (mime == QLatin1String("application/pdf")) return Kind::Pdf;
    if (mime.startsWith(QLatin1String("image/"))) return Kind::Image;
    if (mime.startsWith(QLatin1String("audio/"))) return Kind::Audio;
    if (mime.startsWith(QLatin1String("video/"))) return Kind::Video;
    if (mime.contains(QLatin1String("zip")) ||
        mime.contains(QLatin1String("rar")) ||
        mime.contains(QLatin1String("7z")) ||
        mime.contains(QLatin1String("tar")) ||
        mime.contains(QLatin1String("gzip")))
        return Kind::Archive;
    if (mime.contains(QLatin1String("msdownload")) ||
        mime.contains(QLatin1String("msi")) ||
        mime.contains(QLatin1String("executable")))
        return Kind::Application;
    if (mime.startsWith(QLatin1String("text/"))) return Kind::Document;
    return Kind::Generic;
}

struct Look {
    QColor top;
    QColor bottom;
    icons::Name glyph;
};

Look look_for(Kind kind) {
    switch (kind) {
        case Kind::Archive:
            return {QColor(0xF5, 0x9E, 0x0B), QColor(0xD9, 0x77, 0x06),
                    icons::Name::FileArchive};
        case Kind::Application:
            return {QColor(0x4F, 0x8B, 0xFF), QColor(0x25, 0x63, 0xEB),
                    icons::Name::Settings};
        case Kind::DiskImage:
            return {QColor(0x64, 0x74, 0x8B), QColor(0x33, 0x41, 0x55),
                    icons::Name::FileArchive};
        case Kind::Audio:
            return {QColor(0xF4, 0x72, 0xB6), QColor(0xE1, 0x1D, 0x74),
                    icons::Name::FileAudio};
        case Kind::Image:
            return {QColor(0x38, 0xBD, 0xF8), QColor(0x02, 0x84, 0xC7),
                    icons::Name::FileImage};
        case Kind::Video:
            return {QColor(0x22, 0xD3, 0xEE), QColor(0x25, 0x63, 0xEB),
                    icons::Name::Play};
        case Kind::Pdf:
            return {QColor(0xF8, 0x71, 0x71), QColor(0xDC, 0x26, 0x26),
                    icons::Name::FileDocument};
        case Kind::Document:
            return {QColor(0x60, 0xA5, 0xFA), QColor(0x1D, 0x4E, 0xD8),
                    icons::Name::FileDocument};
        case Kind::Spreadsheet:
            return {QColor(0x34, 0xD3, 0x99), QColor(0x05, 0x96, 0x69),
                    icons::Name::FileDocument};
        case Kind::Presentation:
            return {QColor(0xFB, 0x92, 0x3C), QColor(0xEA, 0x58, 0x0C),
                    icons::Name::FileDocument};
        case Kind::Code:
            return {QColor(0xA7, 0x8B, 0xFA), QColor(0x6D, 0x28, 0xD9),
                    icons::Name::FileCode};
        case Kind::Generic:
            break;
    }
    return {QColor(0x94, 0xA3, 0xB8), QColor(0x47, 0x55, 0x69),
            icons::Name::FileGeneric};
}

} // namespace

Kind classify(const QString& file_name, const QString& mime) {
    const Kind by_ext = classify_extension(real_extension(file_name));
    if (by_ext != Kind::Generic) return by_ext;
    return classify_mime(mime.trimmed().toLower());
}

QString kind_label(Kind kind) {
    switch (kind) {
        case Kind::Archive: return QStringLiteral("Archives");
        case Kind::Application: return QStringLiteral("Apps & setups");
        case Kind::DiskImage: return QStringLiteral("Disk images");
        case Kind::Audio: return QStringLiteral("Music & audio");
        case Kind::Image: return QStringLiteral("Images");
        case Kind::Video: return QStringLiteral("Videos");
        case Kind::Pdf: return QStringLiteral("PDFs");
        case Kind::Document: return QStringLiteral("Documents");
        case Kind::Spreadsheet: return QStringLiteral("Spreadsheets");
        case Kind::Presentation: return QStringLiteral("Presentations");
        case Kind::Code: return QStringLiteral("Code");
        case Kind::Generic: break;
    }
    return QStringLiteral("Other files");
}

QPixmap badge(Kind kind, int px, qreal dpr) {
    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    const int size = qMax(1, qRound(px * scale));
    QPixmap canvas(size, size);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const qreal s = static_cast<qreal>(size);
    const QRectF tile(s * 0.06, s * 0.06, s * 0.88, s * 0.88);
    const qreal radius = s * 0.2;
    QPainterPath shape;
    shape.addRoundedRect(tile, radius, radius);

    const Look look = look_for(kind);
    if (kind == Kind::Archive) {
        // The archive tile reads like the WinRAR "stack of books": three
        // colored bands held by a gold strap down the middle.
        p.save();
        p.setClipPath(shape);
        const QColor bands[] = {QColor(0xA8, 0x55, 0xF7), QColor(0x10, 0xB9, 0x81),
                                QColor(0x3B, 0x82, 0xF6)};
        const qreal band_h = tile.height() / 3.0;
        for (int i = 0; i < 3; ++i) {
            p.fillRect(QRectF(tile.left(), tile.top() + band_h * i,
                              tile.width(), band_h + 0.5),
                       bands[i]);
        }
        const qreal strap_w = tile.width() * 0.24;
        const QRectF strap(tile.center().x() - strap_w / 2.0, tile.top(),
                           strap_w, tile.height());
        p.fillRect(strap, look.top);
        p.fillRect(QRectF(strap.left(), tile.center().y() - strap_w * 0.45,
                          strap.width(), strap_w * 0.9),
                   look.bottom.darker(115));
        p.restore();
    } else {
        QLinearGradient grad(tile.topLeft(), tile.bottomLeft());
        grad.setColorAt(0.0, look.top);
        grad.setColorAt(1.0, look.bottom);
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawPath(shape);

        // The white type glyph, rendered at device pixels.
        const int glyph = qMax(1, qRound(tile.width() * 0.62));
        QPixmap mark = icons::pixmap(look.glyph, glyph, QColor(Qt::white), 1.0);
        mark.setDevicePixelRatio(1.0);
        mark = mark.scaled(glyph, glyph, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
        p.drawPixmap(QPointF(tile.center().x() - mark.width() / 2.0,
                             tile.center().y() - mark.height() / 2.0),
                     mark);
    }
    p.end();
    canvas.setDevicePixelRatio(scale);
    return canvas;
}

QIcon icon_for(const QString& path, const QString& mime, bool file_exists,
               int px, qreal dpr) {
    // 1. Exactly what the desktop shows for this file.
    if (file_exists && !path.isEmpty()) {
        const QIcon system = QFileIconProvider().icon(QFileInfo(path));
        if (!system.isNull()) return system;
    }
#if defined(Q_OS_LINUX)
    // 2. Linux desktops: the icon theme's icon for the type, which is what
    //    the file manager would show once the file lands.
    if (!QIcon::themeName().isEmpty()) {
        const QMimeDatabase db;
        QMimeType type = mime.isEmpty()
            ? QMimeType()
            : db.mimeTypeForName(mime.trimmed().toLower());
        if (!type.isValid() || type.isDefault()) {
            type = db.mimeTypeForFile(path, QMimeDatabase::MatchExtension);
        }
        if (type.isValid() && !type.isDefault()) {
            QIcon themed = QIcon::fromTheme(type.iconName());
            if (themed.isNull()) themed = QIcon::fromTheme(type.genericIconName());
            if (!themed.isNull()) return themed;
        }
    }
#endif
    // 3. Aurora's painted type tile.
    return QIcon(badge(classify(path, mime), px, dpr));
}

} // namespace aurora::ui::file_icons

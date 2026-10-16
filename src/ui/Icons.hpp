// Aurora — icon system.
//
// One coherent vector icon language for the entire chrome. Icons use a 24×24
// SVG grid and a 1.9px round base stroke (the small close X is shorter and
// lighter for clarity at 12-16px; filled variants use fill). Icons are
// rendered to pixmaps through QSvgRenderer and cached by
// (name, color, size, devicePixelRatio) so paint paths never re-parse SVG.
//
// Color is applied at render time by substituting a __C__ token in the SVG
// markup, so the same glyph can be tinted to any semantic role (muted text,
// full text, accent, faint, danger). Buttons pick the tint per interaction
// state in IconButton::paintEvent; menus bake a single tint into a QIcon.
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

class QPainter;
class QRectF;

namespace aurora::ui::icons {

enum class Name {
    Back,
    Forward,
    Reload,
    Stop,
    Home,
    Search,
    Plus,
    Close,
    NewTab,
    StarOutline,
    StarFilled,
    Bookmarks,
    History,
    Downloads,
    Settings,
    More,        // vertical three dots (browser menu)
    Puzzle,      // extensions
    Code,        // developer tools
    ZoomIn,
    ZoomOut,
    Fullscreen,
    FullscreenExit,
    ChevronLeft,
    ChevronRight,
    ChevronUp,
    ChevronDown,
    Pin,
    Unpin,
    Mute,
    Unmute,
    Copy,
    Duplicate,
    Sun,
    Moon,
    Shield,      // security: secure
    Globe,       // security: standard
    Info,        // security: not-secure / info
    Bookmark,
    Trash,
    Minimize,
    Maximize,
    Restore,
    Lock,
    Spinner,     // animated loading indicator (drawn by TabSpinner, not static)
    Folder,      // downloads popover rows
    FolderOpen,   // "show in folder" reveal button (open folder glyph)
    // --- File-type icons for the downloads popover ---
    FileImage,    // png, jpg, jpeg, gif, bmp, webp, svg, ico
    FileVideo,    // mp4, avi, mkv, webm, mov, wmv
    FileAudio,    // mp3, wav, ogg, flac, m4a, aac
    FileArchive,  // zip, rar, 7z, tar, gz
    FileDocument, // pdf, doc, docx, txt, rtf, odt
    FileCode,     // js, html, css, json, xml, py, cpp, hpp
    FileGeneric,  // fallback for unknown file types
    OpenExternal, // "open the full page" affordance (popover footers)
    Mic,         // microphone permission prompt
    Camera,      // camera permission prompt
    Bell,        // notification permission prompt
    // --- Downloads controls ---
    Pause,       // pause a running download (two bars)
    Play,        // resume a paused download (triangle)
    Retry,       // retry a failed/cancelled download (circular arrow)
    // --- Aurora Shields (ad blocking) ---
    ShieldOff,   // shields down for this site (slashed shield)
    Block,       // blocked-item glyph for the shields popup rows
    // --- Aurora neon homepage + settings rail ---
    Sparkle,   // four-point star (homepage search bar trailing glyph)
    CloudSun,  // weather widget (cloud with a partial sun)
    User,      // rounded-corner avatar silhouette
    Gauge,     // performance / memory-saver dial
    // --- Weather conditions (homepage weather chip) ---
    WeatherCloud,
    WeatherRain,
    WeatherSnow,
    WeatherStorm,
    WeatherFog,
};

// Paint an icon centered within `rect` using `color`. The pixmap is cached.
void paint(QPainter* p, Name name, const QRectF& rect, const QColor& color);

// Render a standalone high-DPI pixmap (logical `px`×`px`) for use in QIcon.
QPixmap pixmap(Name name, int px, const QColor& color, qreal dpr = 1.0);

// Convenience QIcon for menu actions (Normal + Disabled modes).
QIcon icon(Name name, const QColor& color, int px = 16, qreal dpr = 1.0);

// The raw SVG markup for `name`, with the color token __C__ to be replaced.
QString svg_markup(Name name);

} // namespace aurora::ui::icons

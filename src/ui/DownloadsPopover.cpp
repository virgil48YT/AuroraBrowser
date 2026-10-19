#include "ui/DownloadsPopover.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QStringList>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtMath>

#include <unordered_set>
#include <utility>

#include "downloads/DownloadService.hpp"
#include "ui/FileTypeIcon.hpp"
#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

// These Qt-backed DownloadService methods live in the application translation
// unit so the existing aurora_core target remains Qt-free.
namespace aurora::downloads {

bool DownloadService::open_file(int64_t id) {
    const auto item = find(id);
    if (!item || item->state != DownloadState::Complete ||
        item->target_path.empty()) {
        return false;
    }

    const QFileInfo file(
        QDir::cleanPath(QString::fromStdString(item->target_path)));
    if (!file.exists() || !file.isFile()) return false;

    return QDesktopServices::openUrl(
        QUrl::fromLocalFile(file.absoluteFilePath()));
}

bool DownloadService::reveal_in_folder(int64_t id) {
    const auto item = find(id);
    if (!item || item->state != DownloadState::Complete ||
        item->target_path.empty()) {
        return false;
    }

    const QFileInfo file(
        QDir::cleanPath(QString::fromStdString(item->target_path)));
    if (!file.exists() || !file.isFile()) return false;

#if defined(OS_WIN) || defined(Q_OS_WIN)
    const QString path = QDir::toNativeSeparators(file.absoluteFilePath());
    return QProcess::startDetached(
        QStringLiteral("explorer.exe"),
        QStringList{QStringLiteral("/select,"), path});
#else
    return QDesktopServices::openUrl(
        QUrl::fromLocalFile(file.absolutePath()));
#endif
}

} // namespace aurora::downloads

namespace aurora::ui {

namespace {

class DownloadRow final : public QWidget {
public:
    DownloadRow(downloads::DownloadService* service, int64_t id, bool openable,
                QWidget* parent)
        : QWidget(parent), service_(service), id_(id), openable_(openable) {}

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (openable_ && event->button() == Qt::LeftButton) {
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (openable_ && service_ && event->button() == Qt::LeftButton &&
            rect().contains(event->position().toPoint())) {
            service_->open_file(id_);
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    downloads::DownloadService* service_ = nullptr;
    int64_t id_ = 0;
    bool openable_ = false;
};

// Geometry. A compact desktop dropdown: narrow-ish, tight, square-ish.
constexpr int kRowCount = 8;          // recent(N) shown; the rest scrolls
constexpr int kContentWidth = 420;    // panel content width (was 360 — too narrow,
                                     // the reveal button was clipped)
constexpr int kMaxListHeight = 336;   // beyond this the list scrolls; the
                                      // footer is a layout sibling and can
                                      // never cover the last item
// The panel's corner radius. Mirrors kPopoverRadius in StylesheetBuilder --
// the shadow rings painted below MUST follow the same radius, or the shadow
// rounds harder than the card and the popover reads as a pill again.
constexpr int kPanelRadius = Radius::md;  // 6px
// Row geometry: uniform two-line rows, so the list scans as a clean column
// instead of a stack of differently-sized bubbles.
constexpr int kRowVPad = Spacing::sm - 1;
constexpr int kIconBox = 20;
constexpr int kFileIconSize = 18;
constexpr int kRevealButtonBox = 28;  // was 24 — the folder icon was clipped
// The "stop this transfer" button, shown only while a row is in progress. A
// touch smaller than the reveal button so the destructive action does not
// out-weigh the neutral one.
constexpr int kCancelButtonBox = 24;
// The in-progress bar under the file name. A hairline, not a widget: it has to
// read as part of the row rather than as a control.
constexpr int kProgressHeight = 3;
// How often the open popover re-reads DownloadService while a transfer runs.
// Chrome's own dropdown updates at roughly this rate; anything faster is
// invisible to the eye and only costs paints.
constexpr int kTickMs = 250;
// Width available to the file-name label (panel width minus the list inset,
// the row padding, the icon column, the reveal button and their gaps). Names
// are elided to it so a long file name can never stretch or wrap the row.
// The cancel button only exists on in-progress rows, but the width is
// reserved for every row: a name that stays put when its download finishes
// reads far better than one that re-elides under the cursor.
constexpr int kNameWidth = kContentWidth - 2 * Spacing::xs - 2 * Spacing::sm
                           - kIconBox - kRevealButtonBox - kCancelButtonBox
                           - 3 * Spacing::sm;
// Room reserved around the panel for the painted drop shadow.
constexpr int kShadowL = 8, kShadowT = 6, kShadowR = 12, kShadowB = 14;

QString human_size(int64_t bytes) {
    if (bytes < 0) return QStringLiteral("unknown size");
    return QLocale::system().formattedDataSize(bytes);
}

// A transfer rate as "1.4 MB/s" / "340 KB/s". Empty for a non-positive rate,
// so a caller can simply skip the segment rather than print "0 KB/s".
QString format_speed(double bytes_per_sec) {
    if (!(bytes_per_sec > 0.0)) return {};
    constexpr double kKB = 1024.0;
    constexpr double kMB = 1024.0 * 1024.0;
    if (bytes_per_sec >= kMB) {
        return QStringLiteral("%1 MB/s").arg(bytes_per_sec / kMB, 0, 'f', 1);
    }
    // Never round a live transfer down to "0 KB/s": anything slower than a
    // kilobyte a second is still moving.
    return QStringLiteral("%1 KB/s")
        .arg(qMax(1.0, bytes_per_sec / kKB), 0, 'f', 0);
}

// "just now" / "4 minutes ago" / "1 hour ago" / "3 days ago" / a date.
QString relative_time(std::chrono::system_clock::time_point tp) {
    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                           tp.time_since_epoch())
                           .count();
    if (epoch <= 0) return QStringLiteral("—");
    const auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - tp).count();
    if (secs < 60) return QStringLiteral("just now");
    const auto mins = secs / 60;
    if (mins < 60) {
        return QStringLiteral("%1 minute%2 ago").arg(mins).arg(
            mins == 1 ? "" : "s");
    }
    const auto hours = mins / 60;
    if (hours < 24) {
        return QStringLiteral("%1 hour%2 ago").arg(hours).arg(
            hours == 1 ? "" : "s");
    }
    const auto days = hours / 24;
    if (days < 7) {
        return QStringLiteral("%1 day%2 ago").arg(days).arg(days == 1 ? "" : "s");
    }
    return QDateTime::fromSecsSinceEpoch(epoch).toString(QStringLiteral("MMM d, yyyy"));
}

// The "size • when" line under the file name. `speed` is the pre-formatted
// transfer rate for a live row (empty for anything else), appended so the
// sub-line reads "Downloading… 42% • 1.4 MB/s • 12.0 MB".
QString meta_text(const downloads::DownloadItem& d,
                  const QString& speed = QString()) {
    using downloads::DownloadState;
    QStringList parts;
    if (d.state == DownloadState::InProgress ||
        d.state == DownloadState::Paused) {
        const int pct = d.percent();
        if (d.state == DownloadState::Paused) {
            parts << (pct >= 0 ? QStringLiteral("Paused — %1%").arg(pct)
                               : QStringLiteral("Paused"));
        } else {
            parts << (pct >= 0 ? QStringLiteral("Downloading… %1%").arg(pct)
                               : QStringLiteral("Downloading…"));
        }
        if (!speed.isEmpty()) parts << speed;
        if (d.total_bytes > 0) parts << human_size(d.total_bytes);
    } else {
        parts << human_size(d.total_bytes > 0 ? d.total_bytes : d.received);
        if (d.state == DownloadState::Cancelled) parts << QStringLiteral("Cancelled");
        if (d.state == DownloadState::Interrupted) parts << QStringLiteral("Interrupted");
        parts << relative_time(d.finished_at.value_or(d.started_at));
    }
    return parts.join(QStringLiteral(" • "));
}

// Render file artwork into a fixed logical-size canvas. QIcon::pixmap() accepts
// a pixel size, and passing kIconBox * dpr straight to QLabel leaves some
// platform icons tagged as DPR 1; QLabel then sees an oversized logical pixmap
// and clips it to the label. Normalising the source to physical pixels and
// tagging only the finished canvas with the display DPR keeps every row at the
// same 20px footprint. KeepAspectRatio plus the transparent canvas also
// letterboxes non-square thumbnails/icons instead of cropping or stretching.
QPixmap fitted_file_icon(const QIcon& source, qreal dpr) {
    if (source.isNull()) return {};

    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    const int box_px = qMax(1, qRound(kIconBox * scale));
    const int artwork_px = qMax(1, qRound(kFileIconSize * scale));

    QPixmap artwork = source.pixmap(QSize(artwork_px, artwork_px));
    if (artwork.isNull()) return {};
    // Work in physical pixels while composing the canvas, regardless of how
    // the platform icon engine tagged the returned pixmap.
    artwork.setDevicePixelRatio(1.0);
    artwork = artwork.scaled(artwork_px, artwork_px, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    artwork.setDevicePixelRatio(1.0);

    QPixmap canvas(box_px, box_px);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.drawPixmap((box_px - artwork.width()) / 2,
                       (box_px - artwork.height()) / 2, artwork);
    painter.end();
    canvas.setDevicePixelRatio(scale);
    return canvas;
}

} // namespace

DownloadsPopover::DownloadsPopover(downloads::DownloadService* service,
                                   QWidget* parent, Actions actions)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), service_(service),
      actions_(std::move(actions)) {
    setObjectName(QStringLiteral("DownloadsPopover"));
    // Translucent so the rounded panel and the painted shadow composite
    // cleanly; the panel child carries the actual background via the sheet.
    setAttribute(Qt::WA_TranslucentBackground);
    // WA_TranslucentBackground alone can still let the platform paint a solid
    // system background filling the whole popup rect (the shadow-margin area
    // around the panel) as a sharp rectangle — the eyesore behind the rounded
    // panel. NoSystemBackground + the explicit transparent QSS rule on
    // #DownloadsPopover make the widget truly see-through so only the painted
    // shadow and the rounded panel show.
    setAttribute(Qt::WA_NoSystemBackground);
    // Honor the #DownloadsPopover QSS rule (background: transparent). Without
    // WA_StyledBackground the popup ignores that rule and paints its
    // QPalette::Window (solid, square) across the whole shadow-margin rect --
    // exactly the "square shadow behind the rounded panel" eyesore.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);
    // Fixed at the reference width; only the list height adapts to content.
    setFixedWidth(kContentWidth + kShadowL + kShadowR);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(kShadowL, kShadowT, kShadowR, kShadowB);
    root->setSpacing(0);

    panel_ = new QWidget(this);
    panel_->setObjectName(QStringLiteral("DownloadsPopoverPanel"));
    panel_->setAttribute(Qt::WA_StyledBackground, true);  // plain QWidget + QSS
    root->addWidget(panel_);

    auto* pl = new QVBoxLayout(panel_);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);

    // --- Header: title + close ------------------------------------------
    auto* header = new QWidget(panel_);
    header->setObjectName(QStringLiteral("DownloadsPopoverHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    auto* hl = new QHBoxLayout(header);
    // Header padding lines the title up with the row text below it (the list
    // inset + the row's own left padding) instead of sitting further in, and
    // stays tight: a tall header band is what made the panel look inflated.
    hl->setContentsMargins(Spacing::md, Spacing::sm, Spacing::sm, Spacing::sm);
    hl->setSpacing(Spacing::sm);
    auto* title = new QLabel(QStringLiteral("Recent download history"), header);
    title->setObjectName(QStringLiteral("DownloadsPopoverTitle"));
    hl->addWidget(title, 1);
    auto* close_btn = new IconButton(icons::Name::Close, header);
    close_btn->setObjectName(QStringLiteral("PopoverCloseButton"));
    close_btn->setToolTip(QStringLiteral("Close"));
    close_btn->setFixedSize(22, 22);
    close_btn->setIconPx(12);
    connect(close_btn, &IconButton::clicked, this, &QWidget::close);
    hl->addWidget(close_btn);
    pl->addWidget(header);

    // --- Scrollable list --------------------------------------------------
    // Kept as a member so the live refresh can restore the scroll offset it
    // destroys when it rebuilds the rows.
    auto* scroll = new QScrollArea(panel_);
    scroll_ = scroll;
    scroll->setObjectName(QStringLiteral("DownloadsPopoverScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setMaximumHeight(kMaxListHeight);
    if (scroll->viewport()) scroll->viewport()->setAutoFillBackground(false);

    list_host_ = new QWidget(scroll);
    list_host_->setObjectName(QStringLiteral("DownloadsPopoverList"));
    list_layout_ = new QVBoxLayout(list_host_);
    // Even inset on all four sides so the hover wash of the first and last
    // rows is framed identically by the card edge. Rows sit flush against
    // each other (no inter-row gap) -- a dropdown list, not a set of chips.
    list_layout_->setContentsMargins(Spacing::xs, Spacing::xs, Spacing::xs,
                                     Spacing::xs);
    list_layout_->setSpacing(0);
    scroll->setWidget(list_host_);
    pl->addWidget(scroll);

    // --- Footer: full-history affordance ----------------------------------
    // The footer is a layout sibling BELOW the scroll area — never an overlay
    // — so it cannot cover the last download row.
    auto* footer = new QPushButton(QStringLiteral("Full download history"), panel_);
    footer->setObjectName(QStringLiteral("DownloadsPopoverFooter"));
    footer->setCursor(Qt::PointingHandCursor);
    // Trailing "open page" glyph at the far right (the button's own text is
    // left-aligned via the stylesheet).
    auto* fl = new QHBoxLayout(footer);
    fl->setContentsMargins(0, 0, Spacing::md, 0);
    auto* arrow = new QLabel(footer);
    arrow->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    fl->addStretch(1);
    fl->addWidget(arrow);
    const qreal dpr = qApp && qApp->primaryScreen()
                          ? qApp->primaryScreen()->devicePixelRatio()
                          : 1.0;
    arrow->setPixmap(icons::pixmap(icons::Name::OpenExternal, 15,
                                   current_theme().colors.text_muted, dpr));
    connect(footer, &QPushButton::clicked, this,
            &DownloadsPopover::full_history_requested);
    pl->addWidget(footer);

    // --- Live progress ----------------------------------------------------
    // DownloadService is written from the browser's download callback, which
    // already runs on this (the Qt main) thread — so a plain single-threaded
    // timer is all a live view needs. It only ever runs while the popover is
    // visible (see showEvent/hideEvent).
    tick_ = new QTimer(this);
    tick_->setInterval(kTickMs);
    connect(tick_, &QTimer::timeout, this,
            &DownloadsPopover::refresh_if_changed);
}

void DownloadsPopover::toggle_under(QWidget* anchor) {
    if (!anchor) return;
    anchor_ = anchor;
    if (isVisible()) {
        close();
        return;
    }
    present();
}

void DownloadsPopover::present() {
    rebuild();
    adjustSize();
    reposition();
    show();
    raise();
}

void DownloadsPopover::reposition() {
    // Auto-presentation can happen before the tray button has ever toggled the
    // popover. Recover that anchor from the host's stable downloads tooltip.
    if (!anchor_ && parentWidget()) {
        const auto children = parentWidget()->findChildren<QWidget*>();
        for (auto* child : children) {
            if (child->toolTip() == QStringLiteral("Downloads")) {
                anchor_ = child;
                break;
            }
        }
    }

    if (anchor_ && anchor_->isVisible()) {
        // Right-align the PANEL's right edge with the anchor's right edge.
        // dropdown_pos with align_right aligns the widget's right edge with
        // the anchor's right edge, but the panel is inset by kShadowR from
        // the widget's right (transparent shadow margin). So we shift the
        // widget right by kShadowR to compensate, making the panel's right
        // edge flush with the icon.
        QPoint pos = dropdown_pos(anchor_, size(),
                                  /*align_right=*/true, Spacing::xs);
        pos.setX(pos.x() + kShadowR);
        move(pos);
    }
}

void DownloadsPopover::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (tick_) tick_->start();
}

void DownloadsPopover::hideEvent(QHideEvent* event) {
    // Covers close(), the click-outside dismissal and the window going away:
    // all of them end in a hide event, so there is no path that leaves the
    // ticker running against an invisible panel.
    if (tick_) tick_->stop();
    QWidget::hideEvent(event);
}

QString DownloadsPopover::rows_signature() const {
    if (!service_) return {};
    QString sig;
    for (const auto& d : service_->recent(kRowCount)) {
        sig += QStringLiteral("%1:%2:%3:%4;")
                   .arg(d.id)
                   .arg(static_cast<int>(d.state))
                   .arg(d.received)
                   .arg(d.total_bytes);
    }
    return sig;
}

void DownloadsPopover::refresh_if_changed() {
    if (!isVisible()) return;
    const QString sig = rows_signature();
    // Nothing moved since the last paint: rebuilding here would only make the
    // rows flicker and drop the user's hover.
    if (sig == last_signature_) return;

    // A rebuild recreates every row, so remember where the list was scrolled
    // to and put it back — a user reading row 8 must not be yanked to the top
    // four times a second.
    const int scroll_pos =
        scroll_ && scroll_->verticalScrollBar()
            ? scroll_->verticalScrollBar()->value()
            : 0;
    rebuild();
    // A finished download loses its progress bar, so the panel's ideal height
    // changes; resize and re-place it rather than leaving a gap at the bottom.
    adjustSize();
    reposition();
    if (scroll_ && scroll_->verticalScrollBar()) {
        scroll_->verticalScrollBar()->setValue(scroll_pos);
    }
}

QString DownloadsPopover::speed_text(const downloads::DownloadItem& d) {
    if (d.state != downloads::DownloadState::InProgress) {
        // A finished (or paused) row has no rate to show, and keeping its
        // sample alive would only make the map grow with history.
        speed_samples_.erase(d.id);
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    double bytes_per_sec = 0.0;

    const auto prev = speed_samples_.find(d.id);
    if (prev != speed_samples_.end()) {
        const double elapsed =
            std::chrono::duration<double>(now - prev->second.at).count();
        const int64_t delta = d.received - prev->second.received;
        // A zero (or backwards) interval and a byte count that did not move
        // both mean "this sample says nothing" — fall through to the average
        // below rather than dividing by zero or printing 0 KB/s.
        if (elapsed > 0.0 && delta > 0) {
            bytes_per_sec = static_cast<double>(delta) / elapsed;
        }
    }

    if (bytes_per_sec <= 0.0) {
        // First tick for this download (or a tick in which nothing arrived):
        // the average since the transfer started is the honest fallback, and
        // it keeps the row from flashing an empty slot on its first paint.
        const double since_start =
            std::chrono::duration<double>(std::chrono::system_clock::now() -
                                          d.started_at)
                .count();
        if (since_start > 0.0 && d.received > 0) {
            bytes_per_sec = static_cast<double>(d.received) / since_start;
        }
    }

    speed_samples_[d.id] = SpeedSample{d.received, now};
    return format_speed(bytes_per_sec);
}

void DownloadsPopover::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (!panel_) return;
    QPainter p(this);
    // Clear the whole popup backing store to transparent before drawing the
    // shadow rings. The popup is wider/taller than the panel by the shadow
    // margins; if anything (system background, palette fill) leaves an opaque
    // square in that margin area, it shows as the "square shadow" behind the
    // rounded panel. Clearing here guarantees only the painted rounded rings
    // and the rounded panel render.
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::Antialiasing, true);
    // A soft shadow: three expanding, fading rounded rects offset downward.
    // The theme's shadow_color carries the base alpha.
    const QColor base = current_theme().colors.shadow_color;
    const QRectF panel_rect(panel_->geometry());
    p.setPen(Qt::NoPen);
    for (int i = 3; i >= 1; --i) {
        const qreal g = static_cast<qreal>(i) * 2.5;
        QColor ring(base);
        ring.setAlpha(qMax(6, base.alpha() / (2 * i + 2)));
        p.setBrush(ring);
        // The rings must follow the panel's own (now smaller) corner radius,
        // otherwise the shadow rounds harder than the card it sits behind and
        // the whole popover reads as a capsule.
        p.drawRoundedRect(panel_rect.adjusted(-g, -g + 3.0, g, g + 3.0),
                          kPanelRadius + g, kPanelRadius + g);
    }
}

void DownloadsPopover::rebuild() {
    // Remember what this pass is about to render, so the live ticker can tell
    // whether a later read of the service actually differs from the screen.
    last_signature_ = rows_signature();

    // Drop the previous rows.
    while (auto* item = list_layout_->takeAt(0)) {
        if (auto* w = item->widget()) {
            // Hide before deleteLater: a deleted-but-not-yet-freed row keeps
            // painting over the new one until the event loop catches up, which
            // is visible as flicker at the ticker's refresh rate.
            w->hide();
            w->deleteLater();
        }
        delete item;
    }

    const qreal dpr = screen() ? screen()->devicePixelRatio()
                               : qApp->devicePixelRatio();
    const ThemeColors& c = current_theme().colors;

    const auto add_message = [this](const QString& text) {
        auto* label = new QLabel(text, list_host_);
        label->setObjectName(QStringLiteral("DownloadsPopoverEmpty"));
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        list_layout_->addWidget(label);
    };

    if (!service_) {
        add_message(QStringLiteral(
            "No profile is attached to this window, so downloads are "
            "unavailable."));
    } else {
        const auto items = service_->recent(kRowCount);
        if (items.empty()) {
            add_message(QStringLiteral(
                "No downloads yet. Files you download will be listed here."));
        }
        // Ids still listed, so a sample belonging to a download that has since
        // been removed (or has fallen out of the recent window) is dropped
        // instead of living in the map forever.
        std::unordered_set<int64_t> live_ids;
        for (const auto& d : items) {
            live_ids.insert(d.id);
            const QString target = QString::fromStdString(d.target_path);
            const bool file_available =
                d.state == downloads::DownloadState::Complete &&
                !target.isEmpty() && QFileInfo(target).isFile();
            auto* row = new DownloadRow(service_, d.id, file_available,
                                        list_host_);
            row->setObjectName(QStringLiteral("DownloadRow"));
            row->setAttribute(Qt::WA_StyledBackground, true);
            if (!target.isEmpty()) row->setToolTip(target);
            if (file_available) row->setCursor(Qt::PointingHandCursor);

            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(Spacing::sm, kRowVPad, Spacing::sm, kRowVPad);
            rl->setSpacing(Spacing::sm);

            auto* icon = new QLabel(row);
            icon->setFixedSize(kIconBox, kIconBox);
            icon->setAlignment(Qt::AlignCenter);
            icon->setScaledContents(false);
            icon->setAttribute(Qt::WA_TransparentForMouseEvents);
            // Use the real system file icon (like Chrome does) instead of
            // generic SVG glyphs: QFileIconProvider returns the actual OS icon
            // for the file type — the WinRAR logo for a .rar, a real .exe
            // icon, .pdf icon, image preview, etc. A file that does not exist
            // yet (still downloading) or any more gets the per-extension type
            // tile shared with aurora://downloads (see FileTypeIcon).
            const QString icon_path = target.isEmpty()
                ? QString::fromStdString(d.url) : target;
            const QIcon sys_icon = file_icons::icon_for(
                icon_path, QString::fromStdString(d.mime), file_available,
                kFileIconSize, dpr);
            icon->setPixmap(fitted_file_icon(sys_icon, dpr));
            rl->addWidget(icon, 0, Qt::AlignVCenter);

            auto* text_col = new QVBoxLayout();
            text_col->setContentsMargins(0, 0, 0, 0);
            text_col->setSpacing(1);
            QString name = QString::fromStdString(d.file_name());
            if (name.isEmpty()) name = QString::fromStdString(d.url);
            auto* name_label = new QLabel(row);
            name_label->setObjectName(QStringLiteral("DownloadRowName"));
            name_label->setAttribute(Qt::WA_TransparentForMouseEvents);
            // One line, elided in the middle: every row is the same height,
            // and the extension stays readable on a long name.
            name_label->setWordWrap(false);
            name_label->setText(QFontMetrics(name_label->font())
                                    .elidedText(name, Qt::ElideMiddle,
                                                kNameWidth));
            text_col->addWidget(name_label);
            // Sampled once per row per rebuild (it advances the rate sample),
            // and empty for anything that is not running.
            const QString speed = speed_text(d);
            if (d.state == downloads::DownloadState::InProgress ||
                d.state == downloads::DownloadState::Paused) {
                // Live (or paused-but-alive) transfer: a hairline bar under
                // the name, refreshed by the ticker. A server that sends no
                // Content-Length leaves percent() at -1, which is exactly
                // Qt's indeterminate range (0, 0) — a busy sweep instead of
                // a fake fill.
                auto* progress = new QProgressBar(row);
                progress->setObjectName(QStringLiteral("DownloadRowProgress"));
                progress->setAttribute(Qt::WA_TransparentForMouseEvents);
                progress->setTextVisible(false);
                progress->setFixedHeight(kProgressHeight);
                const int pct = d.percent();
                if (pct < 0) {
                    progress->setRange(0, 0);
                } else {
                    progress->setRange(0, 100);
                    progress->setValue(pct);
                }
                text_col->addWidget(progress);
            }
            auto* sub_label = new QLabel(meta_text(d, speed), row);
            sub_label->setObjectName(QStringLiteral("DownloadRowSub"));
            sub_label->setAttribute(Qt::WA_TransparentForMouseEvents);
            text_col->addWidget(sub_label);
            rl->addLayout(text_col, 1);

            // --- Live-transfer controls ----------------------------------
            // Pause/resume/cancel sit to the LEFT of the reveal button and
            // only exist while the transfer is alive (running or paused).
            // A failed row instead offers Retry. Without the matching host
            // callback there is no engine to talk to, so the control is not
            // offered at all.
            const auto add_action_button =
                [this, &rl, &c, dpr, row, &d](icons::Name icon,
                                     const QString& tip,
                                     const QString& object_name,
                                     std::function<bool(int64_t)> action) {
                    auto* btn = new QToolButton(row);
                    btn->setObjectName(object_name);
                    btn->setAutoRaise(true);
                    btn->setFixedSize(kCancelButtonBox, kCancelButtonBox);
                    btn->setIconSize(QSize(12, 12));
                    btn->setIcon(icons::icon(icon, c.text_muted, 12, dpr));
                    btn->setToolTip(tip);
                    btn->setCursor(Qt::PointingHandCursor);
                    connect(btn, &QToolButton::clicked, this,
                            [this, id = d.id, action = std::move(action)] {
                                if (!action) return;
                                action(id);
                                // The host updates the row as soon as the
                                // engine accepts, so the next tick repaints
                                // this row; refresh now so the click feels
                                // immediate instead of up to 250ms late.
                                // Deferred one turn because the rebuild
                                // destroys the very button emitting this
                                // click.
                                QTimer::singleShot(
                                    0, this, [this] { refresh_if_changed(); });
                            });
                    rl->addWidget(btn, 0, Qt::AlignVCenter);
                };

            const bool alive =
                d.state == downloads::DownloadState::InProgress ||
                d.state == downloads::DownloadState::Paused;
            if (alive) {
                if (d.state == downloads::DownloadState::InProgress &&
                    actions_.pause) {
                    add_action_button(icons::Name::Pause,
                                      QStringLiteral("Pause download"),
                                      QStringLiteral("DownloadRowPause"),
                                      actions_.pause);
                } else if (d.state == downloads::DownloadState::Paused &&
                           actions_.resume) {
                    add_action_button(icons::Name::Play,
                                      QStringLiteral("Resume download"),
                                      QStringLiteral("DownloadRowResume"),
                                      actions_.resume);
                }
                if (actions_.cancel) {
                    add_action_button(icons::Name::Close,
                                      QStringLiteral("Cancel download"),
                                      QStringLiteral("DownloadRowCancel"),
                                      actions_.cancel);
                }
            } else if (d.is_retryable() && actions_.retry) {
                add_action_button(icons::Name::Retry,
                                  QStringLiteral("Retry download"),
                                  QStringLiteral("DownloadRowRetry"),
                                  actions_.retry);
            }

            auto* reveal = new QToolButton(row);
            reveal->setAutoRaise(true);
            reveal->setFixedSize(kRevealButtonBox, kRevealButtonBox);
            reveal->setIconSize(QSize(18, 18));
            // Custom open-folder icon from the icon system instead of the
            // generic platform folder icon (QStyle::SP_DirIcon).
            reveal->setIcon(icons::icon(icons::Name::FolderOpen,
                                        c.text_muted, 18, dpr));
            reveal->setToolTip(QStringLiteral("Show in folder"));
            reveal->setEnabled(file_available);
            if (file_available) reveal->setCursor(Qt::PointingHandCursor);
            connect(reveal, &QToolButton::clicked, this, [this, id = d.id] {
                if (service_) service_->reveal_in_folder(id);
            });
            rl->addWidget(reveal, 0, Qt::AlignVCenter);

            list_layout_->addWidget(row);
        }
        for (auto it = speed_samples_.begin(); it != speed_samples_.end();) {
            if (live_ids.count(it->first) > 0) {
                ++it;
            } else {
                it = speed_samples_.erase(it);
            }
        }
    }
    list_layout_->addStretch(1);
}

} // namespace aurora::ui

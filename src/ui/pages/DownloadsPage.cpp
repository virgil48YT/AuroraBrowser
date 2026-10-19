#include "ui/pages/DownloadsPage.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSize>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

#include "downloads/DownloadService.hpp"
#include "settings/Settings.hpp"
#include "ui/FileTypeIcon.hpp"
#include "ui/GlowFrame.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

using downloads::DownloadItem;
using downloads::DownloadState;

constexpr size_t kPageSize = 300;
// How often the visible page re-reads DownloadService while a transfer runs.
constexpr int kTickMs = 500;
constexpr int kActiveIconPx = 36;
constexpr int kDoneIconPx = 30;
constexpr int kRowButtonPx = 28;
constexpr int kBarHeight = 6;
constexpr int kSummaryWidth = 290;

// Filter dropdown codes. Type filters are kKindBase + file_icons::Kind.
constexpr int kFilterAll = 0;
constexpr int kFilterActive = 1;
constexpr int kFilterCompleted = 2;
constexpr int kFilterFailed = 3;
constexpr int kKindBase = 100;

qreal current_dpr(const QWidget* w) {
    return w && w->screen() ? w->screen()->devicePixelRatio() : 1.0;
}

// "1.2 GB", "245 MB", "78.4 MB" — 1024-based, like the file managers the
// mockup's numbers come from.
QString human_size(int64_t bytes) {
    if (bytes < 0) return QStringLiteral("unknown size");
    static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) return QStringLiteral("%1 B").arg(bytes);
    const int decimals = value >= 100.0 ? 0 : 1;
    return QStringLiteral("%1 %2")
        .arg(QLocale().toString(value, 'f', decimals),
             QLatin1String(kUnits[unit]));
}

QString format_speed(double bytes_per_sec) {
    if (!(bytes_per_sec > 0.0)) return {};
    return human_size(static_cast<int64_t>(bytes_per_sec)) +
           QStringLiteral("/s");
}

QString format_eta(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return {};
    const auto secs = static_cast<int64_t>(std::ceil(seconds));
    if (secs < 60) return QStringLiteral("%1 sec left").arg(secs);
    const int64_t minutes = (secs + 59) / 60;
    if (minutes < 60) return QStringLiteral("%1 min left").arg(minutes);
    const int64_t hours = minutes / 60;
    const int64_t rest = minutes % 60;
    if (rest == 0) return QStringLiteral("%1 h left").arg(hours);
    return QStringLiteral("%1 h %2 min left").arg(hours).arg(rest);
}

QString when_text(const std::chrono::system_clock::time_point& at) {
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch())
            .count();
    if (secs <= 0) return QStringLiteral("—");
    return QDateTime::fromSecsSinceEpoch(secs).toString(
        QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString display_name(const DownloadItem& d) {
    QString name = QString::fromStdString(d.file_name());
    if (name.isEmpty()) name = QString::fromStdString(d.url);
    return name;
}

// The path the type icon is derived from: the target when known, else the
// URL (whose last segment usually carries the extension).
QString icon_path(const DownloadItem& d) {
    const QString target = QString::fromStdString(d.target_path);
    return target.isEmpty() ? QString::fromStdString(d.url) : target;
}

bool file_on_disk(const DownloadItem& d) {
    const QString target = QString::fromStdString(d.target_path);
    return d.state == DownloadState::Complete && !target.isEmpty() &&
           QFileInfo(target).isFile();
}

// One-line label that elides in the middle (the extension stays readable)
// to whatever width the row gives it, and optionally acts as a link.
class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(QWidget* parent) : QLabel(parent) {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
    }

    void set_full_text(const QString& text) {
        if (text == full_) return;
        full_ = text;
        setToolTip(text);
        updateGeometry();
        update();
    }

    std::function<void()> on_click;

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return {fm.horizontalAdvance(full_), fm.height()};
    }
    QSize minimumSizeHint() const override {
        return {0, QFontMetrics(font()).height()};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setFont(font());
        p.setPen(palette().color(QPalette::WindowText));
        const QRect r = contentsRect();
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter,
                   fontMetrics().elidedText(full_, Qt::ElideMiddle, r.width()));
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (on_click && event->button() == Qt::LeftButton &&
            rect().contains(event->position().toPoint())) {
            on_click();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }

private:
    QString full_;
};

// The mockup's progress bar: a dark rounded track with a cyan -> purple
// gradient fill. The gradient spans the whole track, so a short bar is
// mostly cyan and a nearly finished one ends in purple. Unknown totals
// (no Content-Length) show a sweeping segment instead of a fake fill.
class GradientBar final : public QWidget {
public:
    explicit GradientBar(QWidget* parent) : QWidget(parent) {
        setFixedHeight(kBarHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sweep_.setInterval(33);
        QObject::connect(&sweep_, &QTimer::timeout, this, [this] {
            phase_ += 0.018;
            if (phase_ > 1.0) phase_ -= 1.0;
            update();
        });
    }

    // percent < 0: indeterminate.
    void set_state(int percent, bool paused) {
        percent_ = percent;
        paused_ = paused;
        sync_sweep();
        update();
    }

protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event);
        sync_sweep();
    }
    void hideEvent(QHideEvent* event) override {
        sweep_.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        const ThemeColors& c = current_theme().colors;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF track = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = track.height() / 2.0;

        QColor track_color = c.surface_alt;
        track_color.setAlpha(200);
        p.setPen(Qt::NoPen);
        p.setBrush(track_color);
        p.drawRoundedRect(track, radius, radius);

        QRectF fill = track;
        if (percent_ < 0) {
            const qreal seg = track.width() * 0.28;
            const qreal x = track.left() - seg + (track.width() + seg) * phase_;
            fill = QRectF(x, track.top(), seg, track.height())
                       .intersected(track);
        } else {
            fill.setWidth(track.width() * qBound(0, percent_, 100) / 100.0);
        }
        if (fill.width() <= 0.5) return;

        QLinearGradient grad(track.topLeft(), track.topRight());
        if (paused_) {
            grad.setColorAt(0.0, c.text_faint);
            grad.setColorAt(1.0, c.text_muted);
        } else {
            grad.setColorAt(0.0, c.accent);
            grad.setColorAt(0.55, QColor(0x3B, 0x82, 0xF6));
            grad.setColorAt(1.0, c.accent2);
        }
        p.setBrush(grad);
        p.drawRoundedRect(fill, radius, radius);
    }

private:
    void sync_sweep() {
        const bool want = percent_ < 0 && !paused_ && isVisible();
        if (want && !sweep_.isActive()) sweep_.start();
        if (!want) sweep_.stop();
    }

    QTimer sweep_;
    int percent_ = 0;
    bool paused_ = false;
    qreal phase_ = 0.0;
};

QToolButton* row_button(QWidget* parent, icons::Name icon,
                        const QString& tip, const QColor& color, qreal dpr) {
    auto* btn = new QToolButton(parent);
    btn->setObjectName(QStringLiteral("DownloadsRowButton"));
    btn->setAutoRaise(true);
    btn->setFixedSize(kRowButtonPx, kRowButtonPx);
    btn->setIcon(icons::icon(icon, color, 16, dpr));
    btn->setIconSize(QSize(16, 16));
    btn->setToolTip(tip);
    btn->setAccessibleName(tip);
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
}

QFrame* hairline(QWidget* parent, const char* name) {
    auto* line = new QFrame(parent);
    line->setObjectName(QString::fromLatin1(name));
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(Metrics::hairline);
    return line;
}

} // namespace

// ---------------------------------------------------------------------------
// One download row. Its controls are fixed for the state it was built for
// (the page rebuilds when a row changes state); update_from() refreshes the
// numbers on every tick.
class DownloadsPage::Row final : public QWidget {
public:
    enum class Mode { Active, Completed, Failed };

    Row(DownloadsPage* page, const DownloadItem& d, Mode mode, QWidget* parent)
        : QWidget(parent), mode_(mode) {
        setObjectName(QStringLiteral("DownloadsRow"));
        setAttribute(Qt::WA_StyledBackground, true);
        const ThemeColors& c = current_theme().colors;
        const qreal dpr = current_dpr(page);
        const bool on_disk = file_on_disk(d);

        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(Spacing::sm, Spacing::sm, Spacing::sm,
                                Spacing::sm);
        row->setSpacing(Spacing::md);

        const int icon_px = mode == Mode::Active ? kActiveIconPx : kDoneIconPx;
        auto* icon = new QLabel(this);
        icon->setFixedSize(icon_px, icon_px);
        icon->setAlignment(Qt::AlignCenter);
        const QIcon type_icon = file_icons::icon_for(
            icon_path(d), QString::fromStdString(d.mime), on_disk, icon_px,
            dpr);
        icon->setPixmap(type_icon.pixmap(QSize(icon_px, icon_px), dpr));
        row->addWidget(icon, 0, mode == Mode::Active ? Qt::AlignTop
                                                     : Qt::AlignVCenter);

        auto* text = new QVBoxLayout;
        text->setContentsMargins(0, 0, 0, 0);
        text->setSpacing(3);
        name_ = new ElidedLabel(this);
        name_->setObjectName(QStringLiteral("DownloadsRowName"));
        name_->set_full_text(display_name(d));
        if (on_disk) {
            name_->setCursor(Qt::PointingHandCursor);
            name_->setToolTip(QStringLiteral("Open %1")
                                  .arg(QString::fromStdString(d.target_path)));
            name_->on_click = [page, id = d.id] {
                if (page->service_) page->service_->open_file(id);
            };
        }
        text->addWidget(name_);
        meta_ = new ElidedLabel(this);
        meta_->setObjectName(mode == Mode::Active
                                 ? QStringLiteral("DownloadsRowProgressText")
                                 : QStringLiteral("DownloadsRowMeta"));
        text->addWidget(meta_);
        if (mode == Mode::Active) {
            auto* bar_row = new QVBoxLayout;
            bar_row->setContentsMargins(0, 2, 0, 0);
            bar_row->setSpacing(2);
            bar_ = new GradientBar(this);
            bar_row->addWidget(bar_);
            speed_ = new QLabel(this);
            speed_->setObjectName(QStringLiteral("DownloadsRowSpeed"));
            speed_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            bar_row->addWidget(speed_);
            text->addLayout(bar_row);
        }
        row->addLayout(text, 1);

        const Actions& actions = page->actions_;
        if (mode == Mode::Active) {
            if (d.state == DownloadState::InProgress && actions.pause) {
                auto* pause = row_button(this, icons::Name::Pause,
                                         QStringLiteral("Pause download"),
                                         c.text, dpr);
                connect(pause, &QToolButton::clicked, page, [page, id = d.id] {
                    page->run_action(page->actions_.pause, id,
                                     QStringLiteral("Download paused"));
                });
                row->addWidget(pause, 0, Qt::AlignVCenter);
            } else if (d.state == DownloadState::Paused && actions.resume) {
                auto* resume = row_button(this, icons::Name::Play,
                                          QStringLiteral("Resume download"),
                                          c.text, dpr);
                connect(resume, &QToolButton::clicked, page, [page, id = d.id] {
                    page->run_action(page->actions_.resume, id,
                                     QStringLiteral("Download resumed"));
                });
                row->addWidget(resume, 0, Qt::AlignVCenter);
            }
            if (actions.cancel) {
                auto* cancel = row_button(this, icons::Name::Close,
                                          QStringLiteral("Cancel download"),
                                          c.text, dpr);
                connect(cancel, &QToolButton::clicked, page, [page, id = d.id] {
                    page->run_action(page->actions_.cancel, id,
                                     QStringLiteral("Download cancelled"));
                });
                row->addWidget(cancel, 0, Qt::AlignVCenter);
            }
        } else {
            if (mode == Mode::Completed) {
                auto* open_folder = new QPushButton(
                    QStringLiteral("Open folder"), this);
                open_folder->setObjectName(
                    QStringLiteral("DownloadsOpenFolder"));
                open_folder->setIcon(
                    icons::icon(icons::Name::FolderOpen, c.accent2, 16, dpr));
                open_folder->setIconSize(QSize(16, 16));
                open_folder->setEnabled(on_disk);
                open_folder->setToolTip(
                    on_disk ? QStringLiteral("Show the file in its folder")
                            : QStringLiteral("The file was moved or deleted"));
                if (on_disk) open_folder->setCursor(Qt::PointingHandCursor);
                connect(open_folder, &QPushButton::clicked, page,
                        [page, id = d.id] {
                            if (page->service_ &&
                                !page->service_->reveal_in_folder(id)) {
                                emit page->status_message(QStringLiteral(
                                    "Could not open the download's folder"));
                            }
                        });
                row->addWidget(open_folder, 0, Qt::AlignVCenter);
            } else if (d.is_retryable() && actions.retry) {
                auto* retry = row_button(this, icons::Name::Retry,
                                         QStringLiteral("Retry download"),
                                         c.text, dpr);
                connect(retry, &QToolButton::clicked, page, [page, id = d.id] {
                    page->run_action(page->actions_.retry, id,
                                     QStringLiteral("Download restarted"));
                });
                row->addWidget(retry, 0, Qt::AlignVCenter);
            }
            auto* remove = row_button(this, icons::Name::Close,
                                      QStringLiteral("Remove from list"),
                                      c.text_muted, dpr);
            connect(remove, &QToolButton::clicked, page, [page, id = d.id] {
                if (!page->service_) return;
                if (page->service_->remove(id)) {
                    emit page->status_message(
                        QStringLiteral("Removed from the download list (the "
                                       "file stays on disk)"));
                }
                // Deferred: refresh() destroys this row and its button.
                QTimer::singleShot(0, page, [page] { page->refresh(); });
            });
            row->addWidget(remove, 0, Qt::AlignVCenter);
        }
    }

    void update_from(const DownloadItem& d, double speed) {
        if (mode_ == Mode::Active) {
            const int pct = d.percent();
            const bool paused = d.state == DownloadState::Paused;
            meta_->set_full_text(
                d.total_bytes > 0
                    ? QStringLiteral("%1 / %2 (%3%)")
                          .arg(human_size(d.received), human_size(d.total_bytes))
                          .arg(pct)
                    : QStringLiteral("%1 downloaded").arg(human_size(d.received)));
            bar_->set_state(pct, paused);
            QString line;
            if (paused) {
                line = QStringLiteral("Paused");
            } else if (speed > 0.0) {
                line = format_speed(speed);
                if (d.total_bytes > 0 && d.received < d.total_bytes) {
                    const QString eta = format_eta(
                        static_cast<double>(d.total_bytes - d.received) / speed);
                    if (!eta.isEmpty()) line += QStringLiteral(" • ") + eta;
                }
            } else {
                line = QStringLiteral("Starting…");
            }
            speed_->setText(line);
            return;
        }
        const int64_t bytes = d.total_bytes > 0
                                  ? std::max(d.total_bytes, d.received)
                                  : d.received;
        const QString when = when_text(d.finished_at.value_or(d.started_at));
        if (mode_ == Mode::Completed) {
            meta_->set_full_text(
                QStringLiteral("%1 • %2").arg(human_size(bytes), when));
        } else {
            const QString what = d.state == DownloadState::Cancelled
                                     ? QStringLiteral("Cancelled")
                                     : QStringLiteral("Failed");
            const QString amount =
                d.total_bytes > 0
                    ? QStringLiteral("%1 of %2").arg(human_size(d.received),
                                                     human_size(d.total_bytes))
                    : human_size(d.received);
            meta_->set_full_text(
                QStringLiteral("%1 • %2 • %3").arg(what, amount, when));
        }
    }

private:
    Mode mode_ = Mode::Completed;
    ElidedLabel* name_ = nullptr;
    ElidedLabel* meta_ = nullptr;
    GradientBar* bar_ = nullptr;
    QLabel* speed_ = nullptr;
};

// ---------------------------------------------------------------------------

DownloadsPage::DownloadsPage(downloads::DownloadService* service,
                             QWidget* parent, Actions actions)
    : InternalPageWidget(QStringLiteral("Downloads"), parent), service_(service),
      actions_(std::move(actions)) {
    const ThemeColors& c = current_theme().colors;
    const qreal dpr = current_dpr(this);

    // --- Header: download glyph + title (the skeleton's own title label
    // stays for accessibility but is hidden, like the History page).
    if (auto* t = page_title_label()) t->hide();
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("DownloadsHeader"));
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(14);
    auto* glyph = new QLabel(header);
    glyph->setPixmap(icons::pixmap(icons::Name::Downloads, 34, c.accent, dpr));
    glyph->setFixedSize(38, 38);
    header_row->addWidget(glyph, 0, Qt::AlignVCenter);
    auto* title = new QLabel(QStringLiteral("Downloads"), header);
    title->setObjectName(QStringLiteral("DownloadsTitle"));
    header_row->addWidget(title, 0, Qt::AlignVCenter);
    header_row->addStretch(1);
    root_layout()->insertWidget(0, header);

    // --- Body: [toolbar + cards] | [summary]
    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("DownloadsBody"));
    auto* body_row = new QHBoxLayout(body);
    body_row->setContentsMargins(0, Spacing::xs, 0, 0);
    body_row->setSpacing(Spacing::lg + Spacing::xs);

    auto* left = new QWidget(body);
    left->setObjectName(QStringLiteral("DownloadsLeft"));
    auto* left_col = new QVBoxLayout(left);
    left_col->setContentsMargins(0, 0, 0, 0);
    left_col->setSpacing(Spacing::md);

    // Toolbar: omnibar-shaped search, filter dropdown, "Clear all".
    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(Spacing::md);
    auto* search_wrap = new GlowFrame(left);
    search_wrap->setObjectName(QStringLiteral("DownloadsSearchWrap"));
    {
        GlowFrame::Edge edge;
        edge.color1 = QColor(40, 190, 255, 230);   // cyan-blue
        edge.color2 = QColor(150, 90, 255, 215);   // violet
        edge.glow = QColor(70, 120, 255, 100);
        edge.glow_extent = 3;
        search_wrap->set_style(edge, QColor(5, 9, 30, 200), Radius::omnibox);
        auto* l = new QHBoxLayout(search_wrap);
        l->setContentsMargins(14, 8, 14, 8);
        l->setSpacing(10);
        auto* mag = new QLabel(search_wrap);
        mag->setPixmap(icons::pixmap(icons::Name::Search, 18, c.text_muted, dpr));
        mag->setFixedSize(20, 20);
        l->addWidget(mag);
        search_ = new QLineEdit(search_wrap);
        search_->setObjectName(QStringLiteral("DownloadsSearch"));
        search_->setPlaceholderText(QStringLiteral("Search downloads..."));
        search_->setClearButtonEnabled(true);
        l->addWidget(search_, 1);
    }
    toolbar->addWidget(search_wrap, 1);

    filter_ = new QComboBox(left);
    filter_->setObjectName(QStringLiteral("DownloadsFilter"));
    filter_->setCursor(Qt::PointingHandCursor);
    filter_->setToolTip(QStringLiteral("Show only some downloads"));
    filter_->addItem(QStringLiteral("All"), kFilterAll);
    filter_->addItem(QStringLiteral("Active"), kFilterActive);
    filter_->addItem(QStringLiteral("Completed"), kFilterCompleted);
    filter_->addItem(QStringLiteral("Failed & cancelled"), kFilterFailed);
    filter_->insertSeparator(filter_->count());
    for (const auto kind : {file_icons::Kind::Archive,
                            file_icons::Kind::Application,
                            file_icons::Kind::Video, file_icons::Kind::Audio,
                            file_icons::Kind::Image, file_icons::Kind::Pdf,
                            file_icons::Kind::Document,
                            file_icons::Kind::Spreadsheet,
                            file_icons::Kind::Presentation,
                            file_icons::Kind::DiskImage,
                            file_icons::Kind::Code,
                            file_icons::Kind::Generic}) {
        filter_->addItem(file_icons::kind_label(kind),
                         kKindBase + static_cast<int>(kind));
    }
    filter_->setMinimumWidth(120);
    toolbar->addWidget(filter_, 0, Qt::AlignVCenter);

    clear_all_ = new QPushButton(QStringLiteral("Clear all"), left);
    clear_all_->setObjectName(QStringLiteral("DownloadsLinkButton"));
    clear_all_->setCursor(Qt::PointingHandCursor);
    clear_all_->setToolTip(QStringLiteral(
        "Remove every finished, failed and cancelled download from the list "
        "(files stay on disk; running downloads are kept)"));
    toolbar->addWidget(clear_all_, 0, Qt::AlignVCenter);
    left_col->addLayout(toolbar);

    // Cards column, scrolling as a whole.
    auto* scroll = new QScrollArea(left);
    scroll->setObjectName(QStringLiteral("DownloadsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* column_host = new QWidget(scroll);
    column_host->setObjectName(QStringLiteral("DownloadsColumn"));
    auto* column = new QVBoxLayout(column_host);
    column->setContentsMargins(0, 0, Spacing::xs, 0);
    column->setSpacing(Spacing::lg);

    active_ = make_card(column_host, QStringLiteral("Active downloads"), nullptr);
    column->addWidget(active_.frame);

    clear_completed_ = new QPushButton(QStringLiteral("Clear all"), column_host);
    clear_completed_->setObjectName(QStringLiteral("DownloadsLinkButton"));
    clear_completed_->setCursor(Qt::PointingHandCursor);
    clear_completed_->setToolTip(QStringLiteral(
        "Remove completed downloads from the list (files stay on disk)"));
    completed_ = make_card(column_host, QStringLiteral("Completed"),
                           clear_completed_);
    column->addWidget(completed_.frame);

    clear_failed_ = new QPushButton(QStringLiteral("Clear all"), column_host);
    clear_failed_->setObjectName(QStringLiteral("DownloadsLinkButton"));
    clear_failed_->setCursor(Qt::PointingHandCursor);
    clear_failed_->setToolTip(QStringLiteral(
        "Remove failed and cancelled downloads from the list"));
    failed_ = make_card(column_host, QStringLiteral("Failed & cancelled"),
                        clear_failed_);
    failed_.frame->hide();
    column->addWidget(failed_.frame);
    column->addStretch(1);
    scroll->setWidget(column_host);
    left_col->addWidget(scroll, 1);
    body_row->addWidget(left, 1);

    // Summary column.
    auto* right = new QWidget(body);
    right->setObjectName(QStringLiteral("DownloadsRight"));
    right->setFixedWidth(kSummaryWidth);
    auto* right_col = new QVBoxLayout(right);
    right_col->setContentsMargins(0, 0, 0, 0);
    right_col->setSpacing(0);
    build_summary(right, right_col);
    right_col->addStretch(1);
    body_row->addWidget(right, 0);

    set_body(body);

    connect(search_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(filter_, &QComboBox::currentIndexChanged, this,
            [this](int) { refresh(); });
    connect(clear_all_, &QPushButton::clicked, this,
            &DownloadsPage::clear_finished);
    connect(clear_completed_, &QPushButton::clicked, this,
            &DownloadsPage::clear_completed);
    connect(clear_failed_, &QPushButton::clicked, this,
            &DownloadsPage::clear_failed);

    // Live progress. The browser's download callback writes DownloadService on
    // this (the Qt main) thread, so a plain timer on the same thread is a
    // complete live view — no extra threads, no service changes.
    tick_ = new QTimer(this);
    tick_->setInterval(kTickMs);
    connect(tick_, &QTimer::timeout, this, &DownloadsPage::refresh_if_active);
}

DownloadsPage::Card DownloadsPage::make_card(QWidget* parent,
                                             const QString& title,
                                             QPushButton* header_action) {
    Card card;
    card.frame = new GlowFrame(parent);
    card.frame->setObjectName(QStringLiteral("DownloadsCard"));
    card.frame->apply_card_style(14, 3);
    auto* layout = new QVBoxLayout(card.frame);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(Spacing::xs);

    auto* head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, Spacing::xs);
    head->setSpacing(Spacing::sm + 2);
    auto* label = new QLabel(title, card.frame);
    label->setObjectName(QStringLiteral("DownloadsCardTitle"));
    head->addWidget(label, 0, Qt::AlignVCenter);
    card.badge = new QLabel(QStringLiteral("0"), card.frame);
    card.badge->setObjectName(QStringLiteral("DownloadsCountBadge"));
    card.badge->setAlignment(Qt::AlignCenter);
    head->addWidget(card.badge, 0, Qt::AlignVCenter);
    head->addStretch(1);
    if (header_action) {
        header_action->setParent(card.frame);
        head->addWidget(header_action, 0, Qt::AlignVCenter);
    }
    layout->addLayout(head);

    card.rows = new QVBoxLayout;
    card.rows->setContentsMargins(0, 0, 0, 0);
    card.rows->setSpacing(0);
    layout->addLayout(card.rows);

    card.placeholder = new QLabel(card.frame);
    card.placeholder->setObjectName(QStringLiteral("DownloadsPlaceholder"));
    card.placeholder->setWordWrap(true);
    layout->addWidget(card.placeholder);
    return card;
}

void DownloadsPage::build_summary(QWidget* parent, QVBoxLayout* into) {
    const ThemeColors& c = current_theme().colors;
    const qreal dpr = current_dpr(this);

    auto* card = new GlowFrame(parent);
    card->setObjectName(QStringLiteral("DownloadsSummary"));
    card->apply_card_style(14, 3);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(Spacing::md);

    auto* stats_row = new QHBoxLayout;
    stats_row->setSpacing(Spacing::lg);
    auto* big = new QLabel(card);
    big->setPixmap(icons::pixmap(icons::Name::Downloads, 40, c.accent, dpr));
    big->setFixedSize(46, 46);
    stats_row->addWidget(big, 0, Qt::AlignTop);
    auto* stats = new QVBoxLayout;
    stats->setSpacing(2);
    const auto add_stat = [card, stats](const QString& name) {
        auto* label = new QLabel(name, card);
        label->setObjectName(QStringLiteral("DownloadsStatLabel"));
        stats->addWidget(label);
        auto* value = new QLabel(QStringLiteral("0"), card);
        value->setObjectName(QStringLiteral("DownloadsStatValue"));
        stats->addWidget(value);
        stats->addSpacing(Spacing::sm);
        return value;
    };
    total_value_ = add_stat(QStringLiteral("Total downloaded"));
    active_value_ = add_stat(QStringLiteral("Active downloads"));
    completed_value_ = add_stat(QStringLiteral("Completed"));
    stats_row->addLayout(stats, 1);
    layout->addLayout(stats_row);

    layout->addWidget(hairline(card, "DownloadsSummaryDivider"));

    auto* loc_head = new QHBoxLayout;
    loc_head->setSpacing(Spacing::sm);
    auto* loc_icon = new QLabel(card);
    loc_icon->setPixmap(icons::pixmap(icons::Name::Downloads, 15, c.text_muted, dpr));
    loc_icon->setFixedSize(16, 16);
    loc_head->addWidget(loc_icon);
    auto* loc_label = new QLabel(QStringLiteral("Download location"), card);
    loc_label->setObjectName(QStringLiteral("DownloadsSectionLabel"));
    loc_head->addWidget(loc_label, 1);
    layout->addLayout(loc_head);

    auto* loc_box = new QFrame(card);
    loc_box->setObjectName(QStringLiteral("DownloadsLocationBox"));
    auto* loc_row = new QHBoxLayout(loc_box);
    loc_row->setContentsMargins(Spacing::md, Spacing::xs, Spacing::xs, Spacing::xs);
    loc_row->setSpacing(Spacing::xs);
    location_ = new QLineEdit(loc_box);
    location_->setObjectName(QStringLiteral("DownloadsLocation"));
    location_->setReadOnly(true);
    location_->setCursorPosition(0);
    loc_row->addWidget(location_, 1);
    auto* folder = new QToolButton(loc_box);
    folder->setObjectName(QStringLiteral("DownloadsRowButton"));
    folder->setAutoRaise(true);
    folder->setFixedSize(kRowButtonPx + 4, kRowButtonPx + 4);
    folder->setIcon(icons::icon(icons::Name::FolderOpen, c.accent2, 18, dpr));
    folder->setIconSize(QSize(18, 18));
    folder->setToolTip(QStringLiteral("Open the download folder"));
    folder->setCursor(Qt::PointingHandCursor);
    connect(folder, &QToolButton::clicked, this,
            &DownloadsPage::open_download_location);
    loc_row->addWidget(folder);
    layout->addWidget(loc_box);

    auto* settings_link = new QPushButton(card);
    settings_link->setObjectName(QStringLiteral("DownloadsSettingsLink"));
    settings_link->setCursor(Qt::PointingHandCursor);
    settings_link->setAccessibleName(QStringLiteral("Download settings"));
    settings_link->setMinimumHeight(42);
    auto* link_row = new QHBoxLayout(settings_link);
    link_row->setContentsMargins(Spacing::md, 0, Spacing::md, 0);
    link_row->setSpacing(Spacing::sm + 2);
    auto* gear = new QLabel(settings_link);
    gear->setPixmap(icons::pixmap(icons::Name::Settings, 18, c.accent2, dpr));
    gear->setFixedSize(20, 20);
    gear->setAttribute(Qt::WA_TransparentForMouseEvents);
    link_row->addWidget(gear);
    auto* link_text = new QLabel(QStringLiteral("Download settings"), settings_link);
    link_text->setObjectName(QStringLiteral("DownloadsSettingsLinkText"));
    link_text->setAttribute(Qt::WA_TransparentForMouseEvents);
    link_row->addWidget(link_text, 1);
    auto* chevron = new QLabel(settings_link);
    chevron->setPixmap(icons::pixmap(icons::Name::ChevronRight, 14, c.text_muted, dpr));
    chevron->setFixedSize(16, 16);
    chevron->setAttribute(Qt::WA_TransparentForMouseEvents);
    link_row->addWidget(chevron);
    connect(settings_link, &QPushButton::clicked, this,
            &DownloadsPage::download_settings_requested);
    layout->addWidget(settings_link);

    into->addWidget(card);
}

void DownloadsPage::set_prefs(settings::Settings* prefs) {
    prefs_ = prefs;
    if (location_) location_->setText(download_location());
}

QString DownloadsPage::download_location() const {
    // Same resolution as the engine's download handler: the downloads.path
    // preference, else the platform's standard Downloads folder.
    QString path = prefs_ ? QString::fromStdString(prefs_->downloads_path())
                          : QString();
    if (path.trimmed().isEmpty()) {
        path = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    if (path.isEmpty()) path = QDir::homePath() + QStringLiteral("/Downloads");
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

void DownloadsPage::open_download_location() {
    const QString path = QDir::fromNativeSeparators(download_location());
    if (!QDir(path).exists()) {
        emit status_message(
            QStringLiteral("The download folder does not exist yet: %1")
                .arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        emit status_message(QStringLiteral("Could not open the download folder"));
    }
}

void DownloadsPage::showEvent(QShowEvent* event) {
    InternalPageWidget::showEvent(event);
    if (tick_) tick_->start();
}

void DownloadsPage::hideEvent(QHideEvent* event) {
    // Switching tabs hides the page without destroying it, so this is the only
    // place that can stop the poll.
    if (tick_) tick_->stop();
    InternalPageWidget::hideEvent(event);
}

bool DownloadsPage::has_active_download() const {
    if (!service_) return false;
    for (const auto& d : service_->recent(kPageSize)) {
        if (d.state == DownloadState::InProgress) return true;
    }
    return false;
}

void DownloadsPage::refresh_if_active() {
    if (!isVisible()) return;
    // One more refresh after the last transfer stops, so a download that
    // just completed (or failed) moves to its card without a manual reload.
    const bool active = has_active_download();
    if (active || had_active_) refresh();
    had_active_ = active;
}

bool DownloadsPage::matches(const DownloadItem& d) const {
    const QString needle = search_ ? search_->text().trimmed() : QString();
    if (!needle.isEmpty() &&
        !display_name(d).contains(needle, Qt::CaseInsensitive) &&
        !QString::fromStdString(d.url).contains(needle, Qt::CaseInsensitive)) {
        return false;
    }
    const int code = filter_ ? filter_->currentData().toInt() : kFilterAll;
    switch (code) {
        case kFilterAll: return true;
        case kFilterActive: return d.is_active();
        case kFilterCompleted: return d.state == DownloadState::Complete;
        case kFilterFailed: return d.is_retryable();
        default: break;
    }
    if (code >= kKindBase) {
        const auto kind = file_icons::classify(icon_path(d),
                                               QString::fromStdString(d.mime));
        return static_cast<int>(kind) == code - kKindBase;
    }
    return true;
}

void DownloadsPage::refresh() {
    if (!service_) {
        set_empty_state(true,
                        QStringLiteral("No profile is attached to this window, "
                                       "so downloads are unavailable."));
        return;
    }
    set_empty_state(false);
    if (location_) location_->setText(download_location());

    const auto items = service_->recent(kPageSize);
    std::unordered_map<int64_t, double> speeds;
    std::unordered_set<int64_t> live_ids;
    std::vector<const DownloadItem*> active_rows;
    std::vector<const DownloadItem*> completed_rows;
    std::vector<const DownloadItem*> failed_rows;
    int64_t total_bytes = 0;
    int active_count = 0;
    int completed_count = 0;
    int finished_count = 0;
    int failed_count = 0;
    for (const auto& d : items) {
        live_ids.insert(d.id);
        // Sampled exactly once per row per refresh.
        speeds[d.id] = sample_speed(d);
        if (d.is_active()) {
            ++active_count;
            total_bytes += std::max<int64_t>(0, d.received);
        } else if (d.state == DownloadState::Complete) {
            ++completed_count;
            total_bytes += d.total_bytes > 0 ? std::max(d.total_bytes, d.received)
                                             : std::max<int64_t>(0, d.received);
        }
        if (d.is_finished()) ++finished_count;
        if (d.is_retryable()) ++failed_count;
        if (!matches(d)) continue;
        if (d.is_active()) {
            active_rows.push_back(&d);
        } else if (d.state == DownloadState::Complete) {
            completed_rows.push_back(&d);
        } else {
            failed_rows.push_back(&d);
        }
    }
    for (auto it = speed_samples_.begin(); it != speed_samples_.end();) {
        it = live_ids.count(it->first) ? std::next(it) : speed_samples_.erase(it);
    }
    for (auto it = smoothed_speed_.begin(); it != smoothed_speed_.end();) {
        it = live_ids.count(it->first) ? std::next(it) : smoothed_speed_.erase(it);
    }

    // Summary card (always the whole list, not the filtered view).
    total_value_->setText(human_size(total_bytes));
    active_value_->setText(QString::number(active_count));
    completed_value_->setText(QString::number(completed_count));
    clear_all_->setEnabled(finished_count > 0);
    clear_completed_->setEnabled(completed_count > 0);
    clear_failed_->setEnabled(failed_count > 0);

    // Rebuild only when the listed rows (or their state / file presence)
    // changed; otherwise the rows below are updated in place.
    QString key;
    const auto sign = [&key](QChar tag, const DownloadItem& d, bool extra) {
        key += tag;
        key += QString::number(d.id);
        key += QLatin1Char(':');
        key += QString::number(static_cast<int>(d.state));
        key += extra ? QLatin1Char('+') : QLatin1Char('-');
        key += QLatin1Char(';');
    };
    for (const auto* d : active_rows) sign(QLatin1Char('A'), *d, false);
    for (const auto* d : completed_rows) sign(QLatin1Char('C'), *d, file_on_disk(*d));
    for (const auto* d : failed_rows) sign(QLatin1Char('F'), *d, false);

    if (key != layout_key_) {
        layout_key_ = key;
        for (auto& entry : rows_) {
            entry.second->hide();
            entry.second->deleteLater();
        }
        rows_.clear();
        for (Card* card : {&active_, &completed_, &failed_}) {
            while (QLayoutItem* item = card->rows->takeAt(0)) {
                if (QWidget* w = item->widget()) {
                    w->hide();
                    w->deleteLater();
                }
                delete item;
            }
        }
        const auto fill = [this](Card& card,
                                 const std::vector<const DownloadItem*>& list,
                                 Row::Mode mode) {
            bool first = true;
            for (const auto* d : list) {
                if (!first) {
                    card.rows->addWidget(hairline(card.frame, "DownloadsRowDivider"));
                }
                first = false;
                auto* row = new Row(this, *d, mode, card.frame);
                card.rows->addWidget(row);
                rows_[d->id] = row;
            }
        };
        fill(active_, active_rows, Row::Mode::Active);
        fill(completed_, completed_rows, Row::Mode::Completed);
        fill(failed_, failed_rows, Row::Mode::Failed);
    }
    for (const auto* list : {&active_rows, &completed_rows, &failed_rows}) {
        for (const auto* d : *list) {
            const auto it = rows_.find(d->id);
            if (it != rows_.end()) it->second->update_from(*d, speeds[d->id]);
        }
    }

    const bool narrowed = (search_ && !search_->text().trimmed().isEmpty()) ||
                          (filter_ && filter_->currentData().toInt() != kFilterAll);
    active_.badge->setText(QString::number(active_rows.size()));
    completed_.badge->setText(QString::number(completed_rows.size()));
    failed_.badge->setText(QString::number(failed_rows.size()));
    active_.placeholder->setText(
        narrowed ? QStringLiteral("No active downloads match.")
                 : QStringLiteral("No active downloads. Files you download "
                                  "show their progress here."));
    active_.placeholder->setVisible(active_rows.empty());
    completed_.placeholder->setText(
        narrowed ? QStringLiteral("No completed downloads match.")
                 : QStringLiteral("No completed downloads yet."));
    completed_.placeholder->setVisible(completed_rows.empty());
    failed_.placeholder->setVisible(false);
    failed_.frame->setVisible(!failed_rows.empty());
}

double DownloadsPage::sample_speed(const DownloadItem& d) {
    if (d.state != DownloadState::InProgress) {
        // Nothing is moving any more, and a kept sample would only make the
        // map grow with history.
        speed_samples_.erase(d.id);
        smoothed_speed_.erase(d.id);
        return 0.0;
    }

    const auto now = std::chrono::steady_clock::now();
    double bytes_per_sec = 0.0;
    const auto prev = speed_samples_.find(d.id);
    if (prev != speed_samples_.end()) {
        const double elapsed =
            std::chrono::duration<double>(now - prev->second.at).count();
        const int64_t delta = d.received - prev->second.received;
        // A non-positive interval or byte delta says nothing about the rate:
        // fall through to the average instead of dividing by zero.
        if (elapsed > 0.0 && delta > 0) {
            bytes_per_sec = static_cast<double>(delta) / elapsed;
        }
    }
    if (bytes_per_sec <= 0.0) {
        // First refresh for this download (or one in which nothing arrived):
        // the average since it started is the honest fallback.
        const double since_start =
            std::chrono::duration<double>(std::chrono::system_clock::now() -
                                          d.started_at)
                .count();
        if (since_start > 0.0 && d.received > 0) {
            bytes_per_sec = static_cast<double>(d.received) / since_start;
        }
    }
    speed_samples_[d.id] = SpeedSample{d.received, now};

    // Exponential smoothing keeps the speed / ETA line readable.
    const auto smoothed = smoothed_speed_.find(d.id);
    if (smoothed != smoothed_speed_.end() && smoothed->second > 0.0 &&
        bytes_per_sec > 0.0) {
        bytes_per_sec = 0.6 * smoothed->second + 0.4 * bytes_per_sec;
    }
    smoothed_speed_[d.id] = bytes_per_sec;
    return bytes_per_sec;
}

void DownloadsPage::run_action(const std::function<bool(int64_t)>& action,
                               int64_t id, const QString& done_message) {
    if (!action) return;
    if (action(id) && !done_message.isEmpty()) emit status_message(done_message);
    // The host updates the row as soon as the engine accepts, so repaint: the
    // ticker only runs while something is still in flight and may never fire
    // again. Deferred to the next event-loop turn because a state change
    // rebuilds the rows — including the button emitting this click.
    QTimer::singleShot(0, this, [this] { refresh(); });
}

void DownloadsPage::clear_finished() {
    if (!service_) return;
    const int removed = service_->clear_finished();
    emit status_message(
        QStringLiteral("Cleared %1 finished download(s) from the list").arg(removed));
    refresh();
}

void DownloadsPage::clear_completed() {
    if (!service_) return;
    int removed = 0;
    for (const auto& d : service_->recent(kPageSize)) {
        if (d.state == DownloadState::Complete && service_->remove(d.id)) ++removed;
    }
    emit status_message(
        QStringLiteral("Cleared %1 completed download(s) from the list").arg(removed));
    refresh();
}

void DownloadsPage::clear_failed() {
    if (!service_) return;
    int removed = 0;
    for (const auto& d : service_->recent(kPageSize)) {
        if (d.is_retryable() && service_->remove(d.id)) ++removed;
    }
    emit status_message(
        QStringLiteral("Cleared %1 failed or cancelled download(s)").arg(removed));
    refresh();
}

} // namespace aurora::ui

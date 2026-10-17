#include "ui/pages/InternalPageWidget.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QGuiApplication>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

#include "ui/PageBackground.hpp"
#include "ui/Theme.hpp"

namespace aurora::ui {

namespace {

// Desktop, not mobile. The content column fills the window's width minus a
// comfortable gutter and is only capped on very wide displays -- the previous
// skeleton split the window three ways between two elastic gutters and the
// column, so the card ended up ~a third of the screen with the rest empty
// (and page titles / rows truncated inside it).
constexpr int kMaxContentWidth = 1400;  // only bites on ultrawide displays
// Side gutter. Wide enough to breathe, small enough that the content still
// owns the window. Deliberately no minimum width on the column: the window's
// own 400px minimum must stay achievable, and the body scrolls when narrow.
constexpr int kSideGutter = 56;
constexpr int kTopGutter = 28;

} // namespace

InternalPageWidget::InternalPageWidget(const QString& title, QWidget* parent)
    : QWidget(parent) {
    setObjectName("InternalPage");
    // The aurora wallpaper is painted by paintEvent() over the whole page;
    // the styled background is built into the column so translucent surfaces
    // can lie on top of it without alpha-compositing seams over QSS.
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    // The title also identifies the page for accessibility tooling and for
    // anything inspecting the widget.
    setAccessibleName(title);

    // Outer row: a fixed gutter plus a column that takes ALL remaining width.
    // The gutters are zero-stretch: they only reclaim space once the column
    // hits kMaxContentWidth, which keeps the content centred on ultrawides
    // while filling the window everywhere else.
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(kSideGutter, kTopGutter, kSideGutter,
                              Spacing::xl);
    outer->setSpacing(0);
    outer->addStretch(0);

    auto* column = new QWidget(this);
    column->setObjectName("PageColumn");
    column->setMaximumWidth(kMaxContentWidth);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    outer->addWidget(column, 1);
    outer->addStretch(0);

    root_ = new QVBoxLayout(column);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(Spacing::md);

    // --- Header ------------------------------------------------------------
    // Aurora neon: every internal page leads with its section name and a
    // one-line subtitle ("General — Customize your browsing experience.").
    // Both render above the toolbar row; an empty subtitle hides its label.
    title_label_ = new QLabel(title, column);
    title_label_->setObjectName("SettingsPageTitle");
    root_->addWidget(title_label_);

    subtitle_ = new QLabel(column);
    subtitle_->setObjectName("SettingsPageSubtitle");
    subtitle_->setWordWrap(true);
    subtitle_->hide();   // populated by set_subtitle()
    root_->addWidget(subtitle_);

    // The actions row sits beneath the header block.
    toolbar_host_ = new QWidget(column);
    toolbar_host_->setObjectName("PageToolbar");
    toolbar_ = new QHBoxLayout(toolbar_host_);
    toolbar_->setContentsMargins(0, 0, 0, 0);
    toolbar_->setSpacing(Spacing::sm);
    root_->addWidget(toolbar_host_);

    // Hairline between the actions row and the body, so the controls read as
    // a header band rather than floating on top of the list.
    divider_ = new QFrame(column);
    divider_->setObjectName("PageDivider");
    divider_->setFrameShape(QFrame::NoFrame);
    divider_->setFixedHeight(Metrics::hairline);
    divider_->hide();   // the aurora pages use spacing, not hairlines
    root_->addWidget(divider_);

    empty_label_ = new QLabel(column);
    empty_label_->setObjectName("PageEmptyState");
    empty_label_->setAlignment(Qt::AlignCenter);
    empty_label_->setWordWrap(true);
    empty_label_->hide();
    root_->addWidget(empty_label_, 1);
}

// Full-bleed aurora backdrop, cached so paints cost one drawPixmap instead
// of a full-image scale. Only the size changing re-rasterises the wallpaper
// (resizeEvent below).
void InternalPageWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    if (bg_source_.isNull()) {
        bg_source_ = load_page_wallpaper();
        bg_source_.setDevicePixelRatio(devicePixelRatioF());
    }
    if (bg_.isNull() && !bg_source_.isNull()) {
        bg_ = scale_page_wallpaper(bg_source_, size(), devicePixelRatioF());
    }
    // A DPR change (window dragged to another monitor) invalidates the
    // cached scale; re-scale rather than stretch blurry pixels.
    if (!bg_.isNull() && bg_.devicePixelRatio() != devicePixelRatioF()) {
        bg_ = scale_page_wallpaper(bg_source_, size(), devicePixelRatioF());
    }
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!bg_.isNull()) {
        // Centre-crop: the pixmap is at least widget-sized per the scale
        // mode, so the offsets clip the longer axis around the middle.
        const int x = (width() - bg_.width() / devicePixelRatioF()) / 2;
        const int y = (height() - bg_.height() / devicePixelRatioF()) / 2;
        p.drawPixmap(x, y, bg_);
    } else {
        // Missing artwork must never leave the page unpainted; fall back to
        // the window background token while keeping the page readable.
        p.fillRect(rect(), QGuiApplication::palette().color(QPalette::Window));
    }
}

void InternalPageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    bg_ = {};  // repaint lazily via paintEvent's empty-cache check
}

void InternalPageWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // A page with no toolbar controls leaves an empty row's worth of spacing
    // under the header; hide the host and its hairline when that happens.
    const bool has_controls = toolbar_ && toolbar_->count() > 0;
    if (toolbar_host_) toolbar_host_->setVisible(has_controls);
    // The divider is always hidden on the aurora pages (spacing does the
    // separation); it stays connected only as an artifact of construction.
    if (divider_) divider_->setVisible(false);
}

QHBoxLayout* InternalPageWidget::toolbar_row() { return toolbar_; }

void InternalPageWidget::set_subtitle(const QString& text) {
    if (!subtitle_) return;
    subtitle_->setText(text);
    subtitle_->setVisible(!text.trimmed().isEmpty());
}

void InternalPageWidget::set_body(QWidget* body) {
    body_ = body;
    // Insert above the empty-state label so only one of the two is visible.
    root_->insertWidget(root_->count() - 1, body, 1);
}

void InternalPageWidget::set_empty_state(bool visible, const QString& message) {
    if (!message.isEmpty()) empty_label_->setText(message);
    empty_label_->setVisible(visible);
    if (body_) body_->setVisible(!visible);
}

} // namespace aurora::ui

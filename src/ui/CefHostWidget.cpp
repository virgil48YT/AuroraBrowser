#include "ui/CefHostWidget.hpp"

#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QTimer>
#include <QWindow>
#include <cmath>
#include "ui/ThemeState.hpp"
#include "util/Log.hpp"
#include "webview/WebViewController.hpp"

namespace aurora::ui {

CefHostWidget::CefHostWidget(aurora::core::TabId tab,
                            std::string_view initial_url,
                            aurora::webview::WebViewController* controller,
                            QWidget* parent)
    : QWidget(parent), tab_(tab), initial_url_(initial_url), controller_(controller) {
    // NOTE: Qt::WA_NativeWindow is deliberately NOT set here. Setting it makes
    // Qt materialize a native window handle for every tab as soon as the host
    // is constructed (and QStackedWidget::addWidget reparents it, which is
    // enough to realize it). During a 20-tab session restore that meant 20
    // native windows before the event loop turned. The attribute is set in
    // create_browser(), immediately before winId() is actually needed.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    // Opaque paint is only meaningful when CEF is compositing into this window;
    // without CEF we paint the themed background ourselves (paintEvent) so the
    // placeholder is never a black hole.
    setAttribute(Qt::WA_OpaquePaintEvent, controller_ != nullptr);
    setObjectName("CefHost");
    // Ensure this widget fills its parent completely.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    if (controller_) {
        controller_->set_preferred_color_scheme(tab_, current_theme().is_dark);
        theme_observer_id_ = add_theme_observer(
            [this](const Theme& theme) {
                if (controller_)
                    controller_->set_preferred_color_scheme(tab_, theme.is_dark);
            });
    }

    if (!controller_) {
        // Build without CEF: a clearly-labeled placeholder, not a fake page.
        auto* layout = new QVBoxLayout(this);
        layout->setAlignment(Qt::AlignCenter);
        auto* label = new QLabel(
            "CEF is not enabled in this build.\n\n"
            "Run scripts/fetch_cef.sh and reconfigure with "
            "-DAURORA_ENABLE_CEF=ON to load web content.", this);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        label->setStyleSheet(
            QString("color: %1;")
                .arg(current_theme().colors.text_muted.name()));
        layout->addWidget(label);
    }
}

CefHostWidget::~CefHostWidget() {
    remove_theme_observer(theme_observer_id_);
}

void CefHostWidget::paintEvent(QPaintEvent*) {
    // With CEF the child window paints the whole surface; only the placeholder
    // build needs a themed backdrop.
    if (!controller_) {
        QPainter p(this);
        p.fillRect(rect(), current_theme().colors.background);
    }
}

void CefHostWidget::make_current() {
    const bool was_current = current_;
    current_ = true;
    if (!was_current) ++activations_;
    if (!controller_ || created_ || creating_ || closing_ || suspended_) return;
    // Not on screen yet (the window itself may still be hidden): showEvent will
    // finish the job, because current_ is now set.
    if (!isVisible()) return;
    create_browser();
}

void CefHostWidget::relinquish() {
    current_ = false;
}

void CefHostWidget::suspend(std::string_view resume_url) {
    // URL and history belong to TabState; only the last committed URL is
    // needed to seed CEF's replacement browser.
    initial_url_ = std::string(resume_url);
    suspended_ = true;
    if (!controller_ || !created_ || closing_) return;
    closing_ = true;
    controller_->destroy(tab_);
}

void CefHostWidget::restore() {
    suspended_ = false;
    if (current_ && isVisible() && !closing_) create_browser();
}

void CefHostWidget::browser_closed() {
    closing_ = false;
    created_ = false;
    if (current_ && isVisible() && !suspended_) create_browser();
}

void CefHostWidget::create_browser() {
    // One-shot: never two browsers for one tab, and never re-entered from a
    // showEvent/resizeEvent delivered while CEF creates its child window.
    if (!controller_ || created_ || creating_ || closing_ || suspended_) return;
    creating_ = true;
    // Realize the native window only now, at the moment CEF needs a parent.
    setAttribute(Qt::WA_NativeWindow);
    controller_->create(tab_, initial_url_, reinterpret_cast<void*>(winId()));
    created_ = true;
    creating_ = false;
    AURORA_LOG(util::LogLevel::Info, "ui")
        << "created web host for tab " << tab_.value();
    // CEF creates the child window at the bounds we passed (0,0,0,0).
    // Give it the real size now so the first frame isn't a 0x0 black hole.
    // Use a deferred sync so the layout has settled.
    QTimer::singleShot(0, this, [this] { sync_geometry(); });
    controller_->set_visible(tab_, true);
}

void CefHostWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // Creation happens here only for a host the window already designated as
    // current while it was still hidden. A host that merely got shown because
    // QStackedWidget shuffled its children stays uncreated.
    if (current_ && !created_ && !closing_ && !suspended_) create_browser();
    if (controller_ && created_) controller_->set_visible(tab_, true);
}

void CefHostWidget::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (controller_ && created_) controller_->set_visible(tab_, false);
}

void CefHostWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    // In windowed CEF mode the child browser window does NOT follow the parent
    // automatically — we must notify CEF on every resize or the page renders
    // at the original size and the rest stays black.
    push_size(e->size().width(), e->size().height());
}

void CefHostWidget::sync_geometry() {
    push_size(width(), height());
}

void CefHostWidget::push_size(int logical_w, int logical_h) {
    if (!controller_ || !created_ || logical_w <= 0 || logical_h <= 0) return;
    // Qt hands us logical (device-independent) pixels; a native child window is
    // sized in *device* pixels. On a display scaled to 150% or 200% — the
    // normal case on Windows laptops — passing logical pixels straight through
    // made the web view cover only 1/2 or 1/4 of the widget, with the rest
    // black, and made entering fullscreen look like the page had been blown up.
    // Scale by the window's device pixel ratio; round rather than truncate so
    // fractional scaling (125%, 175%) does not leave a one-pixel gap.
    const qreal ratio = devicePixelRatioF();
    const int w = static_cast<int>(std::lround(logical_w * ratio));
    const int h = static_cast<int>(std::lround(logical_h * ratio));
    controller_->resize(tab_, w, h);
}

} // namespace aurora::ui

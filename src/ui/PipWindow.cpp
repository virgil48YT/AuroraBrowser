#include "ui/PipWindow.hpp"

#if defined(AURORA_HAS_CEF)

#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/CefHostWidget.hpp"
#include "ui/Icons.hpp"
#include "ui/MainWindow.hpp"
#include "ui/ThemeState.hpp"
#include "webview/WebViewController.hpp"

namespace aurora::ui {
namespace {
constexpr int kBorder = 6;
constexpr int kHeader = 34;
constexpr int kMinContentWidth = 256;
constexpr double kAspect = 16.0 / 9.0;
} // namespace

bool PipWindow::can_open(MainWindow* source, core::TabId tab) {
    if (!source || !source->webview_ || source->page_of(tab)) return false;
    const auto* state = source->tabs().find(tab);
    return state && state->url.is_valid() && !state->url.is_internal_scheme() &&
           !state->url.is_search_query();
}

PipWindow* PipWindow::open(MainWindow* source, core::TabId tab) {
    if (!can_open(source, tab)) return nullptr;
    const auto& tabs = source->tabs().tabs();
    const auto found = std::find_if(tabs.begin(), tabs.end(),
                                    [tab](const auto& state) { return state.id == tab; });
    const auto index = static_cast<std::size_t>(found - tabs.begin());
    auto* pip = new PipWindow(source, tab, index);
    if (!pip->take_page()) { delete pip; return nullptr; }
    pip->place_in_corner();
    pip->show();
    // The stack is visible now; activate only the one transferred host, using
    // the same lazy browser-creation gate as the full-size window.
    pip->owner_->show_host_for(tab);
    if (auto* host = qobject_cast<CefHostWidget*>(pip->content_->currentWidget()))
        host->sync_geometry();
    pip->raise();
    return pip;
}

PipWindow::PipWindow(MainWindow* source, core::TabId tab, std::size_t index)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      source_(source), tab_(tab), original_index_(index) {
    setObjectName(QStringLiteral("PipWindow"));
    setWindowTitle(QStringLiteral("Picture in Picture — Aurora"));
    setAccessibleName(QStringLiteral("Picture in Picture"));
    setAttribute(Qt::WA_DeleteOnClose);
#if defined(Q_OS_MACOS)
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
#endif
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(constrained_size(kMinContentWidth));

    return_button_ = new QToolButton(this);
    return_button_->setToolTip(QStringLiteral("Return to tab"));
    return_button_->setAccessibleName(QStringLiteral("Return to tab"));
    close_button_ = new QToolButton(this);
    close_button_->setToolTip(QStringLiteral("Close Picture in Picture and restore tab"));
    close_button_->setAccessibleName(QStringLiteral("Close Picture in Picture"));
    for (auto* button : {return_button_, close_button_}) {
        button->setAutoRaise(true);
        button->setCursor(Qt::ArrowCursor);
        button->setIconSize(QSize(16, 16));
        connect(button, &QToolButton::clicked, this, [this] { close(); });
    }
    update_style();
    theme_observer_id_ = add_theme_observer([this](const Theme&) { update_style(); });
    source->installEventFilter(this);
    connect(source, &QObject::destroyed, this, [this] {
        // Unexpected deletion (rather than closeEvent): restore into a new
        // normal window next turn, never dereference the expired source.
        QTimer::singleShot(0, this, [this] { if (!restored_) close(); });
    });
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        // No new renderer may be created after app.exec() returns. Preserve
        // the model/session, then drain our controller while CEF is alive.
        shutting_down_ = true;
        restore_page(true);
        hide();
        // Deferred deletes are still delivered as the event loop exits; do
        // not leave a hidden owner outliving the profile services in main().
        deleteLater();
    });
}

PipWindow::~PipWindow() {
    remove_theme_observer(theme_observer_id_);
    if (source_) source_->removeEventFilter(this);
    if (owner_) {
        // QObject deletion can bypass closeEvent (e.g. app-owned cleanup).
        // Preserve the detached page's model/session even in that path. A
        // destructor cannot wait for a previous host's close or create engines.
        if (!restored_) restore_page(true);
        // The native host stays alive throughout asynchronous engine shutdown.
        // Delete it before its controller: its destructor unregisters theme
        // callbacks that would otherwise retain a dangling controller pointer.
        owner_->prepare_shutdown();
        delete content_;
        content_ = nullptr;
        owner_->content_stack_ = nullptr;
        delete owner_;
    }
}

bool PipWindow::take_page() {
    if (!source_) return false;
    owner_ = new MainWindow(source_->services_);
    owner_->inherit_theme_from(source_);
    configure_controller();
    auto transfer = source_->release_tab_for_transfer(tab_);
    if (!transfer) return false;
    const auto rollback = *transfer;
    if (!owner_->adopt_transferred_tab(std::move(*transfer), 0, true)) {
        source_->adopt_transferred_tab(rollback, original_index_, false);
        return false;
    }

    content_ = owner_->content_stack_;
    content_->setParent(this);
    content_->show();
    layout_content();
    // Unlike an ordinary tear-off, PiP must retain its return destination.
    // Keep a normal browser window available when its only tab was detached.
    if (source_->tabs().count() == 0) source_->on_new_tab();
    return true;
}

void PipWindow::configure_controller() {
    if (!owner_->webview_) return;
    auto* controller = owner_->webview_.get();
    controller->set_new_tab_callback([this](std::string url) {
        if (!source_ || returning_ || restored_) return false;
        QTimer::singleShot(0, source_.data(), [source = source_, url = std::move(url)] {
            if (source) source->open_site_initiated_tab(url);
        });
        return true;
    });
    controller->set_permission_callback([this](webview::PermissionRequest request) {
        // Permission chrome must be visible, never queued in the hidden owner.
        // Dropping a request when its original window is gone denies it.
        if (!source_ || returning_ || restored_) return;
        QTimer::singleShot(0, source_.data(), [source = source_, request = std::move(request)]() mutable {
            if (source) source->on_permission_request(std::move(request));
        });
    });
    controller->set_extension_install_callback([this](const webview::ExtensionInstallRequest& request) {
        if (!source_ || returning_ || restored_) return;
        QTimer::singleShot(0, source_.data(), [source = source_, request] {
            if (source) source->on_extension_install_request(request);
        });
    });
    controller->set_fullscreen_callback([this](core::TabId id, bool fullscreen) {
        if (fullscreen) QTimer::singleShot(0, this, [this, id] {
            if (owner_ && owner_->webview_) owner_->webview_->exit_fullscreen(id);
        });
    });
    controller->set_key_event_callback([this](core::TabId, const webview::EngineKeyEvent& key) {
        if (!key.key_down) return false;
        // A hidden MainWindow must never process Ctrl+W/F11 on PiP's behalf.
        // Return-to-tab is deferred out of the native engine key callback.
        if ((key.ctrl && key.key_code == 0x57) || key.key_code == 0x1b) {
            QTimer::singleShot(0, this, [this] { close(); });
            return true;
        }
        return false;
    });
}

bool PipWindow::restore_page(bool shutting_down) {
    if (restored_ || !owner_) return true;
    if (returning_) return false;
    // A very quick return can beat the source browser's asynchronous close.
    // Do not reuse its TabId in that controller until its old host is gone.
    if (!shutting_down && source_ && source_->closing_hosts_.count(tab_) != 0) {
        QTimer::singleShot(50, this, [this] { close(); });
        return false;
    }
    returning_ = true;
    MainWindow* destination = source_.data();
    bool created_destination = false;
    if (!destination && !shutting_down) {
        destination = new MainWindow(owner_->services_);
        destination->setAttribute(Qt::WA_DeleteOnClose);
        destination->inherit_theme_from(owner_);
        created_destination = true;
    }
    if (destination) {
        auto transfer = owner_->release_tab_for_transfer(tab_);
        if (transfer) {
            const auto rollback = *transfer;
            // During quit, only restore model/session state: creating another
            // browser now would race the application's global CefShutdown.
            const bool was_bulk = destination->bulk_open_;
            if (shutting_down) destination->bulk_open_ = true;
            const bool adopted = destination->adopt_transferred_tab(
                std::move(*transfer), original_index_, created_destination);
            destination->bulk_open_ = was_bulk;
            if (!adopted) {
                owner_->adopt_transferred_tab(rollback, 0, false);
                if (created_destination) delete destination;
                returning_ = false;
                return false;
            }
        }
        destination->save_session();
        if (!shutting_down && !close_source_after_return_) {
            if (destination->isMinimized()) destination->showNormal();
            else destination->show();
            destination->raise();
            destination->activateWindow();
        }
    } else {
        // The source disappeared during application shutdown; the detached
        // page is still recoverable on the next launch.
        owner_->save_session();
    }
    restored_ = true;
    owner_->prepare_shutdown();
    if (close_source_after_return_ && source_ && !shutting_down) {
        QTimer::singleShot(0, source_.data(), [source = source_] { if (source) source->close(); });
    }
    returning_ = false;
    return true;
}

void PipWindow::closeEvent(QCloseEvent* event) {
    if (!restore_page(shutting_down_)) { event->ignore(); return; }
    QWidget::closeEvent(event);
}

bool PipWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == source_.data() && event->type() == QEvent::Close && !restored_) {
        // Restore before MainWindow saves its session and shuts down CEF.
        // If the source's old host is still closing, defer that close too.
        close_source_after_return_ = true;
        if (!close()) {
            event->ignore();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QSize PipWindow::constrained_size(int content_width) const {
    const int w = std::max(kMinContentWidth, content_width);
    return QSize(w + 2 * kBorder, static_cast<int>(std::lround(w / kAspect)) + kHeader + kBorder);
}

void PipWindow::place_in_corner() {
    QScreen* display = source_ ? source_->screen() : QGuiApplication::primaryScreen();
    if (!display) { resize(constrained_size(480)); return; }
    const QRect area = display->availableGeometry();
    const int available_width = std::min(area.width() - 2 * kBorder - 32,
        static_cast<int>((area.height() - kHeader - kBorder - 32) * kAspect));
    resize(constrained_size(std::min(480, available_width)));
    move(area.right() - width() - 15, area.bottom() - height() - 15);
}

void PipWindow::layout_content() {
    if (content_) content_->setGeometry(kBorder, kHeader, width() - 2 * kBorder,
                                        height() - kHeader - kBorder);
    if (return_button_) return_button_->setGeometry(width() - 62, 5, 26, 24);
    if (close_button_) close_button_->setGeometry(width() - 32, 5, 26, 24);
}

void PipWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // Also constrain external/keyboard/WM resizes, not only mouse drags.
    const QSize wanted = constrained_size(width() - 2 * kBorder);
    if (wanted != size()) resize(wanted);
    layout_content();
}

void PipWindow::update_style() {
    const auto& colors = current_theme().colors;
    setStyleSheet(QStringLiteral(
        "QWidget#PipWindow { background: %1; }"
        "QWidget#PipWindow QToolButton { border: none; border-radius: 4px; }"
        "QWidget#PipWindow QToolButton:hover { background: %2; }")
        .arg(colors.surface_raised.name(), colors.surface_hover.name()));
    return_button_->setIcon(icons::icon(icons::Name::OpenExternal, colors.text, 16, devicePixelRatioF()));
    close_button_->setIcon(icons::icon(icons::Name::Close, colors.text, 16, devicePixelRatioF()));
    update();
}

void PipWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto& colors = current_theme().colors;
    painter.fillRect(rect(), colors.surface_raised);
    painter.setPen(colors.border_subtle);
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    painter.setPen(colors.text_muted);
    painter.drawText(QRect(12, 0, width() - 82, kHeader), Qt::AlignVCenter,
                     QStringLiteral("Picture in Picture"));
    // The controls sit in an inset overlay rail above the native browser:
    // ordinary Qt children drawn over a CEF child HWND are otherwise occluded.
    painter.drawLine(width() - 12, height() - 3, width() - 3, height() - 12);
    painter.drawLine(width() - 7, height() - 3, width() - 3, height() - 7);
}

Qt::Edges PipWindow::resize_edges(const QPoint& point) const {
    Qt::Edges edges;
    if (point.x() < kBorder) edges |= Qt::LeftEdge;
    if (point.x() >= width() - kBorder) edges |= Qt::RightEdge;
    if (point.y() < kBorder) edges |= Qt::TopEdge;
    if (point.y() >= height() - kBorder) edges |= Qt::BottomEdge;
    return edges;
}

void PipWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) { QWidget::mousePressEvent(event); return; }
    press_global_ = event->globalPosition().toPoint();
    press_geometry_ = geometry();
    edges_ = resize_edges(event->position().toPoint());
    dragging_ = !edges_ && event->position().y() < kHeader;
    // Let the compositor move tool windows where client-positioned moves
    // are restricted (notably Wayland), with a portable mouse-drag fallback.
    if (dragging_ && windowHandle() && windowHandle()->startSystemMove()) {
        dragging_ = false;
        event->accept();
        return;
    }
    if (dragging_ || edges_) { grabMouse(); event->accept(); }
}

void PipWindow::mouseMoveEvent(QMouseEvent* event) {
    const QPoint delta = event->globalPosition().toPoint() - press_global_;
    if (dragging_) {
        QPoint target = press_geometry_.topLeft() + delta;
        // Keep the header reachable while moving between displays.
        if (auto* display = QGuiApplication::screenAt(event->globalPosition().toPoint())) {
            const auto area = display->availableGeometry();
            target.setY(std::clamp(target.y(), area.top(), area.bottom() - kHeader));
        }
        move(target);
    } else if (edges_) {
        const int dx = edges_.testFlag(Qt::LeftEdge) ? -delta.x() : delta.x();
        const int dy = edges_.testFlag(Qt::TopEdge) ? -delta.y() : delta.y();
        const bool horizontal = edges_.testFlag(Qt::LeftEdge) || edges_.testFlag(Qt::RightEdge);
        const bool vertical = edges_.testFlag(Qt::TopEdge) || edges_.testFlag(Qt::BottomEdge);
        const bool use_height = vertical && (!horizontal || std::abs(dy * kAspect) > std::abs(dx));
        int content_width = press_geometry_.width() - 2 * kBorder +
                            (use_height ? static_cast<int>(std::lround(dy * kAspect)) : dx);
        if (auto* display = screen()) {
            const auto area = display->availableGeometry();
            const int max_width = std::min(area.width() - 2 * kBorder,
                static_cast<int>((area.height() - kHeader - kBorder) * kAspect));
            content_width = std::min(content_width, max_width);
        }
        const QSize wanted = constrained_size(content_width);
        QPoint corner = press_geometry_.topLeft();
        if (edges_.testFlag(Qt::LeftEdge)) corner.setX(press_geometry_.right() - wanted.width() + 1);
        if (edges_.testFlag(Qt::TopEdge)) corner.setY(press_geometry_.bottom() - wanted.height() + 1);
        setGeometry(QRect(corner, wanted));
    } else {
        const auto edges = resize_edges(event->position().toPoint());
        if ((edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) ||
            (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge))) setCursor(Qt::SizeFDiagCursor);
        else if ((edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::TopEdge)) ||
                 (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::BottomEdge))) setCursor(Qt::SizeBDiagCursor);
        else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) setCursor(Qt::SizeHorCursor);
        else if (edges) setCursor(Qt::SizeVerCursor);
        else setCursor(Qt::SizeAllCursor);
    }
    event->accept();
}

void PipWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        edges_ = {};
        if (mouseGrabber() == this) releaseMouse();
    }
    QWidget::mouseReleaseEvent(event);
}

void PipWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape ||
        (event->key() == Qt::Key_W && event->modifiers().testFlag(Qt::ControlModifier))) {
        close();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace aurora::ui
#endif // AURORA_HAS_CEF

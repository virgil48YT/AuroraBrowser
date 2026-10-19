#include "ui/TabBar.hpp"

#include <QEasingCurve>
#include <QApplication>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFont>
#include <QLayout>
#include <QLayoutItem>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QProxyStyle>
#include <QResizeEvent>
#include <QScreen>
#include <QStyleFactory>
#include <QStyleOptionTab>
#include <QTimer>
#include <QVariantAnimation>

#include "core/InternalUrl.hpp"
#include "ui/FaviconCache.hpp"
#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/TabContextMenu.hpp"
#include "ui/TabSpinner.hpp"
#include "ui/ThemeState.hpp"

#if defined(Q_OS_WIN)
// The native window-move loop can consume the button-up message before Qt
// updates QApplication::mouseButtons(). Query Windows directly while polling
// snap-back so tracking always ends with the physical gesture.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aurora::ui {

namespace {

constexpr char kAuroraTabMimeType[] = "application/x-aurora-tab";
constexpr int kTabCloseButtonPx = 20;
constexpr int kTabCloseGlyphPx = 12;
// Chrome: a tab may wander a few px vertically during an in-strip reorder.
// Crossing this slop in either vertical direction is an intentional tear-off;
// a tiny click-jitter never detaches.
constexpr int kTearOffThresholdPx = 14;
// Chrome's dock zone is slightly more generous than the painted strip so
// grazing the top/bottom edge still snaps back.
constexpr int kSnapBackOutsetPx = 6;
// A strip that refuses the tab is retried at a few Hz, never locked out.
constexpr int kSnapBackRetryMs = 120;
// Match MainWindow's minimum size; the follow window is also clamped to
// ~98% of the work-area so it never reads as maximized (Brave keeps a
// torn-off tab windowed, sized within the work area — never fullscreen).
constexpr int kDetachedMinWidth = 400;
constexpr int kDetachedMinHeight = 320;
constexpr qreal kDetachedMaxWorkAreaFrac = 0.98;

bool left_mouse_button_down() {
#if defined(Q_OS_WIN)
    return (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
    return QApplication::mouseButtons().testFlag(Qt::LeftButton);
#endif
}

// Global rect of the painted strip, outset so grazing the edge still docks.
QRect snap_zone_global(const TabBar* bar) {
    if (!bar) return {};
    QRect r(bar->mapToGlobal(QPoint(0, 0)), bar->size());
    return r.adjusted(-kSnapBackOutsetPx, -kSnapBackOutsetPx,
                      kSnapBackOutsetPx, kSnapBackOutsetPx);
}

bool snap_zone_contains(const TabBar* bar, const QPoint& global_pos) {
    return snap_zone_global(bar).contains(global_pos);
}

QString close_button_stylesheet() {
    const auto& c = current_theme().colors;
    const auto rgba = [](QColor color, int alpha) {
        color.setAlpha(alpha);
        return QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(color.alphaF(), 0, 'f', 2);
    };
    return QStringLiteral(
               "QToolButton#TabCloseButton {"
               " min-width:%1px; max-width:%1px;"
               " min-height:%1px; max-height:%1px;"
               " margin:0px; padding:0px; border:none;"
               " border-radius:%2px; background:transparent;"
               "}"
               "QToolButton#TabCloseButton:hover { background:%3; }"
               "QToolButton#TabCloseButton:pressed { background:%4; }")
        .arg(kTabCloseButtonPx)
        .arg(kTabCloseButtonPx / 2)
        .arg(rgba(c.danger, 40), rgba(c.danger, 72));
}

// ---------------------------------------------------------------------------
// TabStripStyle — all tab painting. A QProxyStyle (base: Fusion) installed on
// the TabBar; it is consulted for the tab grab pixmap during drag-reorder, so
// a dragged tab is pixel-identical to a resting one.
//
// Notes on the base-style ownership and the (intentional) QSS interaction:
//  * QProxyStyle takes ownership of its base style, so it is constructed with
//    a fresh Fusion instance — never a shared one.
//  * Installing a per-widget style detaches that widget from the application
//    stylesheet (children such as the scroll buttons and the per-tab close
//    buttons keep it, which is what we want).
//  * Because of that, QTabBar's QSS-driven `qproperty-drawBase` would do
//    nothing; TabBar::TabBar calls setDrawBase(false) in code instead, and
//    the strip's background comes entirely from the #TabStrip stylesheet
//    rule on the parent widget.
// ---------------------------------------------------------------------------
class TabStripStyle : public QProxyStyle {
public:
    explicit TabStripStyle(TabBar* owner)
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))),
          owner_(owner) {
        // QWidget::setStyle does NOT take ownership — parent the style to
        // the bar so it is not leaked.
        setParent(owner);
    }

    void drawControl(ControlElement element, const QStyleOption* option,
                     QPainter* painter, const QWidget* widget) const override {
        if (element != CE_TabBarTab || !owner_) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        const auto* opt = qstyleoption_cast<const QStyleOptionTab*>(option);
        auto* bar = qobject_cast<const TabBar*>(widget);
        if (!opt || !bar) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }

        const Theme& theme = current_theme();
        const ThemeColors& c = theme.colors;
        const int idx = opt->tabIndex;
        const bool selected = opt->state.testFlag(State_Selected);
        const QRectF r(opt->rect);

        // Hover-share animation per tab index. Possession (underMouse vs the
        // strip's own hover tracking) is not the deciding factor: the bar's
        // hovered_index tells us where the pointer actually is.
        const int hovered = bar->hovered_index();
        auto* prog = hover_prog_[idx];
        if (!prog) {
            prog = new QVariantAnimation(const_cast<TabStripStyle*>(this));
            prog->setStartValue(0.0);
            prog->setEasingCurve(QEasingCurve::OutCubic);
            hover_prog_[idx] = prog;
        }
        const qreal target = (idx == hovered && !selected) ? 1.0 : 0.0;
        const qreal cur = prog->currentValue().toReal();
        if (prog->state() == QAbstractAnimation::Stopped && cur != target) {
            prog->stop();
            prog->setStartValue(cur);
            prog->setEndValue(target);
            prog->setDuration(cur < target ? Motion::base : Motion::fast);
            prog->start();
            // Keep repainting while the value moves.
            QObject::connect(prog, &QVariantAnimation::valueChanged,
                             owner_, [this]() { owner_->update(); },
                             Qt::UniqueConnection);
        }
        const qreal h = selected ? 0.0 : prog->currentValue().toReal();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        // Aurora glass, per the mockup: tabs are separate rounded tiles
        // floating on the strip with a real gap between neighbours (the 4px
        // horizontal inset on each side adds up to an 8px gutter). The active
        // pill is a translucent wash the strip shows through, outlined with
        // the nebula purple; inactive tabs are fully transparent until
        // hovered, and the gap alone separates them — no dividers.
        const QRectF pill = r.adjusted(4, 4, -4, -4);
        // Softly rounded rectangles, not stadiums: the settings/downloads
        // mockups give the tabs a small corner radius (the full
        // height/2 capsule read as too round next to the barely-rounded
        // omnibox). Clamped so a very short tab still draws a valid rect.
        const qreal tab_radius =
            qMin<qreal>(Radius::lg, pill.height() / 2.0);
        if (selected) {
            // A faint light wash over the dark strip (a faint dark wash over
            // the light strip) instead of an opaque plate, so the pill reads
            // as lit glass rather than a solid chip.
            QColor fill = theme.is_dark ? QColor(255, 255, 255)
                                        : QColor(15, 23, 42);
            fill.setAlpha(theme.is_dark ? 22 : 26);
            if (bar->property("tor").toBool()) fill = tor_tint(fill);
            else if (bar->property("incognito").toBool()) fill = incognito_tint(fill);
            painter->setBrush(fill);
            QColor edge = c.accent2;
            edge.setAlpha(90);
            painter->setPen(QPen(edge, 1.0));
            painter->drawRoundedRect(pill, tab_radius, tab_radius);
        } else if (h > 0.02) {
            // Hover lift: a subtly raised pill, fully inside the tab.
            QColor fill = c.tab_hover_bg;
            fill.setAlphaF(fill.alphaF() * static_cast<float>(h));
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(pill, tab_radius, tab_radius);
        }

        // ---- Contents -----------------------------------------------------
        if (!selected && owner_->tab_suspended(idx)) painter->setOpacity(0.55);
        const bool loading = owner_->tab_loading(idx);
        const bool pinned = owner_->tab_pinned(idx);
        const bool compact = pinned || r.width() < 96.0;

        QFont font = owner_->font();
        font.setPixelSize(12);  // tab titles read one step below body text
        font.setWeight(selected ? QFont::Medium : QFont::Normal);
        painter->setFont(font);
        const QFontMetrics fm(font);

        qreal x = r.left() + 10.0;
        const qreal rightpad = compact ? 8.0 : 30.0; // reserve for close btn
        const qreal max_right = r.right() - rightpad;
        const qreal cy = r.center().y();

        if (loading) {
            // The spinner widget already sits in the left slot; leave it room
            // and do not paint the favicon on top of it.
            x = r.left() + 30.0;
        } else if (!opt->icon.isNull()) {
            constexpr qreal kIcon = 16.0;
            opt->icon.paint(painter,
                            QRectF(x, cy - kIcon / 2.0, kIcon, kIcon).toRect(),
                            Qt::AlignCenter);
            x += kIcon + (compact ? 0.0 : 7.0);
        } else if (!compact) {
            // No icon yet: a single faint dot so the row doesn't jump when
            // the favicon lands.
            QColor dot = c.text_faint;
            dot.setAlpha(90);
            painter->setPen(Qt::NoPen);
            painter->setBrush(dot);
            painter->drawEllipse(QPointF(x + 2, cy), 2, 2);
            x += 11.0;
        }

        if (!compact && !opt->text.isEmpty()) {
            const QString elided = fm.elidedText(
                opt->text, Qt::ElideRight, qMax(0, int(max_right - x)));
            painter->setPen(selected ? c.text : c.text_muted);
            const QRectF tr(x, r.top(), qMax(0.0, max_right - x), r.height());
            painter->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, elided);
        }
        painter->restore();
    }

    QRect subElementRect(SubElement element, const QStyleOption* option,
                         const QWidget* widget) const override {
        const auto* opt = qstyleoption_cast<const QStyleOptionTab*>(option);
        if (!opt) return QProxyStyle::subElementRect(element, option, widget);
        const QRect r = opt->rect;
        if (element == SE_TabBarTabLeftButton) {
            // Loading spinner slot: centered where the favicon would sit.
            return QRect(r.left() + 8, r.center().y() - 8, 16, 16);
        }
        if (element == SE_TabBarTabRightButton) {
            // Place the close button with enough room from the tab's right
            // edge so its hover background (rounded square) is never clipped.
            // The hit target and the layout slot are identical.
            return QRect(r.right() - 6 - kTabCloseButtonPx,
                         r.center().y() - kTabCloseButtonPx / 2,
                         kTabCloseButtonPx, kTabCloseButtonPx);
        }
        return QProxyStyle::subElementRect(element, option, widget);
    }

private:
    TabBar* owner_;
    mutable QHash<int, QVariantAnimation*> hover_prog_;
};

} // namespace

TabBar::TabBar(core::TabManager* manager, QWidget* parent)
    : QTabBar(parent), manager_(manager) {
    setMovable(true);
    setTabsClosable(false);  // custom close buttons (RightSide) per tab
    setElideMode(Qt::ElideRight);
    setUsesScrollButtons(true);
    setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);
    setMouseTracking(true);
    setDocumentMode(true);
    setExpanding(false);
    // The strip paints its own background (#TabStrip), the tabs paint
    // themselves via TabStripStyle — QTabBar's style-sheet base fill is off.
    setAutoFillBackground(false);
    setDrawBase(false);
    // Parented to this bar inside the ctor; a per-widget style also detaches
    // the bar from the application sheet, which is why the tabs paint here.
    setStyle(new TabStripStyle(this));
    setAttribute(Qt::WA_Hover, true);
    setAcceptDrops(true);
    // 16 ms ≈ one frame: the detached window's follow and the snap-back
    // target detection both run off this single timer (see poll_detached_drag).
    // Hover card: shown once the pointer has rested on a tab for ~2 seconds.
    hover_card_timer_.setSingleShot(true);
    hover_card_timer_.setInterval(2000);
    connect(&hover_card_timer_, &QTimer::timeout, this, [this] {
        if (hovered_tab_ >= 0 && drag_state_ == DragState::Idle &&
            QApplication::mouseButtons() == Qt::NoButton) {
            emit hover_dwell(hovered_tab_);
        }
    });
    snap_back_timer_.setInterval(16);
    snap_back_timer_.setTimerType(Qt::PreciseTimer);
    connect(&snap_back_timer_, &QTimer::timeout, this,
            &TabBar::poll_detached_drag);
    manager_->add_observer(this);
    // Adaptive tab widths follow the STRIP's width, not ours (our own width
    // is the result of the hint, so watching it would be circular).
    if (parentWidget()) parentWidget()->installEventFilter(this);
    // Favicons are shared with the history and bookmarks pages, so a site's
    // icon is fetched once and every surface shows the same image.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString& host) {
                const auto& tabs = manager_->tabs();
                for (size_t i = 0; i < tabs.size(); ++i) {
                    const auto url = QString::fromStdString(tabs[i].url.display());
                    if (FaviconCache::host_of(url) == host) {
                        refresh_tab(static_cast<int>(i));
                    }
                }
            });
    refresh_all();

    connect(this, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (index >= 0 && static_cast<size_t>(index) < manager_->count()) {
            request_close(manager_->tabs()[static_cast<size_t>(index)].id);
        }
    });
    connect(this, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from >= 0 && static_cast<size_t>(from) < manager_->count()) {
            manager_->move(manager_->tabs()[static_cast<size_t>(from)].id,
                          static_cast<size_t>(to));
        }
    });
    connect(this, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && static_cast<size_t>(index) < manager_->count()) {
            manager_->set_active(manager_->tabs()[static_cast<size_t>(index)].id);
        }
        update_close_visibility();   // close affordance follows selection
        update();
    });
}

bool TabBar::tab_loading(int index) const {
    const auto& tabs = manager_->tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return false;
    return tabs[static_cast<size_t>(index)].loading;
}

bool TabBar::tab_pinned(int index) const {
    const auto& tabs = manager_->tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return false;
    return tabs[static_cast<size_t>(index)].pinned;
}

bool TabBar::tab_suspended(int index) const {
    const auto& tabs = manager_->tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return false;
    return tabs[static_cast<size_t>(index)].suspended;
}

int TabBar::strip_available_width() const {
    // How much room the strip row can actually give the tabs: the row's width
    // minus the chrome that shares it (tab search, new-tab, window controls)
    // and the layout's margins/spacing.
    //
    // This must NOT be derived from OUR OWN width: the row's layout sizes the
    // bar from these very hints (QSizePolicy::Maximum), so the old
    // width()-based value fed back into the hints and the strip collapsed by
    // (12px / tab count) on EVERY relayout. Any tab text/icon/geometry change
    // triggers a relayout, so within seconds of normal browsing every tab sat
    // at the 76px floor and lost its text — the "tabs shrink to icons with
    // only 2 tabs open" bug. Deriving from the parent strip's width breaks
    // the loop: the strip's width comes from the window, not from our hints.
    const QWidget* strip = parentWidget();
    if (!strip || strip->width() <= 0) return 640;  // not laid out yet
    const auto* lay = strip->layout();
    if (!lay) {
        return qMax(strip->width() - 12, Metrics::tab_min_width);
    }
    const auto m = lay->contentsMargins();
    int avail = strip->width() - m.left() - m.right();
    int items = 0;
    for (int i = 0; i < lay->count(); ++i) {
        const auto* item = lay->itemAt(i);
        if (!item) continue;
        ++items;
        const auto* w = item->widget();
        if (!w || w == this) continue;  // stretches take the leftover slack
        if (w->isHidden()) continue;
        // A sibling that has not been laid out yet reports width 0; its size
        // hint (all strip chrome is fixed-size) is the correct reservation.
        avail -= qMax(w->width(), w->sizeHint().width());
    }
    if (items > 1) avail -= qMax(lay->spacing(), 0) * (items - 1);
    return qMax(avail, Metrics::tab_min_width);
}

QSize TabBar::sizeHint() const {
    int width = 0;
    for (int i = 0; i < count(); ++i) width += tabSizeHint(i).width();
    return {width, Metrics::tab_height};
}

QSize TabBar::tabSizeHint(int index) const {
    if (tab_pinned(index)) {
        // Compact pinned tab: favicon/spinner only, no text or close button.
        return {Metrics::tab_pinned_width, Metrics::tab_height};
    }
    int pinned_count = 0;
    int normal_count = 0;
    const auto& tabs = manager_->tabs();
    for (const auto& t : tabs) (t.pinned ? pinned_count : normal_count)++;
    if (normal_count < 1) normal_count = 1;
    const int avail =
        strip_available_width() - pinned_count * Metrics::tab_pinned_width;
    const int w = qBound(Metrics::tab_min_width, avail / normal_count,
                         Metrics::tab_max_width);
    return {w, Metrics::tab_height};
}

void TabBar::update_close_visibility() {
    for (auto it = close_btns_.begin(); it != close_btns_.end(); ++it) {
        const int idx = index_of(it.key());
        auto* btn = it.value();
        if (!btn || idx < 0) continue;
        if (tab_pinned(idx)) {
            btn->hide();  // pinned tabs have no close affordance
            continue;
        }
        btn->setVisible(idx == currentIndex() || idx == hovered_tab_);
    }
}

void TabBar::mouseMoveEvent(QMouseEvent* event) {
    if ((drag_state_ == DragState::TearOffPending ||
         drag_state_ == DragState::Reordering) &&
        pressed_tab_.valid() && event->buttons().testFlag(Qt::LeftButton)) {
        const bool drag_started =
            (event->pos() - drag_start_pos_).manhattanLength() >=
            QApplication::startDragDistance();
        if (drag_started) {
            const QWidget* strip = parentWidget();
            const QPoint global = event->globalPosition().toPoint();
            const QRect strip_rect =
                strip ? QRect(strip->mapToGlobal(QPoint(0, 0)), strip->size())
                      : QRect(mapToGlobal(QPoint(0, 0)), size());

            // Chrome: horizontal drag inside the strip is an in-place reorder
            // (QTabBar's grab-pixmap path, painted by TabStripStyle so the
            // dragged tab is pixel-identical to a resting one). Crossing the
            // vertical slop, or leaving the window entirely, tears the tab off
            // into a real top-level window that follows the cursor. QDrag is
            // not used for originating tear-off: its nested loop consumes the
            // mouse-up and cannot drive live window-follow / snap-back.
            const bool past_vertical =
                global.y() > strip_rect.bottom() + kTearOffThresholdPx ||
                global.y() < strip_rect.top() - kTearOffThresholdPx;
            const QWidget* top = window();
            const bool outside_window =
                top && !top->frameGeometry().contains(global);
           if ((past_vertical || outside_window) && manager_ &&
                manager_->count() > 1) {
                begin_immediate_tear_off(pressed_tab_, global);
                return;
            }
            if (drag_state_ == DragState::TearOffPending) {
                drag_state_ = DragState::Reordering;
            }
        }
    }
    QTabBar::mouseMoveEvent(event);
    const int idx = tabAt(event->pos());
    if (idx != hovered_tab_) {
        hovered_tab_ = idx;
        update_close_visibility();
        update();
        // A new tab under the pointer: hide the old card and start the wait.
        hover_card_timer_.stop();
        if (idx >= 0 && drag_state_ == DragState::Idle &&
            event->buttons() == Qt::NoButton) {
            if (hover_card_visible_) emit hover_dwell(idx);
            else hover_card_timer_.start();
        } else {
            emit hover_dwell_cancelled();
        }
    }
}

void TabBar::mouseReleaseEvent(QMouseEvent* event) {
    QTabBar::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton) return;
    pressed_tab_ = core::TabId(0);
    if (drag_state_ == DragState::Detached) {
        // The destination bar usually never saw the original press (it was
        // created mid-gesture), so mouse-up is detected by poll_detached_drag
        // via left_mouse_button_down(). Handle it here too if we do get it.
        finish_detached_drag(event->globalPosition().toPoint());
        return;
    }
    if (drag_state_ != DragState::Reattaching) {
        drag_state_ = DragState::Idle;
        drag_grab_offset_ = QPoint();
    }
}

void TabBar::begin_immediate_tear_off(core::TabId id,
                                      const QPoint& global_pos) {
    if ((drag_state_ != DragState::TearOffPending &&
         drag_state_ != DragState::Reordering) ||
        !manager_ || !manager_->contains(id)) {
        return;
    }
 if (manager_->count() <= 1) return;
    // One window per gesture: mark Detached before the synchronous transfer
    // rebuilds both bars and can re-enter event processing.
    drag_state_ = DragState::Detached;

    // Cancel QTabBar's private movable-tab gesture before the model transfer
    // removes the tab from under it. Any horizontal moves already emitted via
    // tabMoved are retained in the model.
    setMovable(false);
    setMovable(true);
    releaseMouse();

    pressed_tab_ = core::TabId(0);
    // Keep the tab-local grab point across the synchronous transfer: the
    // destination bar re-anchors the cursor to this exact point once the
    // detached window's final geometry is known.
    const QPoint grab_offset = drag_grab_offset_;
    emit tab_tear_off_requested(id, global_pos, grab_offset);
    if (manager_->contains(id)) {
        // The owner rejected the transfer; leave the source usable rather
        // than trying to move it as though a detached destination existed.
        drag_state_ = DragState::Idle;
        return;
    }

    // MainWindow handles the signal synchronously and shows the destination
    // window before returning. Hand the still-held pointer gesture to that
    // top-level window so its 16 ms timer can follow the cursor and dock into
    // any visible Aurora strip.
    continue_dragging_detached_window(id, this, grab_offset);

    // The newly created window now owns the live gesture. The origin may stay
    // open when other tabs remain, so do not leave its independent state
    // machine parked in Detached after the hand-off.
    drag_state_ = DragState::Idle;
    drag_grab_offset_ = QPoint();
}

void TabBar::continue_dragging_detached_window(core::TabId id,
                                               TabBar* origin,
                                               const QPoint& grab_offset) {
    if (!origin) return;

    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (!top || !top->isVisible() || top->isMinimized()) continue;
        if (auto* top_bar = qobject_cast<TabBar*>(top);
            top_bar && top_bar->manager_ && top_bar->manager_->contains(id)) {
            top_bar->begin_detached_drag_tracking(id, origin, grab_offset);
            return;
        }
        const auto bars = top->findChildren<TabBar*>();
        for (TabBar* bar : bars) {
            if (!bar || !bar->isVisibleTo(top) || !bar->manager_ ||
                !bar->manager_->contains(id) || !bar->window()) {
                continue;
            }

            // Do not hand this gesture to startSystemMove(). On Windows that
            // enters a native modal move loop, where delivery of the Qt timer
            // used for snap-back is not guaranteed. The same 16 ms timer moves
            // the window from QCursor::pos() and checks docking targets, so
            // both operations remain in Qt's normal event loop.
            bar->begin_detached_drag_tracking(id, origin, grab_offset);
            return;
        }
    }
}

void TabBar::prepare_detached_follow_window(QWidget* detached_window) {
    if (!detached_window) return;

    // Chrome un-maximizes on tear-off so the follow window is a floating
    // rectangle, never a full-screen/maximized sheet. Brave keeps the
    // torn-off window windowed after the drop, too.
    if (detached_window->isFullScreen() || detached_window->isMaximized()) {
        detached_window->showNormal();
    }

    QScreen* screen = detached_window->screen();
    if (!screen) screen = QApplication::screenAt(QCursor::pos());
    if (!screen) screen = QApplication::primaryScreen();
    if (!screen) return;

    const QRect work = screen->availableGeometry();
    const int max_w = qMax(kDetachedMinWidth,
                           static_cast<int>(work.width() * kDetachedMaxWorkAreaFrac));
    const int max_h = qMax(kDetachedMinHeight,
                           static_cast<int>(work.height() * kDetachedMaxWorkAreaFrac));
    const QSize sz(qBound(kDetachedMinWidth, detached_window->width(), max_w),
                   qBound(kDetachedMinHeight, detached_window->height(), max_h));
    if (sz != detached_window->size()) detached_window->resize(sz);
}

void TabBar::begin_detached_drag_tracking(core::TabId id, TabBar* origin,
                                          const QPoint& grab_offset) {
    if (!origin || origin == this || !manager_ || !manager_->contains(id)) {
        return;
    }

    // A duplicate hand-off must not create a second timer-driven transfer.
    // A stale Detached run can be reset safely; Reattaching is synchronous
    // and must be allowed to finish without changing any of its inputs.
    if (drag_state_ == DragState::Reattaching) return;
    if (drag_state_ == DragState::Detached) stop_detached_drag_tracking();
    if (drag_state_ != DragState::Idle) return;

    QWidget* detached_window = window();
    if (!detached_window) return;

    // Settle the FINAL follow-window geometry first (un-maximize + work-area
    // clamp). The old order captured the window-relative offset BEFORE this
    // call could resize the window, so the offset went stale the moment the
    // resize landed and the window jumped under the cursor — the tear-off
    // mouse-offset bug. tear_off_tab() pre-clamps, so this is usually a no-op.
    prepare_detached_follow_window(detached_window);

    const QPoint cursor = QCursor::pos();
     const QPoint frame_offset =
        detached_window->mapToGlobal(QPoint(0, 0)) - detached_window->pos();

    // Re-anchor the cursor to the same tab-local point now that the final
    // geometry is known. The detached window's tab can be narrower than the
    // source tab was (adaptive widths after the clamp), so the grab point is
    // clamped into the tab rect — Chrome keeps the cursor inside the dragged
    // tab the same way.
    const int idx = index_of(id);
    if (idx >= 0) {
        const QRect tab_rect = tabRect(idx);
        if (tab_rect.isValid()) {
            const QPoint grab(
                qBound(0, grab_offset.x(), qMax(0, tab_rect.width() - 1)),
                qBound(0, grab_offset.y(), qMax(0, tab_rect.height() - 1)));
            const QPoint grab_global =
                mapToGlobal(tab_rect.topLeft() + grab) - frame_offset;
            detached_window->move(detached_window->pos() +
                                  (cursor - grab_global));
        }
    }

    // Capture the follow offset only AFTER the final geometry and the anchor
    // move, so the 16 ms polling loop preserves exactly the anchored point.
    // Anything captured earlier would be stale the moment the window resized.
    detached_window_offset_ =
        cursor - detached_window->mapToGlobal(QPoint(0, 0));
    detached_window->raise();

    detached_tab_ = id;
    snap_back_target_.clear();
    snap_retry_clock_.invalidate();
    drag_state_ = DragState::Detached;

    if (!left_mouse_button_down()) {
        // Release raced with window creation: treat as a drop at the cursor.
        finish_detached_drag(cursor);
        return;
    }

    snap_back_timer_.start();
    // Do not wait one interval for the first follow after MainWindow has
    // handed the still-held gesture to the newly shown detached window.
    poll_detached_drag();
}

void TabBar::stop_detached_drag_tracking() {
    snap_back_timer_.stop();
    snap_back_target_.clear();
    detached_tab_ = core::TabId(0);
    detached_window_offset_ = QPoint();
    snap_retry_clock_.invalidate();
    if (drag_state_ == DragState::Detached ||
        drag_state_ == DragState::Reattaching) {
        pressed_tab_ = core::TabId(0);
        drag_state_ = DragState::Idle;
    }
}

int TabBar::insertion_index_at(const QPoint& pos) const {
    int insertion = tabAt(pos);
    if (insertion < 0) {
        if (count() <= 0) return 0;
        // Outset-zone / trailing-strip hits have no tab under the cursor.
        // Dock to the left of the first tab or after the last one.
        return pos.x() <= tabRect(0).center().x() ? 0 : count();
    }
    if (pos.x() > tabRect(insertion).center().x()) ++insertion;
    return insertion;
}

TabBar* TabBar::snap_back_target_at(const QPoint& global_pos) const {
    const QWidget* detached_window = window();

    const auto accepts_cursor = [this, detached_window,
                                 &global_pos](TabBar* target) {
        if (!target || target == this || !target->manager_ ||
            target->manager_->contains(detached_tab_)) {
            return false;
        }

        QWidget* target_window = target->window();
        if (!target_window || target_window == detached_window ||
            !target_window->isVisible() || target_window->isMinimized() ||
            !target->isVisibleTo(target_window) || target->width() <= 0 ||
            target->height() <= 0) {
            return false;
        }

        return snap_zone_contains(target, global_pos);
    };

    // Keep the current candidate while the cursor remains in its (outset)
    // zone. Qt documents topLevelWidgets() as unordered, so this also keeps
    // overlapping window geometries from flipping the candidate every tick.
    if (accepts_cursor(snap_back_target_.data())) {
        return snap_back_target_.data();
    }

    // Geometry is checked explicitly instead of QWidget::widgetAt(): the
    // detached window follows the pointer and is therefore normally the
    // top-most widget at the cursor, even when an Aurora strip is beneath it.
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (!top || top == detached_window || !top->isVisible() ||
            top->isMinimized()) {
            continue;
        }

        if (auto* top_bar = qobject_cast<TabBar*>(top);
            accepts_cursor(top_bar)) {
            return top_bar;
        }
        const auto bars = top->findChildren<TabBar*>();
        for (TabBar* target : bars) {
            if (accepts_cursor(target)) return target;
        }
    }
    return nullptr;
}

bool TabBar::accept_snap_back(TabBar* source, core::TabId id,
                              const QPoint& global_pos) {
    if (!source || source == this || !source->manager_ ||
        source->drag_state_ != DragState::Reattaching ||
        source->detached_tab_ != id ||
        source->snap_back_target_.data() != this ||
        !source->manager_->contains(id) || !manager_ || manager_->contains(id)) {
        return false;
    }

    if (!isVisible() || !snap_zone_contains(this, global_pos)) return false;

    // Same signal as a QDrag drop. MainWindow's existing handler performs the
    // complete release/adopt transaction, including closing the source CEF
    // host before creating a destination-window host and deferring deletion
    // of the now-empty detached window.
    emit external_tab_dropped(source, id,
                              insertion_index_at(mapFromGlobal(global_pos)));
    return manager_->contains(id) && !source->manager_->contains(id);
}

bool TabBar::try_reattach_to(TabBar* target, const QPoint& global_pos) {
    if (!target || drag_state_ != DragState::Detached) return false;
    const core::TabId id = detached_tab_;
    if (!id.valid() || !manager_ || !manager_->contains(id)) {
        stop_detached_drag_tracking();
        return false;
    }

    snap_back_target_ = target;
    drag_state_ = DragState::Reattaching;
    snap_retry_clock_.restart();

    if (target->accept_snap_back(this, id, global_pos)) {
        // Chrome: the tab docks into the target strip and that window is
        // activated. MainWindow closes the now-empty detached window.
        if (QWidget* dest = target->window()) {
            dest->raise();
            dest->activateWindow();
        }
        stop_detached_drag_tracking();
        return true;
    }

    // Failed adoption is rolled back by MainWindow::transfer_tab_to(). Keep
    // following the intact detached tab; the retry clock gates the next try.
    if (drag_state_ != DragState::Reattaching) return false;
    if (manager_ && manager_->contains(id)) {
        drag_state_ = DragState::Detached;
        return false;
    }
    stop_detached_drag_tracking();
    return false;
}

void TabBar::finish_detached_drag(const QPoint& global_pos) {
    if (drag_state_ != DragState::Detached) {
        stop_detached_drag_tracking();
        return;
    }

    // Chrome: mouse-up over ANY open window's tab strip (not just the source)
    // re-attaches at the drop index, destroys the follow window, and
    // activates the target. Mouse-up over empty space keeps the torn-off
    // window exactly where it was dropped — windowed, never fullscreen.
    if (TabBar* target = snap_back_target_at(global_pos)) {
        if (try_reattach_to(target, global_pos)) return;
    }

    if (QWidget* w = window()) {
        w->raise();
        w->activateWindow();
    }
    stop_detached_drag_tracking();
}

void TabBar::poll_detached_drag() {
    // A nested event loop entered by the synchronous transfer must not let a
    // second timer delivery tear down or restart the in-flight transaction.
    if (drag_state_ == DragState::Reattaching) return;

    if (drag_state_ != DragState::Detached || !detached_tab_.valid() ||
        !manager_ || !manager_->contains(detached_tab_)) {
        stop_detached_drag_tracking();
        return;
    }

    const QPoint cursor = QCursor::pos();

    // Keep the same point in the torn-off tab under the cursor. This manual
    // move deliberately shares the timer with target detection: unlike a
    // platform move loop, neither half can starve the other on Windows. It
    // also works across monitors: QCursor::pos() is in virtual-desktop
    // coordinates, so the follow window crosses screens with the pointer.
    if (QWidget* detached_window = window()) {
        const QPoint frame_offset =
            detached_window->mapToGlobal(QPoint(0, 0)) -
            detached_window->pos();
        const QPoint desired_pos =
            cursor - detached_window_offset_ - frame_offset;
        if (detached_window->pos() != desired_pos) {
            detached_window->move(desired_pos);
        }
        detached_window->raise();
    } else {
        stop_detached_drag_tracking();
        return;
    }

    // The destination bar never received the original press, so mouse-up is
    // detected here (GetAsyncKeyState on Win). Parked windows must not dock
    // on a later unrelated pointer movement, so tracking always ends with
    // the physical gesture.
    if (!left_mouse_button_down()) {
        finish_detached_drag(cursor);
        return;
    }

    TabBar* target = snap_back_target_at(cursor);
    if (!target) {
        snap_back_target_.clear();
        snap_retry_clock_.invalidate();
        return;
    }

    if (target != snap_back_target_.data()) {
        snap_back_target_ = target;
        snap_retry_clock_.invalidate();  // try immediately on entering a strip
    }

    // Retry a refusing target at a few Hz while the cursor stays over it,
    // never locked out. Do not hammer every 16 ms tick.
    if (snap_retry_clock_.isValid() &&
        snap_retry_clock_.elapsed() < kSnapBackRetryMs) {
        return;
    }
    try_reattach_to(target, cursor);
}

void TabBar::dragEnterEvent(QDragEnterEvent* event) {
    auto* source = qobject_cast<TabBar*>(event->source());
    if (source && source->manager_ &&
        event->mimeData()->hasFormat(kAuroraTabMimeType)) {
        bool ok = false;
        const auto raw = event->mimeData()->data(kAuroraTabMimeType);
        const auto value = raw.toULongLong(&ok);
        const core::TabId id(static_cast<uint64_t>(value));
        if (ok && source->manager_->contains(id)) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
            return;
        }
    }
    event->ignore();
}

void TabBar::dragMoveEvent(QDragMoveEvent* event) {
    auto* source = qobject_cast<TabBar*>(event->source());
    if (source && event->mimeData()->hasFormat(kAuroraTabMimeType)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    event->ignore();
}

void TabBar::dropEvent(QDropEvent* event) {
    // Receive path for a Qt drag from another Aurora strip. Tear-off itself
    // is a live window-follow (not QDrag); this remains so a drop on this bar
    // can still adopt a tab without going through the detached-window tracker.
    auto* source = qobject_cast<TabBar*>(event->source());
    if (!source || !event->mimeData()->hasFormat(kAuroraTabMimeType)) {
        event->ignore();
        return;
    }

    bool ok = false;
    const auto raw = event->mimeData()->data(kAuroraTabMimeType);
    const auto value = raw.toULongLong(&ok);
    const core::TabId id(static_cast<uint64_t>(value));
    if (!ok || !source->manager_ || !source->manager_->contains(id)) {
        event->ignore();
        return;
    }

    int insertion = insertion_index_at(event->position().toPoint());

    if (source == this) {
        // A tab dragged vertically away and then returned to its own strip is
        // still just a reorder; do not detach/recreate its web view.
        const int from = index_of(id);
        if (from >= 0 && count() > 0) {
            if (from < insertion) --insertion;
            insertion = qBound(0, insertion, count() - 1);
            if (from != insertion) moveTab(from, insertion);
        }
    } else {
        emit external_tab_dropped(source, id, insertion);
    }

    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TabBar::leaveEvent(QEvent* event) {
    QTabBar::leaveEvent(event);
    hover_card_timer_.stop();
    emit hover_dwell_cancelled();
    if (hovered_tab_ != -1) {
        hovered_tab_ = -1;
        update_close_visibility();
        update();
    }
}

bool TabBar::eventFilter(QObject* obj, QEvent* e) {
    // Tab placement reserves chrome to the left of the strip (tab search) and
    // right of the strip (new-tab, window controls); when that chrome resizes
    // with the window, the adaptive widths must be re-derived.
    if (obj == parentWidget() && e->type() == QEvent::Resize) {
        updateGeometry();
        update();
    }
    return QTabBar::eventFilter(obj, e);
}

void TabBar::set_theme_accent(const QColor& accent) {
    accent_ = accent;
    for (auto it = spinners_.begin(); it != spinners_.end(); ++it) {
        if (*it) (*it)->setAccent(accent);
    }
    for (auto it = close_btns_.begin(); it != close_btns_.end(); ++it) {
        if (*it) (*it)->setStyleSheet(close_button_stylesheet());
    }
}

void TabBar::refresh_all() {
    // Rebuild the bar from the model. O(n) but only on structural changes.
    blockSignals(true);
    // Spinners are owned by the tab bar and freed when tabs are removed; clear
    // our map so we don't hold dangling pointers after the rebuild.
    spinners_.clear();
    close_btns_.clear();
    // Drop retained icons of tabs that no longer exist.
    for (auto it = last_icons_.begin(); it != last_icons_.end();) {
        if (manager_->contains(it.key())) ++it;
        else it = last_icons_.erase(it);
    }
    while (count() > 0) removeTab(0);
    for (const auto& tab : manager_->tabs()) {
        int idx = addTab(QString::fromStdString(tab.title.empty() ? "New Tab" : tab.title));
        auto* spinner = new TabSpinner(this);
        spinner->setAccent(accent_.isValid() ? accent_ : current_theme().colors.accent);
        spinner->setFixedSize(16, 16);
        spinners_.insert(tab.id, spinner);
        setTabButton(idx, QTabBar::LeftSide, spinner);
        set_loading(tab.id, tab.loading);

        auto* close = new IconButton(icons::Name::Close, this);
        close->setObjectName("TabCloseButton");
        close->setFixedSize(kTabCloseButtonPx, kTabCloseButtonPx);
        close->setIconPx(kTabCloseGlyphPx);
        // Override the broad QTabBar QToolButton box metrics locally. This
        // preserves a centered, unclipped X regardless of the application QSS.
        close->setStyleSheet(close_button_stylesheet());
        close->setToolTip("Close tab");
        const core::TabId tid = tab.id;
        // Resolve the tab by identity, never by index: indices shift when tabs
        // are added, closed or dragged. Deferred so the click event finishes
        // unwinding before the tab (and this very button) is destroyed.
        connect(close, &IconButton::clicked, this, [this, tid] {
            request_close(tid);
        });
        close_btns_.insert(tab.id, close);
        setTabButton(idx, QTabBar::RightSide, close);

        // Pin state, mute indicator and favicon come from the model.
        refresh_tab(idx);
    }
    int active = index_of(manager_->active());
    if (active >= 0) setCurrentIndex(active);
    blockSignals(false);
    update_close_visibility();
}

void TabBar::set_loading(core::TabId id, bool loading) {
    auto it = spinners_.find(id);
    if (it == spinners_.end()) return;
    TabSpinner* s = it.value();
    if (!s) return;
    if (loading) {
        s->start();
    } else {
        s->stop();
    }
}

void TabBar::refresh_tab(int idx) {
    const auto& tabs = manager_->tabs();
    if (idx < 0 || static_cast<size_t>(idx) >= tabs.size()) return;
    const auto& tab = tabs[static_cast<size_t>(idx)];

    // Pinned tabs render as compact icon-only tabs (fixed width, no text, no
    // close button) — the reference layout. Everyone else keeps their title.
    if (tab.pinned) {
        setTabText(idx, QString());
    } else {
        setTabText(idx, QString::fromStdString(tab.title.empty() ? "New Tab" : tab.title));
    }
    // No native tool tip on tabs: the hover card (title, site, memory) is the
    // one hover surface, and a URL tip popping up first would sit on top of it.
    setTabToolTip(idx, QString());

    // Muted tabs show the muted-volume glyph as the tab icon so the user can
    // see mute applied (the audio itself is muted through CEF).
    if (tab.muted) {
        setTabIcon(idx, icons::icon(icons::Name::Mute,
                                    current_theme().colors.text_muted));
    } else if (const auto internal = core::parse_internal_url(tab.url.raw())) {
        // Built-in pages have no site favicon to fetch: give each its own
        // glyph (the Aurora mark for the home page) so the tab never shows
        // the empty placeholder dot.
        const QColor accent = current_theme().colors.accent;
        switch (*internal) {
            case core::InternalPage::Home:
                setTabIcon(idx, QIcon(QStringLiteral(":/brand/aurora_mark.png")));
                break;
            case core::InternalPage::History:
                setTabIcon(idx, icons::icon(icons::Name::History, accent));
                break;
            case core::InternalPage::Downloads:
                setTabIcon(idx, icons::icon(icons::Name::Downloads, accent));
                break;
            case core::InternalPage::Bookmarks:
                setTabIcon(idx, icons::icon(icons::Name::Bookmarks, accent));
                break;
            case core::InternalPage::Extensions:
                setTabIcon(idx, icons::icon(icons::Name::Puzzle, accent));
                break;
            case core::InternalPage::Settings:
                setTabIcon(idx, icons::icon(icons::Name::Settings, accent));
                break;
            case core::InternalPage::SiteSettings:
                setTabIcon(idx, icons::icon(icons::Name::Shield, accent));
                break;
            case core::InternalPage::Reader:
                setTabIcon(idx, icons::icon(icons::Name::FileDocument, accent));
                break;
        }
    } else {
        const auto page_url = QString::fromStdString(tab.url.display());
        auto& favicons = FaviconCache::instance();
        // Teach the cache the real icon URL the engine found for this page, so
        // history and bookmarks rows for the same site get the same icon.
        if (!tab.favicon_url.empty()) {
            favicons.register_favicon(page_url,
                                      QString::fromStdString(tab.favicon_url));
        }
        // Never overwrite a known-good icon with a null one: the cache
        // answers null while the fetch is in flight AND forever for hosts
        // that have no icon, so an unconditional setTabIcon() blanked the
        // favicon on every refresh (title change, tab activation, a failed
        // fetch reporting "no icon"). Replace only when a real icon arrives;
        // otherwise re-assert the last valid one (survives the tab rebuild in
        // refresh_all(), which drops QTabBar's own stored icons).
        // The retained icon belongs to the host it was fetched for. A tab
        // that navigated to a DIFFERENT site must not keep showing the
        // previous site's icon while the new one is still being fetched —
        // that is the "wrong website icon" the user sees. Drop it on a host
        // change.
        const QString host = FaviconCache::host_of(page_url);
        const auto prev_host = last_icon_hosts_.constFind(tab.id);
        if (prev_host != last_icon_hosts_.constEnd() &&
            prev_host.value() != host) {
            last_icons_.remove(tab.id);
            last_icon_hosts_.remove(tab.id);
        }
        const QIcon icon = favicons.icon_for_url(page_url);
        if (!icon.isNull()) {
            last_icon_hosts_[tab.id] = host;
            last_icons_[tab.id] = icon;
            setTabIcon(idx, icon);
        } else if (const auto kept = last_icons_.constFind(tab.id);
                   kept != last_icons_.constEnd()) {
            setTabIcon(idx, kept.value());
        }
        // Otherwise nothing was ever known for this tab: leave the icon null
        // and the strip style paints the faint placeholder dot until
        // FaviconCache::icon_ready() triggers refresh_tab() again.
    }
    update_close_visibility();
}

void TabBar::request_close(core::TabId id) {
    // Queued: see the comment on the declaration. The id is validated again
    // when the timer fires, because the tab may have gone away in between.
    QTimer::singleShot(0, this, [this, id] {
        if (!manager_ || !manager_->contains(id)) return;
        // Do not put a transient loading bit into recently-closed state;
        // a reopened tab waits for the new engine's real load-start event.
        manager_->set_loading(id, false, -1);

        // Chrome / Ctrl+W (MainWindow::on_close_tab): closing the last tab
        // NEVER destroys the window — it is replaced with a fresh new-tab.
        // Calling window()->close() here was closing this window and, via the
        // CEF host teardown, could take every other Aurora window with it.
        if (manager_->tabs().size() <= 1) {
            manager_->close(id);
            if (manager_->count() == 0) {
                // Host has not wired the signal (or chose not to open). Empty
                // URL → TabManager's new-tab page; MainWindow's Added observer
                // substitutes the configured search-engine home.
                if (QWidget* top = window()) top->close();
            }
            return;
        }
        manager_->close(id);
    });
}

int TabBar::index_of(core::TabId id) const {
    const auto& tabs = manager_->tabs();
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

bool TabBar::is_interactive_at(const QPoint& pos) const {
    // A real tab: clicking it activates/drags it, so it is client area.
    if (tabAt(pos) >= 0) return true;
    // Per-tab child widgets (loading spinners, close buttons) are placed by
    // QTabBar inside the tab rects, but check them explicitly so a button
    // that overhangs its tab by a pixel is never swallowed by the caption
    // drag.
    for (QObject* obj : children()) {
        const auto* w = qobject_cast<const QWidget*>(obj);
        if (w && w->isVisible() && w->geometry().contains(pos)) return true;
    }
    // Everything else inside the tab bar's rect is background: the gap after
    // the last tab, and the vertical padding above/below the tabs. Dragging
    // there must move the window.
    return false;
}

void TabBar::mousePressEvent(QMouseEvent* event) {
    // Any press (click, drag start, right-click menu) dismisses the hover card.
    hover_card_timer_.stop();
    emit hover_dwell_cancelled();
    if (event->button() == Qt::MiddleButton) {
        int index = tabAt(event->pos());
        if (index >= 0 && static_cast<size_t>(index) < manager_->count()) {
            request_close(manager_->tabs()[static_cast<size_t>(index)].id);
            return;
        }
    }
    if (event->button() == Qt::RightButton) {
        int index = tabAt(event->pos());
        // Context menus work on pinned tabs too (unpin / reload / copy URL);
        // the "close" entry is the only harmful one there, and the menu
        // itself disables it for pinned tabs.
        if (index >= 0) {
            show_context_menu(event->globalPosition().toPoint(), index);
            return;
        }
    }
    if (event->button() == Qt::LeftButton) {
        const int index = tabAt(event->pos());
        if (index >= 0 && static_cast<size_t>(index) < manager_->count()) {
            pressed_tab_ = manager_->tabs()[static_cast<size_t>(index)].id;
            drag_start_pos_ = event->pos();
            drag_grab_offset_ = event->pos() - tabRect(index).topLeft();
            drag_state_ = DragState::TearOffPending;
        } else {
            pressed_tab_ = core::TabId(0);
            drag_grab_offset_ = QPoint();
            drag_state_ = DragState::Idle;
        }
    }
    QTabBar::mousePressEvent(event);
}

void TabBar::show_context_menu(const QPoint& pos, int index) {
    TabContextMenu menu(manager_, index, this);
    connect(&menu, &TabContextMenu::duplicate_tab, this, &TabBar::context_menu_duplicate);
    connect(&menu, &TabContextMenu::close_tab, this, &TabBar::context_menu_close);
    connect(&menu, &TabContextMenu::close_other_tabs, this,
            &TabBar::context_menu_close_others);
    connect(&menu, &TabContextMenu::pin_tab, this, &TabBar::context_menu_pin);
    connect(&menu, &TabContextMenu::mute_tab, this, &TabBar::context_menu_mute);
    connect(&menu, &TabContextMenu::reload_tab, this, &TabBar::context_menu_reload);
    connect(&menu, &TabContextMenu::suspend_tab, this, &TabBar::context_menu_suspend);
    connect(&menu, &TabContextMenu::restore_tab, this, &TabBar::context_menu_restore);
    connect(&menu, &TabContextMenu::exclude_tab_from_suspension, this,
            &TabBar::context_menu_exclude_suspension);
    connect(&menu, &TabContextMenu::copy_url, this, &TabBar::context_menu_copy_url);
    connect(&menu, &TabContextMenu::reopen_closed, this, &TabBar::context_menu_reopen_closed);
    // The cursor can sit right at a screen edge (the strip spans the whole
    // top of a maximized window) — keep the menu fully on screen.
    exec_menu_at(menu, pos, this);
}

void TabBar::on_tab_changed(core::TabId id, core::TabChange change) {
    using core::TabChange;
    switch (change) {
        case TabChange::Added:
        case TabChange::Removed:
            refresh_all();
            updateGeometry();  // adaptive widths follow the tab count
            break;
        case TabChange::Moved:
            // QTabBar already moved the tab (and its button widgets follow
            // it); the model was synced from the tabMoved signal. Rebuilding
            // here would destroy + recreate every close button mid-drag,
            // which is exactly the desync / flicker seen during tab
            // rearrange. No-op.
            break;
        case TabChange::ActiveChanged: {
            int idx = index_of(id);
            if (idx >= 0 && currentIndex() != idx) setCurrentIndex(idx);
            update_close_visibility();
            break;
        }
        case TabChange::TitleChanged: {
            int idx = index_of(id);
            if (idx >= 0) refresh_tab(idx);
            break;
        }
        case TabChange::LoadingStateChanged:
        case TabChange::ProgressChanged: {
            const auto* t = manager_->find(id);
            set_loading(id, t && t->loading);
            update();
            break;
        }
        case TabChange::PinnedChanged: {
            // A width change (full ↔ compact) needs a full relayout.
            int idx = index_of(id);
            if (idx >= 0) refresh_tab(idx);
            updateGeometry();
            update_close_visibility();
            update();
            break;
        }
        case TabChange::MutedChanged: {
            int idx = index_of(id);
            if (idx >= 0) refresh_tab(idx);
            update();
            break;
        }
        case TabChange::Crashed:
            set_loading(id, false);
            update();
            break;
        case TabChange::SuspendedChanged:
            if (int idx = index_of(id); idx >= 0) refresh_tab(idx);
            update();
            break;
        case TabChange::Recovered:
            update();
            break;
    }
}

} // namespace aurora::ui

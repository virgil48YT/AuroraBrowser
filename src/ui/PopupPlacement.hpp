// Aurora — popup placement.
//
// One helper for every dropdown / context menu / popover the chrome opens.
// With a frameless window, multi-monitor setups and fractional scaling it is
// easy for a mapToGlobal()-positioned popup to land (partly) outside the
// visible screen — the "menu escapes the window" bug. These functions clamp
// the popup into the available geometry of the anchor's screen, and, for
// anchored dropdowns, flip above the anchor when the space below runs out.
//
// Apply to every mapToGlobal(...) + exec()/show() site; do not open-code
// popup positions.
#pragma once

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMargins>
#include <QMenu>
#include <QMouseEvent>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QWidget>

#include "ui/Theme.hpp"

namespace aurora::ui {

inline QPoint clamped_to_screen(QPoint desired, const QSize& size,
                                const QWidget* anchor);

namespace popup_detail {

// Explicit Escape/outside-click handling for anchored popovers. Qt::Popup
// already supplies native mouse capture (which is important when a native CEF
// child window fills the page below it); this filter adds deterministic Escape
// handling and also supports the occasional non-Popup caller. It never
// consumes a mouse event, and it treats every popup descendant as inside, so a
// press on a row or one of its labels always reaches that row.
class DismissFilter final : public QObject {
public:
    DismissFilter(QWidget* popup, QWidget* anchor)
        : QObject(popup), popup_(popup), anchor_(anchor) {
        if (qApp) qApp->installEventFilter(this);
    }

    ~DismissFilter() override {
        if (qApp) qApp->removeEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (!popup_ || !popup_->isVisible()) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                popup_->close();
                return true;
            }
        } else if (event->type() == QEvent::ApplicationDeactivate ||
                   (event->type() == QEvent::WindowDeactivate &&
                    watched == popup_.data())) {
            popup_->close();
        } else if (event->type() == QEvent::MouseButtonPress ||
                   event->type() == QEvent::NonClientAreaMouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint global = mouse->globalPosition().toPoint();
            QWidget* target = qobject_cast<QWidget*>(watched);
            const bool in_popup =
                (target && (target == popup_ || popup_->isAncestorOf(target))) ||
                popup_->frameGeometry().contains(global);
            // Treat the anchor as part of the popup for dismissal purposes.
            // Otherwise the press closes the popup and the anchor's ensuing
            // clicked() signal immediately opens it again.
            const bool in_anchor =
                anchor_ &&
                ((target &&
                  (target == anchor_ || anchor_->isAncestorOf(target))) ||
                 QRect(anchor_->mapToGlobal(QPoint()), anchor_->size())
                     .contains(global));
            if (!in_popup && !in_anchor) popup_->close();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QPointer<QWidget> popup_;
    QPointer<QWidget> anchor_;
};

inline QString css_color(const QColor& color) {
    if (color.alpha() == 255) return color.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alphaF(), 0, 'f', 2);
}

} // namespace popup_detail

// Shared setup for non-blocking toolbar popovers. Use Qt::Popup by default:
// unlike Qt::Tool it takes native mouse input away from the CEF child window
// underneath, exactly like DownloadsPopover. The popup should be constructed
// with the owning browser window as its QWidget parent, not with the small
// toolbar button as its parent.
struct PopupPlacement {
    Qt::WindowType mode = Qt::Popup;
    bool clickOutsideToClose = false;
    QWidget* anchor = nullptr;

    void apply(QWidget* popup) const {
        if (!popup) return;
        popup->setWindowFlags(Qt::WindowFlags(mode) |
                              Qt::FramelessWindowHint |
                              Qt::NoDropShadowWindowHint);
        if (clickOutsideToClose) {
            new popup_detail::DismissFilter(popup, anchor);
        }
    }

    // Position a popup whose transparent outer margins reserve room for a
    // shadow. `panelMargins` describe those margins, allowing the visible
    // PANEL (rather than the popup's transparent backing widget) to align with
    // the anchor. The result is flipped above when necessary and clamped to
    // the anchor's available screen.
    static QRect popupGeometryFromAnchor(
        const QWidget* anchor, const QSize& popupSize,
        bool alignRight = true, int gap = 4,
        const QMargins& panelMargins = QMargins()) {
        if (!anchor) return QRect(QPoint(), popupSize);

        const QPoint anchorTopLeft = anchor->mapToGlobal(QPoint());
        const QRect anchorRect(anchorTopLeft, anchor->size());
        QPoint desired;
        if (alignRight) {
            desired.setX(anchorRect.right() + 1 -
                         (popupSize.width() - panelMargins.right()));
        } else {
            desired.setX(anchorRect.left() - panelMargins.left());
        }
        desired.setY(anchorRect.bottom() + 1 + gap - panelMargins.top());

        QScreen* screen = anchor->screen();
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect avail = screen->availableGeometry();
            const int panelHeight =
                popupSize.height() - panelMargins.top() - panelMargins.bottom();
            const int roomBelow =
                avail.bottom() + 1 - (anchorRect.bottom() + 1 + gap);
            const int roomAbove = anchorRect.top() - gap - avail.top();
            if (panelHeight > roomBelow && roomAbove > roomBelow) {
                desired.setY(anchorRect.top() - gap -
                             (popupSize.height() - panelMargins.bottom()));
            }

            // Clamp the visible panel, not its transparent shadow canvas.
            // This preserves exact right-edge alignment at a screen edge while
            // allowing the soft shadow itself to fade harmlessly off-screen.
            QRect panelRect(
                desired + QPoint(panelMargins.left(), panelMargins.top()),
                QSize(popupSize.width() - panelMargins.left() -
                          panelMargins.right(),
                      panelHeight));
            if (panelRect.right() > avail.right())
                desired.rx() -= panelRect.right() - avail.right();
            if (panelRect.left() < avail.left())
                desired.rx() += avail.left() - panelRect.left();
            if (panelRect.bottom() > avail.bottom())
                desired.ry() -= panelRect.bottom() - avail.bottom();
            if (panelRect.top() < avail.top())
                desired.ry() += avail.top() - panelRect.top();
            return QRect(desired, popupSize);
        }
        return QRect(desired, popupSize);
    }

    // Maximum visible PANEL height available on the better side of an anchor.
    // Callers subtract their fixed header/footer chrome from this value and
    // give the remainder to a QScrollArea. popupGeometryFromAnchor() uses the
    // same below-vs-above comparison, so the resulting popup both fits and
    // flips in the expected direction.
    static int availablePanelHeightFromAnchor(const QWidget* anchor,
                                              int gap = 4) {
        QScreen* screen = anchor ? anchor->screen() : nullptr;
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return 720;
        const QRect avail = screen->availableGeometry();
        if (!anchor) return avail.height();

        const QRect anchorRect(anchor->mapToGlobal(QPoint()), anchor->size());
        const int roomBelow =
            qMax(0, avail.bottom() + 1 - (anchorRect.bottom() + 1 + gap));
        const int roomAbove =
            qMax(0, anchorRect.top() - gap - avail.top());
        return qMax(roomBelow, roomAbove);
    }
};

// The visual treatment shared by the menu and extensions panels. The panel is
// an ordinary mouse-receiving QWidget; do not make it transparent to input.
inline QString roundedPanelStyle(const QString& objectName,
                                 const ThemeColors& colors,
                                 int radius = Radius::popover) {
    return QStringLiteral(
               "QWidget#%1 {"
               " background: %2;"
               " border: %3px solid %4;"
               " border-radius: %5px;"
               "}")
        .arg(objectName,
             popup_detail::css_color(colors.surface_raised))
        .arg(Metrics::hairline)
        .arg(popup_detail::css_color(colors.border_subtle))
        .arg(radius);
}

inline void installShadowBorder(QWidget* panel, const QColor& shadowColor) {
    if (!panel) return;
    auto* shadow = new QGraphicsDropShadowEffect(panel);
    QColor color(shadowColor);
    color.setAlpha(qMax(38, color.alpha()));
    shadow->setColor(color);
    shadow->setBlurRadius(24.0);
    shadow->setOffset(0.0, 6.0);
    panel->setGraphicsEffect(shadow);
}

// Clamp `desired` (the global top-left for a popup of `size`) so the whole
// popup stays inside the available geometry of `anchor`'s screen.
inline QPoint clamped_to_screen(QPoint desired, const QSize& size,
                                const QWidget* anchor) {
    QScreen* screen = anchor ? anchor->screen() : nullptr;
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return desired;
    const QRect avail = screen->availableGeometry();
    QPoint pos = desired;
    if (pos.x() + size.width() > avail.right() + 1)
        pos.setX(avail.right() + 1 - size.width());
    if (pos.y() + size.height() > avail.bottom() + 1)
        pos.setY(avail.bottom() + 1 - size.height());
    if (pos.x() < avail.left()) pos.setX(avail.left());
    if (pos.y() < avail.top()) pos.setY(avail.top());
    return pos;
}

// Global top-left for a dropdown hung under `anchor`: left edges aligned, or
// right edges aligned when `align_right` (controls at the window's trailing
// edge, e.g. the downloads button), `gap` px below the anchor. When the space
// below cannot fit the popup and the space above fits it better, the dropdown
// flips above the anchor; the result is always clamped to the screen.
inline QPoint dropdown_pos(const QWidget* anchor, const QSize& size,
                           bool align_right, int gap = 6) {
    if (!anchor) return QPoint();
    const QPoint anchor_tl = anchor->mapToGlobal(QPoint(0, 0));
    QPoint desired(align_right ? anchor_tl.x() + anchor->width() - size.width()
                               : anchor_tl.x(),
                   anchor_tl.y() + anchor->height() + gap);
    if (QScreen* screen = anchor->screen()) {
        const QRect avail = screen->availableGeometry();
        const int room_below = avail.bottom() + 1 - desired.y();
        const int room_above = anchor_tl.y() - gap - avail.top();
        if (size.height() > room_below && room_above > room_below) {
            desired.setY(anchor_tl.y() - gap - size.height());
        }
    }
    return clamped_to_screen(desired, size, anchor);
}

// exec() a menu at an explicit (e.g. cursor) position, clamped to the screen.
// Replaces open-coded `menu.exec(widget->mapToGlobal(...))` calls that let
// menus escape the window when it is not fullscreen. Returns the triggered
// action, like QMenu::exec().
inline QAction* exec_menu_at(QMenu& menu, QPoint desired,
                             const QWidget* anchor) {
    menu.ensurePolished();
    return menu.exec(clamped_to_screen(desired, menu.sizeHint(), anchor));
}

// exec() a menu as a dropdown under `anchor` (see dropdown_pos()).
inline QAction* exec_menu_under(QMenu& menu, const QWidget* anchor,
                                bool align_right = false) {
    menu.ensurePolished();
    return menu.exec(dropdown_pos(anchor, menu.sizeHint(), align_right));
}

} // namespace aurora::ui

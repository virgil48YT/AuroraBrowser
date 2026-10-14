#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QHashFunctions>
#include <QIcon>
#include <QPointer>
#include <QTabBar>
#include <QTimer>

#include "core/Types.hpp"
#include "tabs/TabManager.hpp"

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMouseEvent;

// core::TabId is a Qt-free value type (core/Types.hpp only specializes
// std::hash for it), but the tab strip keys its per-tab widgets by TabId in a
// QHash. Qt looks the hash function up by ADL, so it has to live in TabId's
// own namespace.
namespace aurora::core {
inline size_t qHash(const TabId& id, size_t seed = 0) noexcept {
    return ::qHash(static_cast<quint64>(id.value()), seed);
}
} // namespace aurora::core

namespace aurora::ui {

class TabSpinner;
class IconButton;

// The browser tab strip.
//
// Real browser-tab painting lives in a QProxyStyle subclass (TabStripStyle,
// defined in the .cpp and installed with setStyle()): inactive tabs are
// transparent over the strip with hairline dividers between calm neighbors,
// and the active tab is a folder tab that fuses seamlessly with the toolbar
// plate below it (rounded top corners, outward bottom fillets, no seam).
// Painting through the style — rather than overriding paintEvent — keeps
// QTabBar's drag-reorder, slide animations and per-tab button placement
// fully intact, because Qt renders the dragged tab's grab pixmap through
// the very same style.
//
// This class additionally owns: adaptive tab widths (tabSizeHint), per-tab
// loading spinners, close buttons that appear on hover/active only, compact
// icon-only pinned tabs, the tab context menu, and the Brave/Chromium-style
// tear-off / snap-back gesture (a live window-follow, not a QDrag).
class TabBar : public QTabBar, public core::TabObserver {
    Q_OBJECT

public:
    TabBar(core::TabManager* manager, QWidget* parent = nullptr);

    void refresh_all();
    void set_loading(core::TabId id, bool loading);
    void set_theme_accent(const QColor& accent);

    // Re-render one tab's label and icon (mute indicator, or the site
    // favicon from FaviconCache) from the model.
    void refresh_tab(int idx);

    // Model-state queries used by the tab-strip style while painting.
    bool tab_loading(int index) const;
    bool tab_pinned(int index) const;
    bool tab_suspended(int index) const;
    int hovered_index() const { return hovered_tab_; }

    // Frameless-window hit-test helper.
    //
    // The tab strip doubles as the window caption, so the host window has to
    // know which parts of THIS widget are genuinely clickable (a tab, or one
    // of the per-tab spinner / close-button children) and which are just
    // strip background that should drag the window instead. |pos| is in this
    // widget's coordinates. The tab bar is laid out with a Maximum size
    // policy, but it still gets whatever height the row has and, once the
    // tabs shrink to their minimum width, trailing background inside its own
    // rect — that background must remain draggable.
    bool is_interactive_at(const QPoint& pos) const;

    QSize tabSizeHint(int index) const override;

    // Un-maximize and clamp to the work-area so the follow window never reads
    // as maximized (Chrome un-maximizes the source on tear-off and never
    // hands the user a full-screen sheet mid-drag). Public and static so
    // MainWindow::tear_off_tab can settle the detached window's FINAL
    // geometry before the grab point is anchored — anchoring first and
    // resizing later is what produced the cursor-offset jump on tear-off.
    static void prepare_detached_follow_window(QWidget* detached_window);

signals:
    void context_menu_duplicate(int index);
    void context_menu_close(int index);
    void context_menu_pin(int index);
    void context_menu_mute(int index);
    void context_menu_reload(int index);
    void context_menu_suspend(int index);
    void context_menu_restore(int index);
    void context_menu_exclude_suspension(int index);
    void context_menu_copy_url(int index);
    void context_menu_reopen_closed();
    // A Qt move-drag from another Aurora tab bar was dropped at `index`, or a
    // live detached-window drag snapped back there (count() means append).
    // MainWindow owns the cross-window transfer because it must move both
    // model state and window-specific bookkeeping.
    void external_tab_dropped(TabBar* source, core::TabId id, int index);
    // A drag crossed the tear-off threshold. The owning MainWindow creates a
    // new window with the same point in the tab (`grab_offset`) under the
    // cursor and transfers `id`. Signature is fixed so MainWindow can stay
    // unchanged.
    void tab_tear_off_requested(core::TabId id, const QPoint& global_pos,
                                const QPoint& grab_offset);
    // Closing the last tab via the tab X (or middle-click) must never destroy
    // this window — Chrome replaces it with a fresh new-tab. MainWindow should
    // connect this to on_new_tab(). TabBar also opens an empty-URL tab if the
    // manager is still empty after the signal returns, so an unwired host
    // cannot leave a tab-less window.
    

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    // One state owns each gesture phase so a re-entrant model refresh cannot
    // create two detached windows for the same mouse drag.
    enum class DragState {
        Idle,
        TearOffPending,
        Reordering,
        ExternalDrag,
        Detached,
        Reattaching,
    };

    void show_context_menu(const QPoint& pos, int index);
    int index_of(core::TabId id) const;
    void update_close_visibility();
    int strip_available_width() const;
    void begin_immediate_tear_off(core::TabId id, const QPoint& global_pos);
    // `grab_offset` is the original press point relative to the dragged tab;
    // it rides along with the gesture hand-off so the destination bar can
    // re-anchor the cursor to the exact same tab-local point once the
    // detached window's final geometry is known.
    static void continue_dragging_detached_window(core::TabId id,
                                                  TabBar* origin,
                                                  const QPoint& grab_offset);
    void begin_detached_drag_tracking(core::TabId id, TabBar* origin,
                                      const QPoint& grab_offset);
    void poll_detached_drag();
    void stop_detached_drag_tracking();
    // Chrome: mouse-up over any strip docks the tab; mouse-up over empty
    // space leaves the torn-off window as a normal independent window.
    void finish_detached_drag(const QPoint& global_pos);
    bool try_reattach_to(TabBar* target, const QPoint& global_pos);
    TabBar* snap_back_target_at(const QPoint& global_pos) const;
    bool accept_snap_back(TabBar* source, core::TabId id,
                          const QPoint& global_pos);
    int insertion_index_at(const QPoint& pos) const;

    // Close a tab on the next event-loop turn.
    //
    // Closing synchronously from inside a mouse handler or a close-button
    // click destroys the very widgets Qt is still delivering the event to
    // (refresh_all() removes every tab, its spinner and its close button),
    // which crashed the browser — most visibly on the last tab. Deferring by
    // one turn lets the event finish unwinding before the model changes.
    void request_close(core::TabId id);

    void on_tab_changed(core::TabId id, core::TabChange change) override;

    core::TabManager* manager_;
    QHash<core::TabId, TabSpinner*> spinners_;
    QHash<core::TabId, IconButton*> close_btns_;
    // Last REAL icon shown for each tab. The favicon cache answers null while
    // an icon is still being fetched (and forever for hosts that have none);
    // remembering the last valid icon here keeps refresh_tab() from blanking
    // a tab's favicon on every ActiveChanged/TitleChanged repaint.
    QHash<core::TabId, QIcon> last_icons_;
    // The host each retained icon was fetched for, so the icon is dropped the
    // moment a tab navigates to another site instead of lingering on the new
    // page (which looked exactly like "the favicon shows the wrong website").
    QHash<core::TabId, QString> last_icon_hosts_;
    QColor accent_;
    int hovered_tab_ = -1;
    core::TabId pressed_tab_{0};
    QPoint drag_start_pos_;
    // Position of the original press relative to the dragged tab. Kept
    // separately from drag_start_pos_, whose coordinates are relative to this
    // bar and stop identifying the same point after a native tab reorder.
    QPoint drag_grab_offset_;
    DragState drag_state_ = DragState::Idle;
    // Current tab bar under the cursor during detached-window tracking.
    // QPointer makes a target window closing during a transfer attempt safe.
    QPointer<TabBar> snap_back_target_;
    // A value member has exactly the TabBar's lifetime: it cannot leak or
    // leave a heap timer queued after this bar is destroyed.
    QTimer snap_back_timer_;
    core::TabId detached_tab_{0};
    // Cursor position relative to the detached top-level window's framed
    // position. The polling loop preserves it while manually moving the
    // window, avoiding the platform-specific native move loop.
    QPoint detached_window_offset_;
    // Restarted on each snap-back attempt so a strip that refused the tab is
    // retried every ~120 ms while the cursor stays over it, never locked out.
    QElapsedTimer snap_retry_clock_;
};

} // namespace aurora::ui

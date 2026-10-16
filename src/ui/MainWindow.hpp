// Aurora — main browser window.
//
// Frameless, premium chrome. One "chrome plate" (#ChromeTop) carries both rows
// so the selected tab visually connects to the toolbar surface beneath it:
//
//   1. tab strip (tabs + "+" button + draggable caption + window controls)
//   2. navigation toolbar (back/forward/reload/home, capsule omnibox,
//      extensions, menu)
//   3. content area — either the web-content host for the active tab or one of
//      the native internal pages (history/downloads/bookmarks/settings)
//   4. find bar (hidden until Ctrl+F)
//
// There is intentionally NO QStatusBar: nothing may reserve layout height for
// transient information. Hover-link URLs, load progress and one-shot
// confirmations surface in a small floating "status bubble" at the bottom-left
// of the content area that exists only while it has something to say.
//
// The window owns the TabManager and creates one CefHostWidget per tab inside
// a QStackedWidget; the active tab's host is shown and the others are hidden
// (CEF browsers are told to hide too, so background tabs suspend rendering).
// Internal pages live in the same stack and are swapped in place of the host
// when a tab navigates to an aurora:// URL.
#pragma once

#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QMainWindow>
#include <QLabel>
#include <QPointer>
#include <QSize>
#include <QString>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include "core/ChromeNav.hpp"
#include "core/InternalUrl.hpp"
#include "core/Types.hpp"
#include "tabs/TabManager.hpp"
#include "ui/BrowserMenu.hpp"
#include "ui/ExtensionsButton.hpp"
#include "ui/FindBar.hpp"
#include "ui/IconButton.hpp"
#include "ui/Omnibox.hpp"
#include "core/IncognitoPolicy.hpp"
#include "searchengines/SearchEngine.hpp"
#include "ui/ServiceBundle.hpp"
#include "ui/ShieldButton.hpp"
#include "ui/TabBar.hpp"
#include "webview/WebViewController.hpp"

class QStackedWidget;
class QVBoxLayout;
class QHBoxLayout;
class QToolButton;
class QDockWidget;
class QTimer;
class QAbstractNativeEventFilter;

// A native QWidget that hosts the docked DevTools CEF browser as a child
// window. It realizes its native window handle eagerly (CEF needs a real HWND
// to parent into) and forwards resizes so CefAdapter can resize the DevTools
// child window to match.
class DevToolsHost : public QWidget {
    Q_OBJECT
public:
    explicit DevToolsHost(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_NativeWindow);
        setMinimumSize(280, 160);
        setObjectName("DevToolsHost");
    }
signals:
    void resized(int w, int h);
protected:
    void resizeEvent(QResizeEvent*) override { emit resized(width(), height()); }
};

namespace aurora::permissions { class PermissionStore; }

namespace aurora::ui {

class BookmarkBar;
class CefHostWidget;
class DownloadsPopover;
class InternalPageWidget;
class PermissionPrompt; // header-only widget, included from the .cpp

class MainWindow : public QMainWindow, public core::TabObserver {
    Q_OBJECT
public:
    // `services` are borrowed and must outlive the window (main.cpp owns the
    // Database and the services). A default-constructed bundle is valid: the
    // window runs without a profile and the internal pages say so.
    //
    // `is_incognito` creates a private window. In private mode the window
    // never writes to the regular profile: history, download metadata and
    // session snapshots are skipped (see core::IncognitoPolicy), preferences
    // and bookmarks are read-only, extensions are disabled, and the window's
    // tabs share a private in-memory CEF request context that is discarded
    // when the window closes. The title suffix "(Incognito)", accent-tinted
    // chrome and an Incognito badge make the mode visible.
    explicit MainWindow(const ServiceBundle& services = ServiceBundle{},
                        QWidget* parent = nullptr, bool is_incognito = false);
    ~MainWindow() override;

    // True when this window is a private (incognito) window.
    bool is_incognito() const { return incognito_; }

    // core::TabObserver: drives host-widget lifecycle from the tab model.
    void on_tab_changed(core::TabId id, core::TabChange change) override;

    // Accessor for the tab manager (used by main.cpp for session save/restore).
    core::TabManager& tabs() { return tabs_; }
    const core::TabManager& tabs() const { return tabs_; }

    // Restore the previous session's tabs. Safe to call once, after the window
    // exists: every stored row is validated first (see session::plan_restore),
    // the window is never left with zero tabs, and the seed tab is only closed
    // after at least one restored tab is open. Returns the number of tabs
    // restored (0 when there was nothing valid to restore).
    int restore_session();

    // Persist the open tabs so the next launch can restore them.
    void save_session();

    // Close every web view synchronously and pump its engine until the closes
    // complete (or a bounded timeout). Must run on the UI thread, after the
    // last window closes and before the CEF runtime is shut down, so that the
    // GPU/renderer subprocesses are reaped and the app folder can be deleted on
    // Windows. Safe to call more than once. Tears down ONLY this window's
    // controller / hosts / extension popup — never another MainWindow's browsers
    // and never the global CEF runtime.
    void prepare_shutdown();

    // Copy the source window's live theme mode and re-apply the stylesheet
    // before show(), so a newly spawned window never flashes the fallback light
    // theme (and never publishes a mismatched global palette that would
    // frankenstein the source window's custom-painted widgets).
    void inherit_theme_from(const MainWindow* src);

    // Open `url` in a new foreground tab. Used for URLs given on the command
    // line (`aurora_browser https://example.com`, and the OS "open with"
    // handoff that passes a file or link to an already-chosen browser).
    void open_url_in_new_tab(const core::Url& url);

    // Open an extension popup (default_popup) as a small anchored popup window
    // rather than a full tab. The URL is a chrome-extension:// URL;
    // `extension_id` is the owning extension (optional, used for the
    // click-the-same-button-again toggle).
    //
    // The popup belongs to the tab it was invoked from: it is shown without
    // taking activation and never becomes the window's current content, so the
    // invoking tab stays the active one for the engine's extension APIs (an
    // extension popup that queried "the current tab" used to get the popup
    // itself).
    void open_extension_popup(const QString& url,
                              const QString& extension_id = QString());

    // Close the active extension popup (if any) and clear the tracked pointer.
    // The application-wide event filter stays installed (it also serves caption
    // dragging). The CEF browser cleanup is handled by the popup's destroyed()
    // signal.
    void close_extension_popup();

    // Called from the Windows native outside-click filter when a mouse button is
    // pressed anywhere on screen while an extension popup is open. If the press
    // landed outside the popup (and outside the extensions button that opens
    // it), the popup is dismissed — this catches clicks on the native CEF child
    // window that Qt's qApp eventFilter never sees. No-op on non-Windows.
    void notify_native_outside_click(const QPoint& global_pos);

    // The extension registry changed (install / enable / disable / remove).
    // Drops the extension surfaces this window is hosting for the previous
    // state — notably any open extension popup — so an uninstall takes effect
    // in the UI at once instead of at the next launch.
    void on_extension_registry_changed();

    // Open one of the native pages in a new foreground tab.
    void show_page(core::InternalPage page);

    // Stop a running download, addressed by its DownloadService row id (the
    // only handle the downloads UI has). Reverse-maps it to the engine's own
    // download id, asks the web view to abort, and only then marks the row
    // cancelled so the popover/page update without waiting for the engine's
    // terminal event. Returns false when no live engine download matches the
    // row — in which case nothing is written, because finish(Cancelled) is
    // monotonic and would block a legitimate later Complete. Injected into
    // DownloadsPopover / DownloadsPage as their cancel callback.
    bool cancel_download(int64_t row_id);
    // Engine-facing download controls: pause/resume address a live transfer
    // through the same id map as cancel; retry re-requests a failed row's URL
    // as a fresh transfer (the old row stays as history, the new one arrives
    // through the normal download callback). All three are no-ops that return
    // false when there is no engine or no matching row.
    bool pause_download(int64_t row_id);
    bool resume_download(int64_t row_id);
    bool retry_download(int64_t row_id);
    // The action struct handed to the downloads popover and page — one
    // builder so the dropdown and the aurora://downloads page can never drift
    // apart in what they offer. (Their Actions types are structurally
    // identical but distinct classes, hence the template.)
    template <typename Actions>
    Actions downloads_actions() {
        Actions a;
        a.cancel = [this](int64_t row) { return cancel_download(row); };
        a.pause = [this](int64_t row) { return pause_download(row); };
        a.resume = [this](int64_t row) { return resume_download(row); };
        a.retry = [this](int64_t row) { return retry_download(row); };
        return a;
    }

    // --- Diagnostics, used by the headless smoke test ----------------------
    // Live hosts whose web engine was actually created. With CEF disabled this
    // is always 0; use hosts_activated() to assert visibility bookkeeping in a
    // Qt-only build.
    int hosts_created() const;
    // Live hosts that have ever been made the current content widget. A bulk
    // session restore must leave this at exactly 1.
    int hosts_activated() const;
    // Live hosts that have already been given a native window handle. Forcing
    // one per tab is half of the restore crash, so the smoke test asserts it
    // stays at most 1.
    int hosts_with_native_window() const;
    // The tab whose content currently occupies the content area.
    core::TabId shown_tab() const { return shown_tab_; }

protected:
    void closeEvent(QCloseEvent* e) override;
    // Window state changes (fullscreen, maximize, restore) resize the content
    // area without a resizeEvent reaching the host in every case; re-assert the
    // web view's geometry when the state settles.
    void changeEvent(QEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    // Frameless window: the tab strip doubles as the caption (drag to move,
    // double-click to maximize) and the window edges drive a system resize.
    bool eventFilter(QObject* obj, QEvent* e) override;
    // Windows-native frameless support: on Windows we do NOT use
    // Qt::FramelessWindowHint (that yields a WS_POPUP window with no
    // WS_THICKFRAME, and Windows only offers Aero Snap to thick-frame windows).
    // Instead we keep the native resizable frame and strip the visible title bar
    // in nativeEvent() by handling WM_NCCALCSIZE (return 0 -> client == window).
    // WM_NCHITTEST then returns HTCAPTION over empty tab-strip space (so the
    // drag there is a native caption drag — the only drag that engages Aero
    // Snap: drag-to-top maximize, snap to left/right halves, drag-restore) and
    // HTCLIENT over tabs/buttons (so they keep working). Left/right/bottom
    // resize borders are also reported here. ensure_native_frame() (called from
    // showEvent) guarantees WS_THICKFRAME | WS_CAPTION | WS_MIN/MAXIMIZEBOX are
    // present. On Linux/macOS this is a no-op and the eventFilter fallback
    // (startSystemMove/startSystemResize) handles drag/resize.
    bool nativeEvent(const QByteArray& eventType, void* message,
                     qintptr* result) override;

    // True when the given GLOBAL point sits on draggable caption space: the
    // tab strip's own background (including the gap after the last tab), the
    // layout stretch, and the bare row padding — but never a tab or one of the
    // strip's buttons. Shared by the Windows WM_NCHITTEST handler and the
    // Linux/macOS startSystemMove fallback so both agree on what "caption"
    // means. Platform-independent on purpose: it is pure widget geometry.
    bool is_caption_drag_point(const QPoint& global) const;

    // Diagnostic latch: set the first time a WM_NCHITTEST actually reaches
    // nativeEvent, and logged once. "Is the native hit-test even running?" is
    // the first question to answer whenever window dragging misbehaves.
    bool nchittest_seen_ = false;
    // Move-threshold-gated startSystemMove() fallback (see eventFilter). Arms on
    // a press over caption space; fires startSystemMove() only after the pointer
    // moves past QApplication::startDragDistance(), so a plain click or a
    // double-click-to-maximize never starts a move.
    QPoint drag_press_global_;
    bool drag_pending_ = false;
    // Set once ensure_native_frame() has run, so showEvent() does not repeatedly
    // call SetWindowPos/SWP_FRAMECHANGED on every subsequent show.
    bool native_frame_ensured_ = false;

private slots:
    void on_new_tab();
    void on_close_tab();
    void on_reopen_closed();
    void on_back();
    void on_forward();
    void on_reload();
    void on_stop();
    void on_home();
    void on_focus_omnibox();
    void on_find();
    void on_find_close();
    void on_fullscreen_toggle();
    // Web content entered or left fullscreen (video fullscreen button, the
    // Fullscreen API). Hides/restores Aurora's own chrome around the page.
    void on_page_fullscreen(core::TabId tab, bool fullscreen);
    // Re-assert the active web view's geometry on the next event-loop turns.
    // Window-state transitions arrive in stages on both Windows and X11, so a
    // single immediate sync can land while the window still has its old size.
    void sync_active_host_geometry();
    // Delete a host widget whose web view has finished closing.
    void finish_closing_host(core::TabId tab);
    void on_devtools();
    void close_devtools_dock();
    void on_bookmark_page();
    // Menu action / preference: show or hide the bookmarks bar under the
    // toolbar. Persists through services_.prefs so the choice survives a
    // restart, like the theme does.
    void on_bookmark_bar_toggled();
    void on_zoom_in();
    void on_zoom_out();
    void on_zoom_reset();
    void on_next_tab();
    void on_prev_tab();
    void on_theme_toggled();
    void on_clear_browsing_data();
    void clear_browsing_data(bool history, bool downloads, bool cookies);
    void clear_on_exit();
    // The downloads tray button / Ctrl+J: toggle the recent-downloads dropdown
    // popover under the button (the full history lives on aurora://downloads,
    // reachable from the popover's footer and the browser menu).
    void show_downloads_popover();
    // A .crx download the engine rerouted to us: validate via
    // ExtensionService::install_from_file, report the honest outcome in a
    // dialog, and clean up the private temp copy.
    void on_extension_install_request(
        const webview::ExtensionInstallRequest& req);
    // A page asked for a capability (mic/camera/notification). Remembered
    // decisions answer inline; anything else shows the prompt under the
    // omnibox. Default-deny: dismissal denies, nothing auto-grants.
    void on_permission_request(webview::PermissionRequest req);
    // Custom window controls (frameless window).
    void on_window_minimize();
    void on_window_toggle_maximize();
    void on_window_close();
    void update_window_control_icons();
    void on_duplicate_tab(int index);
    void on_close_tab_at(int index);
    void on_pin_tab(int index);
    void on_mute_tab(int index);
    void on_reload_tab(int index);
    void on_suspend_tab(int index);
    void on_restore_tab(int index);
    void on_exclude_tab_from_suspension(int index);
    void on_copy_tab_url(int index);

private:
    // PiP reuses the window-owned controller and existing transfer lifecycle.
    friend class PipWindow;

    struct TransferredTab {
        core::TabState state;
        std::optional<core::InternalPage> page;
        std::optional<core::ChromeNavStack> chrome_nav;
    };

    // Builds the whole chrome plate: tab strip row + toolbar row.
    void setup_chrome(QVBoxLayout* root);
    void setup_window_controls(QHBoxLayout* tab_row);
    void setup_status_bubble();
    void setup_menu_button();
    void setup_devtools_dock();
    void setup_shortcuts();
    void setup_connections();
    // Decide synchronously whether a key the engine forwarded (while its
    // native child window owned the keyboard) is one of OUR window shortcuts.
    // True consumes the key engine-side so the page never sees it. The action
    // itself is dispatched on the next event-loop turn (QTimer::singleShot(0))
    // by calling the same handler methods the QShortcut table uses — never via
    // a synthesized QKeyEvent, because with the native child window owning
    // focus there is no focus widget for Qt's shortcut map to resolve against.
    bool forward_engine_key(const webview::EngineKeyEvent& ev);
    void apply_theme();
    void show_host_for(core::TabId id);
    // The content widget a tab should show (its internal page, or its web
    // host), or nullptr when the tab has neither. Pure: shows nothing.
    QWidget* content_for(core::TabId id);
    // The single place allowed to call content_stack_->setCurrentWidget().
    // Relinquishes every other host so only the widget passed here may create
    // a web engine, and enforces the eager-creation cap during a bulk restore.
    void set_current_content(QWidget* w, core::TabId owner = core::TabId(0));
    // Re-assert the invariant "the current content widget belongs to the active
    // tab" (or the inert placeholder during a bulk restore). Called after the
    // stack's children change, because QStackedWidget promotes a sibling to
    // current on its own when the current widget is removed.
    void sync_current_content();
    void update_nav_button_states();
    void update_loading_state(core::TabId id);
    void navigate_active_tab(const core::Url& url);
    // Transient status text (hover-link URL, load progress). Shown in the
    // floating status bubble; an empty string hides it. NEVER the tab title
    // or the current page's URL — that used to leave a dead band along the
    // bottom of the window and is gone for good.
    void set_status(const QString& text);
    // One-shot confirmation ("Bookmarked: …", "URL copied"). Shows in the
    // status bubble and auto-hides after `msec`.
    void flash_status(const QString& text, int msec = 3200);
    // Keep the bubble pinned to the bottom-left of the content area (above
    // the find bar when that is open).
    void position_status_bubble();
    // Searchable tab-list dropdown at the far left of the tab strip.
    void show_tab_search_menu();
    // Omnibox star: visible whenever a tab is active, filled while the
    // *current* page of the *active* tab is bookmarked. Called on tab switch,
    // navigation, URL/title change, from show_page() and after a toggle, so
    // the star can never keep showing the previous page's state.
    void update_bookmark_star();
    // Apply `visible` to the bookmarks bar and the menu's toggle label. Does
    // not persist: the caller decides whether this is a preference change or
    // just a restore (leaving content fullscreen).
    void apply_bookmark_bar_visibility(bool visible);
    // Re-read the bar after any bookmark mutation (Ctrl+D, the star, the
    // bookmarks page). Cheap and safe when the bar does not exist yet.
    void reload_bookmark_bar();
    // The URL the active tab currently shows, or empty when there is none.
    std::string active_tab_url() const;
    // True when the active tab shows a native aurora:// page. Those are not
    // bookmarkable, so the star must be clear and inert.
    bool active_tab_is_internal() const;

    // Cross-window tab transfer. Releasing removes the model entry without
    // putting it in "recently closed"; adopting preserves the TabId and the
    // model/chrome history. CEF controllers are window-owned, so the receiving
    // window recreates the renderer at the current URL after the old host has
    // closed rather than trying to use one controller from two windows.
    std::optional<TransferredTab> release_tab_for_transfer(core::TabId id);
    bool adopt_transferred_tab(TransferredTab transfer, size_t index,
                               bool replace_seed);
    bool transfer_tab_to(MainWindow& destination, core::TabId id, size_t index,
                         bool replace_seed = false);
    void tear_off_tab(core::TabId id, const QPoint& release_global,
                      const QPoint& grab_offset);

    // Spawn a sibling window that shares this window's ServiceBundle (so prefs,
    // extensions and history are the same profile) and inherits its live theme.
    void open_new_window();
    // Spawn a private (incognito) sibling window: same visual chrome, no
    // writes to the regular profile. See core::IncognitoPolicy.
    void open_incognito_window();
    // Re-paint the live extension popup's 1px frame from the active palette so
    // a theme toggle never leaves a light border on a dark popup (or vice versa).
    void apply_extension_popup_theme();

    // Preferences read at startup and whenever the settings page writes them.
    void load_preferences();
    // Resolve the stored theme preference ("system"|"light"|"dark") to a mode
    // and re-apply the stylesheet.
    void apply_theme_preference(const QString& preference);
    // The page the tab currently shows, or nullopt for web content.
    std::optional<core::InternalPage> page_of(core::TabId id) const;

    // --- Chrome-level back/forward (internal pages) ------------------------
    // Internal pages are Qt widgets, so navigating to one never touches the
    // engine's history and CEF::CanGoBack() stays false — which is why Back
    // did nothing on aurora:// pages. These helpers maintain a small per-tab
    // stack that records internal-page navigations alongside the web URLs
    // they were opened from; the engine still owns history *within* web
    // content, so normal Back/Forward on real pages is unchanged.
    core::ChromeNavStack& nav_stack(core::TabId id);
    const core::ChromeNavStack* find_nav_stack(core::TabId id) const;
    // Record an internal-page navigation for `id`, seeding the web page it was
    // opened from as the entry beneath it when the stack is still empty.
    void record_internal_nav(core::TabId id, core::InternalPage page);
    // Record a web navigation the chrome initiated (omnibox, Home, restore).
    void record_web_nav(core::TabId id, const core::Url& url);
    // Show the tab's current chrome entry (internal page or web URL) without
    // pushing history. Returns false when there is nothing to show.
    bool apply_chrome_entry(core::TabId id, const core::ChromeNavEntry& entry);
    bool chrome_go_back(core::TabId id);
    bool chrome_go_forward(core::TabId id);
    // Lazily create the shared widget for a page.
    InternalPageWidget* page_widget(core::InternalPage page);
    // The URL a fresh tab and the Home button should open.
    std::string new_tab_url() const;
    // The default search engine (built-in or custom) for omnibox queries and
    // the home/new-tab page. Reads the SearchEngineService snapshot only.
    searchengines::ResolvedEngine resolved_search_engine() const;
    // Complete a site-initiated popup handoff after the CEF callback unwinds.
    // An empty target means JavaScript called window.open() with no URL and
    // must produce a genuinely blank page, not the configured new-tab page.
    void open_site_initiated_tab(std::string url);

    core::TabManager tabs_;
    ServiceBundle services_;
    // Own only the private view, never the shared profile Settings or database.
    std::unique_ptr<settings::Settings> read_only_prefs_;
    // Private (incognito) window flag. Drives core::IncognitoPolicy decisions
    // throughout this class; see the constructor for how it changes the bundle.
    bool incognito_ = false;
    // Gate late engine writes while clear-on-exit drains asynchronous callbacks.
    bool privacy_exit_started_ = false;
    // The policy the window consults for "may I write this?" decisions.
    core::IncognitoPolicy policy_;
    std::unique_ptr<webview::WebViewController> webview_;
    TabBar* tab_bar_ = nullptr;
    // Container holding the tab bar, the new-tab button, the draggable caption
    // area and the window controls.
    QWidget* tab_strip_ = nullptr;
    IconButton* new_tab_btn_ = nullptr;
    Omnibox* omnibox_ = nullptr;
    // The toolbar row is a plain widget (not a QToolBar) so the layout — not
    // QMainWindow's dock areas — decides that it sits *below* the tab strip.
    QWidget* toolbar_ = nullptr;
    QHBoxLayout* toolbar_layout_ = nullptr;
    // Row of saved pages directly under the toolbar, inside the same chrome
    // plate. Hidden while web content is fullscreen and while the preference
    // is off, but always built so toggling it costs nothing.
    BookmarkBar* bookmark_bar_ = nullptr;
    QStackedWidget* content_stack_ = nullptr;
    FindBar* find_bar_ = nullptr;
    BrowserMenu* menu_button_ = nullptr;
    ExtensionsButton* extensions_btn_ = nullptr;
    // The active extension popup window (chrome-extension:// default_popup).
    // Uses Qt::Tool (not Qt::Popup) so the CEF child window inside it receives
    // mouse events normally — Qt::Popup grabs the mouse and starves the native
    // CEF surface of input. Tracked so we can close it on click-outside.
    QPointer<QWidget> extension_popup_;
    // The CefHostWidget inside extension_popup_. Tracked so a deferred
    // sync_geometry() and a theme restyle can find it after show().
    QPointer<CefHostWidget> extension_popup_host_;
    // Synthetic TabId of the popup's CEF browser. destroy() uses this so we
    // never close a real tab's engine when the popup goes away.
    core::TabId extension_popup_tab_{0};
#if defined(Q_OS_WIN)
    // Native outside-click filter (see notify_native_outside_click). Installed
    // once on qApp in the constructor and removed in the destructor. Forward-
    // declared so <QAbstractNativeEventFilter> only needs to be included in the
    // .cpp, keeping windows.h out of this widely-included header.
    std::unique_ptr<QAbstractNativeEventFilter> native_outside_filter_;
#endif
    // The chrome-extension:// URL the live popup is showing. Picking the same
    // extension again toggles the popup shut; picking a different one replaces
    // it (instead of only closing the old popup and opening nothing).
    QString extension_popup_url_;
    // The id of the extension the live popup belongs to (empty when the caller
    // did not supply one). Used for the toggle so that re-picking the same
    // extension closes its popup even when its popup URL varies between
    // openings.
    QString extension_popup_extension_id_;
    // The tab the popup was invoked from. The popup is that tab's surface, not
    // a tab of its own: it never becomes the window's current content and
    // never takes activation, so this tab stays active for the extension APIs.
    core::TabId extension_popup_owner_tab_{0};
    // Age of the live popup. Showing a Qt::Tool window generates presses and
    // activation churn of its own (historically including the browser window's
    // own deactivation, before the popup stopped taking activation); the
    // dismissal branches in eventFilter() ignore that burst, otherwise the
    // popup closed itself the moment it opened.
    QElapsedTimer extension_popup_age_;
    IconButton* downloads_btn_ = nullptr;
    // Aurora Shields: the shield glyph left of the downloads tray. Badge count
    // and per-site toggle state are pushed from the page-info/tab-change
    // callbacks below; the button itself owns nothing.
    ShieldButton* shield_btn_ = nullptr;
    // Page-lifetime blocked-request counts per tab, keyed like hosts_. The
    // engine keeps its own copy (it increments from the IO thread); this is
    // the UI-side mirror used to repaint the badge when switching tabs.
    std::unordered_map<core::TabId, uint64_t> adblock_counts_;
    // Push the shield button's state for the ACTIVE tab: glyph up/down for the
    // current site's exception state, badge = this page's blocked count,
    // disabled entirely on internal pages where there is nothing to block.
    void update_shield_for_tab();
    // Apply a per-site shields toggle for the active tab's site: persist it in
    // the adblock service (survives restart), push the new exception set to
    // the engine, and refresh the button. The caller decides about reloading.
    void set_site_blocking(bool enabled);
    // Lazily created recent-downloads dropdown (opened by downloads_btn_ and
    // Ctrl+J). Child of the window; a Qt::Popup, so outside clicks dismiss it.
    DownloadsPopover* downloads_popover_ = nullptr;
    // The mic/camera/notification prompt, created on first request. One
    // popup at a time; concurrent page requests queue inside it. Decisions
    // are per-origin via permission_store_ (prefs-backed, no schema change).
    PermissionPrompt* permission_prompt_ = nullptr;
    std::unique_ptr<permissions::PermissionStore> permission_store_;
    // The tab-search dropdown chevron lives inside the tab strip row, at the
    // far left (subtle rounded button, per the reference layout).
    IconButton* tab_search_btn_ = nullptr;
    // Floating status bubble: a QLabel child of the central widget that is
    // shown/hidden wholesale — it never occupies layout height.
    QLabel* status_bubble_ = nullptr;
    // Auto-hide for flash_status(); restarted on every new flash.
    QTimer* status_timer_ = nullptr;

    IconButton* back_btn_ = nullptr;
    IconButton* fwd_btn_ = nullptr;
    IconButton* reload_stop_btn_ = nullptr;
    IconButton* home_btn_ = nullptr;

    // Frameless window: the draggable caption (the tab strip row) and the
    // custom minimize/maximize/close buttons that fill the top-right corner.
    QWidget* caption_ = nullptr;
    IconButton* win_min_ = nullptr;
    IconButton* win_max_ = nullptr;
    IconButton* win_close_ = nullptr;

    // Docked DevTools panel. devtools_tab_ is the tab being inspected (invalid
    // when DevTools is closed). One DevTools browser at a time.
    QDockWidget* devtools_dock_ = nullptr;
    DevToolsHost* devtools_host_ = nullptr;
    core::TabId devtools_tab_{0};

    std::map<core::TabId, CefHostWidget*> hosts_;
    // Hosts whose tab is gone and whose web view is still closing. The widget
    // owns the native window the engine is tearing down, so it must outlive the
    // close; it is deleted when the engine reports the tab closed (or by a
    // bounded fallback timer if that report never arrives).
    std::map<core::TabId, CefHostWidget*> closing_hosts_;
    // Set while web content is fullscreen so Esc/F11 leave content fullscreen
    // through the engine instead of desynchronising the page and the window.
    core::TabId fullscreen_tab_{0};
    bool chrome_hidden_ = false;
    // The status bubble hides itself while it has nothing to say; this flag
    // keeps it hidden while web content owns the whole window (content
    // fullscreen).
    bool status_hidden_by_fullscreen_ = false;
    bool was_maximized_ = false;
    // Which tabs are showing an internal page instead of web content.
    std::map<core::TabId, core::InternalPage> tab_pages_;
    // Per-tab chrome-level history (internal pages + the web URLs around
    // them). See record_internal_nav / chrome_go_back.
    std::map<core::TabId, core::ChromeNavStack> nav_stacks_;
    // Last main-frame URL the engine reported per tab, so returning to a web
    // entry only re-loads it when the engine is actually somewhere else.
    std::map<core::TabId, std::string> engine_url_;
    // CEF has no API to inject a destroyed browser's native history. Once a
    // tab has been suspended, use its preserved model navigation stack for
    // Back/Forward, loading the selected entry in the replacement browser.
    std::set<core::TabId> restored_history_;
    // One shared widget per page, created on first use.
    std::map<core::InternalPage, InternalPageWidget*> pages_;

    // The default search engine preference: a built-in id or "custom:<id>".
    std::string search_engine_;
    bool dark_mode_ = false;
    // The user's bookmarks-bar preference. Distinct from the widget's own
    // visibility, which content fullscreen also drives.
    bool bookmark_bar_visible_ = true;

    // Maps CEF download ids (per-session) to DownloadService row ids, so
    // progress/finish updates land on the right download record.
    std::unordered_map<uint32_t, int64_t> download_ids_;

    // --- Bulk-open (session restore) guard ---------------------------------
    // While true, Added/ActiveChanged must not make any host visible: hosts are
    // built but stay dormant, and the one chosen tab is shown once after the
    // loop. See restore_session() and session::run_restore().
    bool bulk_open_ = false;
    // Hosts whose engine we allowed to be created during the current bulk
    // restore. Hard cap; a corrupt session must never be able to spawn a
    // browser per row.
    int bulk_hosts_created_ = 0;
    // Inert widget that owns index 0 of the content stack, so adding a tab host
    // to an otherwise-empty stack never implicitly makes it current, and so
    // there is always something harmless to fall back to.
    QWidget* content_placeholder_ = nullptr;
    core::TabId shown_tab_{0};
    // Wall-clock moment the window first became visible, so closeEvent can
    // report how long the browser lived. A close within ~1s of show is the
    // signature of the startup crash we are hunting.
    std::chrono::steady_clock::time_point shown_at_{};
};
} // namespace aurora::ui

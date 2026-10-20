#include "ui/MainWindow.hpp"

#include <QApplication>
#include <chrono>
#include <QCloseEvent>
#include <QShowEvent>
#include <QClipboard>
#include <QImage>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QMetaObject>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <QDir>
#include <QFileDialog>
#include <QPointer>
#include <QRegularExpression>
#include <QDockWidget>
#include <QFile>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QDateTime>
#include <QProgressDialog>
#include <cstdint>
#include <atomic>
#include <unordered_map>

#if defined(Q_OS_WIN)
// Used only by the native outside-click filter below, which catches mouse
// presses that land on the CEF child window (Qt's qApp eventFilter never sees
// them because CEF consumes the native messages first).
//
// We deliberately do NOT include <windows.h> here: this translation unit also
// declares local `VK_*` constexprs inside forward_engine_key(), and windows.h
// would macro-substitute those names and break the build. Instead we carry the
// three values we need as minimal local types.
#  include <QAbstractNativeEventFilter>
namespace {
// Minimal stand-in for the Win32 MSG struct: we only ever read `message`.
struct NativeMsg {
    void* hwnd;
    unsigned int message;
};
constexpr unsigned int kWmLButtonDown = 0x0201;
constexpr unsigned int kWmRButtonDown = 0x0204;
constexpr unsigned int kWmMButtonDown = 0x0207;
constexpr unsigned int kWmNcLButtonDown = 0x00A1;
} // namespace
#endif

// Windows native frameless support.
//
// Why not <windows.h>: this translation unit also declares local `VK_*`
// constexpr ints inside forward_engine_key() (VK_T, VK_W, VK_F5, ...). The
// Windows SDK defines those exact names as object-like macros, so including
// <windows.h> would textually rewrite `VK_T` -> `0x54` and break the constexpr
// declarations. We therefore declare the tiny subset of the Win32 API we need
// ourselves, exactly as the existing NativeMsg struct already did.
//
// The approach mirrors what every modern frameless-on-Windows toolkit does
// (Chrome, Edge, Windows Terminal, qwindowkit, PyQt-Frameless-Window):
//   - keep a NATIVE resizable window (WS_THICKFRAME | WS_CAPTION | WS_MIN/
//     MAXIMIZEBOX) so the OS itself drives Aero Snap (drag-to-top maximize,
//     snap to left/right halves, Win+Arrow, snap layouts);
//   - handle WM_NCCALCSIZE to make the client area fill the whole window,
//     removing the standard title bar + resize border (the "frameless look");
//   - handle WM_NCHITTEST to report HTCAPTION over the empty tab-strip space
//     (so dragging there is a native caption drag — the only drag that engages
//     Aero Snap) and resize hit-codes at the window edges.
//
// The previous code set Qt::FramelessWindowHint, which makes Qt create the HWND
// with WS_POPUP and NO WS_THICKFRAME. A popup window CAN be moved by returning
// HTCAPTION, but Windows only offers snap gestures to thick-frame windows, so
// snapping was silently unavailable. Restoring WS_THICKFRAME is the fix.
#if defined(Q_OS_WIN)
namespace aurora_win {
// Minimal MSG-compatible struct (stable Win64 ABI: 48 bytes, same layout as
// tagMSG from winuser.h). We read message, wParam, lParam and hwnd.
struct NativeMsg {
    void* hwnd;                   // HWND
    unsigned int message;         // UINT
    unsigned long long wParam;    // WPARAM
    long long lParam;             // LPARAM
    unsigned int time;           // DWORD
    long pt_x;                   // LONG (POINT.x)
    long pt_y;                   // LONG (POINT.y)
};

// --- Window messages --------------------------------------------------------
constexpr unsigned int kWM_NCHITTEST  = 0x0084;
constexpr unsigned int kWM_NCCALCSIZE = 0x0083;
constexpr unsigned int kWM_NCACTIVATE = 0x0086;

// --- Non-client hit-test codes ---------------------------------------------
constexpr qintptr kHTCAPTION = 2;
constexpr qintptr kHTLEFT = 10;
constexpr qintptr kHTRIGHT = 11;
constexpr qintptr kHTBOTTOM = 15;
constexpr qintptr kHTBOTTOMLEFT = 16;
constexpr qintptr kHTBOTTOMRIGHT = 17;

// --- GetWindowLongPtr index + window styles ---------------------------------
constexpr int kGWL_STYLE = -16;
constexpr qintptr kWS_POPUP       = 0x80000000ull;
constexpr qintptr kWS_SYSMENU     = 0x00080000ull;
constexpr qintptr kWS_MINIMIZEBOX  = 0x00020000ull;
constexpr qintptr kWS_MAXIMIZEBOX  = 0x00010000ull;
constexpr qintptr kWS_THICKFRAME  = 0x00040000ull;
constexpr qintptr kWS_CAPTION     = 0x00C00000ull; // WS_BORDER | WS_DLGFRAME
constexpr qintptr kWS_MAXIMIZE    = 0x01000000ull;

// --- SetWindowPos flags -----------------------------------------------------
constexpr unsigned int kSWP_NOSIZE     = 0x0001;
constexpr unsigned int kSWP_NOMOVE     = 0x0002;
constexpr unsigned int kSWP_NOZORDER   = 0x0004;
constexpr unsigned int kSWP_NOACTIVATE = 0x0010;
constexpr unsigned int kSWP_FRAMECHANGED = 0x0020;

// --- GetSystemMetrics indices ----------------------------------------------
constexpr int kSM_CXSIZEFRAME = 32;  // resize border width
constexpr int kSM_CYSIZEFRAME = 33;  // resize border height
constexpr int kSM_CXPADDEDBORDER = 92;

// --- Minimal Win32 structs (stable ABI) ------------------------------------
struct Rect { long left; long top; long right; long bottom; };
struct WindowPos {
    void* hwnd;            // HWND
    void* hwndInsertAfter; // HWND
    int x, y, cx, cy;
    unsigned int flags;
};
struct NcCalcSizeParams {
    Rect rgrc[3];
    WindowPos* lppos;
};

// --- The tiny subset of user32 we link against (user32.lib is always pulled
//     in by Qt; on Win64 there is no name decoration, so extern "C" resolves
//     directly against the undecorated W exports). NB: MSVC (with /permissive-)
//     requires the __stdcall calling-convention keyword AFTER the return type
//     (`qintptr __stdcall foo`), the same form the Windows SDK uses
//     (`LONG_PTR WINAPI foo` where WINAPI == __stdcall). Putting it before the
//     return type is parsed as a data modifier and fails to compile.
extern "C" {
qintptr __stdcall GetWindowLongPtrW(void* hwnd, int index);
qintptr __stdcall SetWindowLongPtrW(void* hwnd, int index, qintptr value);
int __stdcall SetWindowPos(void* hwnd, void* hwndInsertAfter,
                           int x, int y, int cx, int cy, unsigned int flags);
int __stdcall GetSystemMetrics(int index);
} // extern "C"

// Ensure the top-level HWND carries the resizable native frame Windows needs
// before it will offer Aero Snap. Qt's default overlapped window already has
// these bits; this is a defensive safety net for the cases where Qt's flags
// left them off. Idempotent: only writes when something is missing. Returns true
// when it actually changed the style.
inline bool ensure_native_frame(void* hwnd) {
    if (!hwnd) return false;
    const qintptr want = kWS_THICKFRAME | kWS_CAPTION | kWS_MINIMIZEBOX |
                         kWS_MAXIMIZEBOX | kWS_SYSMENU;
    qintptr style = GetWindowLongPtrW(hwnd, kGWL_STYLE);
    if ((style & want) == want) return false;
    style |= want;
    SetWindowLongPtrW(hwnd, kGWL_STYLE, style);
    // SWP_FRAMECHANGED makes the window re-evaluate its frame; NOSIZE/NOMOVE/
    // NOZORDER/NOACTIVATE keep position/size/z-order untouched.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                kSWP_NOSIZE | kSWP_NOMOVE | kSWP_NOZORDER | kSWP_NOACTIVATE |
                    kSWP_FRAMECHANGED);
    return true;
}

} // namespace aurora_win
#endif

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QScreen>
#include <QUrl>
#include <vector>
#include <QMessageBox>

#include "core/InternalUrl.hpp"
#include "adblock/AdblockEngine.hpp"
#include "adblock/AdblockService.hpp"
#include "bookmarks/BookmarkService.hpp"
#include "downloads/DownloadService.hpp"
#include "extensions/ExtensionService.hpp"
#include "history/HistoryService.hpp"
#include "privacy/BrowsingData.hpp"
#include "session/SessionRestore.hpp"
#include "searchengines/SearchEngine.hpp"
#include "searchengines/SearchEngineService.hpp"
#include "ui/OmniboxSuggestController.hpp"
#include "session/SessionService.hpp"
#include "settings/Settings.hpp"
#include "settings/SiteNightModeStore.hpp"
#include "settings/SiteZoomStore.hpp"
#include "ui/QrShareDialog.hpp"
#include "ui/TorLauncher.hpp"
#include "ui/ScreenshotDialogs.hpp"
#include "ui/pages/ReaderPage.hpp"
#include "ui/BookmarkBar.hpp"
#include "ui/CefHostWidget.hpp"
#include "ui/DownloadsPopover.hpp"
#include "ui/FilterListUpdater.hpp"
#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"
#include "ui/PermissionPrompt.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/StylesheetBuilder.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"
#include "ui/pages/BookmarksPage.hpp"
#include "ui/pages/DownloadsPage.hpp"
#include "ui/pages/ExtensionsPage.hpp"
#include "ui/pages/HistoryPage.hpp"
#include "ui/LeftDock.hpp"
#include "ui/ShortcutDialog.hpp"
#include "ui/TabHoverCard.hpp"
#include "ui/ShortcutList.hpp"
#include "ui/pages/HomePage.hpp"
#include "ui/pages/SettingsPage.hpp"
#include "ui/pages/SiteSettingsPage.hpp"
#include "util/Log.hpp"

#if defined(AURORA_HAS_CEF)
#include "cef/CefRuntime.hpp"
#include "ui/PipWindow.hpp"
#endif

namespace aurora::ui {

namespace {
// Hard cap on how many web hosts may have their engine created during a single
// bulk restore. Exactly one tab is visible after a restore, so one is all that
// is ever legitimate; the cap turns a future regression into a logged warning
// instead of N native browsers in one event-loop turn.
constexpr int kMaxHostsCreatedDuringBulkOpen = 1;

// Visible MainWindow count, used by closeEvent so closing one window never
// quits the app while another browser window is still open. Hidden windows
// (the stack-allocated first window after the user closed it, Qt::Tool
// popups, etc.) are not "open".
int visible_main_window_count() {
    int n = 0;
    if (!qApp) return 0;
    for (QWidget* w : qApp->topLevelWidgets()) {
        if (qobject_cast<MainWindow*>(w) && w->isVisible()) ++n;
    }
    return n;
}

#if defined(Q_OS_WIN)
// Closes the extension popup when the user presses anywhere outside it —
// including on the native CEF child window, whose WM_LBUTTONDOWN/
// WM_RBUTTONDOWN/WM_MBUTTONDOWN/WM_NCLBUTTONDOWN messages CEF consumes before
// Qt's qApp eventFilter ever sees them (so the existing MouseButtonPress branch
// in MainWindow::eventFilter only catches clicks on Qt-owned chrome). This
// restores Qt::Popup-style click-outside-to-close for the Qt::Tool extension
// popup without reverting to Qt::Popup, which starves the CEF child of input
// (see open_extension_popup). The filter never consumes a message, so the
// underlying click still reaches its intended target.
class ExtensionPopupOutsideClickFilter : public QAbstractNativeEventFilter {
public:
    explicit ExtensionPopupOutsideClickFilter(MainWindow* owner)
        : owner_(owner) {}

    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* /*result*/) override {
        if (eventType != "windows_generic_MSG" || !message) return false;
        const auto* msg = static_cast<const NativeMsg*>(message);
        const unsigned int m = msg->message;
        if (m != kWmLButtonDown && m != kWmRButtonDown &&
            m != kWmMButtonDown && m != kWmNcLButtonDown) {
            return false;
        }
        if (owner_) owner_->notify_native_outside_click(QCursor::pos());
        return false;  // never consume — let the click reach its target
    }

private:
    MainWindow* owner_ = nullptr;
};
#endif
// --- Tor incognito ----------------------------------------------------------

// A Tor daemon the user runs themselves: AURORA_TOR_PROXY for one run, else
// the tor.proxy preference. Empty when neither is set.
std::string external_tor_proxy(const settings::Settings* prefs) {
    std::string proxy = qEnvironmentVariable("AURORA_TOR_PROXY").trimmed().toStdString();
    if (proxy.empty() && prefs)
        proxy = prefs->get_string(settings::PrefKeys::kTorProxy, "");
    return proxy;
}

// Use the Tor that ships with Aurora (unless the user pointed Tor windows at
// their own daemon, or switched the built-in one off in Settings > Privacy).
bool use_bundled_tor(const settings::Settings* prefs) {
    if (!external_tor_proxy(prefs).empty()) return false;
    if (prefs && !prefs->get_bool(settings::PrefKeys::kTorUseBundled,
                                  settings::PrefDefaults::kTorUseBundled)) {
        return false;
    }
    return TorLauncher::instance().has_bundled_tor();
}

// The SOCKS5 proxy for Tor windows: an explicitly configured daemon
// (AURORA_TOR_PROXY / tor.proxy), else the running built-in Tor, else a
// local Tor daemon's default port. Chromium's spelling is socks5:// (it
// always resolves host names through a SOCKS5 proxy), so the curl-style
// socks5h:// is accepted and normalized.
std::string tor_proxy_setting(const settings::Settings* prefs) {
    std::string proxy = external_tor_proxy(prefs);
    if (proxy.empty() && use_bundled_tor(prefs)) proxy = TorLauncher::instance().proxy();
    if (proxy.empty()) proxy = "socks5://127.0.0.1:9050";
    if (proxy.rfind("socks5h://", 0) == 0) proxy = "socks5://" + proxy.substr(10);
    return proxy;
}

// Is there a SOCKS5 server (in practice: a running Tor daemon) at `proxy`?
// Performs the SOCKS5 greeting ("no authentication") and checks the reply,
// so an unrelated service on the port is not mistaken for Tor. Blocks for at
// most ~3 s, which is acceptable for an explicit "New Tor window" click.
bool probe_socks5(const std::string& proxy, QString* why) {
    const QUrl url(QString::fromStdString(proxy));
    if (url.scheme() != QLatin1String("socks5") || url.host().isEmpty() || url.port() <= 0) {
        if (why) *why = QStringLiteral("\"%1\" is not a socks5://host:port address.")
                            .arg(QString::fromStdString(proxy));
        return false;
    }
    QTcpSocket socket;
    socket.connectToHost(url.host(), static_cast<quint16>(url.port()));
    if (!socket.waitForConnected(1500)) {
        if (why) *why = QStringLiteral("Nothing is listening on %1:%2 (%3).")
                            .arg(url.host()).arg(url.port()).arg(socket.errorString());
        return false;
    }
    const char greeting[3] = {0x05, 0x01, 0x00};  // SOCKS5, 1 method: no auth
    socket.write(greeting, sizeof(greeting));
    if (!socket.waitForBytesWritten(1000) || !socket.waitForReadyRead(1500)) {
        if (why) *why = QStringLiteral("%1:%2 did not answer as a SOCKS5 proxy.")
                            .arg(url.host()).arg(url.port());
        return false;
    }
    const QByteArray reply = socket.read(2);
    socket.abort();
    if (reply.size() != 2 || reply[0] != 0x05 || reply[1] != 0x00) {
        if (why) *why = QStringLiteral("%1:%2 is not a SOCKS5 proxy that accepts "
                                       "unauthenticated connections.")
                            .arg(url.host()).arg(url.port());
        return false;
    }
    return true;
}

// Night mode is keyed by origin, like per-site zoom; only web pages qualify.
std::string night_mode_origin(const std::string& url) {
    const core::Url parsed(url);
    if (!parsed.is_valid() || (parsed.scheme() != "http" && parsed.scheme() != "https"))
        return {};
    return parsed.origin();
}

} // namespace

MainWindow::MainWindow(const ServiceBundle& services, QWidget* parent,
                       bool is_incognito, bool is_tor)
    : QMainWindow(parent), tabs_(), services_(services) {
    // Tor mode is incognito plus a proxy: every private-window guarantee
    // (in-memory context, no profile writes) applies unchanged.
    tor_ = is_tor;
    if (tor_) is_incognito = true;
    incognito_ = is_incognito;
    policy_.is_incognito = is_incognito;  // drive IncognitoPolicy checks below
    if (incognito_) {
        // core::IncognitoPolicy: a private window must never write to the
        // regular profile. History, download metadata, session snapshots,
        // bookmark writes and preference writes are all skipped; extensions
        // are disabled (their service workers/cookies are global, not private-
        // context-scoped). Unavailable write-capable services are nulled below;
        // preferences stay attached through a read-only Settings view so the
        // page can read real values and remain interactive without persisting.
        services_.history = nullptr;
        services_.downloads = nullptr;
        services_.session = nullptr;
        services_.adblock = nullptr;
        services_.bookmarks = nullptr;
        // Updating the filter list rewrites a profile file.
        services_.filter_lists = nullptr;
    }
    // Extensions are loaded process-wide by the engine at startup (the
    // --load-extension switch, see cef/CefLifecycle.cpp), but a private window
    // never exposes them: no extensions button, popups, options pages or .crx
    // install path. The engine side is covered by the private window's
    // in-memory ("incognito mode") request context, where Chromium does not
    // run extensions that were not allowed in incognito.
    if (!policy_.enable_extensions()) services_.extensions = nullptr;
    if (services_.prefs &&
        (incognito_ || services_.prefs->is_read_only())) {
        // Never mark the shared Settings read-only: that would also block
        // normal windows. Each private/sibling view borrows only the database,
        // so deleting its source window cannot leave a dangling prefs pointer.
        read_only_prefs_ = services_.prefs->read_only_view();
        services_.prefs = read_only_prefs_.get();
    }
    setWindowTitle(tor_ ? QStringLiteral("Aurora (Tor Incognito)")
                   : incognito_ ? QStringLiteral("Aurora (Incognito)")
                                : QStringLiteral("Aurora"));
    // FRESHNESS PROBE: written to the Desktop (NOT %TEMP%) because the remote
    // device bridge cannot read the user's %TEMP% folder, which is why the
    // earlier "log missing" conclusion was bogus. If this line appears in
    // aurora_drag.log but no "nativeEvent ENTRY" line follows, the running
    // binary is fresh but nativeEvent never fires. applicationFilePath() tells
    // us EXACTLY which .exe ran (package vs SFX extraction vs stale).
    {
        QFile f(QStandardPaths::writableLocation(
                    QStandardPaths::DesktopLocation) +
                QLatin1String("/aurora_drag.log"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            f.write(QStringLiteral("ALIVE pid=%1 exe=%2\n")
                        .arg(QCoreApplication::applicationPid())
                        .arg(QCoreApplication::applicationFilePath())
                        .toUtf8());
            f.close();
        }
    }
    resize(1280, 820);
    setMinimumSize(400, 320);

    // On Windows we deliberately do NOT use Qt::FramelessWindowHint: that flag
    // makes Qt create the HWND with WS_POPUP and no WS_THICKFRAME, and Windows
    // only offers Aero Snap (drag-to-top maximize, snap halves, Win+Arrow) to
    // thick-frame windows. Instead we keep the native resizable frame and remove
    // the title bar ourselves in nativeEvent() via WM_NCCALCSIZE — the same
    // technique Chrome/Edge use. WM_NCHITTEST then reports HTCAPTION over the
    // empty tab-strip space so dragging there is a native caption drag (the
    // only drag that engages Aero Snap). ensure_native_frame() is called from
    // showEvent() to guarantee the WS_THICKFRAME bit is present.
    //
    // On Linux/macOS there is no native caption hit-test, so we keep the
    // frameless flag and fall back to startSystemMove()/startSystemResize() in
    // eventFilter() (compiled out on Windows).
#if !defined(Q_OS_WIN)
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
#endif
    setMouseTracking(true);
    qApp->installEventFilter(this);
#if defined(Q_OS_WIN)
    // Catches clicks that land on the native CEF child window (outside the
    // extension popup) so the popup dismisses like a real Qt::Popup would,
    // without reverting to Qt::Popup (which starves the CEF child of input).
    native_outside_filter_ = std::make_unique<ExtensionPopupOutsideClickFilter>(this);
    qApp->installNativeEventFilter(native_outside_filter_.get());
#endif

    // Homepage / search engine / theme come from the profile when there is one.
    load_preferences();

#if defined(AURORA_HAS_CEF)
    webview_ = cef::make_webview_controller();
    // Incognito: hand the engine a private in-memory request context before
    // any tab is created. If the engine cannot do private mode at all, refuse
    // to open the window rather than browse in the regular profile.
    if (incognito_ && !webview_->set_private_context(true)) {
        AURORA_LOG(util::LogLevel::Error, "main")
            << "engine does not support private mode; aborting window";
        QTimer::singleShot(0, this, &MainWindow::close);
        return;
    }
    // Tor: the proxy is scoped to this window's private context and applied
    // before any tab exists. If the engine cannot, the window must not open:
    // browsing "in Tor mode" without the proxy would be a silent leak.
    if (tor_) {
        tor_proxy_ = tor_proxy_setting(services_.prefs);
        if (!webview_->set_private_proxy(tor_proxy_)) {
            AURORA_LOG(util::LogLevel::Error, "main")
                << "engine cannot proxy the private context; aborting Tor window";
            QTimer::singleShot(0, this, &MainWindow::close);
            return;
        }
    }
    // Open target=_blank / window.open links as new in-app tabs instead of
    // letting CEF spawn a popup (which on some setups escapes to the OS
    // default browser). Deferred to the next event-loop turn so we don't
    // re-enter CEF browser creation from inside OnBeforePopup.
    webview_->set_new_tab_callback(
        [this](std::string url) {
            QTimer::singleShot(0, this, [this, url = std::move(url)]() mutable {
                open_site_initiated_tab(std::move(url));
            });
            return true;
        });
    // Right-click menu actions only the host can perform (clipboard, search the
    // selection, inspect, ...). Deferred a turn: several open dialogs or run
    // nested event loops, which must not happen inside an engine callback.
    webview_->set_context_action_callback(
        [this](core::TabId tab,
               webview::WebViewController::ContextActionRequest req) {
            QTimer::singleShot(
                0, this, [this, tab, req = std::move(req)]() mutable {
                    handle_context_action(tab, std::move(req));
                });
        });
    // Downloads: files go to the OS Downloads folder and every download's
    // lifecycle is recorded in DownloadService so the Downloads page can show
    // them. Clicking a download link no longer opens the OS default browser.
    // The engine side falls back to its CWD when handed an empty path, which
    // would scatter files wherever the binary was launched from — so resolve
    // (and create) a real folder here: QStandardPaths first, ~/Downloads as
    // the fallback when the platform reports none (minimal environments).
    QString downloads_dir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloads_dir.isEmpty())
        downloads_dir = QDir::homePath() + QStringLiteral("/Downloads");
    if (!QDir().mkpath(downloads_dir))
        AURORA_LOG(util::LogLevel::Warn, "ui")
            << "could not create downloads dir: "
            << downloads_dir.toStdString();
    webview_->set_downloads_dir(downloads_dir.toStdString());
    webview_->set_download_callback(
        [this](const webview::DownloadEvent& ev) {
            if (privacy_exit_started_ || !services_.downloads) return;
            const auto state_to_ds = [](int s) {
                switch (s) {
                    case 1: return downloads::DownloadState::Complete;
                    case 2: return downloads::DownloadState::Cancelled;
                    case 3: return downloads::DownloadState::Interrupted;
                    default: return downloads::DownloadState::InProgress;
                }
            };
            auto it = download_ids_.find(ev.id);
            if (it == download_ids_.end()) {
                // New download: record the start, then the initial progress.
                const int64_t row = services_.downloads->begin(
                    ev.url, ev.target_path, ev.mime, ev.total_bytes);
                if (row > 0) {
                    download_ids_[ev.id] = row;
                    services_.downloads->update_progress(row, ev.received,
                                                         ev.total_bytes);
                }
                // Surface a newly-started transfer immediately. The popover
                // owns refresh/placement in present(); create it lazily just
                // like the toolbar action does.
                if (!downloads_popover_) {
                    downloads_popover_ = new DownloadsPopover(
                        services_.downloads, this,
                        downloads_actions<DownloadsPopover::Actions>());
                    connect(
                        downloads_popover_,
                        &DownloadsPopover::full_history_requested, this,
                        [this] {
                            if (downloads_popover_) downloads_popover_->close();
                            show_page(core::InternalPage::Downloads);
                        });
                }
                // Defer the popover present to the next event-loop tick so
                // it appears AFTER any save-file dialog has fully closed.
                QTimer::singleShot(0, this, [this] {
                    if (downloads_popover_) downloads_popover_->present();
                });
            } else {
                const int64_t row = it->second;
                services_.downloads->update_progress(row, ev.received,
                                                     ev.total_bytes);
                // Pause state rides along every update. 1 marks the transfer
                // as paused right now (whether the engine paused it or our
                // pending queue did); 0 marks it running again. Both map onto
                // no-ops when the row is already in the matching state, so no
                // need to check the current one first.
                if (ev.paused == 1) {
                    services_.downloads->pause(row);
                } else if (ev.paused == 0) {
                    services_.downloads->resume(row);
                }
                // The destination is only known for certain once the engine has
                // it; the first event can carry an empty or temporary path.
                if (!ev.target_path.empty()) {
                    services_.downloads->set_target_path(row, ev.target_path);
                }
                if (ev.state != 0) {
                    services_.downloads->finish(row, state_to_ds(ev.state));
                    // Interrupted is NOT necessarily terminal: Chromium retries
                    // a recoverable interruption by itself and keeps reporting
                    // on the same download id. Keep the id -> row mapping so
                    // the resumed download updates THIS row (and can still end
                    // up Complete) instead of being recorded a second time as a
                    // new download. Complete/Cancelled really are terminal.
                    if (ev.state != 3) download_ids_.erase(it);
                }
            }
        });
    // --- Aurora Shields (network ad blocking) -----------------------------
    // Rules + enabled flag come from the host (main.cpp loads the profile's
    // filter list); the per-site exceptions come from the profile database so
    // shields-down survives a restart. The blocked callback arrives on the UI
    // thread per blocked request, already carrying the page total.
    if (services_.adblock) {
        const bool adblock_on =
            services_.prefs
                ? services_.prefs->get_bool(settings::PrefKeys::kAdblockEnabled,
                                            /*default=*/true)
                : true;
        // After an in-app update the updater holds the newer list; the
        // bundle's copy is what main.cpp loaded at startup.
        webview_->set_adblock_rules(services_.filter_lists
                                        ? services_.filter_lists->rules()
                                        : services_.adblock_rules,
                                    adblock_on);
        if (services_.filter_lists) services_.filter_lists->mark_engine_current();
        std::vector<std::string> exceptions;
        for (const auto& site : services_.adblock->list_exception_sites()) {
            exceptions.push_back(site);
        }
        webview_->set_adblock_exceptions(exceptions);
    }
    // Per-site zoom lives in the prefs store under "zoom.<origin>". The
    // adapter calls these on the UI thread (zoom actions and main-frame
    // commits). In a private window services_.prefs is the read-only view:
    // remembered levels still apply, but nothing the user zooms there is
    // written back — the same rule every other preference follows.
    if (services_.prefs) {
        settings::Settings* prefs = services_.prefs;
        webview::WebViewController::ZoomStore zoom_store;
        zoom_store.load = [prefs](const std::string& origin, double& level) {
            return settings::SiteZoomStore(*prefs).level(origin, level);
        };
        zoom_store.save = [prefs](const std::string& origin, double level) {
            settings::SiteZoomStore(*prefs).set_level(origin, level);
        };
        zoom_store.clear = [prefs](const std::string& origin) {
            settings::SiteZoomStore(*prefs).clear(origin);
        };
        webview_->set_zoom_store(std::move(zoom_store));
    }
    // Manual filter-list updates: every normal window listens so the
    // shared engine is rebuilt even if the page that started it is gone.
    if (services_.filter_lists) {
        connect(services_.filter_lists, &FilterListUpdater::finished, this,
                &MainWindow::on_filter_list_update_finished);
    }
    webview_->set_adblock_blocked_callback(
        [guard = QPointer<MainWindow>(this)](core::TabId tab,
                                             uint64_t blocked) {
            // Posted from CEF's IO thread to this one. The task can outlive
            // this window (it may be drained during the post-exec CEF
            // shutdown pump), so the raw pointer is guarded with a QPointer
            // captured by value: if the window is gone by the time the task
            // runs, the guard is null and nothing happens.
            if (!guard) return;
            guard->adblock_counts_[tab] = blocked;
            if (guard->shield_btn_ && tab == guard->tabs_.active()) {
                guard->shield_btn_->set_blocked_count(blocked);
            }
        });

    // A tab's web view finishes closing asynchronously. Until it does, the host
    // widget (and therefore the native window CEF is tearing down) has to stay
    // alive — see closing_hosts_.
    webview_->set_tab_closed_callback([this](core::TabId id) {
        QTimer::singleShot(0, this, [this, id] {
            if (auto it = hosts_.find(id); it != hosts_.end())
                it->second->browser_closed();
            else
                finish_closing_host(id);
        });
    });
    // Permissions (mic/camera/notifications): remembered decisions answer
    // inline; unknown origins get the styled prompt under the omnibox.
    // Default-deny end to end: dismissal denies, nothing auto-grants, and a
    // dropped request denies engine-side.
    if (services_.prefs)
        permission_store_ =
            std::make_unique<permissions::PermissionStore>(*services_.prefs);
    webview_->set_permission_callback(
        [this](webview::PermissionRequest req) {
            // Deferred to the next event-loop turn so the popup is not
            // opened from inside a CEF callback (it grabs the input).
            QTimer::singleShot(0, this, [this, req]() mutable {
                on_permission_request(std::move(req));
            });
        });
    // Packaged extensions (.crx / .crx3): the engine rerouted the download
    // to a private temp file and hands it here instead of recording a
    // download row. Deferred to the next event-loop turn so we never run a
    // dialog from inside a CEF callback.
    webview_->set_extension_install_callback(
        [this](const webview::ExtensionInstallRequest& req) {
            QTimer::singleShot(
                0, this, [this, req] { on_extension_install_request(req); });
        });
    // Video / Fullscreen API. Deferred to the next event-loop turn so we never
    // reconfigure the window from inside a CEF callback.
    webview_->set_fullscreen_callback([this](core::TabId id, bool on) {
        QTimer::singleShot(0, this, [this, id, on] { on_page_fullscreen(id, on); });
    });
    // Keyboard: while a web page has focus the native CEF child window owns
    // the keyboard and Qt sees nothing, so the engine offers each
    // accelerator-looking key here first. forward_engine_key answers
    // synchronously (consume or not) and, when consuming, dispatches the
    // action on the next event-loop turn — never inside the engine callback.
    webview_->set_key_event_callback(
        [this](core::TabId /*tab*/, const webview::EngineKeyEvent& ev) {
            return forward_engine_key(ev);
        });
    webview_->set_page_info_callback(
        [this](core::TabId id, const webview::PageInfo& info) {
            if (privacy_exit_started_) return;
            const auto* state = tabs_.find(id);
            const auto host = hosts_.find(id);
            // A closing browser can still emit load/title callbacks. They
            // must not mutate the saved snapshot or wake a suspended tab.
            if (!state || state->suspended ||
                (host != hosts_.end() && host->second->browser_closing()))
                return;
            if (info.memory_bytes >= 0) {
                // A renderer's answer to the hover card's memory query; it
                // carries no navigation state, so nothing else applies.
                tab_memory_[id] = info.memory_bytes;
                if (tab_hover_card_ && tab_hover_card_->isVisible() &&
                    hover_card_tab_ == id) {
                    tab_hover_card_->set_memory_text(
                        QStringLiteral("Memory usage: %1")
                            .arg(TabHoverCard::format_memory(
                                info.memory_bytes)));
                }
                return;
            }
            if (info.has_audible_state) tabs_.set_audible(id, info.audible);
            if (info.pending_downloads >= 0)
                tabs_.set_pending_downloads(id, static_cast<size_t>(info.pending_downloads));
            if (info.renderer_crashed) {
                tabs_.mark_crashed(id, info.renderer_out_of_memory
                    ? core::TabCrashReason::OutOfMemory
                    : core::TabCrashReason::RendererGone);
            } else if (info.renderer_recovered) {
                tabs_.mark_recovered(id);
            }
            if (!info.title.empty()) {
                tabs_.set_title(id, info.title);
                // Backfill the title on the history row for this tab's current
                // URL. Titles always arrive after the navigation commits, so
                // the visit was recorded title-less and the history page showed
                // the raw URL instead of the page name.
                if (services_.history) {
                    const auto* tab = tabs_.find(id);
                    if (tab && tab->url.is_valid() &&
                        !core::parse_internal_url(tab->url.raw()).has_value()) {
                        services_.history->set_title(tab->url, info.title,
                                                     info.favicon_url);
                    }
                }
            }
            // OnLoadingStateChange is the sole authority for start/finish.
            // OnLoadingProgressChange only fills load_progress and leaves the
            // PageInfo loading field at its default false; treating that as a
            // finish stopped/started indicators at arbitrary progress events.
            if (info.has_nav_state) {
                tabs_.set_loading(id, info.loading, info.load_progress);
            } else if (info.load_progress >= 0) {
                const auto* tab = tabs_.find(id);
                if (tab) {
                    tabs_.set_loading(id, tab->loading, info.load_progress);
                }
            }
            // CEF's real back/forward capability — only OnLoadingStateChange
            // sets has_nav_state, so title/address callbacks cannot reset the
            // toolbar buttons. This is the fix for "back/forward don't work":
            // the buttons used the tab's own history stack (omnibox entries
            // only) and were permanently disabled after any link navigation.
            if (info.has_nav_state) {
                tabs_.set_nav_state(id, info.can_go_back, info.can_go_forward);
            }
            // Favicon URL from CEF's OnFaviconURLChange. Other callbacks leave
            // favicon_url empty and must not clear it (handled by the guard).
            if (!info.favicon_url.empty()) {
                tabs_.set_favicon(id, info.favicon_url);
            }
            // A main-frame address change carries a real URL. Title/loading
            // callbacks leave info.url empty, and MUST NOT touch the omnibox
            // — previously they set it from the (empty) security_origin field
            // on every title callback, which cleared the address bar the
            // moment a page finished loading. Now the omnibox only updates on
            // a genuine address change, and never while the user is editing.
            if (!info.url.empty()) {
                const std::string u = info.url;
                const bool is_internal =
                    core::parse_internal_url(u).has_value();
                // A new main-frame document: the page-lifetime blocked count
                // starts at zero, both engine-side (so the next block is #1,
                // not #37 across the session) and in the badge.
                if (webview_) webview_->reset_adblock_count(id);
                adblock_counts_.erase(id);
                if (shield_btn_) {
                    if (id == tabs_.active()) update_shield_for_tab();
                }
                // Keep the tab model's URL in sync so a saved session and an
                // omnibox-on-tab-switch show where the user actually is.
                tabs_.set_url(id, u);
                if (!is_internal) tabs_.record_web_navigation(id, u);
                if (!is_internal) {
                    // Keep the chrome-level stack's current web entry pointing
                    // at where the engine actually is, so backing out of an
                    // internal page later lands on the right page (and does not
                    // needlessly re-load it).
                    engine_url_[id] = u;
                    nav_stack(id).note_web_url(core::Url(u), info.title);
                    // Night mode follows the site, so re-decide on every
                    // main-frame address change.
                    apply_night_mode(id, u);
                }
                if (id == tabs_.active() && omnibox_ &&
                    !omnibox_->editing()) {
                    omnibox_->set_current_url(QString::fromStdString(u),
                                              false);
                }
                // Record a history visit for real web pages only: internal
                // pages (aurora://) and invalid URLs are excluded. This is
                // the fix for "history doesn't work" — visits were never
                // recorded because nothing called add_visit on navigation.
                if (services_.history && !is_internal) {
                    const core::Url visited(u);
                    // "view-source:" pages are a view of a page already in
                    // history, not a visit of their own.
                    if (visited.is_valid() && visited.scheme() != "view-source") {
                        services_.history->add_visit(
                            visited, info.title, std::string{}, false);
                    }
                }
            }
            // Hover-link URL goes to the status bubble. Empty status text
            // means the pointer left the link — let update_loading_state()
            // decide what (if anything) the bubble should say next.
            const bool had_hover_text = !info.status_text.empty();
            update_loading_state(id);
            if (id == tabs_.active() && had_hover_text) {
                set_status(QString::fromStdString(info.status_text));
            }
            update_nav_button_states();
            update_bookmark_star();
        });
#endif

    auto* central = new QWidget(this);
    central->setObjectName("Root");
    central->setMouseTracking(true);
    // The far-left launcher dock and the main column (chrome + content) sit
    // side by side; `root` keeps owning the vertical chrome stack inside the
    // column so everything below (setup_chrome, content, find bar) is
    // unchanged.
    auto* outer = new QHBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* main_column = new QWidget(central);
    main_column->setObjectName("MainColumn");
    auto* root = new QVBoxLayout(main_column);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    outer->addWidget(main_column, 1);

    // Chrome-style stacking order. The whole chrome plate (tab strip + toolbar)
    // is a child of the central layout, never a docked QToolBar (which would
    // always sit above everything and put the tabs underneath it).
    //   chrome plate (tab strip -> toolbar) -> content -> find bar
    setup_chrome(root);

    // Content area: one native host widget per tab, stacked, plus the shared
    // internal-page widgets.
    content_stack_ = new QStackedWidget(this);
    content_stack_->setObjectName("ContentStack");
    root->addWidget(content_stack_, 1);

    // Index 0 is an inert placeholder. QStackedWidget makes the first widget
    // added to an empty stack current, and promotes a sibling to current when
    // the current widget is removed; owning index 0 with something harmless
    // means neither can ever silently reveal (and thus create) a tab host.
    content_placeholder_ = new QWidget(content_stack_);
    content_placeholder_->setObjectName("ContentPlaceholder");
    content_stack_->addWidget(content_placeholder_);

    // Find bar (hidden by default, shown on Ctrl+F).
    find_bar_ = new FindBar(this);
    find_bar_->setObjectName("FindBarContainer");
    root->addWidget(find_bar_);
    find_bar_->hide();

    // The launcher dock rides the left edge of the whole window, beside both
    // the chrome and the content (see setup_left_dock).
    setup_left_dock(outer);

    setCentralWidget(central);

    // Docked DevTools panel (bottom dock). Hidden until the user opens DevTools.
    setup_devtools_dock();

    // Transient hover-link / load-progress / confirmation bubble. There is no
    // QStatusBar: nothing may reserve layout height for transient text.
    setup_status_bubble();

    // Menu button in the toolbar.
    setup_menu_button();

    // The bookmarks-bar preference was resolved in load_preferences(), before
    // the bar and the menu existed. Apply it now that both do.
    apply_bookmark_bar_visibility(bookmark_bar_visible_);

    // Observe the tab model so hosts are created/destroyed alongside tabs.
    tabs_.add_observer(this);

    // Re-read the cached preference on each tick: an existing window observes
    // changes made in any settings page without reopening tabs or a restart.
    // The model sends SuspendedChanged for both sleep and wake transitions.
    auto check_suspension = [this] {
        const int64_t minutes = services_.prefs
            ? services_.prefs->get_int(settings::PrefKeys::kTabSuspendAfterMinutes,
                                       settings::PrefDefaults::kTabSuspendAfterMinutes)
            : settings::PrefDefaults::kTabSuspendAfterMinutes;
        tabs_.set_suspend_after(std::chrono::minutes(minutes));
        (void)tabs_.tick();
    };
    auto* suspend_timer = new QTimer(this);
    suspend_timer->setInterval(15000);
    connect(suspend_timer, &QTimer::timeout, this, check_suspension);
    check_suspension();
    suspend_timer->start();

    // Seed one tab so the window is never empty.
    on_new_tab();

    setup_shortcuts();
    setup_connections();
    update_bookmark_star();

    // Initialize find bar connections.
    connect(find_bar_, &FindBar::search_requested, this,
            [this](const QString& query, bool forward) {
                auto active = tabs_.active();
                if (active.valid() && webview_) {
                    webview_->find(active, query.toStdString(), forward);
                }
            });
    connect(find_bar_, &FindBar::close_requested, this, [this] {
        find_bar_->hide();
        auto active = tabs_.active();
        if (active.valid() && webview_) {
            webview_->stop_find(active);
        }
    });
}

MainWindow::~MainWindow() {
#if defined(Q_OS_WIN)
    if (native_outside_filter_) qApp->removeNativeEventFilter(native_outside_filter_.get());
#endif
    // If closeEvent already ran, the popup is gone. If we are being deleted
    // without a close (e.g. the stack-allocated first window at process exit),
    // destroy THIS window's synthetic popup browser while webview_ is still
    // alive, then delete the widget so its destroyed() handler cannot run
    // after webview_ has been torn down. Never touches another window.
    if (extension_popup_) {
        extension_popup_->disconnect(this);
        if (webview_ && extension_popup_tab_.valid()) {
            webview_->destroy(extension_popup_tab_);
        }
        delete extension_popup_.data();
        extension_popup_ = nullptr;
        extension_popup_host_ = nullptr;
        extension_popup_tab_ = core::TabId(0);
        extension_popup_url_.clear();
        extension_popup_extension_id_.clear();
        extension_popup_owner_tab_ = core::TabId(0);
        extension_popup_age_.invalidate();
    }
    // Hosts parked in closing_hosts_ have no parent widget, so nothing else
    // would ever free them. Their web views are already closed (or are being
    // closed by WebViewController::shutdown() before the engine goes down).
    for (auto& [tab, host] : closing_hosts_) delete host;
    closing_hosts_.clear();
}

void MainWindow::apply_theme() {
    auto theme = dark_mode_ ? dark_theme() : light_theme();
    // Publish the active palette before the stylesheet is applied: the widgets
    // that paint themselves (Omnibox capsule, IconButton glyphs, TabSpinner,
    // CefHostWidget placeholder) read it through current_theme().
    set_current_theme(theme);
    setStyleSheet(StylesheetBuilder::build(theme));
    if (menu_button_) menu_button_->set_dark_mode(dark_mode_);
    if (tab_bar_) tab_bar_->set_theme_accent(theme.colors.accent);
    apply_extension_popup_theme();
}

void MainWindow::inherit_theme_from(const MainWindow* src) {
    if (!src || src == this) return;
    dark_mode_ = src->dark_mode_;
    apply_theme();
}

void MainWindow::apply_extension_popup_theme() {
    if (!extension_popup_) return;
    const auto& colors = current_theme().colors;
    extension_popup_->setStyleSheet(QStringLiteral(
        "QWidget#ExtensionPopup { background: %1; border: 1px solid %2; }")
        .arg(colors.surface.name(), colors.border.name()));
}

void MainWindow::open_new_window() {
    // Share the source window's profile (prefs / extensions / history) so the
    // new window loads the saved theme and the settings page can persist
    // changes. `new MainWindow()` used the empty ServiceBundle and fell back
    // to the light "system" theme — the frankenstein new-window bug.
    auto* w = new MainWindow(services_);
    w->setAttribute(Qt::WA_DeleteOnClose, true);
    w->inherit_theme_from(this);
    w->resize(size());
    if (!isMaximized() && !isFullScreen()) {
        w->move(pos() + QPoint(24, 24));
    }
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::open_incognito_window() {
    // The source window's profile pointers are shared read-only; the private
    // window's constructor nulls the write-capable ones (history / downloads /
    // session / extensions / adblock / bookmarks), and wraps prefs read-only.
    // See core::IncognitoPolicy and the MainWindow ctor.
    auto* w = new MainWindow(services_, /*parent=*/nullptr,
                             /*is_incognito=*/true);
    w->setAttribute(Qt::WA_DeleteOnClose, true);
    w->inherit_theme_from(this);
    w->resize(size());
    if (!isMaximized() && !isFullScreen()) {
        w->move(pos() + QPoint(24, 24));
    }
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::load_preferences() {
    // Defaults first, so a window without a profile still behaves sensibly.
    search_engine_ = std::string(settings::PrefDefaults::kSearchEngine);
    QString theme_pref = QString::fromUtf8(settings::PrefDefaults::kTheme.data(),
                                           static_cast<qsizetype>(
                                               settings::PrefDefaults::kTheme.size()));
    if (services_.prefs) {
        search_engine_ = services_.prefs->get_string(
            settings::PrefKeys::kSearchEngine, settings::PrefDefaults::kSearchEngine);
        theme_pref = QString::fromStdString(services_.prefs->get_string(
            settings::PrefKeys::kTheme, settings::PrefDefaults::kTheme));
        bookmark_bar_visible_ = services_.prefs->get_bool(
            settings::PrefKeys::kShowBookmarkBar, true);
    }
    apply_theme_preference(theme_pref);
    // Null-safe: preferences are loaded before the chrome exists at startup,
    // and the constructor re-applies the resolved value once the bar and the
    // menu are built.
    apply_bookmark_bar_visibility(bookmark_bar_visible_);
}

void MainWindow::apply_bookmark_bar_visibility(bool visible) {
    if (bookmark_bar_) bookmark_bar_->setVisible(visible);
    if (menu_button_) menu_button_->set_bookmark_bar_visible(visible);
}

void MainWindow::on_bookmark_bar_toggled() {
    bookmark_bar_visible_ = !bookmark_bar_visible_;
    apply_bookmark_bar_visibility(bookmark_bar_visible_);
    if (services_.prefs) {
        services_.prefs->set_bool(settings::PrefKeys::kShowBookmarkBar,
                                  bookmark_bar_visible_);
    }
    flash_status(bookmark_bar_visible_
                     ? QStringLiteral("Bookmarks bar shown")
                     : QStringLiteral("Bookmarks bar hidden"));
}

void MainWindow::reload_bookmark_bar() {
    if (bookmark_bar_ && bookmark_bar_->isVisible()) bookmark_bar_->reload();
}

void MainWindow::apply_theme_preference(const QString& preference) {
    if (preference == "dark") {
        dark_mode_ = true;
    } else if (preference == "light") {
        dark_mode_ = false;
    } else {
        // "system": follow the platform palette.
        dark_mode_ =
            QApplication::palette().color(QPalette::Window).lightness() < 128;
    }
    apply_theme();
}

std::string MainWindow::new_tab_url() const {
    // Every new tab opens the native aurora://home page: the aurora-glass
    // new-tab surface with the centered search pill. Its pill still searches
    // through the user's configured engine (google.com, bing.com, ...) —
    // HomePage asks MainWindow to project each query through the active
    // engine, so the search-engine setting keeps deciding where searches go;
    // only the page the user lands on is now in-app glass instead of a web
    // engine's front door.
    return core::internal_url(core::InternalPage::Home);
}

searchengines::ResolvedEngine MainWindow::resolved_search_engine() const {
    // In-memory only: the service's snapshot, never SQLite. A preference that
    // names a deleted custom engine resolves to DuckDuckGo.
    return services_.search_engines
        ? services_.search_engines->resolve(search_engine_)
        : searchengines::resolve_default_engine(search_engine_, {});
}

void MainWindow::open_site_initiated_tab(std::string url) {
    // Settings > Tabs & browsing > "Open tabs in new window": a link the site
    // opens in a new tab (target=_blank, window.open) gets its own browser
    // window instead, in the same privacy mode. Tor windows keep the tab: a
    // Tor window is only created after its proxy checks (open_tor_window).
    if (!url.empty() && !tor_ && services_.prefs &&
        services_.prefs->get_bool(settings::PrefKeys::kOpenTabsInNewWindow,
                                  settings::PrefDefaults::kOpenTabsInNewWindow)) {
        const core::Url target = core::parse_omnibox_input(url);
        if (target.is_valid()) {
            auto* w = new MainWindow(services_, /*parent=*/nullptr, incognito_);
            w->setAttribute(Qt::WA_DeleteOnClose, true);
            w->inherit_theme_from(this);
            w->resize(size());
            if (!isMaximized() && !isFullScreen()) {
                w->move(pos() + QPoint(24, 24));
            }
            w->show();
            w->raise();
            w->activateWindow();
            // The new window's seed tab becomes the link's page.
            w->navigate_active_tab(target);
            return;
        }
    }
    // CEF documents target_url as empty when the popup request does not name a
    // destination (window.open() with no arguments). TabManager treats an empty
    // URL as Aurora's configured new-tab/home page, which is not JavaScript's
    // about:blank semantics, so spell out the blank URL before opening.
    tabs_.open(url.empty() ? "about:blank" : url);
}

// Sticky status source (hover-link URL, load progress). Replaces whatever the
// bubble is saying; an empty string hides the bubble outright.
void MainWindow::set_status(const QString& text) {
    if (!status_bubble_) return;
    if (text.isEmpty()) {
        status_bubble_->hide();
        return;
    }
    status_bubble_->setText(text);
    status_bubble_->adjustSize();
    position_status_bubble();
    // While web content is fullscreen the whole chrome is hidden, so a status
    // update must not pop the bubble over the page.
    if (status_hidden_by_fullscreen_) {
        status_bubble_->hide();
    } else {
        status_bubble_->show();
        status_bubble_->raise();
    }
}

void MainWindow::flash_status(const QString& text, int msec) {
    if (!status_bubble_) return;
    set_status(text);
    // Restartable single-shot: a newer flash extends the visible window.
    status_timer_->start(msec);  // interval arg > timer's own interval
}

void MainWindow::setup_status_bubble() {
    status_bubble_ = new QLabel(centralWidget());
    status_bubble_->setObjectName("StatusBubble");
    status_bubble_->setTextInteractionFlags(Qt::NoTextInteraction);
    status_bubble_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // Paint the themed background/border/radius from the #StatusBubble QSS
    // instead of falling back to an opaque (black) default fill — this is
    // what left a black square behind the loading/hover status text.
    status_bubble_->setAttribute(Qt::WA_StyledBackground, true);
    status_bubble_->setAutoFillBackground(false);
    // Elide long hover URLs instead of smearing a huge bubble across the
    // content; the full target is still in the tooltip on the link itself.
    status_bubble_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    status_bubble_->hide();

    status_timer_ = new QTimer(this);
    status_timer_->setSingleShot(true);
    connect(status_timer_, &QTimer::timeout, this, [this] {
        if (status_bubble_) status_bubble_->hide();
    });
}

void MainWindow::position_status_bubble() {
    if (!status_bubble_ || !centralWidget()) return;
    auto* host = centralWidget();
    const int margin = 10;
    // Max width capped at ~60% of the window so a long hover URL never
    // covers the whole bottom edge.
    status_bubble_->setMaximumWidth(qMax(160, host->width() * 3 / 5));
    status_bubble_->adjustSize();
    int clear_above = 0;
    if (find_bar_ && !find_bar_->isHidden()) clear_above = find_bar_->height();
    const int x = margin;
    const int y = host->height() - margin - clear_above - status_bubble_->height();
    status_bubble_->move(x, qMax(margin, y));
}

void MainWindow::setup_chrome(QVBoxLayout* root) {
    // Unified chrome surface: tab strip on top, toolbar below — the layout a
    // modern browser uses, with the selected tab visually connecting to the
    // toolbar surface beneath it.
    auto* chrome = new QWidget(this);
    chrome->setObjectName("ChromeTop");
    chrome->setProperty("incognito", incognito_);
    chrome->setProperty("tor", tor_);
    auto* cl = new QVBoxLayout(chrome);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    // --- Tab strip row (also the window caption: draggable) -----------------
    auto* tab_row = new QWidget(chrome);
    tab_row->setObjectName("TabStrip");
    tab_row->setProperty("incognito", incognito_);
    tab_row->setProperty("tor", tor_);
    // Kept as a member so content fullscreen can hide the whole strip: the tab
    // bar, the new-tab button and the window controls are siblings inside it,
    // and hiding only the tab bar left a stray "+" floating over the video.
    tab_strip_ = tab_row;
    caption_ = tab_row;
    tab_row->setMouseTracking(true);
    tab_row->installEventFilter(this);
    auto* tl = new QHBoxLayout(tab_row);
    tl->setContentsMargins(Spacing::xs + Spacing::xxs, 0, 0, 0);
    tl->setSpacing(2);

    // Tab-search dropdown chevron at the FAR LEFT of the strip, in a subtle
    // rounded button — the reference layout's strip affordance.
    tab_search_btn_ = new IconButton(icons::Name::ChevronDown, this);
    tab_search_btn_->setObjectName("TabSearchButton");
    tab_search_btn_->setToolTip("Search tabs (Ctrl+Shift+A)");
    tab_search_btn_->setFixedSize(28, 28);
    tab_search_btn_->setIconPx(15);
    connect(tab_search_btn_, &IconButton::clicked, this,
            &MainWindow::show_tab_search_menu);
    tl->addWidget(tab_search_btn_);

    tab_bar_ = new TabBar(&tabs_, tab_row);
    tab_bar_->setObjectName("TabBar");
    tab_bar_->setProperty("incognito", incognito_);
    tab_bar_->setProperty("tor", tor_);
    // Maximum policy so the strip is only as wide as its tabs: the new-tab
    // button then hugs the last tab instead of being pushed to the far edge.
    tab_bar_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    tl->addWidget(tab_bar_);

    new_tab_btn_ = new IconButton(icons::Name::Plus, IconButton::Role::Primary, this);
    new_tab_btn_->setObjectName("NewTabButton");
    new_tab_btn_->setToolTip("New Tab (Ctrl+T)");
    new_tab_btn_->setFixedSize(30, 30);
    new_tab_btn_->setIconPx(16);
    connect(new_tab_btn_, &IconButton::clicked, this, &MainWindow::on_new_tab);
    tl->addWidget(new_tab_btn_);
    if (tor_) {
        auto* badge = new QLabel(QStringLiteral("Tor Incognito"), tab_row);
        badge->setObjectName("TorBadge");
        badge->setAccessibleName(QStringLiteral("Tor incognito window"));
        badge->setToolTip(QStringLiteral(
            "Private browsing through Tor (%1). Not the Tor Browser: see the "
            "README for what this does and does not protect against.")
                              .arg(QString::fromStdString(tor_proxy_)));
        tl->addWidget(badge);
    } else if (incognito_) {
        auto* badge = new QLabel(QStringLiteral("Incognito"), tab_row);
        badge->setObjectName("IncognitoBadge");
        badge->setAccessibleName(QStringLiteral("Incognito window"));
        badge->setToolTip(QStringLiteral(
            "Private browsing - history and settings changes are not saved"));
        tl->addWidget(badge);
    }
    // The remaining strip is the draggable caption (functional, not dead space);
    // the window controls fill the far-right corner.
    tl->addStretch(1);
    setup_window_controls(tl);
    cl->addWidget(tab_row);

    // --- Toolbar row --------------------------------------------------------
    // A plain widget, not a QToolBar: a docked toolbar always sits above the
    // central widget, which would push the tab strip below it.
    auto* toolbar = new QWidget(chrome);
    toolbar->setObjectName("ToolbarRow");
    toolbar->setProperty("incognito", incognito_);
    toolbar->setProperty("tor", tor_);
    toolbar_ = toolbar;
    auto* bl = new QHBoxLayout(toolbar);
    bl->setContentsMargins(Spacing::xs + Spacing::xxs, Spacing::xxs,
                           Spacing::xs + Spacing::xxs, Spacing::xs);
    bl->setSpacing(Spacing::xxs);
    toolbar_layout_ = bl;

    back_btn_ = new IconButton(icons::Name::Back, this);
    back_btn_->setObjectName("NavButton");
    back_btn_->setToolTip("Back (Alt+Left)");
    connect(back_btn_, &IconButton::clicked, this, &MainWindow::on_back);
    bl->addWidget(back_btn_);

    fwd_btn_ = new IconButton(icons::Name::Forward, this);
    fwd_btn_->setObjectName("NavButton");
    fwd_btn_->setToolTip("Forward (Alt+Right)");
    connect(fwd_btn_, &IconButton::clicked, this, &MainWindow::on_forward);
    bl->addWidget(fwd_btn_);

    reload_stop_btn_ = new IconButton(icons::Name::Reload, this);
    reload_stop_btn_->setObjectName("NavButton");
    reload_stop_btn_->setToolTip("Reload (F5)");
    connect(reload_stop_btn_, &IconButton::clicked, this, [this] {
        // When loading, act as Stop; otherwise Reload.
        auto active = tabs_.active();
        if (active.valid()) {
            const auto* t = tabs_.find(active);
            if (t && t->loading) on_stop();
            else on_reload();
        } else {
            on_reload();
        }
    });
    bl->addWidget(reload_stop_btn_);

    home_btn_ = new IconButton(icons::Name::Home, this);
    home_btn_->setObjectName("NavButton");
    home_btn_->setToolTip("Home");
    connect(home_btn_, &IconButton::clicked, this, &MainWindow::on_home);
    bl->addWidget(home_btn_);

    // The omnibox spans the toolbar between the navigation buttons and the
    // trailing controls (as in the mockup) instead of floating between two
    // flexible gaps.
    bl->addSpacing(Spacing::sm);

    omnibox_ = new Omnibox(this);
    omnibox_->setProperty("incognito", incognito_);
    omnibox_->setProperty("tor", tor_);
    omnibox_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bl->addWidget(omnibox_, 1);

    bl->addSpacing(Spacing::sm);

    // The downloads button, extensions button and menu button are appended
    // in setup_menu_button() (they need the services + connections).

    cl->addWidget(toolbar);

    // --- Bookmarks bar row --------------------------------------------------
    // The last band of the chrome plate: below the omnibox, above the content,
    // so the whole browser furniture stays one continuous surface. Built even
    // when the preference hides it, so the menu toggle is instant.
    bookmark_bar_ = new BookmarkBar(services_.bookmarks, chrome);
    connect(bookmark_bar_, &BookmarkBar::open_requested, this,
            [this](const core::Url& url, bool new_tab) {
                if (new_tab) open_url_in_new_tab(url);
                else navigate_active_tab(url);
            });
    // The bar has no idea what the window is showing, so the window answers
    // its "Add current page" request with the active tab's URL and title.
    connect(bookmark_bar_, &BookmarkBar::add_current_page_requested, this,
            [this] {
                if (!bookmark_bar_) return;
                if (active_tab_is_internal()) {
                    flash_status(QStringLiteral(
                        "Aurora pages cannot be added to the bookmarks bar"));
                    return;
                }
                const std::string url = active_tab_url();
                if (url.empty()) return;
                const auto* t = tabs_.find(tabs_.active());
                const std::string title =
                    (t && !t->title.empty()) ? t->title : url;
                bookmark_bar_->add_page(QString::fromStdString(title),
                                        QString::fromStdString(url));
                update_bookmark_star();
            });
    connect(bookmark_bar_, &BookmarkBar::manage_requested, this,
            [this] { show_page(core::InternalPage::Bookmarks); });
    connect(bookmark_bar_, &BookmarkBar::status_message, this,
            [this](const QString& msg) { flash_status(msg); });
    cl->addWidget(bookmark_bar_);

    root->addWidget(chrome);
}

void MainWindow::setup_left_dock(QHBoxLayout* outer) {
    // The aurora chrome keeps a slim launcher rail glued to the far-left
    // window edge: app shortcuts on top, the glowing settings gear pinned to
    // the bottom. The dock routes to the same internal pages as the browser
    // menu (chrome::kHome plus CorePages), so behavior stays single-sourced.
    left_dock_ = new LeftDock(this);
    const auto open_page = [this](core::InternalPage page) {
        show_page(page);
    };
    connect(left_dock_, &LeftDock::home_requested, this, [this, open_page] {
        open_page(core::InternalPage::Home);
    });
    connect(left_dock_, &LeftDock::history_requested, this, [this, open_page] {
        open_page(core::InternalPage::History);
    });
    connect(left_dock_, &LeftDock::downloads_requested, this, [this, open_page] {
        open_page(core::InternalPage::Downloads);
    });
    connect(left_dock_, &LeftDock::bookmarks_requested, this, [this, open_page] {
        open_page(core::InternalPage::Bookmarks);
    });
    connect(left_dock_, &LeftDock::extensions_requested, this, [this, open_page] {
        open_page(core::InternalPage::Extensions);
    });
    connect(left_dock_, &LeftDock::settings_requested, this, [this, open_page] {
        open_page(core::InternalPage::Settings);
    });

    // Pinned sites ("+" on the rail). Both lists start empty; the profile
    // preferences remember them between runs.
    dock_pins_ = load_shortcuts(services_.prefs,
                                settings::PrefKeys::kDockPins);
    left_dock_->set_pins(dock_pins_);
    const auto save_pins = [this] {
        save_shortcuts(services_.prefs, settings::PrefKeys::kDockPins,
                       dock_pins_);
        left_dock_->set_pins(dock_pins_);
    };
    connect(left_dock_, &LeftDock::add_requested, this,
            [this, save_pins](const QPoint& global_pos) {
        QMenu menu(this);
        QAction* pin_current = menu.addAction(
            QStringLiteral("Pin current page"));
        const std::string current = active_tab_url();
        const QString current_url = normalize_shortcut_url(
            QString::fromStdString(current));
        pin_current->setEnabled(!current_url.isEmpty() &&
                                !active_tab_is_internal());
        QAction* by_url = menu.addAction(QStringLiteral("Add site by address…"));
        QAction* chosen = menu.exec(global_pos);
        if (!chosen) return;
        Shortcut result;
        if (chosen == pin_current) {
            result.url = current_url;
            result.title = default_shortcut_title(current_url);
        } else if (chosen == by_url) {
            if (!prompt_shortcut(this, QStringLiteral("Add to sidebar"), {},
                                 &result)) {
                return;
            }
        } else {
            return;
        }
        dock_pins_.append(result);
        save_pins();
    });
    connect(left_dock_, &LeftDock::pin_open_requested, this,
            [this](const QString& url) {
                navigate_active_tab(
                    core::parse_omnibox_input(url.toStdString()));
            });
    connect(left_dock_, &LeftDock::pin_edit_requested, this,
            [this, save_pins](int index) {
        if (index < 0 || index >= dock_pins_.size()) return;
        Shortcut result;
        if (!prompt_shortcut(this, QStringLiteral("Edit sidebar site"),
                             dock_pins_[index], &result)) {
            return;
        }
        dock_pins_[index] = result;
        save_pins();
    });
    connect(left_dock_, &LeftDock::pin_remove_requested, this,
            [this, save_pins](int index) {
        if (index < 0 || index >= dock_pins_.size()) return;
        dock_pins_.removeAt(index);
        save_pins();
    });
    outer->insertWidget(0, left_dock_, 0);
}

void MainWindow::setup_window_controls(QHBoxLayout* tab_row) {
    auto make = [this](icons::Name n, const char* tip) {
        auto* b = new IconButton(n, this);
        b->setObjectName("WindowButton");
        b->setToolTip(tip);
        b->setFixedSize(58, 40);   // full strip height: reaches the top edge
        b->setIconPx(18);
        return b;
    };
    win_min_ = make(icons::Name::Minimize, "Minimize");
    win_max_ = make(icons::Name::Maximize, "Maximize");
    win_close_ = make(icons::Name::Close, "Close");
    win_close_->setObjectName("WindowCloseButton");
    connect(win_min_, &IconButton::clicked, this, &MainWindow::on_window_minimize);
    connect(win_max_, &IconButton::clicked, this,
            &MainWindow::on_window_toggle_maximize);
    connect(win_close_, &IconButton::clicked, this, &MainWindow::on_window_close);
    tab_row->addWidget(win_min_);
    tab_row->addWidget(win_max_);
    tab_row->addWidget(win_close_);
}

void MainWindow::update_window_control_icons() {
    if (win_max_) {
        win_max_->setIconName(isMaximized() ? icons::Name::Restore
                                            : icons::Name::Maximize);
    }
}

void MainWindow::on_window_minimize() {
    showMinimized();
}

void MainWindow::on_window_toggle_maximize() {
    if (isMaximized()) showNormal();
    else showMaximized();
    update_window_control_icons();
}

void MainWindow::on_window_close() {
    close();
}

bool MainWindow::is_caption_drag_point(const QPoint& global) const {
    // The caption row is NOT draggable while fullscreen (the page owns the
    // whole window then). It IS draggable while maximized — Windows uses
    // caption dragging to restore/move a maximized window.
    if (isFullScreen() || !caption_ || !caption_->isVisible()) return false;
    const QPoint caption_local = caption_->mapFromGlobal(global);
    if (!caption_->rect().contains(caption_local)) return false;

    QWidget* child = caption_->childAt(caption_local);
    // Bare caption background: the layout's stretch between the new-tab button
    // and the window controls, the leading content margin, and any padding the
    // stylesheet leaves around the children.
    if (!child) return true;

    // childAt() returns the DEEPEST visible descendant, so a point over the tab
    // bar's empty background may hit a descendant (a per-tab close button, a
    // spinner, a layout filler) rather than the tab bar itself. Comparing by
    // exact identity (child == tab_bar_) therefore misses those points and
    // falls through to "some child exists -> not draggable", leaving the whole
    // tab strip undraggable. Match the tab bar by ancestry instead, and let
    // is_interactive_at() make the real interactive/non-interactive call in
    // tab-bar-local coordinates.
    if (tab_bar_ && (child == tab_bar_ || tab_bar_->isAncestorOf(child))) {
        return !tab_bar_->is_interactive_at(tab_bar_->mapFromGlobal(global));
    }


    // Anything else is a genuinely interactive control (tab-search button,
    // new-tab button, the window minimize/maximize/close buttons, or a widget
    // living inside a tab) and must keep receiving clicks.
    return false;
}

void MainWindow::notify_native_outside_click(const QPoint& global_pos) {
    // This is invoked from the Windows native event filter, so the click that
    // triggered it landed on the native CEF child window (Qt's qApp eventFilter
    // never saw it). Mirror the click-outside-to-close check from eventFilter()
    // here, reusing the same "settled" gate so the popup does not flicker shut
    // the instant it opens.
    const bool popup_settled = extension_popup_ &&
                               extension_popup_age_.isValid() &&
                               extension_popup_age_.elapsed() > 200;
    if (!popup_settled) return;

    const QRect popup_rect(extension_popup_->geometry());
    if (popup_rect.contains(global_pos)) return;  // click inside the popup

    // Click on the extensions button is fine — toggle_popup handles closing
    // the popup itself (see the toggle_popup connection in the constructor).
    if (extensions_btn_) {
        const QRect btn_rect(extensions_btn_->mapToGlobal(QPoint(0, 0)),
                             extensions_btn_->size());
        if (btn_rect.contains(global_pos)) return;
    }
    close_extension_popup();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    const auto t = e->type();

    // Close the extension popup when the user clicks outside of it.
    // Qt::Tool doesn't auto-close like Qt::Popup, so we replicate that
    // behavior here: any mouse press that doesn't land inside the popup
    // (or the extensions button that opened it) closes the popup.
    // Everything below only dismisses a popup that has actually settled on
    // screen. Showing + activating the popup (a Qt::Tool window) produces its
    // own burst of activation/press events; acting on those closed the popup in
    // the same breath as opening it.
    const bool popup_settled = extension_popup_ &&
                               extension_popup_age_.isValid() &&
                               extension_popup_age_.elapsed() > 200;

    if (popup_settled && t == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        const QPoint global_pos = me->globalPosition().toPoint();

        // Is the click inside the popup itself?
        const QRect popup_rect(extension_popup_->geometry());
        bool inside_popup = popup_rect.contains(global_pos);

        // Is the click on the extensions button (toggling it is fine)?
        if (!inside_popup && extensions_btn_) {
            const QRect btn_rect(extensions_btn_->mapToGlobal(QPoint(0, 0)),
                                extensions_btn_->size());
            inside_popup = btn_rect.contains(global_pos);
        }

        if (!inside_popup) {
            close_extension_popup();
            // Don't consume the event — let the intended target handle it.
        }
    }

    // Close the extension popup when the whole APPLICATION loses focus.
    //
    // Deliberately NOT QEvent::WindowDeactivate: this filter is installed on
    // qApp, so it saw the browser window's own deactivation, historically
    // caused by activateWindow() on the freshly created popup (the popup is now
    // shown WA_ShowWithoutActivating, so it no longer steals activation at all,
    // but a deactivate-based dismissal would still be wrong) — the popup
    // killed itself the instant it opened, which is why clicking an extension
    // row looked like it did nothing. Clicks outside are handled by the press
    // branch above and Escape by the branch below; those cover the same intent
    // safely. Generic FocusOut is likewise avoided because the popup's native
    // CEF child fires focus transitions while the user is still interacting
    // with it.
    if (popup_settled && t == QEvent::ApplicationDeactivate) {
        close_extension_popup();
    }

    // Escape closes the extension popup, matching Chrome's behavior.
    if (extension_popup_ && t == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            close_extension_popup();
            return true;
        }
    }

    // Double-click the caption to maximize/restore. The press may land on any
    // widget in the window (the tab bar's own background is caption space too),
    // so the predicate — not the receiving object — decides.
    if (t == QEvent::MouseButtonDblClick) {
        auto* me = static_cast<QMouseEvent*>(e);
        auto* widget = qobject_cast<QWidget*>(obj);
        if (me->button() == Qt::LeftButton && widget &&
            widget->window() == this &&
            is_caption_drag_point(me->globalPosition().toPoint())) {
            drag_pending_ = false;
            on_window_toggle_maximize();
            return true;
        }
    }
    // Keep the floating status bubble pinned to the content area's
    // bottom-left as the window resizes and as the find bar opens/closes.
    if ((obj == centralWidget() && t == QEvent::Resize) ||
        (obj == find_bar_ && (t == QEvent::Show || t == QEvent::Hide ||
                              t == QEvent::Resize))) {
        position_status_bubble();
    }
    if (t == QEvent::MouseMove && drag_pending_) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->buttons() & Qt::LeftButton) {
            const QPoint g = me->globalPosition().toPoint();
            if ((g - drag_press_global_).manhattanLength() >
                QApplication::startDragDistance()) {
                drag_pending_ = false;
                if (auto* w = windowHandle()) {
                    w->startSystemMove();
                }
            }
        }
    }
    if (t == QEvent::MouseButtonRelease) {
        drag_pending_ = false;
    }
    if (t == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::LeftButton) {
            // ARM a move-threshold-gated drag on ALL platforms, including
            // Windows. This is a SAFETY NET for the native WM_NCHITTEST ->
            // HTCAPTION path: when that path works the press is delivered as a
            // non-client message and this Qt MouseButtonPress event never fires,
            // so the arm never happens and the native caption drag (with full
            // Aero Snap) owns the move. The fallback only engages when the
            // native path is NOT claiming the caption -- i.e. exactly the broken
            // case reported. Gated by a movement threshold so a plain click and
            // double-click-to-maximize are completely unaffected. The detailed
            // historical rationale in the next comment block is now obsolete
            // but retained for context.
            //
            // This block must NOT run on Windows. On Windows, Aero Snap comes
            // from the WM_NCHITTEST handler in nativeEvent() answering
            // HTCAPTION, which makes Windows itself perform the caption drag —
            // and a native caption drag is the only drag that engages Aero Snap
            // (drag-to-top maximize, snap left/right, drag-down restore).
            // Qt's startSystemMove() implements its own move loop that has no
            // Snap support, and because it runs from the client-area mouse
            // press it PRE-EMPTS the native caption drag entirely. So on
            // Windows this fallback is compiled out and the native hit-test
            // owns the drag.
            //
            // The press is matched by POSITION, not by receiving object: the
            // tab bar's empty background is caption space, and a press there is
            // delivered to the tab bar (a child), never to caption_ itself —
            // which is why the old `obj == caption_` test could not drag from
            // the very region the user complains about.
            if (!isFullScreen()) {
                auto* widget = qobject_cast<QWidget*>(obj);
                if (widget && widget->window() == this &&
                    is_caption_drag_point(me->globalPosition().toPoint())) {
                    drag_pending_ = true;
                    drag_press_global_ = me->globalPosition().toPoint();
                }
            }

#ifndef Q_OS_WIN
            // Edge resize for the frameless window (left/right/bottom only —
            // never the top edge, which is the tab strip / caption).
            if (!isFullScreen() && !isMaximized()) {
                if (auto* w = windowHandle()) {
                    auto* widget = qobject_cast<QWidget*>(obj);
                    if (widget && widget->window() == this) {
                        const QPoint gp = me->globalPosition().toPoint();
                        const QRect wr = geometry();
                        const int m = 6;
                        Qt::Edges edges;
                        if (gp.x() <= wr.left() + m) edges |= Qt::LeftEdge;
                        if (gp.x() >= wr.right() - m) edges |= Qt::RightEdge;
                        if (gp.y() >= wr.bottom() - m) edges |= Qt::BottomEdge;
                        if (edges) {
                            w->startSystemResize(edges);
                            return true;
                        }
                    }
                }
            }
#endif // Q_OS_WIN
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

#if defined(Q_OS_WIN)
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                            qintptr* result) {
    // FIRST-LINE PROBE (before the eventType guard): the earlier top-of-loop
    // probe was INSIDE the `eventType == "windows_generic_MSG"` guard, so if Qt
    // handed us a different eventType string it wrote nothing and we could not
    // tell "nativeEvent never called" from "called with unexpected type". This
    // probe runs unconditionally and logs the raw eventType, so the log now
    // answers that question directly.
    {
        static int s_entry = 0;
        if (s_entry < 3) {
            ++s_entry;
            QFile f(QStandardPaths::writableLocation(
                        QStandardPaths::DesktopLocation) +
                    QLatin1String("/aurora_drag.log"));
            if (f.open(QIODevice::Append | QIODevice::Text)) {
                f.write(QStringLiteral("nativeEvent ENTRY #%1 eventType=%2\n")
                            .arg(s_entry)
                            .arg(QString::fromUtf8(eventType))
                            .toUtf8());
                f.close();
            }
        }
    }
    // WM_NCHITTEST is the key to Aero Snap on a frameless window: Windows asks
    // "what is under this point?" and we answer with HTCAPTION for the empty
    // tab-strip space (so drag-to-maximize, snap-left/right, and drag-restore all
    // work natively) or HTCLIENT for interactive children (tabs, buttons, the
    // omnibox). The resize borders are also handled here so window-edge resize
    // works without startSystemResize (which would bypass the native caption).
    if ((eventType == "windows_generic_MSG" ||
         eventType == "windows_dispatcher_MSG") &&
        message && result) {
        auto* msg = static_cast<aurora_win::NativeMsg*>(message);

        // --- TEMP DIAGNOSTIC: which messages reach nativeEvent at all? ---
        // First 8 messages of ANY type. If these lines appear but "FIRST
        // WM_NCHITTEST" never does (even after moving the mouse over the
        // window), Qt is intercepting WM_NCHITTEST before the widget's
        // nativeEvent — that would be the drag root cause.
        {
            static int s_any = 0;
            if (s_any < 8) {
                ++s_any;
                QFile f(QStandardPaths::writableLocation(
                            QStandardPaths::DesktopLocation) +
                        QLatin1String("/aurora_drag.log"));
                if (f.open(QIODevice::Append | QIODevice::Text)) {
                    f.write(QStringLiteral("nativeEvent #%1 msg=0x%2\n")
                                .arg(s_any)
                                .arg(static_cast<qulonglong>(msg->message), 0, 16)
                                .toUtf8());
                    f.close();
                }
            }
        }
        // --- END TEMP DIAGNOSTIC ---

        // WM_NCCALCSIZE — make the client area fill the whole window so there is
        // no visible Windows title bar or resize border (the "frameless look"),
        // WHILE keeping the native WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX
        // styles that make Windows offer Aero Snap. This is the half of the fix
        // the old WM_NCHITTEST-only code was missing: returning HTCAPTION on a
        // popup window moves the window but Windows never snaps it.
        //
        // Returning 0 means "accept the proposed client rect as-is". When wParam
        // is TRUE the system has filled rgrc[0] with the proposed window rect;
        // leaving it and returning 0 makes client == window (frame + caption
        // removed). When the window is maximized the proposed rect is larger
        // than the work area by the resize-border thickness, so we inset the
        // client to keep content on-screen rather than bleeding past the
        // screen edges.
        if (msg->message == aurora_win::kWM_NCCALCSIZE) {
            const qintptr style =
                aurora_win::GetWindowLongPtrW(msg->hwnd, aurora_win::kGWL_STYLE);
            const bool maximized = (style & aurora_win::kWS_MAXIMIZE) != 0;
            if (msg->wParam != 0) {  // wParam == TRUE -> lParam is NCCALCSIZE_PARAMS*
                auto* p = reinterpret_cast<aurora_win::NcCalcSizeParams*>(
                    msg->lParam);
                if (maximized) {
                    const int bx = aurora_win::GetSystemMetrics(
                                        aurora_win::kSM_CXSIZEFRAME) +
                                   aurora_win::GetSystemMetrics(
                                        aurora_win::kSM_CXPADDEDBORDER);
                    const int by = aurora_win::GetSystemMetrics(
                                        aurora_win::kSM_CYSIZEFRAME) +
                                   aurora_win::GetSystemMetrics(
                                        aurora_win::kSM_CXPADDEDBORDER);
                    p->rgrc[0].left   += bx;
                    p->rgrc[0].top    += by;
                    p->rgrc[0].right  -= bx;
                    p->rgrc[0].bottom -= by;
                }
                // Non-maximized: leave rgrc[0] == proposed window rect so the
                // client fills the whole window (truly frameless; edge resize is
                // driven by WM_NCHITTEST below).
            }
            // wParam == FALSE: lParam is RECT* (the proposed window rect);
            // returning 0 accepts it as the client rect — also frameless.
            *result = 0;
            return true;
        }

        // WM_NCACTIVATE — with no non-client area to draw, return TRUE so Windows
        // does not repaint a default frame/border on focus changes (prevents the
        // 1px border flicker that otherwise appears on activation).
        if (msg->message == aurora_win::kWM_NCACTIVATE) {
            *result = 1;  // TRUE
            return true;
        }

        if (msg->message == aurora_win::kWM_NCHITTEST) {
            // One-shot breadcrumb: if dragging is ever reported broken again,
            // the log immediately answers "did the native hit-test engage at
            // all?" — the question that cost the most time here.
            if (!nchittest_seen_) {
                nchittest_seen_ = true;
                const qintptr style =
                    aurora_win::GetWindowLongPtrW(msg->hwnd, aurora_win::kGWL_STYLE);
                AURORA_LOG(util::LogLevel::Info, "ui")
                    << "WM_NCHITTEST reached MainWindow::nativeEvent"
                    << " (ws_thickframe="
                    << ((style & aurora_win::kWS_THICKFRAME) ? 1 : 0)
                    << ", ws_caption="
                    << ((style & aurora_win::kWS_CAPTION) ? 1 : 0)
                    << ", dpr=" << devicePixelRatio() << ")";
            }
            // WM_NCHITTEST's lParam is in native screen pixels, whereas Qt's
            // widget geometry is in device-independent coordinates. Dividing
            // it by this window's DPR is not a valid conversion on a mixed-DPI
            // desktop: the virtual screen has no single origin/scale, and the
            // point can still land *inside* the window but in the wrong child.
            // The previous "only use QCursor when the converted point misses
            // the window" fallback therefore returned HTCLIENT across much of
            // the real caption on 125%/150% displays.
            //
            // A pointer-generated WM_NCHITTEST (the case that starts caption
            // drag, Aero Snap and edge resize) is always for the current
            // pointer. QCursor::pos() is already expressed in Qt's global
            // coordinate system and Qt performs the correct per-monitor
            // conversion, so use it as the authoritative point end-to-end.
            const QPoint global = QCursor::pos();
            const QPoint local = mapFromGlobal(global);

            // --- TEMP DIAGNOSTIC (remove once drag is fixed) ---
            // This is a GUI-subsystem app, so std::clog (AURORA_LOG) is
            // invisible without --shell. Write a few records to a temp file
            // so one run answers: is WM_NCHITTEST firing at all? is WS_CAPTION
            // set? for points over the caption, what does childAt() find and
            // does is_caption_drag_point() return true?
            {
                static int s_first = 0;
                if (!s_first) {
                    s_first = 1;
                    const qintptr st =
                        aurora_win::GetWindowLongPtrW(msg->hwnd, aurora_win::kGWL_STYLE);
                    QFile f(QStandardPaths::writableLocation(
                                QStandardPaths::DesktopLocation) +
                            QLatin1String("/aurora_drag.log"));
                    if (f.open(QIODevice::Append | QIODevice::Text)) {
                        f.write(QStringLiteral(
                                    "FIRST WM_NCHITTEST style=0x%1 "
                                    "thickframe=%2 caption=%3 dpr=%4 hwnd=%5\n")
                                    .arg(QString::number(
                                        static_cast<qint64>(st), 16))
                                    .arg((st & aurora_win::kWS_THICKFRAME)
                                             ? QStringLiteral("1")
                                             : QStringLiteral("0"))
                                    .arg((st & aurora_win::kWS_CAPTION)
                                             ? QStringLiteral("1")
                                             : QStringLiteral("0"))
                                    .arg(static_cast<qreal>(devicePixelRatio()))
                                    .arg(static_cast<qulonglong>(
                                        reinterpret_cast<quintptr>(msg->hwnd)))
                                    .toUtf8());
                        f.close();
                    }
                }
                // Log the first N WM_NCHITTEST hits whose point is inside the
                // window, UNCONDITIONALLY (not gated on caption_->rect().contains).
                // A failed contains() must still show the mapped caption point, the
                // caption rect, and the child widget class — the data that
                // localizes a drag bug instead of hiding it behind the gate.
                if (caption_ && caption_->isVisible() && rect().contains(local)) {
                    static int s_cap = 0;
                    if (s_cap < 25) {
                        ++s_cap;
                        const QPoint cl = caption_->mapFromGlobal(global);
                        const QRect cr = caption_->rect();
                        const bool contains = cr.contains(cl);
                        QWidget* ch = caption_->childAt(cl);
                        const bool drag = is_caption_drag_point(global);
                        QFile f(QStandardPaths::writableLocation(
                                    QStandardPaths::DesktopLocation) +
                                QLatin1String("/aurora_drag.log"));
                        if (f.open(QIODevice::Append | QIODevice::Text)) {
                            f.write(QStringLiteral(
                                        "ht #%1 global=(%2,%3) local=(%4,%5) "
                                        "cap_local=(%6,%7) cap_rect=(%8,%9-%10,%11) "
                                        "contains=%12 child=%13:%14 drag=%15\n")
                                        .arg(s_cap)
                                        .arg(global.x()).arg(global.y())
                                        .arg(local.x()).arg(local.y())
                                        .arg(cl.x()).arg(cl.y())
                                        .arg(cr.x()).arg(cr.y())
                                        .arg(cr.right()).arg(cr.bottom())
                                        .arg(contains ? 1 : 0)
                                        .arg(ch ? QString::fromLatin1(ch->metaObject()->className()) : QStringLiteral("(null)"))
                                        .arg(ch ? ch->objectName() : QStringLiteral("(null)"))
                                        .arg(drag ? 1 : 0)
                                        .toUtf8());
                            f.close();
                        }
                    }
                }
            }
            // --- END TEMP DIAGNOSTIC ---

            // Outside the window: let Qt/Windows handle it.
            if (!rect().contains(local))
                return QMainWindow::nativeEvent(eventType, message, result);

            // Resize borders (left/right/bottom) — disabled while maximized or
            // fullscreen. The top edge is never a resize border: it is the tab
            // strip / caption.
            if (!isFullScreen() && !isMaximized()) {
                constexpr int kResizeMargin = 6;
                const QRect r = rect();

                const bool left = local.x() >= r.left() &&
                                  local.x() < r.left() + kResizeMargin;
                // The strip's right edge belongs to the window controls: the
                // very top-right pixel must hit Close, not a resize border.
                const bool in_strip =
                    caption_ && local.y() < caption_->height();
                const bool right = !in_strip && local.x() <= r.right() &&
                                   local.x() > r.right() - kResizeMargin;
                const bool bottom = local.y() <= r.bottom() &&
                                    local.y() > r.bottom() - kResizeMargin;

                if (left && bottom) {
                    *result = aurora_win::kHTBOTTOMLEFT;
                    return true;
                }
                if (right && bottom) {
                    *result = aurora_win::kHTBOTTOMRIGHT;
                    return true;
                }
                if (left) {
                    *result = aurora_win::kHTLEFT;
                    return true;
                }
                if (right) {
                    *result = aurora_win::kHTRIGHT;
                    return true;
                }
                if (bottom) {
                    *result = aurora_win::kHTBOTTOM;
                    return true;
                }
            }

            // Caption: the tab strip (caption_) is the draggable region. Empty
            // space (the layout stretch, or bare tab-strip background) returns
            // HTCAPTION so Windows treats the drag as a caption move — enabling
            // Aero Snap, drag-to-maximize, and drag-restore from maximized.
            // Points over interactive children (tabs, new-tab button, window
            // controls) fall through as HTCLIENT so they still receive clicks.
            //
            // caption_ is NOT treated as draggable while fullscreen (the page
            // owns the whole window then). It IS draggable while maximized —
            // Windows uses caption dragging to restore/move a maximized window.
            if (is_caption_drag_point(global)) {
                *result = aurora_win::kHTCAPTION;
                return true;
            }
        }

        // --- TEMP DIAGNOSTIC: drag-start / move-loop messages ---
        // If WM_NCHITTEST returns HTCAPTION but the window still doesn't move,
        // these reveal whether Windows ever started the native caption drag
        // (WM_NCLBUTTONDOWN with wParam==HTCAPTION, then WM_SYSCOMMAND with
        // SC_MOVE, then WM_ENTERSIZEMOVE). Their absence while drag=1 is the
        // smoking gun that HTCAPTION is being ignored.
        if (msg->message == 0x00A1 /*WM_NCLBUTTONDOWN*/ ||
            msg->message == 0x0112 /*WM_SYSCOMMAND*/ ||
            msg->message == 0x0231 /*WM_ENTERSIZEMOVE*/ ||
            msg->message == 0x0232 /*WM_EXITSIZEMOVE*/) {
            static int s_msg = 0;
            if (s_msg < 40) {
                ++s_msg;
                const char* name =
                    msg->message == 0x00A1 ? "WM_NCLBUTTONDOWN" :
                    msg->message == 0x0112 ? "WM_SYSCOMMAND" :
                    msg->message == 0x0231 ? "WM_ENTERSIZEMOVE" :
                                            "WM_EXITSIZEMOVE";
                QFile f(QStandardPaths::writableLocation(
                            QStandardPaths::DesktopLocation) +
                        QLatin1String("/aurora_drag.log"));
                if (f.open(QIODevice::Append | QIODevice::Text)) {
                    f.write(QStringLiteral("msg #%1 %2 wParam=0x%3 lParam=0x%4\n")
                                .arg(s_msg)
                                .arg(QString::fromLatin1(name))
                                .arg(static_cast<qulonglong>(msg->wParam), 0, 16)
                                .arg(static_cast<qlonglong>(msg->lParam), 0, 16)
                                .toUtf8());
                    f.close();
                }
            }
        }
        // --- END TEMP DIAGNOSTIC ---
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#else
// Declared for every platform in MainWindow.hpp; off Windows there is no
// native hit-test to do (eventFilter's startSystemMove/startSystemResize
// fallback handles the frameless window), so defer to Qt. Without this
// definition the Linux/macOS link failed with an undefined vtable entry.
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                             qintptr* result) {
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif // Q_OS_WIN



void MainWindow::setup_menu_button() {
    // Right of the omnibox, in the aurora reference order: downloads tray,
    // Aurora Shields, the puzzle-piece extensions button, the rounded
    // profile avatar, then the hamburger.
    downloads_btn_ = new IconButton(icons::Name::Downloads, this);
    downloads_btn_->setObjectName("NavButton");
    downloads_btn_->setToolTip("Downloads");
    // A dropdown popover of recent downloads, not a page navigation (the
    // full history is one click away in the popover's footer).
    connect(downloads_btn_, &IconButton::clicked, this,
            &MainWindow::show_downloads_popover);
    if (toolbar_layout_) toolbar_layout_->addWidget(downloads_btn_);

    shield_btn_ = new ShieldButton(this);
    shield_btn_->setObjectName("NavButton");
    // Honest UI: a build without the Rust engine must say so in the panel
    // instead of counting requests it never blocked.
    shield_btn_->set_engine_available(adblock::engine_available());
    if (toolbar_layout_) toolbar_layout_->addWidget(shield_btn_);
    connect(shield_btn_, &IconButton::clicked, shield_btn_,
            &ShieldButton::toggle_popup);
    // Per-site toggle from the panel: persist it, push it to the engine, and
    // reload the page so the change takes effect on what the user is looking
    // at (network rules apply from the next request on, and a page full of
    // already-loaded ads would otherwise look unchanged).
    connect(shield_btn_, &ShieldButton::site_blocking_toggled, this,
            [this](bool enabled) {
                set_site_blocking(enabled);
                if (webview_ && tabs_.active().valid()) {
                    webview_->reload(tabs_.active(), false);
                }
            });

    // Extensions sit to the LEFT of the hamburger, the same place Chrome puts
    // the puzzle-piece button, so opening an extension's options is a single
    // click instead of a trip to a settings page.
    extensions_btn_ = new ExtensionsButton(services_.extensions, this);
    if (toolbar_layout_) toolbar_layout_->addWidget(extensions_btn_);
    connect(extensions_btn_, &IconButton::clicked, extensions_btn_,
            [this]() {
                // Opening the extensions dropdown dismisses any open
                // extension popup so the two surfaces never stack — the
                // popup (a separate CEF browser window) would otherwise sit
                // underneath the dropdown.
                if (extension_popup_) close_extension_popup();
                extensions_btn_->toggle_popup();
            });
    connect(extensions_btn_, &ExtensionsButton::manage_requested, this,
            [this] { show_page(core::InternalPage::Extensions); });
    connect(extensions_btn_, &ExtensionsButton::open_extension_requested, this,
            [this](const QString& url) {
                open_url_in_new_tab(core::Url(url.toStdString()));
            });
    // Extension popups (default_popup in the manifest) open as a small
    // anchored popup window instead of a full tab — the mini dropdown the
    // user expects, matching Chrome's puzzle-piece popup behavior.
    connect(extensions_btn_, &ExtensionsButton::popup_requested, this,
            [this](const QString& extension_id, const QString& popup_url) {
                open_extension_popup(popup_url, extension_id);
            });

    // The aurora chrome keeps a rounded profile/avatar affordance between the
    // extensions puzzle and the hamburger menu. There is no profile surface
    // yet, so it opens the About page — the same place the app introduces
    // itself.
    profile_btn_ = new IconButton(icons::Name::User, this);
    profile_btn_->setObjectName("NavButton");
    profile_btn_->setToolTip(
        QStringLiteral("Aurora profile"));
    connect(profile_btn_, &IconButton::clicked, this, [this] {
        show_page(core::InternalPage::Settings);
    });
    if (toolbar_layout_) toolbar_layout_->addWidget(profile_btn_);

    menu_button_ = new BrowserMenu(this);
    // Insert the menu button at the end of the toolbar row.
    if (toolbar_layout_) toolbar_layout_->addWidget(menu_button_);
    connect(menu_button_, &IconButton::clicked, menu_button_,
            &BrowserMenu::toggle_popup);
    connect(menu_button_, &BrowserMenu::new_incognito_window_requested, this,
            &MainWindow::open_incognito_window);
    connect(menu_button_, &BrowserMenu::new_tor_window_requested, this,
            &MainWindow::open_tor_window);
    connect(menu_button_, &BrowserMenu::reader_mode_requested, this,
            &MainWindow::on_reader_mode);
    connect(menu_button_, &BrowserMenu::night_mode_toggled, this,
            &MainWindow::on_toggle_night_mode);
    connect(menu_button_, &BrowserMenu::screenshot_visible_requested, this,
            [this] { on_screenshot(0); });
    connect(menu_button_, &BrowserMenu::screenshot_full_page_requested, this,
            [this] { on_screenshot(1); });
    connect(menu_button_, &BrowserMenu::screenshot_region_requested, this,
            [this] { on_screenshot(2); });
    connect(menu_button_, &BrowserMenu::share_qr_requested, this,
            &MainWindow::on_share_qr);
    // The menu button was created after the first apply_theme(), so hand it the
    // current mode now (it tints its icons from the active theme).
    menu_button_->set_dark_mode(dark_mode_);

    connect(menu_button_, &BrowserMenu::new_tab_requested, this, &MainWindow::on_new_tab);
    connect(menu_button_, &BrowserMenu::new_window_requested, this,
            &MainWindow::open_new_window);
    connect(menu_button_, &BrowserMenu::find_requested, this, &MainWindow::on_find);
    connect(menu_button_, &BrowserMenu::print_requested, this, &MainWindow::on_print);
    connect(menu_button_, &BrowserMenu::save_as_pdf_requested, this,
            &MainWindow::on_save_as_pdf);
    connect(menu_button_, &BrowserMenu::fullscreen_requested, this, &MainWindow::on_fullscreen_toggle);
    connect(menu_button_, &BrowserMenu::devtools_requested, this, &MainWindow::on_devtools);
    connect(menu_button_, &BrowserMenu::bookmark_page_requested, this, &MainWindow::on_bookmark_page);
    connect(menu_button_, &BrowserMenu::bookmark_bar_toggled, this,
            &MainWindow::on_bookmark_bar_toggled);
    connect(menu_button_, &BrowserMenu::theme_toggled, this, &MainWindow::on_theme_toggled);
    connect(menu_button_, &BrowserMenu::zoom_in_requested, this, &MainWindow::on_zoom_in);
    connect(menu_button_, &BrowserMenu::zoom_out_requested, this, &MainWindow::on_zoom_out);
    connect(menu_button_, &BrowserMenu::zoom_reset_requested, this, &MainWindow::on_zoom_reset);
    connect(menu_button_, &BrowserMenu::history_requested, this,
            [this] { show_page(core::InternalPage::History); });
    connect(menu_button_, &BrowserMenu::downloads_requested, this,
            [this] { show_page(core::InternalPage::Downloads); });
    connect(menu_button_, &BrowserMenu::bookmarks_requested, this,
            [this] { show_page(core::InternalPage::Bookmarks); });
    connect(menu_button_, &BrowserMenu::settings_requested, this,
            [this] { show_page(core::InternalPage::Settings); });
    connect(menu_button_, &BrowserMenu::extensions_requested, this,
            [this] { show_page(core::InternalPage::Extensions); });
    connect(menu_button_, &BrowserMenu::site_settings_requested, this,
            [this] { show_page(core::InternalPage::SiteSettings); });
    connect(menu_button_, &BrowserMenu::clear_browsing_data_requested, this,
            &MainWindow::on_clear_browsing_data);
}

void MainWindow::setup_shortcuts() {
    // Two rules keep this table working:
    //
    // 1) ONE owner per key. The browser menu and the tab context menu used to
    //    register these same keys on their QActions while they were bound
    //    here too. Two active bindings for one key are ambiguous to Qt's
    //    shortcut map, and an ambiguous shortcut fires NEITHER handler — that
    //    is why most of these accelerators silently did nothing from a page.
    //    The menus now keep their keycaps display-only (their action shortcut
    //    context is narrowed to the menu widget itself, which is hidden).
    //
    // 2) Qt::WindowShortcut everywhere (set explicitly). The binding fires
    //    whenever any child of THIS window has focus (omnibox, find bar, tab
    //    strip, an internal page), stays quiet while one of our popups or
    //    dialogs is active (menus and the downloads popover handle their own
    //    keys), and — unlike the old F11/Escape ApplicationShortcut pair —
    //    never turns ambiguous when a second browser window registers the
    //    same keys.
    //    When the native CEF child window owns the keyboard, Qt sees no key
    //    events at all; those keys arrive through forward_engine_key() instead
    //    (the CefKeyboardHandler bridge), which dispatches the same actions.
    auto bind = [this](const char* seq, auto slot) {
        auto* s = new QShortcut(QKeySequence(QString::fromLatin1(seq)), this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, slot);
    };

    // Tabs & windows.
    bind("Ctrl+T", &MainWindow::on_new_tab);
    bind("Ctrl+W", &MainWindow::on_close_tab);
    bind("Ctrl+Shift+T", &MainWindow::on_reopen_closed);
    bind("Ctrl+N", &MainWindow::open_new_window);  // mirrors the browser menu
    bind("Ctrl+Shift+N", &MainWindow::open_incognito_window);
    bind("Ctrl+Alt+Shift+N", &MainWindow::open_tor_window);
    bind("Ctrl+Tab", &MainWindow::on_next_tab);
    bind("Ctrl+Shift+Tab", &MainWindow::on_prev_tab);
    // Qt's spelling is "PgDown"/"PgUp": "Ctrl+PageDown" parses to
    // Key_unknown and silently registers a dead shortcut.
    bind("Ctrl+PgDown", &MainWindow::on_next_tab);
    bind("Ctrl+PgUp", &MainWindow::on_prev_tab);
    bind("Ctrl+Shift+A", &MainWindow::show_tab_search_menu);

    // Navigation.
    bind("Ctrl+L", &MainWindow::on_focus_omnibox);
    bind("F5", &MainWindow::on_reload);
    bind("Ctrl+R", &MainWindow::on_reload);
    bind("Ctrl+Shift+R", [this] {
        if (tabs_.active().valid() && webview_)
            webview_->reload(tabs_.active(), true);  // bypass cache
    });
    bind("Alt+Left", &MainWindow::on_back);
    bind("Alt+Right", &MainWindow::on_forward);
    bind("Alt+Home", &MainWindow::on_home);

    // Ctrl+J opens the full downloads PAGE (like Chrome's Ctrl+J), not the
    // dropdown popover. The toolbar button still shows the dropdown.
    bind("Ctrl+J", [this] { show_page(core::InternalPage::Downloads); });
    // Ctrl+D toggles the omnibox star (bookmark / un-bookmark this page), the
    // same action as clicking the star or "Bookmark This Page" in the menu.
    // macOS: Qt maps "Ctrl" in a QKeySequence to the Command key, so this one
    // binding is Cmd+D there; the engine-side path (forward_engine_key) folds
    // EVENTFLAG_COMMAND_DOWN into `ctrl`, so Cmd+D also works while a page
    // owns the keyboard. Internal aurora:// pages are a no-op by design.
    bind("Ctrl+D", &MainWindow::on_bookmark_page);
    bind("Ctrl+F", &MainWindow::on_find);
    bind("Ctrl+P", &MainWindow::on_print);

    // Page tools: reader mode, screenshots, night mode (content only).
    bind("Ctrl+Alt+R", &MainWindow::on_reader_mode);
    bind("Ctrl+Shift+S", [this] { on_screenshot(0); });
    bind("Ctrl+Alt+Shift+S", [this] { on_screenshot(1); });
    bind("Ctrl+Alt+S", [this] { on_screenshot(2); });
    bind("Ctrl+Alt+N", &MainWindow::on_toggle_night_mode);

    // Page presentation.
    bind("Ctrl++", &MainWindow::on_zoom_in);
    bind("Ctrl+=", &MainWindow::on_zoom_in);
    bind("Ctrl+-", &MainWindow::on_zoom_out);
    bind("Ctrl+0", &MainWindow::on_zoom_reset);
    // F11 was bound twice — once at window context and once at application
    // context — which made it ambiguous and dead. One binding is enough.
    bind("F11", &MainWindow::on_fullscreen_toggle);
    bind("Ctrl+Shift+I", &MainWindow::on_devtools);
    bind("F12", &MainWindow::on_devtools);

    // Escape, priority ordered. This binding pre-empts the widgets' own
    // Escape handling (a QShortcut fires before key events reach the focus
    // widget), so their gestures are re-created here in order:
    //   1. leave the omnibox (its edit blurs on Escape),
    //   2. close the find bar (its edit emits close_requested on Escape),
    //   3. leave fullscreen.
    // Menus/popovers and modal dialogs handle Escape themselves: they own the
    // active window while open, so this window-context binding stays out of
    // their way.
    bind("Escape", [this] {
        if (omnibox_) {
            if (auto* fw = QApplication::focusWidget()) {
                for (auto* p = fw; p; p = p->parentWidget()) {
                    if (p == omnibox_) {
                        fw->clearFocus();
                        return;
                    }
                }
            }
        }
        if (!find_bar_->isHidden()) { on_find_close(); return; }
        if (fullscreen_tab_.valid() || isFullScreen()) on_fullscreen_toggle();
    });
}

bool MainWindow::forward_engine_key(const webview::EngineKeyEvent& ev) {
    // A CEF-forwarded key (Windows virtual-key code + held modifiers), observed
    // by the engine BEFORE the page sees it because the native child window,
    // not any Qt widget, owns the keyboard while a page is focused. We map
    // only KNOWN browser accelerators to their actions and dispatch the action
    // on the next event-loop turn (QTimer::singleShot(0)) -- never inline,
    // because the slots create/close engines and re-entering CEF's key
    // dispatch would be unsafe. Returning true consumes the key engine-side so
    // the page never sees it (Ctrl+T must never type into the page).
    //
    // We dispatch the action ourselves rather than synthesizing a QKeyEvent
    // and relying on Qt's QShortcut map to resolve it: while the native child
    // window owns focus, QApplication::focusWidget() is null, and Qt's
    // QShortcut matching against a synthetic event is not a reliable contract
    // then -- a posted QKeyEvent can be delivered without ever firing the
    // bound QShortcut. Calling the same handler methods setup_shortcuts()
    // binds keeps the behavior identical and is the reliable path.
    if (!ev.key_down) return false;
    const bool ctrl = ev.ctrl;
    const bool shift = ev.shift;
    const int k = ev.key_code;

    // Windows virtual-key codes. CEF populates event.windows_key_code
    // identically on every platform (it is the Chromium key code), so one
    // table covers all.
    constexpr int VK_T = 0x54, VK_W = 0x57, VK_N = 0x4E, VK_R = 0x52,
                  VK_L = 0x4C, VK_J = 0x4A, VK_H = 0x48, VK_D = 0x44,
                  VK_F = 0x46, VK_A = 0x41, VK_I = 0x49, VK_P = 0x50,
                  VK_TAB = 0x09, VK_S = 0x53,
                  VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_HOME = 0x24,
                  VK_LEFT = 0x25, VK_RIGHT = 0x27,
                  VK_F5 = 0x74, VK_F11 = 0x7A, VK_F12 = 0x7B,
                  VK_ESCAPE = 0x1B, VK_OEM_PLUS = 0xBB, VK_OEM_MINUS = 0xBD,
                  VK_0 = 0x30, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D;

    auto post = [this](auto fn) { QTimer::singleShot(0, this, fn); };

    // Escape: only consume when something in the chain would act (find bar open
    // or a tab is fullscreen). Otherwise the page keeps Escape (YouTube, games).
    auto escape_chain = [this] {
        if (!find_bar_->isHidden()) { on_find_close(); return; }
        if (fullscreen_tab_.valid() || isFullScreen()) on_fullscreen_toggle();
    };

    if (ctrl && !shift && !ev.alt) {
        switch (k) {
            case VK_T: post(&MainWindow::on_new_tab); return true;
            case VK_W: post(&MainWindow::on_close_tab); return true;
            case VK_N: post(&MainWindow::open_new_window); return true;
            case VK_R: post(&MainWindow::on_reload); return true;
            case VK_L: post(&MainWindow::on_focus_omnibox); return true;
            case VK_J: post([this] { show_page(core::InternalPage::Downloads); }); return true;
            case VK_H: post([this] { show_page(core::InternalPage::History); }); return true;
            case VK_D: post(&MainWindow::on_bookmark_page); return true;
            case VK_F: post(&MainWindow::on_find); return true;
            case VK_P: post(&MainWindow::on_print); return true;
            case VK_0: post(&MainWindow::on_zoom_reset); return true;
            case VK_OEM_PLUS: case VK_ADD: post(&MainWindow::on_zoom_in); return true;
            case VK_OEM_MINUS: case VK_SUBTRACT: post(&MainWindow::on_zoom_out); return true;
            case VK_TAB: post(&MainWindow::on_next_tab); return true;
            case VK_NEXT: post(&MainWindow::on_next_tab); return true;   // Ctrl+PageDown
            case VK_PRIOR: post(&MainWindow::on_prev_tab); return true;  // Ctrl+PageUp
        }
    } else if (ctrl && shift && !ev.alt) {
        switch (k) {
            case VK_T: post(&MainWindow::on_reopen_closed); return true;
            case VK_N: post(&MainWindow::open_incognito_window); return true;
            case VK_I: post(&MainWindow::on_devtools); return true;
            case VK_A: post(&MainWindow::show_tab_search_menu); return true;
            // Ctrl++ is physically Ctrl+Shift+= on a US layout (VK_OEM_PLUS),
            // so zoom-in must be claimed from the shifted branch too.
            case VK_OEM_PLUS: case VK_ADD: post(&MainWindow::on_zoom_in); return true;
            case VK_R: post([this] {
                if (tabs_.active().valid() && webview_)
                    webview_->reload(tabs_.active(), true);  // bypass cache
            }); return true;
            case VK_TAB: post(&MainWindow::on_prev_tab); return true;
            case VK_S: post([this] { on_screenshot(0); }); return true;
        }
    } else if (ctrl && ev.alt) {
        // Ctrl+Alt page tools (see setup_shortcuts). Only claimed for the
        // letters Aurora binds, so AltGr text input on other keys is untouched.
        switch (k) {
            case VK_R: if (shift) break; post(&MainWindow::on_reader_mode); return true;
            case VK_S: post([this, shift] { on_screenshot(shift ? 1 : 2); }); return true;
            case VK_N:
                if (shift) post(&MainWindow::open_tor_window);
                else post(&MainWindow::on_toggle_night_mode);
                return true;
        }
    } else if (ev.alt && !ctrl && !shift) {
        // Alt combos (back/forward/home) are Qt shortcuts when a Qt widget has
        // focus; while the page owns the keyboard we forward them too.
        switch (k) {
            case VK_LEFT: post(&MainWindow::on_back); return true;
            case VK_RIGHT: post(&MainWindow::on_forward); return true;
            case VK_HOME: post(&MainWindow::on_home); return true;
        }
    } else if (!ctrl && !shift && !ev.alt) {
        // Unmodified function keys.
        switch (k) {
            case VK_F5: post(&MainWindow::on_reload); return true;
            case VK_F11: post(&MainWindow::on_fullscreen_toggle); return true;
            case VK_F12: post(&MainWindow::on_devtools); return true;
            case VK_ESCAPE: {
                // With the extension popup's CEF child focused, Qt sees no
                // key events at all, so the qApp eventFilter's Escape branch
                // can never fire — close the popup from the engine bridge
                // instead (Chrome behavior: Escape dismisses the popup).
                if (extension_popup_) {
                    post([this] { close_extension_popup(); });
                    return true;
                }
                const bool acted = !find_bar_->isHidden() ||
                                   fullscreen_tab_.valid() || isFullScreen();
                if (acted) { post(escape_chain); return true; }
                return false;
            }
        }
    }
    return false;
}

void MainWindow::setup_connections() {
    connect(omnibox_, &Omnibox::navigate_requested, this,
            [this](const core::Url& url) { navigate_active_tab(url); });

    // Address-bar suggestions: history, bookmarks and open tabs plus the
    // default engine's completions. Parented to the window; torn down with it.
    {
        OmniboxSuggestController::Sources sources;
        sources.history = services_.history;
        sources.bookmarks = services_.bookmarks;
        sources.prefs = services_.prefs;
        sources.tabs = &tabs_;
        sources.engine = [this] { return resolved_search_engine(); };
        omnibox_suggest_ = new OmniboxSuggestController(omnibox_, std::move(sources), this);
    }
    // A suggested query is searched verbatim, even if it looks like an address.
    connect(omnibox_, &Omnibox::search_requested, this, [this](const QString& terms) {
        const auto engine = resolved_search_engine();
        navigate_active_tab(core::Url(
            searchengines::expand_template(engine.url_template, terms.toStdString())));
    });
    connect(omnibox_, &Omnibox::switch_tab_requested, this, [this](quint64 id) {
        const core::TabId tab(id);
        if (tabs_.contains(tab)) tabs_.set_active(tab);
    });
    // Trailing star in the omnibox toggles a bookmark for the current page.
    connect(omnibox_, &Omnibox::bookmark_requested, this,
            &MainWindow::on_bookmark_page);

    // Tab bar context menu signals.
    connect(tab_bar_, &TabBar::context_menu_duplicate, this, &MainWindow::on_duplicate_tab);
    connect(tab_bar_, &TabBar::context_menu_close, this, &MainWindow::on_close_tab_at);
    connect(tab_bar_, &TabBar::context_menu_close_others, this,
            &MainWindow::on_close_other_tabs);
    connect(tab_bar_, &TabBar::hover_dwell, this,
            &MainWindow::show_tab_hover_card);
    connect(tab_bar_, &TabBar::hover_dwell_cancelled, this,
            &MainWindow::hide_tab_hover_card);
    connect(tab_bar_, &TabBar::context_menu_pin, this, &MainWindow::on_pin_tab);
    connect(tab_bar_, &TabBar::context_menu_mute, this, &MainWindow::on_mute_tab);
    connect(tab_bar_, &TabBar::context_menu_reload, this, &MainWindow::on_reload_tab);
    connect(tab_bar_, &TabBar::context_menu_suspend, this, &MainWindow::on_suspend_tab);
    connect(tab_bar_, &TabBar::context_menu_restore, this, &MainWindow::on_restore_tab);
    connect(tab_bar_, &TabBar::context_menu_exclude_suspension, this,
            &MainWindow::on_exclude_tab_from_suspension);
    connect(tab_bar_, &TabBar::context_menu_copy_url, this, &MainWindow::on_copy_tab_url);
    connect(tab_bar_, &TabBar::context_menu_reopen_closed, this, &MainWindow::on_reopen_closed);
    connect(tab_bar_, &TabBar::external_tab_dropped, this,
            [this](TabBar* source_bar, core::TabId id, int index) {
                if (!source_bar || index < 0) return;
                auto* source = qobject_cast<MainWindow*>(source_bar->window());
                if (!source || source == this) return;
                source->transfer_tab_to(*this, id, static_cast<size_t>(index));
            });
    connect(tab_bar_, &TabBar::tab_tear_off_requested, this,
            [this](core::TabId id, const QPoint& global,
                   const QPoint& grab_offset) {
                tear_off_tab(id, global, grab_offset);
            });
    if (tab_bar_->metaObject()->indexOfSignal("new_tab_requested()") >= 0) {
        connect(tab_bar_, SIGNAL(new_tab_requested()), this, SLOT(on_new_tab()));
    }
}

std::optional<MainWindow::TransferredTab>
MainWindow::release_tab_for_transfer(core::TabId id) {
    const auto* tab = tabs_.find(id);
    if (!tab) return std::nullopt;

    TransferredTab transfer;
    transfer.state = *tab;
    transfer.page = page_of(id);
    if (const auto nav = nav_stacks_.find(id); nav != nav_stacks_.end()) {
        transfer.chrome_nav = nav->second;
    }

    // DevTools and content fullscreen are tied to this top-level native window;
    // close/leave them before the host window moves away.
    if (devtools_tab_ == id) close_devtools_dock();
    if (fullscreen_tab_ == id && webview_) webview_->exit_fullscreen(id);

    auto detached = tabs_.detach(id);
    if (!detached) return std::nullopt;
    // detach() returns the authoritative state that was actually removed.
    transfer.state = std::move(*detached);
    return transfer;
}

bool MainWindow::adopt_transferred_tab(TransferredTab transfer, size_t index,
                                       bool replace_seed) {
    if (!transfer.state.id.valid() || tabs_.contains(transfer.state.id)) {
        return false;
    }

    if (replace_seed && tabs_.count() == 1) {
        const core::TabId seed = tabs_.tabs().front().id;
        // This window is still hidden, so the seed host has never created a CEF
        // browser. Detach rather than close so Ctrl+Shift+T cannot resurrect it.
        tabs_.detach(seed);
    }

    // Engine back/forward capability belongs to the old CEF browser. The
    // portable model history and chrome-level internal-page history remain
    // intact, while the new browser reports its own capability after loading.
    transfer.state.cef_can_go_back = false;
    transfer.state.cef_can_go_forward = false;
    transfer.state.loading = false;
    transfer.state.load_progress = -1;
    const core::TabId id = transfer.state.id;
    if (!tabs_.attach(std::move(transfer.state), index, true)) return false;

    if (transfer.page) tab_pages_[id] = *transfer.page;
    if (transfer.chrome_nav) nav_stacks_[id] = std::move(*transfer.chrome_nav);
    show_host_for(id);
    update_nav_button_states();
    update_bookmark_star();
    return true;
}

bool MainWindow::transfer_tab_to(MainWindow& destination, core::TabId id,
                                 size_t index, bool replace_seed) {
    if (&destination == this || !tabs_.contains(id) ||
        destination.tabs_.contains(id)) {
        return false;
    }
    // core::IncognitoPolicy: a private tab must never cross into a regular
    // window (it would then browse through the regular profile's request
    // context). Going the other way — dropping a regular tab into a private
    // window — is likewise rejected: the tab's CEF browser keeps its original
    // request context, which would silently re-attach regular-profile cookies
    // into what the user believes is private mode. (Adopting the other
    // direction would also break: the adopter creates a fresh browser for the
    // tab in its own context.)
    if (incognito_ != destination.incognito_) return false;
    // Tor windows only exchange tabs with Tor windows (and their proxy).
    if (tor_ != destination.tor_ || tor_proxy_ != destination.tor_proxy_) return false;
    auto transfer = release_tab_for_transfer(id);
    if (!transfer) return false;
    const TransferredTab rollback = *transfer;
    if (!destination.adopt_transferred_tab(std::move(*transfer), index,
                                          replace_seed)) {
        // This is only reachable if destination state changed re-entrantly
        // during notifications. Keep the user's tab rather than dropping it.
        adopt_transferred_tab(rollback, tabs_.count(), false);
        return false;
    }

    if (tabs_.count() == 0) {
        // Chrome: moving a window's only tab elsewhere closes the emptied
        // source window. Hide it immediately — a visible zero-tab window is
        // exactly the black, tab-less frame this used to leave behind — and
        // close it on the next event-loop turn, after the gesture (mouse
        // handler / drop event) has unwound through this window's TabBar so
        // the bar is never destroyed from inside its own event. closeEvent()
        // skips the last-window quit for already-hidden windows, so hiding
        // first cannot quit the app while the destination window lives on.
        hide();
        QTimer::singleShot(0, this, [this] {
            if (tabs_.count() == 0) {
                close();
            } else {
                // A tab arrived between the transfer and this close (rollback
                // of a later transfer, or a re-entrant open). The window is
                // alive again — never leave live tabs in an invisible window.
                show();
            }
        });
    }
    return true;
}

void MainWindow::tear_off_tab(core::TabId id, const QPoint& release_global,
                              const QPoint& grab_offset) {
    if (!tabs_.contains(id)) return;
    // core::IncognitoPolicy: a torn-off tab keeps its window's privacy mode —
    // a private tab must not end up in a regular-profile window.
    auto* detached = new MainWindow(services_, /*parent=*/nullptr,
                                    /*is_incognito=*/incognito_, /*is_tor=*/tor_);
    detached->setAttribute(Qt::WA_DeleteOnClose, true);
    detached->inherit_theme_from(this);
    // Chrome sizes the torn-off window like the source but never hands the
    // user a maximized/fullscreen sheet mid-drag: seed from the source's
    // NORMAL geometry and clamp to the work area BEFORE the window is shown
    // or the grab point is anchored. Settling the final size first is what
    // keeps the grab anchor valid — any resize after the offset is captured
    // (the old order) shifted the tab under the cursor.
    QSize seed = (isMaximized() || isFullScreen()) ? normalGeometry().size()
                                                   : size();
    if (seed.isEmpty()) seed = size();
    detached->resize(seed);
    // Give the hidden window a nearby initial position. Once its transferred
    // tab has been laid out and the window is visible, correct this using the
    // actual tab geometry so the exact point originally grabbed is under the
    // cursor regardless of frame metrics or strip margins.
    detached->move(release_global - grab_offset);
    TabBar::prepare_detached_follow_window(detached);
    if (!transfer_tab_to(*detached, id, 0, true)) {
        delete detached;
        return;
    }
    detached->show();
    // The tab rect and the host geometry are only meaningful after the chrome
    // has been laid out; force any pending layout now instead of trusting a
    // pre-show guess.
    if (QLayout* lay = detached->layout()) lay->activate();
    // The adopt ran while the window was still hidden; re-assert the content
    // now that it is visible so the web host actually owns the content area
    // (fixes the empty/black detached window on an only-tab tear-off), and
    // re-sync its geometry over the next turns like any state transition.
    detached->show_host_for(id);
    detached->sync_active_host_geometry();
    // Anchor the cursor to the exact grabbed point inside the tab.
    const QRect detached_tab_rect = detached->tab_bar_->tabRect(0);
    if (!detached_tab_rect.isNull()) {
        // The detached window's tab can be narrower than the source tab was;
        // keep the anchor inside the tab, like Chrome.
        const QPoint grab(
            qBound(0, grab_offset.x(), qMax(0, detached_tab_rect.width() - 1)),
            qBound(0, grab_offset.y(),
                   qMax(0, detached_tab_rect.height() - 1)));
        const QPoint actual_grab_global = detached->tab_bar_->mapToGlobal(
            detached_tab_rect.topLeft() + grab);
        detached->move(detached->pos() + release_global - actual_grab_global);
    }
    detached->raise();
    detached->activateWindow();
}

void MainWindow::load_url_in_tab(core::TabId id, const core::Url& url) {
    if (!webview_) return;
    // A tab that has only ever shown an internal page has no engine browser
    // yet, and the engine ignores loads for browsers that do not exist. Hand
    // the address to the host instead: it becomes the browser's start page
    // the moment the host is shown, so the page loads instead of staying blank.
    auto host = hosts_.find(id);
    if (host != hosts_.end() && !host->second->browser_created()) {
        host->second->set_initial_url(url.raw());
        return;
    }
    webview_->load(id, url);
}

void MainWindow::navigate_active_tab(const core::Url& url) {
    auto active = tabs_.active();
    if (!active.valid()) {
        // Nothing to navigate: open a tab at the requested address instead.
        open_url_in_new_tab(url);
        return;
    }

    // aurora:// addresses swap a native page into the content area.
    if (auto page = core::parse_internal_url(url.raw())) {
        show_page(*page);
        return;
    }

    // A bare query goes to the user's configured search engine.
    const bool https_only = services_.prefs
        ? services_.prefs->get_bool(settings::PrefKeys::kHttpsOnly,
                                    settings::PrefDefaults::kHttpsOnly)
        : settings::PrefDefaults::kHttpsOnly;
    const core::Url target(core::resolve_navigation(
        url, resolved_search_engine().url_template, https_only));

    // Leaving an internal page: the tab shows web content again.
    tab_pages_.erase(active);
    tabs_.commit_navigation(active, core::NavigationEntry{target, "", "", {}});
    record_web_nav(active, target);
    if (webview_) {
        load_url_in_tab(active, target);
        engine_url_[active] = target.raw();
    }
    show_host_for(active);
    // The committed URL is the new truth for the star, even before the engine
    // reports back (leaving an internal page must re-enable it immediately).
    update_bookmark_star();
    update_nav_button_states();
}

std::optional<core::InternalPage> MainWindow::page_of(core::TabId id) const {
    auto it = tab_pages_.find(id);
    if (it == tab_pages_.end()) return std::nullopt;
    return it->second;
}

// --- Chrome-level back/forward ---------------------------------------------

core::ChromeNavStack& MainWindow::nav_stack(core::TabId id) {
    return nav_stacks_[id];
}

const core::ChromeNavStack* MainWindow::find_nav_stack(core::TabId id) const {
    auto it = nav_stacks_.find(id);
    if (it == nav_stacks_.end()) return nullptr;
    return &it->second;
}

void MainWindow::record_internal_nav(core::TabId id, core::InternalPage page) {
    auto& stack = nav_stack(id);
    // Opening an internal page from a web page that the stack has never seen
    // (a link-clicked page, a restored tab) must still leave something to go
    // back to, so seed the web page as the entry beneath the internal one.
    if (stack.empty()) {
        const auto* t = tabs_.find(id);
        if (t && t->url.is_valid() &&
            !core::parse_internal_url(t->url.raw()).has_value()) {
            stack.note_web_url(t->url, t->title);
        }
    }
    stack.record(core::ChromeNavEntry{page, core::Url(core::internal_url(page)),
                                      core::internal_page_title(page)});
}

void MainWindow::record_web_nav(core::TabId id, const core::Url& url) {
    nav_stack(id).record(core::ChromeNavEntry{std::nullopt, url, ""});
}

bool MainWindow::apply_chrome_entry(core::TabId id,
                                    const core::ChromeNavEntry& entry) {
    if (!tabs_.contains(id)) return false;

    if (entry.is_internal()) {
        tab_pages_[id] = *entry.page;
        tabs_.set_url(id, entry.url.raw());
        tabs_.set_title(id, core::internal_page_title(*entry.page));
    } else {
        tab_pages_.erase(id);
        tabs_.set_url(id, entry.url.raw());
        if (!entry.title.empty()) tabs_.set_title(id, entry.title);
        // The engine kept the page loaded while the internal page covered it,
        // so normally there is nothing to re-load (re-loading would lose the
        // scroll position and push a redundant engine history entry). Only
        // load when the engine is demonstrably somewhere else.
        auto engine = engine_url_.find(id);
        const bool loaded = engine != engine_url_.end() &&
                            core::Url(engine->second) == entry.url;
        if (!loaded && webview_ && entry.url.is_valid()) {
            load_url_in_tab(id, entry.url);
            engine_url_[id] = entry.url.raw();
        }
    }

    if (id == tabs_.active() && omnibox_) {
        omnibox_->set_current_url(
            QString::fromStdString(entry.url.display()), false);
    }
    show_host_for(id);
    update_nav_button_states();
    update_bookmark_star();
    return true;
}

bool MainWindow::chrome_go_back(core::TabId id) {
    auto it = nav_stacks_.find(id);
    if (it == nav_stacks_.end()) return false;
    const auto* entry = it->second.go_back();
    if (!entry) return false;
    return apply_chrome_entry(id, *entry);
}

bool MainWindow::chrome_go_forward(core::TabId id) {
    auto it = nav_stacks_.find(id);
    if (it == nav_stacks_.end()) return false;
    const auto* entry = it->second.go_forward();
    if (!entry) return false;
    return apply_chrome_entry(id, *entry);
}

QWidget* MainWindow::page_widget(core::InternalPage page) {
    auto it = pages_.find(page);
    if (it != pages_.end()) return it->second;

    QWidget* widget = nullptr;
    switch (page) {
        case core::InternalPage::Home: {
            auto* home_page = new HomePage(this);
            // Live history feeds the "Continue" / "Recently visited" cards;
            // without a profile the page keeps its built-in mock rows.
            home_page->set_history_service(services_.history);
            home_page->set_prefs(services_.prefs);
            // The page emits raw queries; the window owns the search-engine
            // state, so hand it a projector that resolves each query with the
            // ACTIVE engine and routes anything that already parses as a URL.
            home_page->set_query_url_resolver(
                [this](const QString& query) -> QString {
                    const core::Url parsed =
                        core::parse_omnibox_input(query.toStdString());
                    const bool https_only = services_.prefs
                        ? services_.prefs->get_bool(
                              settings::PrefKeys::kHttpsOnly,
                              settings::PrefDefaults::kHttpsOnly)
                        : settings::PrefDefaults::kHttpsOnly;
                    return QString::fromStdString(core::resolve_navigation(
                        parsed, resolved_search_engine().url_template,
                        https_only));
                });
            widget = home_page;
            break;
        }
        case core::InternalPage::History:
            widget = new HistoryPage(services_.history, this);
            break;
        case core::InternalPage::Downloads: {
            // The page's per-row actions go back through the window, which
            // owns the engine-id mapping the engine needs to control a
            // transfer.
            auto* downloads_page = new DownloadsPage(
                services_.downloads, this,
                downloads_actions<DownloadsPage::Actions>());
            // The summary card shows the real download location.
            downloads_page->set_prefs(services_.prefs);
            // "Download settings" opens the settings page.
            connect(downloads_page, &DownloadsPage::download_settings_requested,
                    this, [this] {
                        show_page(core::InternalPage::Settings);
                        if (auto* settings_page = qobject_cast<SettingsPage*>(
                                page_widget(core::InternalPage::Settings))) {
                            settings_page->show_category(
                                QStringLiteral("downloads"));
                        }
                    });
            widget = downloads_page;
            break;
        }
        case core::InternalPage::Bookmarks: {
            auto* bookmarks_page = new BookmarksPage(services_.bookmarks, this);
            // The page reports every edit/delete through status_message, and
            // the bar renders the same store — so treat it as "the bookmark
            // tree changed" and re-read the bar (same trick the extensions
            // page uses to invalidate window-owned surfaces).
            connect(bookmarks_page, &InternalPageWidget::status_message, this,
                    [this](const QString&) { reload_bookmark_bar(); });
            widget = bookmarks_page;
            break;
        }
        case core::InternalPage::Settings: {
            auto* settings_page = new SettingsPage(services_.prefs, services_.search_engines, this);
            connect(settings_page, &SettingsPage::theme_preference_changed, this,
                    [this](const QString& pref) {
                        // Persist immediately so the choice survives restart
                        // (previously only the in-memory dark_mode_ flag was
                        // set and every setting was lost on exit).
                        if (services_.prefs) {
                            services_.prefs->set_string(
                                settings::PrefKeys::kTheme,
                                pref.toStdString());
                        }
                        apply_theme_preference(pref);
                    });
            connect(settings_page, &SettingsPage::search_engine_changed, this,
                    [this](const QString& id) {
                        search_engine_ = id.toStdString();
                        if (services_.prefs) {
                            services_.prefs->set_string(
                                settings::PrefKeys::kSearchEngine,
                                id.toStdString());
                        }
                    });
            // The global Shields switch reaches the engine immediately — the
            // pref is already written by the page, this pushes the live flag.
            connect(settings_page, &SettingsPage::adblock_enabled_changed, this,
                    [this](bool enabled) {
                        if (webview_) webview_->set_adblock_enabled(enabled);
                    });
            // "Update now" talks to the shared updater directly; the result
            // reaches the engine through on_filter_list_update_finished.
            settings_page->set_filter_list_updater(services_.filter_lists);
            connect(settings_page, &SettingsPage::site_settings_requested, this,
                    [this] { show_page(core::InternalPage::SiteSettings); });
            connect(settings_page, &SettingsPage::extensions_requested, this,
                    [this] { show_page(core::InternalPage::Extensions); });
            // The page already wrote appearance.bookmark_bar; mirror it into
            // the bar and the menu's check mark like the menu toggle does.
            connect(settings_page, &SettingsPage::bookmark_bar_visibility_changed,
                    this, [this](bool visible) {
                        bookmark_bar_visible_ = visible;
                        apply_bookmark_bar_visibility(visible);
                    });
            widget = settings_page;
            break;
        }
        case core::InternalPage::SiteSettings: {
            auto* site_page =
                new SiteSettingsPage(services_.prefs, services_.adblock, this);
            // Same engine path as the toolbar shield (set_site_blocking):
            // push the whole exception set, then repaint the shield.
            connect(site_page, &SiteSettingsPage::adblock_exceptions_changed,
                    this, [this] {
                        if (webview_ && services_.adblock) {
                            webview_->set_adblock_exceptions(
                                services_.adblock->list_exception_sites());
                        }
                        update_shield_for_tab();
                    });
            widget = site_page;
            break;
        }
        case core::InternalPage::Reader: {
            auto* reader_page = new ReaderPage(this);
            // "Open original" replaces the reader tab with the source page.
            connect(reader_page, &ReaderPage::open_original_requested, this,
                    [this](const QString& url) {
                        navigate_active_tab(core::parse_omnibox_input(url.toStdString()));
                    });
            widget = reader_page;
            break;
        }
        case core::InternalPage::Extensions: {
            auto* extensions_page =
                new ExtensionsPage(services_.extensions, this);
            // The page reports every registry change (install, enable/disable,
            // remove) through status_message. That is Aurora's signal that the
            // extension set just changed, so the window drops the surfaces it
            // is hosting for the old set right away instead of leaving a popup
            // open for an extension that no longer exists.
            connect(extensions_page, &InternalPageWidget::status_message, this,
                    [this](const QString&) { on_extension_registry_changed(); });
            widget = extensions_page;
            break;
        }
    }

    // Pages report with one shared (QString url) / (QString message) signal
    // contract, but HomePage re-states it on plain QWidget rather than on the
    // InternalPageWidget skeleton, so connect through each page's own typed
    // signal — the SIGNAL() macro form cannot pair with a lambda slot. The
    // home page's submissions keep the tab (the pill IS the tab's content);
    // the tool pages still open links in a new tab so the tool stays put.
    if (auto* home = qobject_cast<HomePage*>(widget)) {
        connect(home, &HomePage::open_url_requested, this,
                [this](const QString& url) {
                    navigate_active_tab(
                        core::parse_omnibox_input(url.toStdString()));
                });
        // Internal-page confirmations are one-shot messages: flash, not stick.
        connect(home, &HomePage::status_message, this,
                [this](const QString& msg) { flash_status(msg); });
    } else {
        auto* skeleton = static_cast<InternalPageWidget*>(widget);
        connect(skeleton, &InternalPageWidget::open_url_requested, this,
                [this](const QString& url) {
                    open_url_in_new_tab(
                        core::parse_omnibox_input(url.toStdString()));
                });
        // Internal-page confirmations are one-shot messages: flash, not stick.
        connect(skeleton, &InternalPageWidget::status_message, this,
                [this](const QString& msg) { flash_status(msg); });
    }

    content_stack_->addWidget(widget);
    pages_[page] = widget;
    return widget;
}

void MainWindow::open_url_in_new_tab(const core::Url& url) {
    if (!url.is_valid()) return;
    const std::string& raw = url.raw();
    // Internal and extension schemes must reach the tab lifecycle verbatim:
    // resolving them as an omnibox query would send them to the search engine.
    if (core::parse_internal_url(raw).has_value() ||
        raw.rfind("chrome-extension://", 0) == 0) {
        tabs_.open(raw);
        return;
    }
    const bool https_only = services_.prefs
        ? services_.prefs->get_bool(settings::PrefKeys::kHttpsOnly,
                                    settings::PrefDefaults::kHttpsOnly)
        : settings::PrefDefaults::kHttpsOnly;
    const core::Url target(core::resolve_navigation(
        url, resolved_search_engine().url_template, https_only));
    tabs_.open(target.raw());
}

void MainWindow::close_extension_popup() {
    if (!extension_popup_) return;
    // Do NOT remove the application-wide event filter here: the very same
    // filter drives caption dragging/double-click for the frameless window and
    // is installed once in the constructor. Removing it on the first popup
    // close silently disabled window dragging. The popup branches of
    // eventFilter() are gated on extension_popup_ instead.
    QWidget* popup = extension_popup_;
    const core::TabId owner = extension_popup_owner_tab_;
    const core::TabId popup_id = extension_popup_tab_;
    extension_popup_ = nullptr;
    extension_popup_host_ = nullptr;
    extension_popup_tab_ = core::TabId(0);
    extension_popup_url_.clear();
    extension_popup_extension_id_.clear();
    extension_popup_owner_tab_ = core::TabId(0);
    extension_popup_age_.invalidate();
    // Cleared first so the popup's destroyed() handler (and any re-entrant
    // close) sees a consistent state. Destroy THIS popup's synthetic browser
    // only — never a real tab, never another window's controller.
    if (webview_ && popup_id.valid()) webview_->destroy(popup_id);
    popup->close();

    // Hand the tab the popup belonged to back its engine-visible state. The
    // popup was that tab's surface, so once it is gone the tab is unambiguously
    // the visible one again — only asserted for a tab that really is the
    // content widget, since visibility is the engine's, not ours, to invent.
    if (owner.valid() && webview_ && content_stack_) {
        auto it = hosts_.find(owner);
        if (it != hosts_.end() &&
            content_stack_->currentWidget() == it->second) {
            webview_->set_visible(owner, true);
        }
    }
}

void MainWindow::on_extension_registry_changed() {
    // An extension was installed, enabled/disabled, or removed. Everything
    // Aurora itself hosts for the previous registry state has to go NOW — the
    // visible complaint about uninstall was that the extension appeared to
    // still be there, and a live popup window (or a stale puzzle-piece list) is
    // exactly what that looks like.
    //
    // An open popup is a chrome-extension:// surface of a specific extension;
    // once the registry changed it may belong to an extension that no longer
    // exists, so it is closed unconditionally rather than guessed about. Its
    // CEF browser is destroyed with it (see close_extension_popup), so the
    // extension stops rendering in Aurora immediately. The puzzle-piece button
    // re-reads the registry every time it opens, so the list is correct on its
    // next open without touching it here.
    close_extension_popup();

    // CEF removed the Alloy CefExtension/CefExtensionHandler and
    // CefRequestContext::LoadExtension APIs in M128. Aurora uses CEF 152
    // Alloy-style native children, so there is no live CefExtension::Unload
    // handle to call; --load-extension remains the supported startup path for
    // this architecture. The registry/UI removal above is immediate and the
    // extensions page truthfully says restart is required for engine unload.
}

void MainWindow::open_extension_popup(const QString& url,
                                      const QString& extension_id) {
    // A Chrome extension's default_popup HTML opens in a small borderless
    // popup window anchored below the extensions button. Qt::Tool (NOT
    // Qt::Popup) is required: Qt::Popup grabs the mouse at the Qt level and
    // starves the native CEF child of WM_MOUSE*, so the popup looks dead.
    // Click-outside and Escape are handled by the existing app-wide
    // eventFilter (and ExtensionPopupOutsideClickFilter on Windows).
    if (extensions_btn_) extensions_btn_->close_popup();

    // Toggle: same extension again closes the live popup. A different
    // extension replaces it. Match on id when both sides have one (popup URLs
    // can grow a query string between openings); otherwise fall back to URL.
    if (extension_popup_) {
        const bool same_extension =
            (!extension_id.isEmpty() && !extension_popup_extension_id_.isEmpty())
                ? extension_popup_extension_id_ == extension_id
                : extension_popup_url_ == url;
        close_extension_popup();
        if (same_extension) return;
    }
    if (url.isEmpty()) return;

    const core::TabId invoking_tab = tabs_.active();

    constexpr int kPopupContentWidth = 400;
    constexpr int kPopupContentHeight = 480;
    constexpr int kPopupMinContentHeight = 200;
    constexpr int kPopupBorder = 1;
    int content_height = kPopupContentHeight;
    if (extensions_btn_) {
        const int room = PopupPlacement::availablePanelHeightFromAnchor(
            extensions_btn_, /*gap=*/2);
        if (room > 0) {
            content_height = qBound(kPopupMinContentHeight,
                                    room - 2 * kPopupBorder,
                                    kPopupContentHeight);
        }
    }
    const QSize content_size(kPopupContentWidth, content_height);
    const QSize popup_size(content_size.width() + 2 * kPopupBorder,
                           content_size.height() + 2 * kPopupBorder);
    const QPoint anchor = extensions_btn_
        ? dropdown_pos(extensions_btn_, popup_size, /*align_right=*/true, 2)
        : clamped_to_screen(QPoint(100, 100), popup_size, nullptr);

    auto* popup = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint);
    popup->setObjectName("ExtensionPopup");
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setAttribute(Qt::WA_QuitOnClose, false);
    // Show without taking activation so the invoking tab stays current. Clicking
    // inside the CEF child still gives it native input focus because we did not
    // use Qt::Popup (which would grab the mouse).
    popup->setAttribute(Qt::WA_ShowWithoutActivating);
    popup->setFixedSize(popup_size);
    popup->move(anchor);

    static std::atomic<uint64_t> s_popup_tab_id{100000};
    const core::TabId popup_id(
        s_popup_tab_id.fetch_add(1, std::memory_order_relaxed));
    if (webview_) {
        const core::TabState* owner_state = tabs_.find(invoking_tab);
        webview_->set_extension_popup_owner(
            popup_id, invoking_tab,
            owner_state ? std::string_view(owner_state->title)
                        : std::string_view{},
            owner_state ? std::string_view(owner_state->url.raw())
                        : std::string_view{});
    }

    auto* host = new CefHostWidget(popup_id, url.toStdString(),
                                   webview_.get(), popup);
    auto* layout = new QVBoxLayout(popup);
    layout->setContentsMargins(kPopupBorder, kPopupBorder,
                               kPopupBorder, kPopupBorder);
    layout->setSpacing(0);
    layout->addWidget(host);
    host->setFixedSize(content_size);

    extension_popup_ = popup;
    extension_popup_host_ = host;
    extension_popup_tab_ = popup_id;
    extension_popup_url_ = url;
    extension_popup_extension_id_ = extension_id;
    extension_popup_owner_tab_ = invoking_tab;
    apply_extension_popup_theme();

    // destroyed() is the single cleanup path for the synthetic CEF browser.
    // The QObject* argument ignores a stale destroyed() from a popup that a
    // newer one has already replaced. Never destroys another window's browsers.
    connect(popup, &QWidget::destroyed, this,
        [this, popup_id](QObject* obj) {
            if (webview_ && popup_id.valid()) webview_->destroy(popup_id);
            if (!extension_popup_.isNull() && extension_popup_.data() != obj) {
                return;
            }
            const core::TabId owner = extension_popup_owner_tab_;
            extension_popup_ = nullptr;
            extension_popup_host_ = nullptr;
            extension_popup_tab_ = core::TabId(0);
            extension_popup_url_.clear();
            extension_popup_extension_id_.clear();
            extension_popup_owner_tab_ = core::TabId(0);
            extension_popup_age_.invalidate();
            if (owner.valid() && webview_ && content_stack_) {
                auto it = hosts_.find(owner);
                if (it != hosts_.end() &&
                    content_stack_->currentWidget() == it->second) {
                    webview_->set_visible(owner, true);
                }
            }
        });

    // Visible first so the host has a real native window and a laid-out size
    // before CEF is asked to create its child. Do not activateWindow() HERE
    // (mid-show, before the browser exists): that historically deactivated the
    // browser and the click-outside filter closed the popup in the same breath
    // as opening it. Activation is handed over one event-loop turn later, once
    // the CEF child exists — see the singleShot below.
    popup->show();
    popup->raise();
    extension_popup_age_.start();

    host->make_current();
    host->sync_geometry();

    // The invoking tab keeps its engine visible. Only re-assert a host that is
    // already the content widget — make_current() creates an engine, and a
    // dormant host must not get one just because a popup opened.
    if (invoking_tab.valid() && content_stack_) {
        auto it = hosts_.find(invoking_tab);
        if (it != hosts_.end() &&
            content_stack_->currentWidget() == it->second) {
            it->second->make_current();
            if (webview_) webview_->set_visible(invoking_tab, true);
        }
    }

    // One event-loop turn later the popup's layout has settled in device
    // pixels (HiDPI). Re-push so the CEF child matches the real size instead
    // of covering a fraction of the widget.
    //
    // Then hand the popup INPUT. WA_ShowWithoutActivating kept the show itself
    // from stealing activation, but it also means nothing ever activates the
    // Qt::Tool window or focuses the native CEF child inside it — so the popup
    // rendered while every click and keystroke went nowhere (on Windows an
    // inactive tool window's child HWND never gets keyboard focus). Activate
    // the popup now and focus the CEF child explicitly. This is safe against
    // the historical close-on-open race: it runs well inside the 200ms
    // "settled" gate, it moves activation BETWEEN windows of this app (so the
    // ApplicationDeactivate branch of eventFilter does not fire), and it
    // synthesizes no mouse press for the click-outside branches to see.
    QPointer<CefHostWidget> host_guard(host);
    QTimer::singleShot(0, this, [this, host_guard] {
        if (!host_guard) return;
        host_guard->sync_geometry();
        if (extension_popup_.isNull() ||
            extension_popup_host_.data() != host_guard.data()) {
            return;  // replaced or closed before the turn ran
        }
        extension_popup_->raise();
        extension_popup_->activateWindow();
        if (webview_ && extension_popup_tab_.valid())
            webview_->focus(extension_popup_tab_);
    });
}

void MainWindow::show_page(core::InternalPage page) {
    const auto url = core::internal_url(page);
    // Internal destinations are tools, not replacements for the document the
    // user is reading. The Added handler recognizes aurora:// and activates
    // the native page in its own foreground tab.
    tabs_.open(url);
}

void MainWindow::on_new_tab() {
    tabs_.open(new_tab_url());
}

void MainWindow::on_clear_browsing_data() {
    if (!services_.history && !services_.downloads) {
        flash_status("No profile is attached, so there is nothing to clear");
        return;
    }
    const auto answer = QMessageBox::question(
        this, "Clear browsing data",
        "Delete all browsing history and the download list? This cannot be "
        "undone.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    clear_browsing_data(true, true, false);
}

void MainWindow::clear_browsing_data(bool history, bool downloads, bool cookies) {
    if (!policy_.may_clear_profile_data()) return;
    const auto rows = privacy::clear_browsing_data(
        policy_, services_.history, services_.downloads, history, downloads);
#if defined(AURORA_HAS_CEF)
    if (cookies && !cef::clear_profile_cookies()) {
        AURORA_LOG(util::LogLevel::Warn, "privacy")
            << "cookie deletion/flush failed or timed out before shutdown";
    }
#else
    (void)cookies;
#endif
    flash_status(QString("Cleared %1 history entries and %2 downloads")
                     .arg(rows.history)
                     .arg(rows.downloads));

    // Any open internal tool page must not keep showing deleted rows. The
    // home page keeps no profile rows, so the skeleton pages — all
    // InternalPageWidget subclasses — are the only ones with a refresh().
    for (auto& [page, widget] : pages_) {
        (void)page;
        if (auto* skeleton = qobject_cast<InternalPageWidget*>(widget)) {
            skeleton->refresh();
        }
    }
}

void MainWindow::clear_on_exit() {
    if (!policy_.may_clear_profile_data() || !services_.prefs) return;
    const bool cookies = services_.prefs->get_bool(
        settings::PrefKeys::kClearOnExitCookies, settings::PrefDefaults::kClearOnExitCookies);
    const bool history = services_.prefs->get_bool(
        settings::PrefKeys::kClearOnExitHistory, settings::PrefDefaults::kClearOnExitHistory);
    const bool downloads = services_.prefs->get_bool(
        settings::PrefKeys::kClearOnExitDownloads, settings::PrefDefaults::kClearOnExitDownloads);
    if (cookies || history || downloads) clear_browsing_data(history, downloads, cookies);
}

void MainWindow::on_close_tab() {
    const auto active = tabs_.active();
    // Chrome behavior: closing the last tab closes THIS window only (matches
    // TabBar::request_close). Never reopen a replacement tab here.
    if (tabs_.tabs().size() <= 1) {
        if (active.valid()) {
            tabs_.set_loading(active, false, -1);
            tabs_.close(active);
        }
        if (tabs_.count() == 0) {
            if (QWidget* top = window()) top->close();
        }
        return;
    }
    if (active.valid()) {
        // Recently-closed state must never preserve a transient load. A
        // reopened tab gets a fresh engine and waits for its real start event.
        tabs_.set_loading(active, false, -1);
        tabs_.close(active);
    }
}

void MainWindow::on_reopen_closed() {
    tabs_.reopen_closed();
}

void MainWindow::on_back() {
    const auto active = tabs_.active();
    if (!active.valid()) return;
    // An internal page is a Qt widget the engine knows nothing about, so its
    // Back can only come from the chrome-level stack.
    if (page_of(active)) {
        chrome_go_back(active);
        return;
    }
    if (restored_history_.contains(active) && tabs_.go_back(active)) {
        const auto* entry = tabs_.find(active)->current_entry();
        if (entry && webview_) load_url_in_tab(active, entry->url);
        update_nav_button_states();
        return;
    }
    // Web content: the engine owns history within the page's own chain, so it
    // stays in charge (link clicks, redirects, form posts). Only when it has
    // nowhere to go does the chrome stack take over — that is the case where
    // the page was opened *from* an internal page.
    const auto* t = tabs_.find(active);
    if (t && t->cef_can_go_back && webview_) {
        webview_->back(active);
        return;
    }
    chrome_go_back(active);
}

void MainWindow::on_forward() {
    const auto active = tabs_.active();
    if (!active.valid()) return;
    if (page_of(active)) {
        chrome_go_forward(active);
        return;
    }
    if (restored_history_.contains(active) && tabs_.go_forward(active)) {
        const auto* entry = tabs_.find(active)->current_entry();
        if (entry && webview_) load_url_in_tab(active, entry->url);
        update_nav_button_states();
        return;
    }
    // After backing out of an internal page, Forward must return to it — the
    // engine's own forward entry (if any) belongs to the web chain and would
    // silently skip the page the user just left.
    if (const auto* stack = find_nav_stack(active)) {
        const auto* next = stack->peek_forward();
        if (next && next->is_internal()) {
            chrome_go_forward(active);
            return;
        }
    }
    const auto* t = tabs_.find(active);
    if (t && t->cef_can_go_forward && webview_) {
        webview_->forward(active);
        return;
    }
    chrome_go_forward(active);
}

void MainWindow::on_reload() {
    if (tabs_.active().valid() && webview_) {
        webview_->reload(tabs_.active(), false);
    }
}

void MainWindow::on_stop() {
    if (tabs_.active().valid() && webview_) {
        webview_->stop(tabs_.active());
    }
}

void MainWindow::on_home() {
    navigate_active_tab(core::parse_omnibox_input(new_tab_url()));
}

void MainWindow::on_focus_omnibox() {
    // focus_search() focuses the inner edit and selects all of it, so typing
    // replaces the address instead of appending to it.
    omnibox_->focus_search();
}

void MainWindow::on_find() {
    if (find_bar_->isHidden()) {
        find_bar_->show();
    }
    find_bar_->focus_search();
}

void MainWindow::on_find_close() {
    if (!find_bar_->isHidden()) {
        find_bar_->hide();
        auto active = tabs_.active();
        if (active.valid() && webview_) {
            webview_->stop_find(active);
        }
    }
}

void MainWindow::on_fullscreen_toggle() {
    // While a page is fullscreen, leaving fullscreen belongs to the page: tell
    // the engine, and the resulting fullscreen-mode change brings the chrome
    // back through on_page_fullscreen(). Doing it the other way round leaves
    // the document convinced it is still fullscreen.
    if (fullscreen_tab_.valid() && webview_) {
        webview_->exit_fullscreen(fullscreen_tab_);
        return;
    }
    if (isFullScreen()) {
        if (was_maximized_) showMaximized();
        else showNormal();
    } else {
        was_maximized_ = isMaximized();
        showFullScreen();
    }
    sync_active_host_geometry();
}

void MainWindow::on_page_fullscreen(core::TabId tab, bool fullscreen) {
    // Only the tab the user is looking at may take the window over.
    if (fullscreen && tab != tabs_.active()) return;
    if (fullscreen) {
        if (chrome_hidden_) return;
        fullscreen_tab_ = tab;
        chrome_hidden_ = true;
        was_maximized_ = isMaximized();
        // Chrome-style content fullscreen: the page gets the entire window, so
        // every piece of browser furniture goes away. The find bar is closed
        // rather than hidden so re-entering does not restore a stale search.
        if (!find_bar_->isHidden()) on_find_close();
        if (tab_strip_) tab_strip_->hide();
        if (toolbar_) toolbar_->hide();
        if (bookmark_bar_) bookmark_bar_->hide();
        if (status_bubble_) status_bubble_->hide();
        status_hidden_by_fullscreen_ = true;
        if (devtools_dock_ && devtools_dock_->isVisible()) devtools_dock_->hide();
        if (!isFullScreen()) showFullScreen();
    } else {
        if (!chrome_hidden_) return;
        fullscreen_tab_ = core::TabId{0};
        chrome_hidden_ = false;
        if (tab_strip_) tab_strip_->show();
        if (toolbar_) toolbar_->show();
        // Restores the *preference*, not simply "visible": a user who hid the
        // bookmarks bar must not get it back by watching a video fullscreen.
        apply_bookmark_bar_visibility(bookmark_bar_visible_);
        status_hidden_by_fullscreen_ = false;
        position_status_bubble();
        if (isFullScreen()) {
            if (was_maximized_) showMaximized();
            else showNormal();
        }
    }
    // The content area changes size twice on the way in and out (chrome hidden,
    // then window state applied), and on X11 the window manager applies the
    // state asynchronously. Re-assert the geometry over the next few turns.
    sync_active_host_geometry();
}

void MainWindow::sync_active_host_geometry() {
    const auto resync = [this] {
        auto it = hosts_.find(tabs_.active());
        if (it != hosts_.end()) it->second->sync_geometry();
    };
    resync();
    // 0 ms: after the current event returns. 120/300 ms: after the window
    // manager has actually applied a fullscreen/restore transition, which is
    // when the old size would otherwise stick and leave the page rendering at
    // a fraction of the window.
    QTimer::singleShot(0, this, resync);
    QTimer::singleShot(120, this, resync);
    QTimer::singleShot(300, this, resync);
}

void MainWindow::finish_closing_host(core::TabId tab) {
    auto it = closing_hosts_.find(tab);
    if (it == closing_hosts_.end()) return;
    auto* host = it->second;
    closing_hosts_.erase(it);
    delete host;
}

void MainWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange) {
        // Frameless window: the maximize button becomes a restore button.
        update_window_control_icons();
        sync_active_host_geometry();
    }
}

void MainWindow::handle_context_action(
    core::TabId tab, webview::WebViewController::ContextActionRequest req) {
    using Action = webview::WebViewController::ContextAction;
    if (privacy_exit_started_ || !tabs_.find(tab)) return;

    switch (req.action) {
        case Action::CopyText:
            if (auto* cb = QGuiApplication::clipboard()) {
                cb->setText(QString::fromStdString(req.text));
                flash_status(QStringLiteral("Copied to clipboard"));
            }
            return;
        case Action::CopyImage: {
            QImage image;
            if (!req.data.empty() &&
                image.loadFromData(req.data.data(),
                                   static_cast<uint>(req.data.size()), "PNG") &&
                !image.isNull()) {
                if (auto* cb = QGuiApplication::clipboard()) {
                    cb->setImage(image);
                    flash_status(QStringLiteral("Image copied to clipboard"));
                    return;
                }
            }
            flash_status(QStringLiteral("Could not copy the image"));
            return;
        }
        case Action::SearchSelection: {
            if (req.text.empty()) return;
            // Opens in a new tab through the user's default engine, like
            // Chrome's "Search Google for ...": the page the text came from
            // stays where it is.
            const auto engine = resolved_search_engine();
            open_url_in_new_tab(core::Url(
                searchengines::expand_template(engine.url_template, req.text)));
            return;
        }
        default: break;
    }

    if (req.action == Action::PictureInPictureFallback) {
#if defined(AURORA_HAS_CEF)
        // The engine could not open native PiP for the video: float the whole
        // tab in Aurora's own picture-in-picture window instead.
        if (PipWindow::can_open(this, tab)) {
            QTimer::singleShot(0, this, [guard = QPointer<MainWindow>(this), tab] {
                if (guard) PipWindow::open(guard.data(), tab);
            });
            flash_status(QStringLiteral("Video moved to a floating window"));
            return;
        }
#endif
        flash_status(QStringLiteral("Picture in picture is not available for this video"));
        return;
    }

    // The remaining actions operate on the active tab. The menu is modal, so
    // the tab it was opened on is still active; if the user somehow switched
    // away first, do nothing rather than act on the wrong page.
    if (tabs_.active() != tab) return;
    switch (req.action) {
        case Action::Back: on_back(); break;
        case Action::Forward: on_forward(); break;
        case Action::Reload: on_reload(); break;
        case Action::SavePageAsPdf: on_save_as_pdf(); break;
        case Action::Print: on_print(); break;
        case Action::Inspect:
            // Toggles the dock, and a right-click on an already-inspected tab
            // should open it, not close it.
            if (!(devtools_dock_ && devtools_dock_->isVisible() &&
                  devtools_tab_ == tab)) {
                on_devtools();
            }
            break;
        default: break;
    }
}

void MainWindow::on_devtools() {
    if (!webview_) return;
    const auto active = tabs_.active();
    if (!active.valid()) return;
    // Toggle: close a visible dock when it is inspecting the active tab, or
    // when its CEF browser has already gone away but the dock is still visible.
    if (devtools_dock_ && devtools_dock_->isVisible() &&
        (!devtools_tab_.valid() || devtools_tab_ == active)) {
        close_devtools_dock();
        return;
    }
    // Switching to a different tab: close the previous DevTools browser first
    // (one at a time) so we don't parent two CEF browsers into the same host.
    if (devtools_tab_.valid() && devtools_tab_ != active) {
        webview_->close_devtools(devtools_tab_);
        devtools_tab_ = {};
    }
    devtools_tab_ = active;
    devtools_dock_->show();
    devtools_dock_->raise();
    devtools_host_->setAttribute(Qt::WA_NativeWindow);
    // Defer the CEF parent+open until Qt has laid out the dock on the next
    // event-loop turn. Calling ShowDevTools with SetAsChild immediately after
    // show() can race the layout and parent the DevTools browser into a 0x0
    // host window, which then never appears. Letting the dock realize its
    // final geometry first gives CEF a real, sized native window to attach to.
    QTimer::singleShot(0, this, [this, active] {
        if (!webview_ || devtools_tab_ != active) return;  // toggled away
        const WId wid = devtools_host_->winId();
        QSize sz = devtools_host_->size();
        if (sz.width() < 50 || sz.height() < 50) sz = QSize(800, 400);
        webview_->open_devtools_docked(active, reinterpret_cast<void*>(wid),
                                      sz.width(), sz.height());
    });
}

void MainWindow::close_devtools_dock() {
    // Hide the dock and tear down the DevTools browser. We clear devtools_tab_
    // BEFORE hiding so the visibilityChanged handler (which fires when hide()
    // makes the dock invisible) sees no tab to close and does nothing —
    // avoiding a double-close of the CEF browser.
    //
    // We use hide() instead of close() and do NOT block the visibilityChanged
    // signal: blocking it prevented the dock from updating its own visibility
    // state, which left the dock visible after the first toggle press and
    // required a second press to actually hide it.
    if (!devtools_dock_) return;
    const auto tab = devtools_tab_;
    devtools_tab_ = {};
    devtools_dock_->hide();
    if (webview_ && tab.valid()) webview_->close_devtools(tab);
}

void MainWindow::setup_devtools_dock() {
    devtools_dock_ = new QDockWidget(QStringLiteral("DevTools"), this);
    devtools_dock_->setObjectName("DevToolsDock");
    devtools_dock_->setAllowedAreas(Qt::BottomDockWidgetArea |
                                    Qt::TopDockWidgetArea |
                                    Qt::LeftDockWidgetArea |
                                    Qt::RightDockWidgetArea);
    devtools_dock_->setFeatures(QDockWidget::DockWidgetClosable |
                                QDockWidget::DockWidgetMovable |
                                QDockWidget::DockWidgetFloatable);
    devtools_host_ = new DevToolsHost(devtools_dock_);
    devtools_dock_->setWidget(devtools_host_);
    addDockWidget(Qt::BottomDockWidgetArea, devtools_dock_);
    devtools_dock_->hide();
    // Resize the CEF DevTools child window to track the host widget.
    connect(devtools_host_, &DevToolsHost::resized, this, [this](int w, int h) {
        if (webview_ && devtools_tab_.valid()) {
            webview_->resize_devtools(devtools_tab_, w, h);
        }
    });
    // When the user closes the dock (close button / docking away), tear down
    // the DevTools browser so we don't leave a hidden CEF browser alive.
    connect(devtools_dock_, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
                if (!visible && devtools_tab_.valid() && webview_) {
                    webview_->close_devtools(devtools_tab_);
                    devtools_tab_ = {};
                }
            });
    // When the CEF DevTools browser closes itself (user closes DevTools from
    // inside its own panel), hide the dock so the user doesn't see an empty
    // dock panel. Without this, the dock remained visible after the DevTools
    // content was gone, requiring a second F12 press to dismiss it.
    if (webview_) {
        webview_->set_devtools_closed_callback([this] {
            // Post to the Qt event loop: CEF callbacks can fire from the UI
            // thread inside CEF's own dispatch, and calling hide() inline
            // could re-enter Qt during a native message pump.
            QMetaObject::invokeMethod(this, [this] {
                if (devtools_dock_ && devtools_dock_->isVisible()) {
                    devtools_tab_ = {};
                    devtools_dock_->hide();
                }
            }, Qt::QueuedConnection);
        });
    }
}

void MainWindow::on_bookmark_page() {
    // Bookmark (or un-bookmark) the active tab's current page. There was no
    // way to bookmark a site at all before — only a Bookmarks *page* existed.
    const auto active = tabs_.active();
    if (!active.valid() || !services_.bookmarks) return;
    const auto* t = tabs_.find(active);
    if (!t) return;
    // Native aurora:// pages are not bookmarkable: Ctrl+D / the star must be
    // a no-op there rather than storing an internal address as a bookmark.
    if (active_tab_is_internal()) {
        update_bookmark_star();
        return;
    }
    const std::string url = active_tab_url();
    if (url.empty()) return;
    const std::string title =
        t->title.empty() ? url : t->title;

    // Toggle: if a bookmark for this URL already exists, remove it; otherwise
    // add it. all_bookmarks() is the only lookup we have (no find_by_url), so
    // scan it — the list is small for a personal browser.
    bool removed = false;
    for (const auto& node : services_.bookmarks->all_bookmarks()) {
        if (node.url == url) {
            services_.bookmarks->remove(node.id);
            flash_status(QStringLiteral("Removed bookmark"));
            removed = true;
            break;
        }
    }
    if (!removed) {
        // Add to the "Bookmarks bar" folder, not the root: the bar only shows
        // that folder's children, so a root bookmark left the star lit with
        // nothing visible on the bar. add_page() resolves (and seeds) the bar
        // folder and reloads the bar itself. Fall back to the root only when
        // the bar widget was never built.
        if (bookmark_bar_) {
            bookmark_bar_->add_page(QString::fromStdString(title),
                                    QString::fromStdString(url));
        } else {
            services_.bookmarks->add(0, title, url, false);
        }
        flash_status(QStringLiteral("Bookmarked: %1")
                         .arg(QString::fromStdString(title)));
    }
    update_bookmark_star();
    // The bar shows a folder of the same store, so a toggle can add or drop
    // one of its items.
    reload_bookmark_bar();
    // Refresh the bookmarks page if it is on screen.
    if (auto page = page_of(active);
        page && *page == core::InternalPage::Bookmarks) {
        static_cast<InternalPageWidget*>(page_widget(*page))->refresh();
    }
}

void MainWindow::update_shield_for_tab() {
    if (!shield_btn_) return;
    const auto active = tabs_.active();
    if (!active.valid()) {
        shield_btn_->set_site(QString());
        shield_btn_->set_blocked_count(0);
        return;
    }
    // Internal pages have no network requests to block: the shield dims
    // rather than promising a count for a Qt widget.
    if (active_tab_is_internal()) {
        shield_btn_->set_site(QString());
        shield_btn_->set_blocked_count(0);
        return;
    }
    const std::string url = active_tab_url();
    if (url.empty()) {
        shield_btn_->set_site(QString());
        shield_btn_->set_blocked_count(0);
        return;
    }
    const std::string site = adblock::site_key(url);
    shield_btn_->set_site(QString::fromStdString(site));
    const auto count = adblock_counts_.find(active);
    shield_btn_->set_blocked_count(count == adblock_counts_.end() ? 0
                                                                  : count->second);
    // Glyph: slashed while this site is in the exception list. A missing
    // service means no per-site state at all — shields read as up.
    const bool allowed =
        !site.empty() && services_.adblock &&
        services_.adblock->is_site_allowed(site);
    shield_btn_->set_site_blocking_enabled(!allowed);
}

void MainWindow::set_site_blocking(bool enabled) {
    const auto active = tabs_.active();
    if (!active.valid() || !services_.adblock) return;
    const std::string url = active_tab_url();
    if (url.empty()) return;
    const std::string site = adblock::site_key(url);
    if (site.empty()) return;

    // Persist (or un-persist) the exception, then push the whole set to the
    // engine. set_site_allowed() and the engine snapshot use the same
    // normalized site key, so a toggle always lands on the key requests are
    // actually checked against.
    services_.adblock->set_site_allowed(site, !enabled);
    if (webview_ && services_.adblock) {
        std::vector<std::string> exceptions;
        for (const auto& s : services_.adblock->list_exception_sites()) {
            exceptions.push_back(s);
        }
        webview_->set_adblock_exceptions(exceptions);
    }
    update_shield_for_tab();
}

std::string MainWindow::active_tab_url() const {
    const auto active = tabs_.active();
    if (!active.valid()) return {};
    const auto* t = tabs_.find(active);
    if (!t) return {};
    return t->url.raw();
}

bool MainWindow::active_tab_is_internal() const {
    const auto active = tabs_.active();
    if (!active.valid()) return false;
    // Two sources of truth, and the star must agree with whatever is actually
    // on screen: tab_pages_ records that a native page took the content area
    // over, and the tab's URL is the aurora:// address itself.
    if (page_of(active).has_value()) return true;
    const std::string url = active_tab_url();
    return !url.empty() && core::parse_internal_url(url).has_value();
}

void MainWindow::update_bookmark_star() {
    if (!omnibox_) return;
    const auto active = tabs_.active();
    if (!active.valid()) {
        omnibox_->set_star_visible(false);
        return;
    }
    omnibox_->set_star_visible(true);

    // Internal pages: clear and inert. This is the stale-star fix — showing
    // aurora://bookmarks used to leave the previous page's filled star on.
    if (active_tab_is_internal()) {
        omnibox_->set_star_enabled(false);
        omnibox_->set_bookmarked(false);
        return;
    }
    omnibox_->set_star_enabled(true);

    const std::string url = active_tab_url();
    if (!services_.bookmarks || url.empty()) {
        omnibox_->set_bookmarked(false);
        return;
    }
    bool found = false;
    for (const auto& node : services_.bookmarks->all_bookmarks()) {
        if (node.url == url) { found = true; break; }
    }
    omnibox_->set_bookmarked(found);
}

void MainWindow::on_extension_install_request(
    const webview::ExtensionInstallRequest& req) {
    // The file is our private temp copy: whatever the outcome, remove it
    // afterwards — a successful install registers its own copy, and a
    // declined install must not leave a stray .crx behind.
    struct TempFileCleanup {
        ~TempFileCleanup() { QFile::remove(QString::fromStdString(path)); }
        std::string path;
    } cleanup{req.path};
    (void)cleanup;

    const QString name = QString::fromStdString(req.file_name);
    if (!services_.extensions) {
        QMessageBox::information(
            this, QStringLiteral("Extension installation is not supported"),
            QStringLiteral("%1 is a packaged Chrome extension, but no "
                           "extension service is attached to this window.")
                .arg(name));
        return;
    }
    const auto result = services_.extensions->install_from_file(req.path);
    switch (result.result) {
        case extensions::CrxInstallResult::Installed:
            QMessageBox::information(
                this, QStringLiteral("Extension installed"),
                QString::fromStdString(result.detail));
            break;
        case extensions::CrxInstallResult::Unsupported:
            // The honest state: a real .crx, but packaged installs need
            // Chrome's signing machinery, which CEF does not expose. Say so
            // clearly instead of having saved a dead .crx file.
            QMessageBox::information(
                this, QStringLiteral("Extension installation is not supported"),
                QStringLiteral("%1\n\n%2")
                    .arg(name, QString::fromStdString(result.detail)));
            break;
        case extensions::CrxInstallResult::NotACrx:
            QMessageBox::warning(
                this, QStringLiteral("Not an extension"),
                QStringLiteral("%1\n\n%2")
                    .arg(name, QString::fromStdString(result.detail)));
            break;
    }
}

void MainWindow::on_permission_request(webview::PermissionRequest req) {
    if (!permission_prompt_) {
        permission_prompt_ = new PermissionPrompt(this);
        permission_prompt_->set_store(permission_store_.get());
    }
    permission_prompt_->enqueue(std::move(req), permission_store_.get(),
                                omnibox_);
}

void MainWindow::show_downloads_popover() {
    if (!downloads_btn_) return;
    if (!downloads_popover_) {
        downloads_popover_ = new DownloadsPopover(
            services_.downloads, this,
            downloads_actions<DownloadsPopover::Actions>());
        connect(downloads_popover_, &DownloadsPopover::full_history_requested,
                this, [this] {
                    if (downloads_popover_) downloads_popover_->close();
                    show_page(core::InternalPage::Downloads);
                });
    }
    downloads_popover_->toggle_under(downloads_btn_);
}

bool MainWindow::cancel_download(int64_t row_id) {
    if (row_id <= 0 || !webview_ || !services_.downloads) return false;

    // download_ids_ is keyed by the engine's download id; the downloads UI
    // only knows the DownloadService row. At most a handful of transfers are
    // ever live at once, so a reverse scan is cheaper than keeping (and
    // invalidating) a second index.
    uint32_t engine_id = 0;
    bool found = false;
    for (const auto& [cef_id, row] : download_ids_) {
        if (row == row_id) {
            engine_id = cef_id;
            found = true;
            break;
        }
    }
    // No live engine download for this row: it already reached a terminal
    // state (the mapping is erased then) or belongs to a previous session.
    // Do NOT mark it cancelled — finish(Cancelled) is monotonic and would
    // block a legitimate later Complete.
    if (!found) return false;

    if (!webview_->cancel_download(engine_id)) return false;

    // The mapping deliberately stays: the engine still owes us this download's
    // terminal "cancelled" event, and the entry is what routes that event to
    // THIS row instead of recording the same download a second time.
    services_.downloads->finish(row_id, downloads::DownloadState::Cancelled);
    return true;
}

bool MainWindow::pause_download(int64_t row_id) {
    if (row_id <= 0 || !webview_ || !services_.downloads) return false;

    // Same row-id → engine-id lookup as cancel; a paused download keeps its
    // mapping (paused is not terminal), so the lookup finds live transfers
    // and paused ones alike — pausing an already-paused row is harmless
    // engine-side and a no-op service-side.
    for (const auto& [cef_id, row] : download_ids_) {
        if (row != row_id) continue;
        if (!services_.downloads->pause(row_id)) return false;
        webview_->pause_download(cef_id);
        return true;
    }
    return false;
}

bool MainWindow::resume_download(int64_t row_id) {
    if (row_id <= 0 || !webview_ || !services_.downloads) return false;

    // The service-side flip happens first: if the row is not paused the click
    // is stale UI and there is nothing to ask the engine about.
    for (const auto& [cef_id, row] : download_ids_) {
        if (row != row_id) continue;
        if (!services_.downloads->resume(row_id)) return false;
        webview_->resume_download(cef_id);
        return true;
    }
    return false;
}

bool MainWindow::retry_download(int64_t row_id) {
    if (row_id <= 0 || !webview_ || !services_.downloads) return false;

    // Only failed transfers offer retry: the UI gates on is_retryable(), but
    // the gate lives here too so a stale or scripted call cannot restart a
    // finished download behind the user's back.
    const auto item = services_.downloads->find(row_id);
    if (!item || !item->is_retryable()) return false;

    // The engine gets a fresh download for the same URL; it surfaces as a new
    // row through the normal download callback. The old failed row stays as
    // history — replacing it would hide what actually happened.
    return webview_->start_download(item->url);
}

void MainWindow::show_tab_search_menu() {
    // A plain menu of open tabs (title + pinned marker), checked on the
    // active one — the reference's strip dropdown. Search/filtering arrives
    // with a future popup; the list is the affordance that matters today.
    QMenu menu(this);
    menu.setObjectName("TabSearchMenu");
    const auto& tabs = tabs_.tabs();
    for (const auto& t : tabs) {
        QString title = QString::fromStdString(t.title.empty() ? "New Tab" : t.title);
        if (t.pinned) title.prepend(QStringLiteral("\u2605 "));
        QAction* a = menu.addAction(title);
        a->setCheckable(true);
        a->setChecked(t.id == tabs_.active());
        const auto id = t.id;
        connect(a, &QAction::triggered, &menu, [this, id] {
            if (tabs_.contains(id)) tabs_.set_active(id);
        });
    }
    if (menu.isEmpty()) {
        QAction* a = menu.addAction(QStringLiteral("No tabs"));
        a->setEnabled(false);
    }
    if (tab_search_btn_) {
        // Hung under the chevron, clamped to the screen (the frameless
        // window can sit flush against a screen edge).
        exec_menu_under(menu, tab_search_btn_);
    }
}

void MainWindow::on_zoom_in() {
    if (tabs_.active().valid() && webview_) {
        webview_->zoom(tabs_.active(), 1);
    }
}

void MainWindow::on_zoom_out() {
    if (tabs_.active().valid() && webview_) {
        webview_->zoom(tabs_.active(), -1);
    }
}

void MainWindow::on_zoom_reset() {
    if (tabs_.active().valid() && webview_) {
        webview_->zoom_reset(tabs_.active());
    }
}

void MainWindow::on_print() {
    const auto tab = tabs_.active();
    // Internal pages are Qt widgets, not engine documents: nothing to print.
    if (!tab.valid() || page_of(tab).has_value() || !webview_ ||
        !webview_->print(tab)) {
        flash_status(QStringLiteral("This page can't be printed"));
        return;
    }
#if defined(Q_OS_LINUX)
    // On Linux the engine asks the desktop for a print dialog (the XDG print
    // portal, else GTK). On a system with neither, Chromium has no dialog to
    // show and gives the embedder no failure signal, so say up front where
    // the always-available fallback is instead of leaving a silent no-op.
    flash_status(QStringLiteral(
        "Opening the system print dialog. If none appears, use Save as PDF "
        "in the browser menu."));
#endif
}

void MainWindow::on_save_as_pdf() {
    const auto tab = tabs_.active();
    if (!tab.valid() || page_of(tab).has_value() || !webview_) {
        flash_status(QStringLiteral("This page can't be saved as PDF"));
        return;
    }
    // Suggest "<page title>.pdf" in the downloads folder, like a download.
    QString name;
    if (const auto* state = tabs_.find(tab))
        name = QString::fromStdString(state->title).trimmed();
    if (name.isEmpty()) name = QStringLiteral("page");
    static const QRegularExpression kUnsafe(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    name.replace(kUnsafe, QStringLiteral("_"));
    if (name.size() > 120) name.truncate(120);
    QString dir = services_.prefs
                      ? QString::fromStdString(services_.prefs->downloads_path())
                      : QString();
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save page as PDF"),
        QDir(dir).filePath(name + QStringLiteral(".pdf")),
        QStringLiteral("PDF files (*.pdf)"));
    if (path.isEmpty()) return;  // cancelled
    if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        path += QStringLiteral(".pdf");
    // The dialog ran a nested event loop: the tab may have closed meanwhile.
    if (!tabs_.find(tab)) return;
    const QPointer<MainWindow> guard(this);
    const bool started = webview_->print_to_pdf(
        tab, path.toStdString(), [guard, path](bool ok) {
            if (!guard) return;
            guard->flash_status(
                ok ? QStringLiteral("Saved PDF to %1")
                         .arg(QDir::toNativeSeparators(path))
                   : QStringLiteral("Could not save the page as PDF"));
        });
    if (!started)
        flash_status(QStringLiteral("This page can't be saved as PDF"));
}

void MainWindow::on_filter_list_update_finished(bool ok, const QString& message) {
    // The ad-block engine is process-wide: the first window to see a new
    // list rebuilds it, later windows find engine_current() already true.
    if (ok && services_.filter_lists && webview_ &&
        !services_.filter_lists->engine_current()) {
        const bool adblock_on =
            !services_.prefs ||
            services_.prefs->get_bool(settings::PrefKeys::kAdblockEnabled, true);
        webview_->set_adblock_rules(services_.filter_lists->rules(), adblock_on);
        services_.filter_lists->mark_engine_current();
    }
    if (isActiveWindow()) flash_status(message);
}

void MainWindow::on_next_tab() {
    const auto& tabs = tabs_.tabs();
    if (tabs.empty()) return;
    size_t idx = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].id == tabs_.active()) { idx = i; break; }
    }
    size_t next = (idx + 1) % tabs.size();
    tabs_.set_active(tabs[next].id);
}

void MainWindow::on_prev_tab() {
    const auto& tabs = tabs_.tabs();
    if (tabs.empty()) return;
    size_t idx = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].id == tabs_.active()) { idx = i; break; }
    }
    size_t prev = (idx == 0) ? tabs.size() - 1 : idx - 1;
    tabs_.set_active(tabs[prev].id);
}

void MainWindow::on_theme_toggled() {
    dark_mode_ = !dark_mode_;
    apply_theme();
    // Persist the explicit choice, otherwise the toggle is lost on restart
    // (the theme "didn't save"). Writing "dark"/"light" — not "system" —
    // because toggling is an explicit user decision.
    if (services_.prefs) {
        services_.prefs->set_string(settings::PrefKeys::kTheme,
                                    dark_mode_ ? "dark" : "light");
    }
    // Keep an open settings page's controls in sync with the toggle.
    auto it = pages_.find(core::InternalPage::Settings);
    if (it != pages_.end() && it->second) {
        static_cast<InternalPageWidget*>(it->second)->refresh();
    }
}

void MainWindow::on_duplicate_tab(int index) {
    const auto& tabs = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return;
    const auto& tab = tabs[static_cast<size_t>(index)];
    tabs_.open(tab.url.display());
}

void MainWindow::on_close_tab_at(int index) {
    const auto& tabs = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return;
    if (tabs.size() <= 1) {
        const core::TabId id = tabs[static_cast<size_t>(index)].id;
        tabs_.set_loading(id, false, -1);
        tabs_.close(id);
        if (tabs_.count() == 0) {
            if (QWidget* top = window()) top->close();
        }
        return;
    }
    const core::TabId id = tabs[static_cast<size_t>(index)].id;
    tabs_.set_loading(id, false, -1);
    tabs_.close(id);
}

void MainWindow::on_close_other_tabs(int index) {
    const auto& tabs = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return;
    const core::TabId keep = tabs[static_cast<size_t>(index)].id;
    // Collect first: closing mutates the model's tab list. Pinned tabs stay,
    // as in Chrome.
    std::vector<core::TabId> to_close;
    for (const auto& tab : tabs) {
        if (tab.id != keep && !tab.pinned) to_close.push_back(tab.id);
    }
    if (to_close.empty()) return;
    tabs_.set_active(keep);
    for (const core::TabId id : to_close) {
        tabs_.set_loading(id, false, -1);
        tabs_.close(id);
    }
}

void MainWindow::show_tab_hover_card(int index) {
    // Settings > Tabs & browsing > "Show tab previews".
    if (services_.prefs &&
        !services_.prefs->get_bool(settings::PrefKeys::kTabHoverPreviews,
                                   settings::PrefDefaults::kTabHoverPreviews)) {
        hide_tab_hover_card();
        return;
    }
    const auto& tabs = tabs_.tabs();
    if (!tab_bar_ || index < 0 || static_cast<size_t>(index) >= tabs.size()) {
        hide_tab_hover_card();
        return;
    }
    const auto& tab = tabs[static_cast<size_t>(index)];
    if (!tab_hover_card_) tab_hover_card_ = new TabHoverCard(this);
    hover_card_tab_ = tab.id;

    const std::string raw = tab.url.raw();
    const bool internal = page_of(tab.id).has_value() ||
                          core::parse_internal_url(raw).has_value();
    QString address;
    if (internal) {
        address = QStringLiteral("Aurora");
    } else {
        address = QUrl(QString::fromStdString(raw)).host();
        if (address.startsWith(QLatin1String("www.")))
            address = address.mid(4);   // "github.com", as in Chrome's card
        if (address.isEmpty()) address = QString::fromStdString(raw);
    }

    QString memory;
    if (tab.suspended) {
        memory = QStringLiteral("Suspended - memory freed");
    } else if (!internal) {
        const auto known = tab_memory_.find(tab.id);
        memory = known != tab_memory_.end()
            ? QStringLiteral("Memory usage: %1")
                  .arg(TabHoverCard::format_memory(known->second))
            : QStringLiteral("Memory usage: measuring...");
        // Ask for a fresh figure; the answer updates the card in place.
        if (webview_) webview_->request_memory(tab.id);
    }

    QString title = QString::fromStdString(tab.title);
    if (title.isEmpty() && internal) title = QStringLiteral("Aurora");
    tab_hover_card_->set_content(title, address, memory);
    const QRect local = tab_bar_->tabRect(index);
    tab_hover_card_->show_below(
        QRect(tab_bar_->mapToGlobal(local.topLeft()), local.size()));
    tab_bar_->set_hover_card_visible(true);
}

void MainWindow::hide_tab_hover_card() {
    if (tab_hover_card_) tab_hover_card_->hide();
    if (tab_bar_) tab_bar_->set_hover_card_visible(false);
}

void MainWindow::on_pin_tab(int index) {
    const auto& tabs = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return;
    const auto& tab = tabs[static_cast<size_t>(index)];
    tabs_.pin(tab.id, !tab.pinned);
}

void MainWindow::on_mute_tab(int index) {
    const auto& tabs = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return;
    const auto& tab = tabs[static_cast<size_t>(index)];
    // Capture the desired new mute state BEFORE calling tabs_.mute(), which
    // modifies tab.muted in-place. Reading !tab.muted afterwards would invert
    // the just-set value, sending the wrong command to the CEF browser and
    // making mute appear to do nothing while unmute works.
    const bool new_muted = !tab.muted;
    tabs_.mute(tab.id, new_muted);
    // Apply the mute state to the live CEF browser too. TabManager only
    // tracks the flag; without this the tab bar icon toggled but the page
    // kept playing audio.
    if (webview_) {
        webview_->set_muted(tab.id, new_muted);
    }
}

void MainWindow::on_reload_tab(int index) {
    const auto& tabs = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return;
    const auto& tab = tabs[static_cast<size_t>(index)];
    if (webview_) webview_->reload(tab.id, false);
}

void MainWindow::on_suspend_tab(int index) {
    const auto& list = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
    (void)tabs_.suspend(list[static_cast<size_t>(index)].id);
}

void MainWindow::on_restore_tab(int index) {
    const auto& list = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
    const auto id = list[static_cast<size_t>(index)].id;
    if (tabs_.restore(id)) tabs_.set_active(id);
}

void MainWindow::on_exclude_tab_from_suspension(int index) {
    const auto& list = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
    const auto& tab = list[static_cast<size_t>(index)];
    (void)tabs_.set_suspend_excluded(tab.id, !tab.exclude_from_suspension);
}

void MainWindow::on_copy_tab_url(int index) {
    const auto& tabs = tabs_.tabs();
    if (index < 0 || static_cast<size_t>(index) >= tabs.size()) return;
    const auto& tab = tabs[static_cast<size_t>(index)];
    QApplication::clipboard()->setText(QString::fromStdString(tab.url.display()));
    flash_status("URL copied to clipboard");
}

void MainWindow::update_nav_button_states() {
    auto active = tabs_.active();
    if (!active.valid()) {
        back_btn_->setEnabled(false);
        fwd_btn_->setEnabled(false);
        return;
    }
    const auto* t = tabs_.find(active);
    if (!t) return;
    const auto* stack = find_nav_stack(active);
    const bool chrome_back = stack && stack->can_go_back();
    const bool chrome_fwd = stack && stack->can_go_forward();
    if (page_of(active)) {
        // On an internal page the engine's capability is meaningless: it
        // describes the (hidden) web page behind the widget.
        back_btn_->setEnabled(chrome_back);
        fwd_btn_->setEnabled(chrome_fwd);
        return;
    }
    back_btn_->setEnabled((restored_history_.contains(active)
                               ? t->can_go_back() : t->cef_can_go_back) || chrome_back);
    fwd_btn_->setEnabled((restored_history_.contains(active)
                              ? t->can_go_forward() : t->cef_can_go_forward) || chrome_fwd);
}

void MainWindow::update_loading_state(core::TabId id) {
    const auto* t = tabs_.find(id);
    if (!t) return;
    // Per-tab spinner on the tab strip.
    if (tab_bar_) tab_bar_->set_loading(id, t->loading);

    if (id != tabs_.active()) return;
    // The toolbar belongs to the active tab. A background tab's load callback
    // must not turn this button into Stop (or back into Reload).
    if (reload_stop_btn_) {
        reload_stop_btn_->setIconName(t->loading ? icons::Name::Stop
                                                : icons::Name::Reload);
        reload_stop_btn_->setToolTip(t->loading ? "Stop" : "Reload (F5)");
    }
    if (t->loading) {
        // Load progress is the ONLY loading-state text the bubble shows; it
        // is transient by nature (a load finishes) so it never becomes the
        // dead title band this used to be.
        if (t->load_progress >= 0) {
            set_status(QString("Loading… %1%").arg(t->load_progress));
        } else {
            set_status("Loading…");
        }
    } else {
        // Loading finished: hide the bubble. We deliberately do NOT show the
        // page title or URL here — that was the bug: a permanent dark band
        // under the page.
        set_status(QString());
    }
}

void MainWindow::on_tab_changed(core::TabId id, core::TabChange change) {
    switch (change) {
        case core::TabChange::Added: {
            std::string initial = new_tab_url();
            for (const auto& t : tabs_.tabs()) {
                if (t.id == id) {
                    initial = t.url.raw().empty() ? new_tab_url() : t.url.raw();
                    break;
                }
            }
            // A tab may be born pointing at an internal page (menu action with
            // no tabs open, or a restored aurora:// row). Record that before
            // the host is shown so the page — not an empty host — appears.
            const auto internal = core::parse_internal_url(initial);
            auto* host = new CefHostWidget(
                id, internal ? std::string() : initial, webview_.get(), this);
            hosts_[id] = host;
            // Dormant until set_current_content() says otherwise: adding a host
            // to the stack must never be what creates its web engine.
            host->relinquish();
            content_stack_->addWidget(host);
            if (internal) {
                tab_pages_[id] = *internal;
                const auto page = *internal;
                // A tab born on an internal page has nothing behind it: the
                // stack holds exactly that one entry, so Back stays disabled.
                record_internal_nav(id, page);
                // Titles are set on the next event-loop turn: we are inside a
                // TabManager notification and must not re-enter it here.
                QTimer::singleShot(0, this, [this, id, page] {
                    if (tabs_.contains(id)) {
                        tabs_.set_title(id, core::internal_page_title(page));
                    }
                });
            }
            // During a bulk restore the host stays dormant and invisible: the
            // one chosen tab is shown once, after the loop. Showing each tab in
            // turn here is what created 11 native browsers inside a single
            // synchronous loop and crashed startup.
            if (bulk_open_) {
                sync_current_content();
            } else {
                show_host_for(id);
            }
            break;
        }
        case core::TabChange::SuspendedChanged:
            if (const auto* tab = tabs_.find(id)) {
                if (auto it = hosts_.find(id); it != hosts_.end()) {
                    if (tab->suspended) {
                        if (devtools_tab_ == id) close_devtools_dock();
                        restored_history_.insert(id);
                        // The old native navigation controller is going away.
                        tabs_.set_nav_state(id, false, false);
                        it->second->suspend(tab->url.raw());
                    } else {
                        it->second->restore();
                    }
                }
                update_nav_button_states();
            }
            break;
        case core::TabChange::ActiveChanged:
            show_host_for(id);
            update_nav_button_states();
            update_loading_state(id);
            // The shield reports the ACTIVE tab's page; switch what it shows.
            update_shield_for_tab();
            // Update omnibox with the active tab's URL.
            if (omnibox_) {
                const auto* t = tabs_.find(id);
                if (t) {
                    omnibox_->set_current_url(
                        QString::fromStdString(t->url.display()), false);
                }
            }
            update_bookmark_star();
            break;
        case core::TabChange::Removed: {
            tab_pages_.erase(id);
            nav_stacks_.erase(id);
            engine_url_.erase(id);
            restored_history_.erase(id);
            adblock_counts_.erase(id);
            tab_memory_.erase(id);
            hide_tab_hover_card();   // the close button swallows the press
            auto it = hosts_.find(id);
            if (it != hosts_.end()) {
                auto* host = it->second;
                hosts_.erase(it);
                content_stack_->removeWidget(host);
                // Do NOT delete the host here. destroy() only *starts* the
                // web view's close; the engine still owns a child window
                // parented to this widget and needs several event-loop turns to
                // finish. Deleting the widget now destroyed that parent window
                // mid-close, which the engine treats as its host window going
                // away and answers by closing our top-level window — i.e.
                // closing one tab closed the whole browser. The widget is
                // parked, hidden, until the engine reports the close.
                host->hide();
                host->setParent(nullptr);
                closing_hosts_[id] = host;
                if (webview_) {
                    webview_->destroy(id);
                    // Fallback: if the engine never reports the close (a
                    // crashed renderer, for example) the widget would leak.
                    QTimer::singleShot(5000, this, [this, id] {
                        finish_closing_host(id);
                    });
                } else {
                    finish_closing_host(id);
                }
            }
            // Removing the current widget makes QStackedWidget promote a
            // sibling on its own; put the right thing back in front.
            sync_current_content();
            break;
        }
        case core::TabChange::LoadingStateChanged:
        case core::TabChange::ProgressChanged:
            update_loading_state(id);
            break;
        case core::TabChange::TitleChanged:
            if (id == tabs_.active()) {
                update_loading_state(id);
                // A title change accompanies a committed navigation, so this
                // is also where a same-tab URL change lands.
                update_bookmark_star();
            }
            break;
        case core::TabChange::PinnedChanged:
        case core::TabChange::MutedChanged:
        case core::TabChange::Moved:
        case core::TabChange::Recovered:
            break;
        case core::TabChange::Crashed:
            // mark_crashed() clears the model's loading bit. Reflect that
            // immediately so a failed renderer cannot leave a spinner/Stop
            // affordance stuck on screen.
            update_loading_state(id);
            break;
    }
}

void MainWindow::show_host_for(core::TabId id) {
    // A bulk open (session restore) shows nothing at all: see restore_session.
    if (bulk_open_) {
        sync_current_content();
        return;
    }
    // Internal pages take the content area over from the web host.
    if (auto page = page_of(id)) {
        auto* widget = page_widget(*page);
        // The reader page is shared by the window's reader tabs: hand it the
        // article of the tab being shown (none for a restored reader tab).
        if (auto* reader = qobject_cast<ReaderPage*>(widget)) {
            const auto it = reader_articles_.find(id.value());
            reader->show_article(it != reader_articles_.end() && it->second
                                     ? *it->second
                                     : ReaderArticle{});
        }
        // The skeleton pages re-read their service on every show; the home
        // page is stateless chrome and skips the refresh it does not have.
        if (auto* skeleton = qobject_cast<InternalPageWidget*>(widget)) {
            skeleton->refresh();
        }
        set_current_content(widget, id);
        return;
    }
    auto it = hosts_.find(id);
    if (it != hosts_.end()) {
        set_current_content(it->second, id);
    }
}

QWidget* MainWindow::content_for(core::TabId id) {
    if (!id.valid()) return nullptr;
    if (auto page = page_of(id)) return page_widget(*page);
    auto it = hosts_.find(id);
    return it == hosts_.end() ? nullptr : it->second;
}

void MainWindow::set_current_content(QWidget* w, core::TabId owner) {
    if (!content_stack_) return;
    if (!w) {
        w = content_placeholder_;
        owner = core::TabId(0);
    }

    // Only the widget we are about to show is allowed to own a web engine; any
    // other host must not create one just because Qt shows or reparents it.
    for (auto& [host_tab, host] : hosts_) {
        if (host != w) host->relinquish();
        (void)host_tab;
    }

    if (content_stack_->currentWidget() != w) {
        content_stack_->setCurrentWidget(w);
    }
    shown_tab_ = owner;

    auto* host = qobject_cast<CefHostWidget*>(w);
    if (!host) return;
    if (bulk_open_ && !host->browser_created()) {
        if (bulk_hosts_created_ >= kMaxHostsCreatedDuringBulkOpen) {
            AURORA_LOG(util::LogLevel::Warn, "ui")
                << "refusing to create a web host for tab " << host->tab().value()
                << " during a bulk open (cap " << kMaxHostsCreatedDuringBulkOpen
                << " reached)";
            return;
        }
        ++bulk_hosts_created_;
    }
    host->make_current();
}

void MainWindow::sync_current_content() {
    if (!content_stack_) return;
    if (bulk_open_) {
        // Nothing may be current mid-restore.
        set_current_content(nullptr);
        return;
    }
    const auto active = tabs_.active();
    set_current_content(content_for(active), active);
}

int MainWindow::hosts_created() const {
    int n = 0;
    for (const auto& [id, host] : hosts_) {
        if (host->browser_created()) ++n;
        (void)id;
    }
    return n;
}

int MainWindow::hosts_activated() const {
    int n = 0;
    for (const auto& [id, host] : hosts_) {
        if (host->activation_count() > 0) ++n;
        (void)id;
    }
    return n;
}

int MainWindow::hosts_with_native_window() const {
    int n = 0;
    for (const auto& [id, host] : hosts_) {
        if (host->internalWinId() != 0) ++n;
        (void)id;
    }
    return n;
}

int MainWindow::open_startup_tabs() {
    // No profile: nothing remembered, the seed tab is the whole startup.
    if (!services_.prefs) return 0;
    switch (settings::resolve_startup_mode(*services_.prefs)) {
        case settings::StartupMode::Continue:
            return restore_session();
        case settings::StartupMode::Home:
            // The constructor's seed tab already opened new_tab_url() — the
            // same page the Home button shows.
            return 0;
        case settings::StartupMode::Specific: {
            // Settings > Startup keeps one address or a set of them,
            // separated by commas or whitespace.
            static const QRegularExpression kSeparators(
                QStringLiteral("[\\s,]+"));
            const QStringList entries =
                QString::fromStdString(services_.prefs->get_string(
                                           settings::PrefKeys::kStartupHomeUrl, ""))
                    .split(kSeparators, Qt::SkipEmptyParts);
            if (entries.size() > 1) return open_startup_pages(entries);
            const std::string text =
                entries.isEmpty() ? std::string() : entries.front().toStdString();
            if (auto page = core::parse_internal_url(text)) {
                // aurora://home is what the seed tab already shows.
                if (*page == core::InternalPage::Home) return 0;
                // show_page() always opens its own tab, so drop the seed
                // afterwards to still start with exactly one tab.
                const auto seed = tabs_.active();
                show_page(*page);
                if (seed.valid() && tabs_.count() > 1) tabs_.close(seed);
                return 0;
            }
            // Interpreted exactly like typing it into the omnibox, so
            // "example.com" works and https-only upgrading still applies.
            const core::Url url = core::parse_omnibox_input(text);
            if (!url.is_valid()) {
                AURORA_LOG(util::LogLevel::Warn, "session")
                    << "startup page '" << text
                    << "' is not a valid address; opening the home page";
                return 0;
            }
            // Reuse the seed tab rather than opening a second one, so the
            // window starts with exactly one tab, like the other modes.
            navigate_active_tab(url);
            return 0;
        }
    }
    return 0;
}

int MainWindow::open_startup_pages(const QStringList& entries) {
    // Several startup pages open through the same bulk path as a restored
    // session (session::run_restore): every tab is opened dormant, the seed
    // tab is closed, and only the first page gets a web host right away.
    const bool https_only = services_.prefs
        ? services_.prefs->get_bool(settings::PrefKeys::kHttpsOnly,
                                    settings::PrefDefaults::kHttpsOnly)
        : settings::PrefDefaults::kHttpsOnly;
    session::RestorePlan plan;
    for (const QString& entry : entries) {
        if (plan.tabs.size() >= session::kMaxRestoredTabs) break;
        const std::string text = entry.toStdString();
        session::SavedTab tab;
        if (core::parse_internal_url(text).has_value()) {
            // Internal pages reach the tab lifecycle verbatim, exactly like
            // open_url_in_new_tab().
            tab.url = text;
        } else {
            const core::Url url = core::parse_omnibox_input(text);
            if (!url.is_valid()) {
                AURORA_LOG(util::LogLevel::Warn, "session")
                    << "startup page '" << text
                    << "' is not a valid address; skipping it";
                continue;
            }
            tab.url = core::Url(core::resolve_navigation(
                                    url, resolved_search_engine().url_template,
                                    https_only))
                          .raw();
        }
        tab.tab_index = static_cast<int>(plan.tabs.size());
        plan.tabs.push_back(std::move(tab));
    }
    if (plan.tabs.empty()) return 0;
    const auto outcome = session::run_restore(
        tabs_, plan, tabs_.active(),
        [this](bool on) {
            bulk_open_ = on;
            if (on) bulk_hosts_created_ = 0;
        },
        [this](core::TabId id) { show_host_for(id); });
    AURORA_LOG(util::LogLevel::Info, "session")
        << "opened " << outcome.opened << " startup page(s)";
    return outcome.opened;
}

int MainWindow::restore_session() {
    if (!services_.session) return 0;
    // Honor the Startup preference: only "continue where you left off"
    // restores. resolve_startup_mode() falls back to the legacy
    // kRestoreSession boolean when no startup.mode was ever saved.
    if (services_.prefs &&
        settings::resolve_startup_mode(*services_.prefs) !=
            settings::StartupMode::Continue) {
        return 0;
    }

    auto saved = services_.session->load_last();
    if (!saved) return 0;

    // Every stored row is validated before it can reach a tab or a widget.
    const auto plan = session::plan_restore(*saved);
    if (plan.skipped > 0) {
        AURORA_LOG(util::LogLevel::Warn, "session")
            << "skipped " << plan.skipped << " unrestorable saved tab(s)";
    }
    if (plan.tabs.empty()) return 0;

    AURORA_LOG(util::LogLevel::Info, "session")
        << "restoring session with " << plan.tabs.size() << " tab(s)";

    // The window always has its seed tab at this point; it is closed only
    // after at least one restored tab exists, so the window is never empty.
    //
    // The sequencing lives in session::run_restore (core, unit-tested): every
    // tab is opened with bulk mode on, so the Added/ActiveChanged handlers keep
    // their hosts dormant and the content stack stays on its placeholder. Bulk
    // mode ends before the single chosen tab is activated, so exactly one web
    // host is ever created — instead of one per restored tab inside this loop,
    // which is what crashed startup with an 11-tab session.
    const auto seed = tabs_.active();
    const auto outcome = session::run_restore(
        tabs_, plan, seed,
        [this](bool on) {
            bulk_open_ = on;
            if (on) bulk_hosts_created_ = 0;
        },
        [this](core::TabId id) { show_host_for(id); },
        [](int done, int total, const std::string& url) {
            AURORA_LOG(util::LogLevel::Info, "session")
                << "restored tab " << done << "/" << total << ": " << url;
        });
    const int opened = outcome.opened;

    AURORA_LOG(util::LogLevel::Info, "session")
        << "restored " << opened << " tab(s); showing tab "
        << tabs_.active().value() << " (" << hosts_.size()
        << " host(s), " << hosts_created() << " engine(s) created)";
    flash_status(QString("Restored %1 tab%2 from your last session")
                     .arg(opened)
                     .arg(opened == 1 ? "" : "s"));
    return opened;
}

void MainWindow::save_session() {
    // core::IncognitoPolicy: a private window never persists its session.
    // `services_.session` is already null in a private window (constructor);
    // this guard is belt-and-braces in case a future code path re-attaches one.
    if (incognito_) return;
    if (!services_.session) return;
    auto snapshot = tabs_.snapshot();
    if (snapshot.empty()) return;
    services_.session->save(snapshot);
    AURORA_LOG(util::LogLevel::Info, "session")
        << "saved session with " << snapshot.size() << " tab(s)";
}

void MainWindow::prepare_shutdown() {
    // Tear down ONLY this window: its extension popup and its own WebViewController.
    // Other MainWindows keep their hosts_/webview_ intact. The global
    // cef::shutdown() lives in main.cpp and runs only after app.exec() returns
    // (i.e. after the last window has explicitly quit).
    close_extension_popup();
    if (webview_) webview_->shutdown();
}

void MainWindow::showEvent(QShowEvent* e) {
    if (shown_at_ == std::chrono::steady_clock::time_point{}) {
        shown_at_ = std::chrono::steady_clock::now();
    }
#if defined(Q_OS_WIN)
    // The HWND exists by the time we get a show event. Guarantee the native
    // resizable frame (WS_THICKFRAME | WS_CAPTION | WS_MIN/MAXIMIZEBOX) is
    // present so Windows will offer Aero Snap; WM_NCCALCSIZE (below) then
    // strips the visible title bar. Idempotent and runs at most once.
    if (!native_frame_ensured_) {
        if (auto* w = windowHandle()) {
            if (aurora_win::ensure_native_frame(reinterpret_cast<void*>(w->winId()))) {
                AURORA_LOG(util::LogLevel::Info, "ui")
                    << "native frame ensured (WS_THICKFRAME set for Aero Snap)";
            }
            native_frame_ensured_ = true;
        }
    }
#endif
    // Another window (or a previous session) may have changed the bookmark
    // tree while this one was hidden.
    reload_bookmark_bar();
    QMainWindow::showEvent(e);
}

void MainWindow::restore_saved_window_state() {
    // Private windows never read or write placement, and neither does a
    // window without a profile.
    if (incognito_ || !services_.prefs) return;
    const std::string state = services_.prefs->get_string(
        settings::PrefKeys::kWindowState, "maximized");
    const QStringList parts = QString::fromStdString(
        services_.prefs->get_string(settings::PrefKeys::kWindowRect, ""))
                                  .split(QLatin1Char(','));
    if (parts.size() == 4) {
        const QRect saved(parts[0].toInt(), parts[1].toInt(),
                          parts[2].toInt(), parts[3].toInt());
        // Only reuse a rectangle that still lands on a connected screen
        // (a monitor may have been unplugged since), and is a sane size.
        bool on_screen = false;
        for (const QScreen* screen : QGuiApplication::screens()) {
            const QRect overlap = screen->availableGeometry().intersected(saved);
            if (overlap.width() >= 120 && overlap.height() >= 80) {
                on_screen = true;
                break;
            }
        }
        if (on_screen && saved.width() >= 400 && saved.height() >= 320) {
            setGeometry(saved);
        }
    }
    // Maximized is applied by showMaximized(); the caller's show() reads this.
    if (state != "normal") setWindowState(windowState() | Qt::WindowMaximized);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (privacy_exit_started_) {
        e->accept();
        return;
    }
    // Remember the window placement for the next launch. normalGeometry() is
    // the restored rectangle even while maximized, so un-maximizing later
    // still lands somewhere sensible. Fullscreen counts as maximized.
    if (!incognito_ && services_.prefs && !services_.prefs->is_read_only()) {
        const bool maximized = isMaximized() || isFullScreen();
        const QRect r = (isMaximized() || isFullScreen()) ? normalGeometry()
                                                          : geometry();
        services_.prefs->set_string(settings::PrefKeys::kWindowState,
                                    maximized ? "maximized" : "normal");
        services_.prefs->set_string(
            settings::PrefKeys::kWindowRect,
            QStringLiteral("%1,%2,%3,%4")
                .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                .toStdString());
    }
    // Download clients retain a copy of the host callback, so clearing the
    // controller's callback alone would not suppress late persistent writes.
    privacy_exit_started_ = true;
    // Diagnostic for the startup crash: the browser was closing within ~1s of
    // "window shown" with a clean shutdown and no Chromium ERROR line. Logging
    // whether the close was OS/window-manager-initiated (spontaneous) and how
    // long the window lived pinpoints whether this is a crash-induced WM_CLOSE
    // or a genuine early exit. spontaneous() is a hint, not proof of a crash.
    const auto lived_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shown_at_)
            .count();
    AURORA_LOG(util::LogLevel::Info, "ui")
        << "window closing: spontaneous=" << (e->spontaneous() ? 1 : 0)
        << " lived=" << lived_ms << "ms tabs=" << tabs_.tabs().size();
    save_session();
    // Close this window's CEF browsers only. With quitOnLastWindowClosed=false
    // we must quit explicitly when the last *visible* MainWindow goes away,
    // otherwise the process would hang; with other windows still open we must
    // NOT quit, otherwise closing one window takes the rest down with it.
    //
    // Only a window that was still VISIBLE when told to close may count as
    // "the last one": an emptied tear-off/transfer source is hidden before
    // its deferred close (see transfer_tab_to), and quitting from its
    // closeEvent would have taken the destination window — and the whole
    // app — down with it.
    // Decide quit eligibility BEFORE hiding: visible_main_window_count()
    // would otherwise drop this window from the visible count and let closing
    // one of several windows quit the whole app. Capture the decision, then
    // hide so the window disappears instantly while the CEF browser teardown
    // (the drain in prepare_shutdown -> webview_->shutdown) runs invisibly.
    // Without this, the window stays on screen and appears frozen for the full
    // teardown (up to the 5s drain) — the "takes forever to close" symptom.
    const bool was_visible = isVisible();
    const bool is_last_visible = was_visible && visible_main_window_count() <= 1;
    if (was_visible) hide();  // vanish now; teardown happens off-screen
    prepare_shutdown();
    clear_on_exit();
    if (is_last_visible) {
        QApplication::quit();
    }
    QMainWindow::closeEvent(e);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        if (!find_bar_->isHidden()) {
            on_find_close();
            return;
        }
        if (fullscreen_tab_.valid() || isFullScreen()) {
            on_fullscreen_toggle();
            return;
        }
    }
    QMainWindow::keyPressEvent(e);
}

// --- Tor incognito windows ------------------------------------------------------

void MainWindow::open_tor_window() {
    // Built-in Tor (like Brave): start the bundled tor on first use, show its
    // bootstrap progress, and open the window once a circuit is ready.
    if (use_bundled_tor(services_.prefs)) {
        TorLauncher& tor = TorLauncher::instance();
        if (tor.state() == TorLauncher::State::Ready) {
            spawn_tor_window();
            return;
        }
        auto* dlg = new QProgressDialog(QStringLiteral("Connecting to the Tor network…"),
                                        QStringLiteral("Cancel"), 0, 100, this);
        dlg->setWindowTitle(QStringLiteral("Tor"));
        dlg->setAttribute(Qt::WA_DeleteOnClose, true);
        dlg->setAutoClose(false);
        dlg->setAutoReset(false);
        dlg->setMinimumDuration(0);
        dlg->setValue(tor.progress());
        const QPointer<MainWindow> guard(this);
        connect(&tor, &TorLauncher::progress_changed, dlg, [dlg](int percent, const QString& summary) {
            dlg->setValue(percent);
            dlg->setLabelText(QStringLiteral("Connecting to the Tor network… %1%\n%2")
                                  .arg(percent).arg(summary));
        });
        connect(&tor, &TorLauncher::ready, dlg, [dlg, guard] {
            dlg->close();
            if (guard) guard->spawn_tor_window();
        });
        connect(&tor, &TorLauncher::failed, dlg, [dlg, guard](const QString& why) {
            dlg->close();
            if (!guard) return;
            QMessageBox box(QMessageBox::Warning, QStringLiteral("Tor could not connect"),
                            QStringLiteral("Aurora's built-in Tor could not connect."),
                            QMessageBox::Ok, guard);
            box.setInformativeText(why + QStringLiteral(
                "\n\nNo page was loaded outside Tor. You can retry, or point Tor "
                "windows at your own Tor daemon with AURORA_TOR_PROXY."));
            box.exec();
        });
        // Cancel only closes the dialog: tor keeps bootstrapping in the
        // background so the next "New Tor window" opens instantly.
        connect(dlg, &QProgressDialog::canceled, dlg, &QWidget::close);
        dlg->show();
        tor.start();
        return;
    }

    const std::string proxy = tor_proxy_setting(services_.prefs);
    QString why;
    if (!probe_socks5(proxy, &why)) {
        QMessageBox box(QMessageBox::Warning, QStringLiteral("Tor is not running"),
                        QStringLiteral("This build of Aurora does not include the built-in "
                                       "Tor, so a Tor incognito window needs a Tor daemon "
                                       "that you run yourself."),
                        QMessageBox::Ok, this);
        box.setInformativeText(
            why + QStringLiteral(
                      "\n\nStart Tor and try again:\n"
                      "  • Linux: sudo apt install tor && sudo systemctl start tor\n"
                      "  • macOS: brew install tor && brew services start tor\n"
                      "  • Windows: run the Tor Expert Bundle (tor.exe)\n"
                      "  • Tor Browser running? Its proxy is on port 9150: set "
                      "AURORA_TOR_PROXY=socks5://127.0.0.1:9150\n\n"
                      "Expected proxy: %1").arg(QString::fromStdString(proxy)));
        box.exec();
        return;
    }
    spawn_tor_window();
}

void MainWindow::spawn_tor_window() {
    auto* w = new MainWindow(services_, /*parent=*/nullptr,
                             /*is_incognito=*/true, /*is_tor=*/true);
    w->setAttribute(Qt::WA_DeleteOnClose, true);
    w->inherit_theme_from(this);
    w->resize(size());
    if (!isMaximized() && !isFullScreen()) w->move(pos() + QPoint(24, 24));
    w->show();
    w->raise();
    w->activateWindow();
}

// --- Reader mode -----------------------------------------------------------------

void MainWindow::on_reader_mode() {
    const auto tab = tabs_.active();
    if (!tab.valid() || !webview_ || page_of(tab).has_value()) {
        flash_status(QStringLiteral("Reader mode works on web pages"));
        return;
    }
    const QString source = QString::fromStdString(active_tab_url());
    flash_status(QStringLiteral("Preparing the reader view…"));
    const QPointer<MainWindow> guard(this);
    webview_->evaluate_script(
        tab, ReaderPage::extraction_script(),
        [guard, source](bool ok, std::string result) {
            if (!guard) return;
            auto article = std::make_shared<ReaderArticle>(ReaderPage::parse_result(
                ok, QString::fromStdString(result), source));
            // Open the tab on the next turn, outside the engine callback.
            QTimer::singleShot(0, guard.data(), [guard, article] {
                if (!guard) return;
                const core::TabId id =
                    guard->tabs_.open(core::internal_url(core::InternalPage::Reader));
                guard->reader_articles_[id.value()] = article;
                if (guard->tabs_.active() == id) guard->show_host_for(id);
                if (!article->ok)
                    guard->flash_status(QStringLiteral("No readable article on this page"));
            });
        });
}

// --- Screenshots -----------------------------------------------------------------

void MainWindow::on_screenshot(int mode) {
    const auto tab = tabs_.active();
    if (!tab.valid() || !webview_ || page_of(tab).has_value()) {
        flash_status(QStringLiteral("Screenshots work on web pages"));
        return;
    }
    QString name = QStringLiteral("Aurora screenshot %1")
                       .arg(QDateTime::currentDateTime().toString(
                           QStringLiteral("yyyy-MM-dd HH.mm.ss")));
    const auto engine_mode = mode == 1
        ? webview::WebViewController::ScreenshotMode::FullPage
        : webview::WebViewController::ScreenshotMode::Visible;
    const QPointer<MainWindow> guard(this);
    webview_->capture_screenshot(
        tab, engine_mode, [guard, mode, name](bool ok, std::vector<uint8_t> png) {
            if (!guard) return;
            QImage image;
            if (ok) {
                image.loadFromData(png.data(), static_cast<int>(png.size()), "PNG");
            }
            if (image.isNull()) {
                guard->flash_status(QStringLiteral("Could not capture this page"));
                return;
            }
            // Dialogs run nested event loops: never inside the engine callback.
            QTimer::singleShot(0, guard.data(), [guard, image, mode, name] {
                if (guard) guard->present_screenshot(image, mode, name);
            });
        });
}

void MainWindow::present_screenshot(const QImage& captured, int mode, const QString& name) {
    QImage image = captured;
    if (mode == 2) {
        RegionSelectDialog select(captured, this);
        if (select.exec() != QDialog::Accepted) return;
        image = select.cropped();
        if (image.isNull()) return;
    }
    if (auto* cb = QGuiApplication::clipboard()) cb->setImage(image);
    auto* dialog = new ScreenshotResultDialog(image, name, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

// --- Night mode ------------------------------------------------------------------

bool MainWindow::night_mode_enabled_for(const std::string& origin) const {
    if (origin.empty()) return false;
    const auto session = night_mode_session_.find(origin);
    if (session != night_mode_session_.end()) return session->second;
    if (!services_.prefs) return false;
    return settings::SiteNightModeStore(*services_.prefs).enabled(origin);
}

void MainWindow::apply_night_mode(core::TabId id, const std::string& url) {
    if (!webview_) return;
    webview_->set_night_mode(id, night_mode_enabled_for(night_mode_origin(url)));
}

void MainWindow::on_toggle_night_mode() {
    const auto tab = tabs_.active();
    const std::string origin = night_mode_origin(active_tab_url());
    if (!tab.valid() || !webview_ || page_of(tab).has_value() || origin.empty()) {
        flash_status(QStringLiteral("Night mode works on web pages"));
        return;
    }
    const bool on = !night_mode_enabled_for(origin);
    // Remembered per site in the profile, like zoom and Shields exceptions;
    // a private window keeps the choice for its own lifetime only.
    if (services_.prefs && !services_.prefs->is_read_only() && !incognito_) {
        settings::SiteNightModeStore(*services_.prefs).set(origin, on);
        night_mode_session_.erase(origin);
    } else {
        night_mode_session_[origin] = on;
    }
    // Every tab of this window on the same site follows.
    for (const auto& t : tabs_.tabs()) {
        if (night_mode_origin(t.url.raw()) == origin) webview_->set_night_mode(t.id, on);
    }
    flash_status(on ? QStringLiteral("Night mode on for %1").arg(QString::fromStdString(origin))
                    : QStringLiteral("Night mode off for %1").arg(QString::fromStdString(origin)));
}

// --- Share as QR code ----------------------------------------------------------

void MainWindow::on_share_qr() {
    const auto tab = tabs_.active();
    if (!tab.valid() || page_of(tab).has_value()) {
        flash_status(QStringLiteral("Only web pages can be shared as a QR code"));
        return;
    }
    QString title;
    if (const auto* state = tabs_.find(tab)) title = QString::fromStdString(state->title);
    QrShareDialog::show_for(this, QString::fromStdString(active_tab_url()), title);
}

} // namespace aurora::ui

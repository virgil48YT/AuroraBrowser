// Aurora — web view controller interface.
//
// The browser core talks to a "web view" through this abstract interface. The
// only concrete implementation today is CEF (src/cef/CefAdapter). Keeping the
// interface free of CEF types means:
//   * The core library and tests never include CEF headers.
//   * A future engine (e.g. a second CEF instance for the chrome UI, or an
//     alternative embedding) can be swapped in behind the same interface.
//
// All methods are called on the UI thread. Callbacks fire on the UI thread
// too; the CEF implementation is responsible for marshalling off-thread
// browser events back to the UI thread via its message loop.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "core/Types.hpp"
#include "core/Url.hpp"

namespace aurora::webview {

struct PageInfo {
    std::string title;
    std::string favicon_url;
    std::string status_text;  // e.g. link URL on hover
    bool loading = false;
    int load_progress = -1;   // 0..100, -1 = none
    // CEF's real back/forward capability for the main browser. Only meaningful
    // when has_nav_state is true (set by OnLoadingStateChange); other callbacks
    // leave these false and MUST NOT reset the toolbar buttons.
    bool has_nav_state = false;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool is_secure = false;   // https with valid cert
    std::string security_origin;  // scheme://host for the lock indicator
    // The main frame's current committed URL. Only populated by an address
    // change on the main frame — never by title/loading callbacks. The host
    // uses this to update the omnibox (unless the user is editing it) and to
    // record a history visit; an empty value means "no address change" and
    // must NOT overwrite the omnibox (which was clearing it on every title
    // callback).
    std::string url;
    // Optional lifecycle deltas. has_audible_state distinguishes silence from
    // callbacks unrelated to audio. Crash/recovery flags are discrete events.
    bool has_audible_state = false;
    bool audible = false;
    bool renderer_crashed = false;
    bool renderer_recovered = false;
    bool renderer_out_of_memory = false;
    // -1 means no download-count update (including in title/progress events).
    int pending_downloads = -1;
    // Memory the tab's renderer process currently uses, in bytes, as answered
    // to request_memory(). -1 means this callback carries no memory figure.
    int64_t memory_bytes = -1;
};

// A raw key event observed by the engine BEFORE the page sees it, forwarded
// to the host so chrome-level accelerators work while the engine's native
// child window owns the keyboard (in windowed CEF, Qt receives no key events
// at all then, so no QShortcut can fire without this bridge).
//
// `key_code` is the Windows virtual-key code (event.windows_key_code in CEF,
// populated identically on every platform), so the host maps it with one
// small table instead of per-OS keymaps. `ctrl/shift/alt` are the held
// modifiers. Only accelerator-looking keys are ever forwarded — plain typing
// stays with the page.
struct EngineKeyEvent {
    int key_code = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool key_down = true;
};

// A media/permission request from a web page (getUserMedia, Notification,
// ...), surfaced so the host can ask the user. The engine calls NOTHING back
// until `respond` runs: the page's request simply waits, exactly like a
// Chrome prompt the user hasn't answered yet.
//
// `respond(true)` grants exactly the capabilities named by the flags
// (never more), `respond(false)` denies. It is safe to call from any later
// turn of the UI thread, exactly once; calling it twice is a no-op, and
// destroying the PermissionRequest without responding denies by default
// (default-deny: no path may grant silently).
struct PermissionRequest {
    core::TabId tab;
    std::string origin;   // scheme://host[:port] of the requesting page
    bool audio = false;   // microphone
    bool video = false;   // camera
    bool notification = false;  // desktop notification permission
    std::function<void(bool /*allow*/)> respond;
};

// A downloaded file that is actually a packaged Chrome extension (.crx /
// .crx3): the engine recognized the MIME type / filename during the download
// and, once the file completed, hands it to the host to install via
// ExtensionService instead of leaving it in the Downloads folder. The host
// decides what to do with `path` (and may delete it — the file is a private
// copy, not a user download).
struct ExtensionInstallRequest {
    core::TabId tab;
    std::string url;        // where the .crx came from
    std::string file_name;  // suggested file name (for dialogs)
    std::string path;       // downloaded file on disk
};

// A download lifecycle event surfaced from the web view (CEF) to the host.
// `state`: 0 = in progress, 1 = complete, 2 = cancelled, 3 = interrupted.
// The host maps these to DownloadService rows (begin/update_progress/finish).
struct DownloadEvent {
    uint32_t id = 0;          // CEF download id (per-session)
    std::string url;
    std::string file_name;    // suggested/actual file name
    std::string target_path;  // full path the file is saved to
    std::string mime;
    int64_t total_bytes = -1; // -1 when the server didn't advertise a length
    int64_t received = 0;
    int state = 0;
    // Tri-state pause flag: -1 = this callback says nothing about pausing
    // (older engines), 0 = running, 1 = paused. A state-0 event with paused=1
    // is how the engine reports a user (or engine) pause without ending the
    // transfer; paused=0 on a previously paused row means it resumed.
    int paused = -1;
};

class WebViewController {
public:
    using PageInfoCallback = std::function<void(core::TabId, const PageInfo&)>;
    using DownloadCallback = std::function<void(const DownloadEvent&)>;

    virtual ~WebViewController() = default;

    // Create and host a web view for `tab`. The native window handle (HWND on
    // Windows, GdkWindow on X11, etc.) is passed in as a platform-specific
    // pointer; the CEF implementation knows how to parent a child browser.
    virtual void create(core::TabId tab, std::string_view initial_url,
                        void* native_parent) = 0;

    virtual void destroy(core::TabId tab) = 0;
    virtual void set_visible(core::TabId tab, bool visible) = 0;
    // Reduce background renderer work without changing ordinary tab visibility.
    virtual void set_suspended(core::TabId tab, bool suspended) {
        (void)tab; (void)suspended;
    }
    // Tell the web view its host surface changed size (width/height in px).
    // In windowed CEF this resizes the child browser window to match.
    virtual void resize(core::TabId tab, int width, int height) = 0;

    // Give `tab`'s browser — i.e. its native child window — input focus.
    // Needed for synthetic surfaces (extension popups) whose Qt window is
    // shown without activation: nothing ever hands the native CEF child
    // keyboard/mouse focus, so the rendered page looks dead. In windowed CEF
    // this calls CefBrowserHost::SetFocus(true). Default no-op keeps non-CEF
    // / test doubles unaffected.
    virtual void focus(core::TabId tab) { (void)tab; }

    virtual void load(core::TabId tab, const core::Url& url) = 0;
    virtual void back(core::TabId tab) = 0;
    virtual void forward(core::TabId tab) = 0;
    virtual void reload(core::TabId tab, bool bypass_cache = false) = 0;
    virtual void stop(core::TabId tab) = 0;

    // Propagate the browser chrome's semantic theme to web content. CEF
    // implementations set Chromium's preferred-color-scheme preference for
    // this browser/request context and notify the renderer so matchMedia()
    // updates without a reload. It is called before create() and whenever the
    // theme changes; implementations should retain a pre-create value.
    virtual void set_preferred_color_scheme(core::TabId tab, bool dark) {
        (void)tab;
        (void)dark;
    }

    // Find in page, zoom, devtools.
    virtual void find(core::TabId tab, std::string_view query, bool forward) = 0;
    virtual void stop_find(core::TabId tab) = 0;
    virtual void zoom(core::TabId tab, int delta) = 0;
    virtual void zoom_reset(core::TabId tab) = 0;

    // Per-site zoom persistence. The engine knows the page's origin and its
    // zoom level; the host owns storage (the prefs store, see
    // settings::SiteZoomStore). With a store installed, zoom() saves the new
    // level for the active page's origin, zoom_reset() clears it, and every
    // main-frame navigation commit applies the remembered level (or the
    // default) for the origin being shown. `origin` is scheme://host[:port]
    // as the engine derives it; levels use the engine's logarithmic scale
    // (0 = 100%). Any member may be empty. Default no-op: zoom stays per tab.
    struct ZoomStore {
        std::function<bool(const std::string& origin, double& level)> load;
        std::function<void(const std::string& origin, double level)> save;
        std::function<void(const std::string& origin)> clear;
    };
    virtual void set_zoom_store(ZoomStore store) { (void)store; }

    // Printing. print() opens the engine's print dialog for the tab's page
    // (returns false when there is no live page to print). print_to_pdf()
    // renders the page to `path` without a dialog and reports completion on
    // the UI thread exactly once; it returns false — and never calls `done` —
    // when the request could not even be started. Defaults: unsupported.
    virtual bool print(core::TabId tab) {
        (void)tab;
        return false;
    }
    using PdfCallback = std::function<void(bool ok)>;
    virtual bool print_to_pdf(core::TabId tab, std::string_view path,
                              PdfCallback done) {
        (void)tab;
        (void)path;
        (void)done;
        return false;
    }

    // Mute/unmute all audio output for a tab's browser. CEF maps this to
    // CefBrowserHost::SetAudioMuted. The default no-op keeps non-CEF / test
    // doubles unaffected.
    virtual void set_muted(core::TabId tab, bool muted) { (void)tab; (void)muted; }

    // Ask the tab's renderer how much memory it is using. The answer arrives
    // later through the page-info callback (PageInfo::memory_bytes). Engines
    // that cannot measure it simply never answer.
    virtual void request_memory(core::TabId tab) { (void)tab; }

    // Called when a page asks to open a URL in a new window (target=_blank,
    // window.open, etc.). `url` is owned because the host normally queues the
    // actual tab creation until after the engine callback returns; it is empty
    // for window.open() with no URL. Return true once the request has been
    // accepted for in-app handling. Returning false lets the engine keep its
    // normal popup behavior instead of cancelling the request into a no-op.
    using NewTabCallback = std::function<bool(std::string url)>;
    virtual void set_new_tab_callback(NewTabCallback cb) = 0;

    // Downloads. The engine saves files under `downloads_dir` (the OS Downloads
    // folder) and reports each download's lifecycle via the callback so the
    // host can record it in DownloadService and refresh the Downloads page.
    virtual void set_downloads_dir(std::string_view dir) { (void)dir; }
    virtual void set_download_callback(DownloadCallback cb) { (void)cb; }

    // Abort a running download, addressed by the engine-side id carried on
    // every DownloadEvent. Returns true when the engine accepted (or queued)
    // the request; the transfer's terminal "cancelled" event still arrives
    // through the download callback, so the host must not invent one. Engines
    // without download support keep the default no-op.
    virtual bool cancel_download(uint32_t download_id) {
        (void)download_id;
        return false;
    }

    // Private (incognito) browsing support.
    //
    // A window calls `set_private_context(true)` BEFORE creating any web view
    // in that window. Every tab/popup created afterwards must use a distinct
    // in-memory request context whose lifetime is tied to this controller
    // (i.e. to the private window), never to the global/shared context, and
    // must never fall back to the global context. Returns false when the
    // engine does not support private contexts at all — the window must then
    // refuse to open as private rather than silently browsing in the regular
    // profile.
    //
    // NOTE: `set_private_context` is NOT virtual default-no-op on purpose: a
    // real adapter that silently ignores the request would browse in the
    // regular profile, which is the exact failure incognito exists to prevent.
    // Each concrete WebViewController must implement it. The CEF adapter
    // creates a CefRequestContext with an empty cache_path (in-memory).
    virtual bool set_private_context(bool on) = 0;

    // Pause/resume a running download through the same engine-side id. The
    // engine reports the new paused state on the next download callback
    // (DownloadEvent::paused); the host may also set its own row state
    // optimistically. Returns true when the request was performed or queued.
    virtual bool pause_download(uint32_t download_id) {
        (void)download_id;
        return false;
    }
    virtual bool resume_download(uint32_t download_id) {
        (void)download_id;
        return false;
    }

    // Start a fresh download of `url` (the retry path: a failed/cancelled
    // row is retried by re-requesting its stored URL). The engine hosts the
    // transfer in any live browser and reports it like any other download
    // through the download callback (a NEW engine id / row). Returns false
    // when no live browser exists to host the transfer.
    virtual bool start_download(std::string_view url) {
        (void)url;
        return false;
    }

    // --- Ad blocking ---------------------------------------------------
    // Installs the network-filter rules (the "adblock syntax" rule list the
    // bundled brave/adblock-rust engine parses) and the initial enabled/
    // per-site exception snapshot. The engine keeps a private copy of the
    // exceptions, so later changes go through set_adblock_exceptions().
    // Thread-safe: the request interception callbacks run off the UI thread.
    virtual void set_adblock_rules(std::string_view rules, bool enabled) {
        (void)rules;
        (void)enabled;
    }
    // Replace the set of site keys (registrable host, "www." stripped) where
    // blocking is disabled. Empty re-enables blocking everywhere.
    virtual void set_adblock_exceptions(const std::vector<std::string>& sites) {
        (void)sites;
    }
    // Enable/disable the blocker globally without touching the rules.
    virtual void set_adblock_enabled(bool enabled) { (void)enabled; }

    // A request was blocked while loading a page. Delivered on the UI thread,
    // already coalesced per tab: the host receives the total blocked count
    // for the page the tab is currently showing, so the shield badge can be
    // repainted with one number instead of one callback per request.
    using AdblockBlockedCallback =
        std::function<void(core::TabId, uint64_t blocked_count)>;
    virtual void set_adblock_blocked_callback(AdblockBlockedCallback cb) {
        (void)cb;
    }

    // Zero the blocked-request counter for a tab's page. The host calls this
    // on every main-frame address change so the shield badge counts the page
    // being loaded, not the session.
    virtual void reset_adblock_count(core::TabId tab) { (void)tab; }

    // Chrome-accelerator forwarding. While a web page has keyboard focus the
    // engine's native child window — not any Qt widget — receives the keys,
    // so the engine offers each accelerator-looking key event to the host
    // first. The callback runs synchronously on the UI thread INSIDE the
    // engine's key dispatch: it must decide (not act) there — return true to
    // consume the key (the page never sees it) and perform the action itself
    // (posted to the Qt event loop, never inline: shortcut slots create and
    // close engines). Return false to let the page have the key.
    using KeyEventCallback =
        std::function<bool(core::TabId, const EngineKeyEvent&)>;
    virtual void set_key_event_callback(KeyEventCallback cb) { (void)cb; }

    // Packaged-extension routing. When a download is a .crx (by MIME type or
    // file name) the engine still downloads it (the bytes are needed to
    // install), then reports the finished file here INSTEAD of recording a
    // DownloadService row, so the host can run ExtensionService::install_
    // from_file on it and clean the file up. Without this callback the .crx
    // is treated as an ordinary download.
    using ExtensionInstallCallback =
        std::function<void(const ExtensionInstallRequest&)>;
    virtual void set_extension_install_callback(ExtensionInstallCallback cb) {
        (void)cb;
    }

    // Associate a synthetic extension-popup browser with the real Aurora tab
    // that invoked it. CEF 128+ removed the Alloy CefExtensionHandler bridge,
    // so an Alloy-style popup has no Chromium window/tab model of its own.
    // The CEF controller uses this association to install a narrow
    // chrome.tabs.query shim at document start; the returned tab id is the
    // invoking browser's real CEF identifier, so subsequent chrome.tabs
    // operations are routed to that browser host. Call before create().
    virtual void set_extension_popup_owner(core::TabId popup,
                                           core::TabId owner,
                                           std::string_view title,
                                           std::string_view url) {
        (void)popup;
        (void)owner;
        (void)title;
        (void)url;
    }

    // Permission prompts (microphone / camera / notifications). Fired on the
    // UI thread when a page requests a capability. The host MUST eventually
    // call request.respond(...) — the engine-side responder is exactly-once
    // and default-deny on destruction, so dropping the request denies it.
    // Anything the host does not handle (geolocation, desktop capture, MIDI,
    // ...) is denied engine-side without ever reaching this callback.
    using PermissionCallback = std::function<void(PermissionRequest)>;
    virtual void set_permission_callback(PermissionCallback cb) { (void)cb; }

    // Right-click menu actions only the host can perform: the engine has no
    // portable clipboard, and "search for the selection" or "inspect" need the
    // host's search engine and DevTools dock. Fired on the UI thread from the
    // engine's context-menu command handler; the host should defer anything
    // that opens a dialog or runs a nested event loop (post it to its own
    // loop) rather than doing it inside the engine callback.
    enum class ContextAction {
        CopyText,        // `text` -> the system clipboard
        CopyImage,       // `data` is PNG bytes -> the system clipboard
        Back,            // the same as the toolbar button for the active tab
        Forward,
        Reload,
        SavePageAsPdf,
        Print,
        SearchSelection, // `text` is the selected text (already trimmed)
        Inspect,         // open DevTools for the tab
    };
    struct ContextActionRequest {
        ContextAction action = ContextAction::CopyText;
        std::string text;
        std::vector<uint8_t> data;
    };
    using ContextActionCallback =
        std::function<void(core::TabId, ContextActionRequest)>;
    virtual void set_context_action_callback(ContextActionCallback cb) { (void)cb; }

    // Docked DevTools. open_devtools_docked parents the DevTools browser to the
    // given native window handle (a Qt widget's winId) so it renders inside the
    // app instead of a separate OS window. resize_devtools mirrors the host's
    // geometry onto the DevTools child window. close_devtools tears it down.
    // One DevTools browser exists at a time per controller.
    virtual void open_devtools_docked(core::TabId tab, void* native_parent,
                                      int w, int h) {
        (void)tab; (void)native_parent; (void)w; (void)h;
    }
    virtual void resize_devtools(core::TabId tab, int w, int h) {
        (void)tab; (void)w; (void)h;
    }
    virtual void close_devtools(core::TabId tab) { (void)tab; }

    // Called when the docked DevTools CEF browser has finished closing (either
    // because the host called close_devtools or because DevTools was closed from
    // inside its own panel). The host uses this to hide the dock so the user
    // does not see an empty dock panel after the DevTools content is gone.
    using DevToolsClosedCallback = std::function<void()>;
    virtual void set_devtools_closed_callback(DevToolsClosedCallback cb) {
        (void)cb;
    }

    virtual void set_page_info_callback(PageInfoCallback cb) = 0;

    // Called once a tab's web view has *finished* closing. Destroying the web
    // view is asynchronous: the engine tears down its child window, its
    // renderer process and its unload handlers over several event-loop turns.
    // The host must therefore keep the native parent window alive until this
    // fires — deleting the host widget immediately after destroy() pulls the
    // window out from under a browser that is still closing, which the engine
    // reports as an unexpected window teardown and answers by closing the
    // whole top-level window.
    using TabClosedCallback = std::function<void(core::TabId)>;
    virtual void set_tab_closed_callback(TabClosedCallback cb) { (void)cb; }

    // Web content entered or left fullscreen (a video's fullscreen button, the
    // Fullscreen API). The host is responsible for the actual transition: hide
    // the browser chrome and make its window fullscreen, then restore. The
    // engine resizes the content to the host surface either way, so the host
    // must also re-assert the surface geometry after the transition.
    using FullscreenCallback = std::function<void(core::TabId, bool)>;
    virtual void set_fullscreen_callback(FullscreenCallback cb) { (void)cb; }

    // Leave content fullscreen from the host side (F11 / Esc while a page is
    // fullscreen), so the page's own fullscreen state stays in sync instead of
    // the window and the document disagreeing.
    virtual void exit_fullscreen(core::TabId tab) { (void)tab; }

    // Synchronously close every browser the controller owns and pump its
    // message loop until the closes have actually completed (or a bounded
    // timeout). Engines that own out-of-process renderers (CEF) MUST tear down
    // all browsers before the host calls its CefShutdown(), otherwise the
    // child processes are not reaped and keep the executable / shared
    // libraries mapped. The default no-op keeps non-CEF / test doubles
    // unaffected. Called once on the UI thread, after the last window closes
    // and before the engine runtime is shut down.
    virtual void shutdown() {}
};

} // namespace aurora::webview

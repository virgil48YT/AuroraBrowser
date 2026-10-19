// Aurora — CEF-backed WebViewController.
//
// Bridges the dependency-free webview::WebViewController interface to CEF's
// CefClient / CefLifeSpanHandler / CefLoadHandler. Each tab gets one
// CefBrowser hosted as a native child window inside the Qt widget tree.
//
// Thread model: all methods are called on the UI thread. CEF fires most
// callbacks on the UI thread when multi_threaded_message_loop is false (which
// is what we set in CefLifecycle). We marshal anything that arrives off-thread
// back to the UI thread via CefPostTask(TID_UI, ...).
//
// Compiled only when AURORA_ENABLE_CEF is on. See CefLifecycle.hpp for the
// honesty note on testing.
#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "cef/CefLifecycle.hpp"
#include "include/cef_browser.h"
#include "webview/WebViewController.hpp"

namespace aurora::cef {

using core::TabId;

class CefAdapter : public webview::WebViewController {
public:
    CefAdapter() = default;
    ~CefAdapter() override;

    // True when this adapter serves a private (incognito) window. Must be set
    // BEFORE the first create() call in that window, otherwise create() would
    // fall back to the global (regular-profile) context.
    bool set_private_context(bool on) override;

    void create(TabId tab, std::string_view initial_url,
                void* native_parent) override;
    void destroy(TabId tab) override;
    void set_visible(TabId tab, bool visible) override;
    void set_suspended(TabId tab, bool suspended) override;
    void resize(TabId tab, int width, int height) override;
    void focus(TabId tab) override;

    void load(TabId tab, const core::Url& url) override;
    void back(TabId tab) override;
    void forward(TabId tab) override;
    void reload(TabId tab, bool bypass_cache = false) override;
    void stop(TabId tab) override;

    void find(TabId tab, std::string_view query, bool forward) override;
    void stop_find(TabId tab) override;
    void zoom(TabId tab, int delta) override;
    void zoom_reset(TabId tab) override;
    void set_zoom_store(ZoomStore store) override;
    bool print(TabId tab) override;
    bool print_to_pdf(TabId tab, std::string_view path,
                      PdfCallback done) override;
    void set_muted(TabId tab, bool muted) override;
    void request_memory(TabId tab) override;
    void evaluate_script(TabId tab, std::string_view script,
                         ScriptResultCallback done) override;
    void capture_screenshot(TabId tab, ScreenshotMode mode,
                            ScreenshotCallback done) override;
    void set_night_mode(TabId tab, bool on) override;
    bool set_private_proxy(std::string_view proxy_server) override;
    void open_devtools_docked(TabId tab, void* native_parent, int w, int h) override;
    void resize_devtools(TabId tab, int w, int h) override;
    void close_devtools(TabId tab) override;
    void set_devtools_closed_callback(DevToolsClosedCallback cb) override;

    void set_new_tab_callback(NewTabCallback cb) override;

    void set_downloads_dir(std::string_view dir) override;
    void set_download_callback(DownloadCallback cb) override;
    // Stop a running transfer. CEF only lets a download be cancelled through
    // the CefDownloadItemCallback handed to OnDownloadUpdated, so the client
    // keeps the most recent one per download id; an id CEF has not reported
    // an update for yet is queued and cancelled on its first update. Always
    // true for that reason: the request is either performed or enqueued.
    bool cancel_download(uint32_t download_id) override;
    bool pause_download(uint32_t download_id) override;
    bool resume_download(uint32_t download_id) override;
    bool start_download(std::string_view url) override;

    void set_adblock_rules(std::string_view rules, bool enabled) override;
    void set_adblock_exceptions(const std::vector<std::string>& sites) override;
    void set_adblock_enabled(bool enabled) override;
    void set_adblock_blocked_callback(AdblockBlockedCallback cb) override;
    void reset_adblock_count(TabId tab) override;

    void set_key_event_callback(KeyEventCallback cb) override;
    void set_extension_install_callback(ExtensionInstallCallback cb) override;
    void set_extension_popup_owner(TabId popup, TabId owner,
                                   std::string_view title,
                                   std::string_view url) override;
    void set_permission_callback(PermissionCallback cb) override;
    void set_context_action_callback(ContextActionCallback cb) override;

    void set_page_info_callback(PageInfoCallback cb) override;
    void set_tab_closed_callback(TabClosedCallback cb) override;
    void set_fullscreen_callback(FullscreenCallback cb) override;
    void exit_fullscreen(TabId tab) override;

    // Set the theme Chromium exposes to CSS `prefers-color-scheme`. The host
    // calls this before create() and whenever its semantic light/dark theme
    // changes; it overrides webview::WebViewController::set_preferred_color_scheme.
    void set_preferred_color_scheme(TabId tab, bool dark) override;

    // Tear down every browser synchronously and pump CEF until each one's
    // OnBeforeClose has fired (or a bounded timeout elapses). MUST be called on
    // the UI thread before CefShutdown: CEF requires all browsers to be closed
    // before CefShutdown, otherwise the GPU/renderer subprocesses are not
    // reaped and keep aurora_browser.exe + the CEF DLLs mapped — which is why
    // Windows refused to delete the app folder with "something is still
    // running". Idempotent.
    void shutdown() override;

private:
    PageInfoCallback callback_;
    std::mutex callback_mutex_;
    NewTabCallback new_tab_callback_;
    std::mutex new_tab_mutex_;
    TabClosedCallback tab_closed_callback_;
    std::mutex tab_closed_mutex_;
    FullscreenCallback fullscreen_callback_;
    std::mutex fullscreen_mutex_;

    // Downloads. downloads_dir_ is the OS Downloads folder; the download
    // callback reports each download's lifecycle to the host so it can record
    // it in DownloadService. downloads_mutex_ guards the callback only.
    std::string downloads_dir_;
    // Private (incognito) request context. Created lazily by
    // set_private_context(true) before the first create() call of the private
    // window. CEF holds no other strong reference to it, so it dies with the
    // adapter (i.e. with the private window) and every browser in this window
    // is discarded along with its cookies/storage. Empty cache_path = in-memory.
    CefRefPtr<CefRequestContext> private_context_;
    std::mutex private_context_mutex_;
    DownloadCallback download_callback_;
    std::mutex downloads_mutex_;

    // Chrome-accelerator forwarding: the host's synchronous "would Qt fire a
    // shortcut for this key?" decision, invoked from TabClient::OnPreKeyEvent
    // while the native child window owns the keyboard. Own mutex: CEF key
    // callbacks arrive on the UI thread, but the setter may race a pump.
    KeyEventCallback key_event_callback_;
    std::mutex key_event_mutex_;

    // Packaged-extension routing: a finished .crx download is handed to the
    // host (which runs ExtensionService::install_from_file on it) instead of
    // becoming a DownloadService row. Set once at window setup; guarded like
    // the download callback.
    ExtensionInstallCallback extension_install_callback_;
    std::mutex extension_install_mutex_;

    // Synthetic popup TabId -> invoking Aurora tab. CEF's Alloy extension API
    // (CefExtensionHandler/GetActiveBrowser) was removed in M128, while Aurora
    // must keep Alloy-style child browsers. This snapshot lets TabClient inject
    // the CEF 152 fallback bridge before a popup's own scripts execute.
    struct ExtensionPopupOwner {
        TabId tab;
        std::string title;
        std::string url;
    };
    std::map<TabId, ExtensionPopupOwner> extension_popup_owners_;
    std::mutex extension_popup_mutex_;

    // Run `source` in the main frame of `tab`, first making sure the page has
    // the receiver stub the popup bridge delivers extension messages through.
    // Used only for tabs that own a registered extension popup.
    bool run_in_owner_tab(TabId tab, const std::string& source);

    // Permission prompts (mic/camera/notification) from pages, forwarded to
    // the host which prompts or answers from a remembered decision.
    PermissionCallback permission_callback_;
    std::mutex permission_mutex_;

    // Right-click menu actions the host performs (clipboard, search, inspect).
    ContextActionCallback context_action_callback_;
    std::mutex context_action_mutex_;

    // Docked DevTools state. One DevTools browser at a time. devtools_session_
    // invalidates stale OnAfterCreated/OnBeforeClose closures from a DevTools
    // browser we replaced, so its async close cannot clobber the new one.
    // devtools_pending_ counts DevTools browsers told to close but not yet
    // OnBeforeClose'd, so shutdown() waits for the *actual* close (avoids
    // reintroducing the "folder still in use" bug).
    std::mutex devtools_mutex_;
    CefRefPtr<CefBrowser> devtools_browser_;
    TabId devtools_tab_{0};
    uint64_t devtools_session_{0};
    std::atomic<int> devtools_pending_{0};
    // Notified when the DevTools CEF browser has closed (from either side).
    DevToolsClosedCallback devtools_closed_callback_;
    std::mutex devtools_closed_mutex_;

    // TabId -> CEF browser handle. Guarded because CEF callbacks can come from
    // the IO thread even with the UI message loop.
    std::mutex browsers_mutex_;
    std::map<TabId, CefRefPtr<CefBrowser>> browsers_;
    // Tabs whose browser has been told to close but whose OnBeforeClose has
    // not fired yet. Kept alongside browsers_ so shutdown() can wait for the
    // *actual* close (not just the map emptying) — destroy() drops the map
    // entry eagerly, so browsers_.empty() alone would be a false "done".
    std::set<TabId> closing_tabs_;
    // True once shutdown() has run, so ~CefAdapter does not make CEF calls
    // after CefShutdown (which would be invalid).
    std::atomic<bool> shutdown_started_{false};
    // Desired scheme for browsers created after the host's latest update.
    // Atomic also keeps a concurrent create/update from reading stale state.
    std::atomic<bool> preferred_dark_{false};

    // Per-site zoom persistence installed by the host (UI thread only: the
    // store reads/writes Settings). zoom_mutex_ guards the copy because
    // set_zoom_store may race a pump on window setup.
    ZoomStore zoom_store_;
    std::mutex zoom_mutex_;
    // Apply the remembered (or default) zoom for the page now committed in
    // `tab`'s main frame. Called from emit() for main-frame address changes.
    void apply_site_zoom(TabId tab, const std::string& url);

    CefRefPtr<CefBrowser> browser_for(TabId tab);
    void emit(TabId tab, const webview::PageInfo& info);
    // Called from TabClient::OnPreKeyEvent on the UI thread. Returns the
    // host's consume decision for an accelerator-looking key.
    bool forward_key_event(TabId tab, const webview::EngineKeyEvent& ev);
    // Called from TabClient::OnDownloadUpdated when a rerouted .crx download
    // completes. Forwards the file to the host for installation.
    void on_extension_downloaded(TabId tab,
                                 const webview::ExtensionInstallRequest& req);
    // Return the document-start shim for a registered extension popup, or an
    // empty string for ordinary browsers.
    //
    // With a non-empty `bridge_request` this instead services one request the
    // running popup sent over the console bridge (see TabClient::kBridgeTag):
    // the requested JavaScript is executed in the OWNING tab's main frame and
    // the returned string is the reply script to run back in the popup. An
    // unknown or malformed request yields an empty string.
    std::string extension_popup_script(TabId popup,
                                       std::string_view bridge_request = {});
    // Called from TabClient's permission handlers on the UI thread.
    void on_permission_request(webview::PermissionRequest req);
    // Called from TabClient's context-menu handler on the UI thread.
    void on_context_action(TabId tab, webview::WebViewController::ContextActionRequest req);
    // Called from TabClient::OnBeforeClose on the UI thread (during a pump).
    void on_browser_closed(TabId tab);
    // Called from TabClient::OnBeforePopup on the UI thread. Returns true only
    // when the host accepted the request, so CEF is not cancelled into a no-op
    // if no host callback is installed.
    bool open_in_new_tab(std::string url);
    // Called from TabClient::OnFullscreenModeChange on the UI thread.
    void on_fullscreen_change(TabId tab, bool fullscreen);
};

} // namespace aurora::cef

// Aurora — CEF host widget.
//
// A QWidget that parents a CEF browser as a native child window. We implement
// it with a plain QWidget and hand its winId() to the CefAdapter; CEF creates
// its own child window inside this widget's native window. This keeps the Qt
// widget tree intact (so tab show/hide, layout, and focus still work) while
// letting CEF own the actual rendering surface.
//
// Creation is lazy. A browser is created only when the
// window has deliberately made this host the *current* content widget
// (MainWindow calls make_current()); a plain showEvent — which QStackedWidget
// can fire for reasons of its own, e.g. when the widget it was showing is
// removed — never creates anything. That ordering matters: creating a browser
// forces a native window handle to exist, and creating N of them inside one
// synchronous loop (session restore) is what crashed the browser on startup.
//
// When the build has CEF disabled, this widget shows a graceful placeholder
// instead of pretending a web view is present (no fake features).
#pragma once

#include <QWidget>
#include <cstdint>
#include <string>
#include <string_view>
#include "core/Types.hpp"

namespace aurora::webview { class WebViewController; }

namespace aurora::ui {

class CefHostWidget : public QWidget {
    Q_OBJECT
public:
    CefHostWidget(aurora::core::TabId tab,
                  std::string_view initial_url,
                  aurora::webview::WebViewController* controller,
                  QWidget* parent = nullptr);
    ~CefHostWidget() override;

    // This host is now the content widget the user is looking at. Creates the
    // browser at most once (deferred to showEvent if the host is not visible
    // yet). Re-entrant and repeat calls are no-ops.
    void make_current();

    // Another widget took the content area. The host will not create a browser
    // until it is made current again.
    void relinquish();
    // Tear down the CEF child while retaining this same tab host/TabId.
    // OnBeforeClose acknowledges the asynchronous close before a replacement
    // can be created in the same native parent.
    void suspend(std::string_view resume_url);
    void restore();
    void browser_closed();

    // Force-resize the CEF child window to match this widget's geometry.
    // Called by the parent when the layout changes or fullscreen is toggled.
    void sync_geometry();

    // --- Diagnostics (used by the headless smoke test and by logging) -------
    bool browser_created() const { return created_; }
    bool browser_closing() const { return closing_; }
    bool is_current() const { return current_; }
    // How many times this host has been made the current content widget.
    int activation_count() const { return activations_; }
    aurora::core::TabId tab() const { return tab_; }

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    // The single place that calls WebViewController::create(). Guarded against
    // re-entrancy and against ever running twice for the same tab.
    void create_browser();

    // Convert a logical (Qt) size to device pixels and hand it to the engine.
    void push_size(int logical_w, int logical_h);

    aurora::core::TabId tab_;
    std::string initial_url_;
    aurora::webview::WebViewController* controller_;  // may be null when CEF disabled
    bool created_ = false;
    bool creating_ = false;   // re-entrancy guard around create()
    bool closing_ = false;
    bool suspended_ = false;
    bool current_ = false;    // set by make_current()/relinquish()
    int activations_ = 0;
    std::uint64_t theme_observer_id_ = 0;
};

} // namespace aurora::ui

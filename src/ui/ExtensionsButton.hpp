// Aurora — extensions toolbar button.
//
// Chrome puts extensions behind a puzzle-piece button in the toolbar: one click
// shows the installed extensions, where choosing one opens its own options (or
// popup/details fallback), plus a link to the full management page.
//
// The button owns no extension state: it reads the registry every time the
// popup opens, while changes still go through the management page.
#pragma once

#include <QString>

#include "ui/IconButton.hpp"

class QScrollArea;
class QVBoxLayout;
class QWidget;

namespace aurora::extensions {
class ExtensionService;
}

namespace aurora::ui {

// The button paints the same vector puzzle-piece glyph every real browser
// uses — previously it was a Unicode font glyph ("\u26ED", gear-without-hub)
// that rendered as a tofu box wherever the fallback font lacked it.
class ExtensionsButton : public IconButton {
    Q_OBJECT
public:
    // `service` may be null (a window without a profile); the button then shows
    // a disabled placeholder rather than pretending it has a registry.
    explicit ExtensionsButton(extensions::ExtensionService* service,
                              QWidget* parent = nullptr);

public slots:
    // Re-read the registry and toggle the anchored extensions panel.
    void toggle_popup();
    // Hide the extensions panel if it is currently visible (no-op otherwise).
    // Used by the host so opening an extension popup dismisses the dropdown
    // instead of stacking on top of it.
    void close_popup();

signals:
    // "Manage extensions" — the host navigates to aurora://extensions.
    void manage_requested();
    // An installed extension row was chosen. The URL is its options page when
    // declared, with aurora://extensions as the details fallback. MainWindow
    // opens it in a new foreground tab.
    void open_extension_requested(const QString& url);
    // An extension with a default_popup was chosen. The host opens a small
    // popup window anchored to this button (like Chrome's extension popups)
    // instead of a full tab. `extension_id` is the chrome-extension:// ID;
    // `popup_url` is the fully-resolved chrome-extension:// URL.
    void popup_requested(const QString& extension_id,
                          const QString& popup_url);

private:
    extensions::ExtensionService* service_ = nullptr;
    QWidget* popup_ = nullptr;
    QWidget* panel_ = nullptr;
    QWidget* header_ = nullptr;
    QWidget* footer_ = nullptr;
    QScrollArea* list_scroll_ = nullptr;
    QWidget* list_host_ = nullptr;
    QVBoxLayout* list_layout_ = nullptr;

    // Rebuilt on every popup so the list reflects registry changes made
    // elsewhere (the extensions page, another window).
    void rebuild();
    void build_popup();
    // Always rebuilt from the live theme + DPR. Never cache-and-skip: a theme
    // switch otherwise leaves the panel looking like the previous theme.
    void update_popup_style();
    // Measure header + rows + footer after polish, floor at minimumSizeHint,
    // clamp to the anchor's screen, and right-align under the puzzle button.
    void fit_and_place_popup();
    // Elide each row name against the stylesheet font after ensurePolished().
    void elide_row_names();
    // Parse every installed manifest and decode every row icon once, after
    // construction, so the first popup open is as fast as the rest.
    void prewarm_caches();
};

} // namespace aurora::ui

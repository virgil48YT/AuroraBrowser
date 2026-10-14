// Aurora — extensions page (aurora://extensions).
//
// Manages the Chrome-extension registry: load an unpacked extension from a
// folder, toggle one on or off, or forget one. Because Chromium reads the load
// list while the engine starts, every change here applies on the next launch —
// the page says so instead of pretending otherwise.
//
// Each extension is a card, not a table row: its real artwork (the largest
// icon its manifest declares) next to the manifest's own name and version,
// with its enable switch and remove action on the card itself. When a manifest
// declares no icon the card falls back to a deterministic monogram tile keyed
// on the extension's folder — never a random stand-in.
//
// Sizing rule for the cards: nothing on a card may claim a width the list
// viewport does not have. The extension folder path is the only unbounded
// string here, so it lives in a label that elides against its own width
// (Qt::ElideMiddle) instead of against a hard-coded pixel budget; otherwise a
// deep path widened the card past the viewport and — horizontal scrolling is
// off — pushed the Enabled/Remove controls out of sight.
//
// The anchored popovers that extensions themselves show are NOT built here:
// the puzzle-piece dropdown lives in ui/ExtensionsButton.cpp and an
// extension's default_popup window is opened by
// MainWindow::open_extension_popup(), both positioned through
// ui/PopupPlacement.hpp. This page is the management surface only.
#pragma once

#include "ui/pages/InternalPageWidget.hpp"

class QScrollArea;
class QVBoxLayout;

namespace aurora::extensions {
class ExtensionService;
struct Extension;
}

namespace aurora::ui {

class ExtensionsPage : public InternalPageWidget {
    Q_OBJECT
public:
    explicit ExtensionsPage(extensions::ExtensionService* service,
                            QWidget* parent = nullptr);

    void refresh() override;

private:
    void load_unpacked();
    // "Install .crx…" — runs the file through
    // ExtensionService::install_from_file and reports its honest verdict.
    void install_crx();
    // "Chrome Web Store" — opens the store in an in-app tab. Never uses
    // QDesktopServices / the OS default browser.
    void open_web_store();
    // Build one card for `e` and append it to the list.
    void add_card(const extensions::Extension& e);
    // Remove every card from the list. Cards are detached and hidden
    // immediately and destroyed on the next event-loop turn: refresh() may be
    // invoked from a card's own Remove handler, so synchronous deletion is
    // unsafe, but a still-parented card would keep painting on top of the
    // rebuilt list.
    void clear_cards();

    extensions::ExtensionService* service_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* card_host_ = nullptr;
    QVBoxLayout* cards_ = nullptr;
};

} // namespace aurora::ui

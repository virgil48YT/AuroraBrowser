// Aurora — the Shields button.
//
// Brave-style shield in the toolbar: the glyph is a full shield while blocking
// is on for the site, a slashed shield while it is off, and a small numeric
// badge carries the count of requests blocked on the current page. Clicking
// opens a compact panel with the count and a per-site "Block ads on this site"
// toggle; the host owns the actual exception state and applies the change (the
// engine needs it before the next page load to be fully effective).
//
// The button owns no ad-block state of its own: the host pushes the blocked
// count, the site label and the toggle state in, and reacts to
// site_blocking_toggled() out. This keeps it usable in any window, including
// ones without a webview behind them.
#pragma once

#include <QString>

#include "ui/IconButton.hpp"

class QCheckBox;
class QLabel;
class QWidget;

namespace aurora::ui {

class ShieldButton : public IconButton {
    Q_OBJECT
public:
    explicit ShieldButton(QWidget* parent = nullptr);

    // Shields-up state for the site the active tab is showing. Drives the
    // glyph: Shield while up, ShieldOff while down.
    void set_site_blocking_enabled(bool enabled);
    bool site_blocking_enabled() const { return site_enabled_; }

    // Page-lifetime count of blocked requests (0 hides the badge). The host
    // resets it on every main-frame navigation.
    void set_blocked_count(uint64_t count);

    // Label of the site shown in the panel (host-supplied, usually the
    // registrable host of the current URL). Empty while the active tab is on
    // an internal page — the host then usually disables the button.
    void set_site(const QString& site);

    // Whether the Rust engine is actually linked in. False dims the panel and
    // turns the toggle into a no-op note, so the UI never promises a block it
    // cannot perform.
    void set_engine_available(bool available);

public slots:
    // Toggle the anchored panel (builds it on first use).
    void toggle_popup();
    // Hide the panel if it is visible (no-op otherwise).
    void close_popup();

signals:
    // The user flipped the per-site toggle. `enabled` is the NEW state: true
    // means block ads on this site, false means allow (shields down).
    void site_blocking_toggled(bool enabled);

protected:
    // IconButton paints the glyph; the badge is painted on top afterwards.
    void paintEvent(QPaintEvent* e) override;

private:
    void build_popup();
    void refresh_popup();
    void fit_and_place_popup();

    bool site_enabled_ = true;
    bool engine_available_ = true;
    uint64_t blocked_ = 0;
    QString site_;

    QWidget* popup_ = nullptr;
    QLabel* count_label_ = nullptr;
    QLabel* site_label_ = nullptr;
    QLabel* unavailable_note_ = nullptr;
    QCheckBox* toggle_ = nullptr;
};

} // namespace aurora::ui

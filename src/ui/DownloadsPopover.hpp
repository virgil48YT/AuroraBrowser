// Aurora — downloads dropdown popover.
//
// The compact "recent downloads" panel that drops from the toolbar's
// downloads button (Phase-1 brief, image-1): a header with a close
// affordance, a scrollable list of DownloadService's most recent rows
// (file glyph, name, size, relative time), and a footer that jumps to the
// full aurora://downloads page. It is a Qt::Popup, so clicking anywhere
// outside dismisses it; the full Downloads page remains the complete view.
#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>

class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace aurora::downloads {
class DownloadService;
struct DownloadItem;
}

namespace aurora::ui {

class DownloadsPopover : public QWidget {
    Q_OBJECT

public:
    // Host-side download actions, addressed by DownloadService row id. Every
    // entry is optional: an empty std::function hides the matching control,
    // so a popover with no engine behind it must not pretend to stop, pause
    // or retry anything. Each returns true when the engine accepted the
    // request (the terminal state still arrives through the download
    // callback).
    struct Actions {
        std::function<bool(int64_t)> cancel;
        std::function<bool(int64_t)> pause;
        std::function<bool(int64_t)> resume;
        std::function<bool(int64_t)> retry;
    };

    explicit DownloadsPopover(downloads::DownloadService* service,
                              QWidget* parent = nullptr,
                              Actions actions = {});

    // Rebuild the list and open the popover under `anchor` (right edges
    // aligned — the button sits at the toolbar's trailing side), clamped to
    // the screen via ui/dropdown_pos(). Calling it while visible closes the
    // popover instead (Chrome-style toggle on the tray button / Ctrl+J).
    void toggle_under(QWidget* anchor);

public slots:
    // Refresh and show without toggling closed. Reuses the most recently known
    // downloads-button anchor when available.
    void present();

signals:
    // The footer's "Full download history" action.
    void full_history_requested();

protected:
    // Soft shadow behind the rounded panel (Qt stylesheets cannot draw one).
    void paintEvent(QPaintEvent* event) override;
    // The live-progress ticker only runs while the panel is on screen: a
    // dismissed popover must cost nothing.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void rebuild();
    // Anchor recovery + placement, shared by present() and the live tick so a
    // rebuild that changes the panel's height cannot leave it misplaced.
    void reposition();
    // Cheap fingerprint of what the visible rows currently show (id, state,
    // bytes). The ticker compares it against the last rendered one and skips
    // the rebuild when nothing moved, so an idle open popover does not
    // recreate its rows four times a second.
    QString rows_signature() const;
    // Rebuild only if the fingerprint changed; called by the ticker.
    void refresh_if_changed();
    // Formatted instantaneous transfer rate for an in-progress row ("1.4 MB/s"),
    // or an empty string when there is nothing meaningful to show. Updates the
    // per-id sample below as a side effect, so it must be called exactly once
    // per row per rebuild.
    QString speed_text(const downloads::DownloadItem& d);

    downloads::DownloadService* service_ = nullptr;
    Actions actions_;
    // Last byte count seen per download row, with the moment it was read, so a
    // rebuild can derive a real instantaneous rate instead of the (much
    // flatter) average since the transfer started. Pruned in rebuild() to the
    // ids that are still in progress, so it cannot grow with history.
    struct SpeedSample {
        int64_t received = 0;
        std::chrono::steady_clock::time_point at{};
    };
    std::unordered_map<int64_t, SpeedSample> speed_samples_;
    QWidget* panel_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* list_host_ = nullptr;
    QVBoxLayout* list_layout_ = nullptr;
    QPointer<QWidget> anchor_;
    // ~250 ms while visible: fast enough that a progress bar reads as live,
    // slow enough to be invisible in a profile.
    QTimer* tick_ = nullptr;
    QString last_signature_;
};

} // namespace aurora::ui

// Aurora — downloads page (aurora://downloads).
//
// Neon layout (matching the downloads mockup), every piece backed by the
// `downloads` table through DownloadService and the window's engine actions:
//
//   * a header (download glyph + "Downloads"), then a search field shaped
//     like the omnibar, a type/state filter dropdown and "Clear all";
//   * an "Active downloads" glass card with a count badge: one row per
//     running or paused transfer with its file-type icon, received / total
//     (percent), a cyan -> purple gradient progress bar, the live speed and
//     time left, and pause / resume and cancel buttons;
//   * a "Completed" card with a count badge and its own "Clear all": size and
//     finish time per row, "Open folder" (reveal in the file manager), and a
//     remove-from-list button. Clicking a file name opens the file;
//   * a "Failed & cancelled" card (only when there are such rows) with Retry
//     and remove;
//   * a summary card on the right: total downloaded, active and completed
//     counts, the real download location (the downloads.path preference or
//     the system Downloads folder) with a button that opens it, and a
//     "Download settings" link to the Downloads section of aurora://settings.
//
// Rows are rebuilt only when the set of listed downloads (or their state)
// changes; the half-second live tick otherwise updates the existing rows in
// place, so buttons never vanish under the pointer mid-click.
#pragma once

#include "ui/pages/InternalPageWidget.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

class QComboBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace aurora::downloads {
class DownloadService;
struct DownloadItem;
}
namespace aurora::settings { class Settings; }

namespace aurora::ui {

class GlowFrame;

class DownloadsPage : public InternalPageWidget {
    Q_OBJECT
public:
    // Host-side download actions, addressed by DownloadService row id. Every
    // entry is optional: an empty std::function hides the matching control,
    // so a page built without an engine cannot promise an action it has no
    // way to perform.
    struct Actions {
        std::function<bool(int64_t)> cancel;
        std::function<bool(int64_t)> pause;
        std::function<bool(int64_t)> resume;
        std::function<bool(int64_t)> retry;
    };

    explicit DownloadsPage(downloads::DownloadService* service,
                           QWidget* parent = nullptr,
                           Actions actions = {});

    // The profile preferences the summary card reads the download location
    // from. Null (no profile) shows the system Downloads folder.
    void set_prefs(settings::Settings* prefs);

    void refresh() override;

signals:
    // "Download settings" in the summary card: the window opens the
    // Downloads section of aurora://settings.
    void download_settings_requested();

protected:
    // The live-progress ticker runs only while the page is the visible tab:
    // aurora://downloads stays alive in the content stack after the user
    // navigates away, and a background page must not poll.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    class Row;
    friend class Row;

    // The three list cards share one shape: a glass card, a header with the
    // title and a count badge, an optional header action, and a row column.
    struct Card {
        GlowFrame* frame = nullptr;
        QLabel* badge = nullptr;
        QVBoxLayout* rows = nullptr;
        QLabel* placeholder = nullptr;
    };
    Card make_card(QWidget* parent, const QString& title,
                   QPushButton* header_action);

    void build_summary(QWidget* parent, QVBoxLayout* into);
    void clear_finished();
    void clear_completed();
    void clear_failed();
    void open_download_location();
    QString download_location() const;
    // True when any listed download is still running — the only case in which
    // the page has something to animate.
    bool has_active_download() const;
    // Ticker slot: refresh only while something is actually in flight.
    void refresh_if_active();
    // The rows matching the search field and the filter dropdown.
    bool matches(const downloads::DownloadItem& d) const;
    // Instantaneous transfer rate (bytes/s) for a running row, 0 when there
    // is nothing meaningful to show. Advances the per-id sample below, so it
    // must be called exactly once per row per refresh.
    double sample_speed(const downloads::DownloadItem& d);
    // Run a host action for a row, report it and repaint on the next turn.
    void run_action(const std::function<bool(int64_t)>& action, int64_t id,
                    const QString& done_message);

    downloads::DownloadService* service_ = nullptr;
    settings::Settings* prefs_ = nullptr;
    Actions actions_;

    struct SpeedSample {
        int64_t received = 0;
        std::chrono::steady_clock::time_point at{};
    };
    std::unordered_map<int64_t, SpeedSample> speed_samples_;
    // Smoothed rate per running row, so the speed / ETA line does not jump
    // with every half-second sample.
    std::unordered_map<int64_t, double> smoothed_speed_;

    QLineEdit* search_ = nullptr;
    QComboBox* filter_ = nullptr;
    Card active_;
    Card completed_;
    Card failed_;
    QPushButton* clear_all_ = nullptr;
    QPushButton* clear_completed_ = nullptr;
    QPushButton* clear_failed_ = nullptr;

    QLabel* total_value_ = nullptr;
    QLabel* active_value_ = nullptr;
    QLabel* completed_value_ = nullptr;
    QLineEdit* location_ = nullptr;

    // Live rows by download id, and the layout signature they were built
    // for: a changed signature rebuilds, an equal one updates in place.
    std::unordered_map<int64_t, Row*> rows_;
    QString layout_key_;

    // Slower than the toolbar dropdown's: half a second still reads as live.
    QTimer* tick_ = nullptr;
    // Whether the previous tick saw a running transfer (see refresh_if_active).
    bool had_active_ = false;
};

} // namespace aurora::ui

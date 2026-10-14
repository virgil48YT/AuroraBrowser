// Aurora — downloads page (aurora://downloads).
//
// Lists rows from the `downloads` table through DownloadService: file name,
// state, progress/size and source URL, with per-row removal and a
// "clear finished" action. Clicking a completed row opens its local file.
#pragma once

#include "ui/pages/InternalPageWidget.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>

class QHideEvent;
class QLabel;
class QShowEvent;
class QTimer;
class QTreeWidget;

namespace aurora::downloads {
class DownloadService;
struct DownloadItem;
}

namespace aurora::ui {

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

    void refresh() override;

protected:
    // The live-progress ticker runs only while the page is the visible tab:
    // aurora://downloads stays alive in the content stack after the user
    // navigates away, and a background page must not poll.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void remove_selected();
    void clear_finished();
    // True when any listed download is still running — the only case in which
    // the page has something to animate.
    bool has_active_download() const;
    // Ticker slot: refresh only while something is actually in flight, so an
    // idle downloads page costs one cheap service read per interval and no
    // rebuild at all.
    void refresh_if_active();
    // Leading label of the actions row ("12 downloads") -- the page has no
    // big title any more, so the count carries the context.
    void set_count(int count);
    // Formatted transfer rate for an in-progress row ("1.4 MB/s"), empty when
    // there is nothing meaningful to show. Advances the per-id sample below,
    // so it must be called exactly once per row per refresh.
    QString speed_text(const downloads::DownloadItem& d);

    downloads::DownloadService* service_ = nullptr;
    Actions actions_;
    // Last byte count seen per download row and when it was read, so a refresh
    // can show a real instantaneous rate rather than the average since the
    // transfer started. Pruned in refresh() to the rows still in progress.
    struct SpeedSample {
        int64_t received = 0;
        std::chrono::steady_clock::time_point at{};
    };
    std::unordered_map<int64_t, SpeedSample> speed_samples_;
    QTreeWidget* list_ = nullptr;
    QLabel* count_label_ = nullptr;
    // Slower than the toolbar dropdown's: this is a full table rebuild, and
    // half a second still reads as live.
    QTimer* tick_ = nullptr;
};

} // namespace aurora::ui

// Aurora — downloads service.
//
// Reads and writes the `downloads` table created by migration v2->v3. The
// service is synchronous and stateless beyond its Database handle, mirroring
// HistoryService/BookmarkService, so the downloads page can be rendered from
// real rows and the whole thing stays unit-testable headlessly.
//
// The CEF download handler (not yet wired, see README) will call begin()/
// update()/finish(); the UI reads through recent()/remove()/clear_finished().
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "storage/Database.hpp"

namespace aurora::downloads {

enum class DownloadState {
    InProgress,
    // The engine paused the transfer (user action). NOT terminal: a paused
    // download still owns its engine-side transfer, is never cleared by
    // clear_finished(), and can be resumed or cancelled.
    Paused,
    Complete,
    Cancelled,
    Interrupted,
};

// Convert to/from the TEXT stored in the `state` column.
std::string to_string(DownloadState state);
DownloadState state_from_string(std::string_view text);

struct DownloadItem {
    int64_t id = 0;
    std::string url;
    std::string target_path;
    std::string mime;
    // -1 when the server did not advertise a length.
    int64_t total_bytes = -1;
    int64_t received = 0;
    DownloadState state = DownloadState::InProgress;
    std::chrono::system_clock::time_point started_at;
    std::optional<std::chrono::system_clock::time_point> finished_at;

    // File name component of target_path (display helper).
    std::string file_name() const;
    // 0..100, or -1 when the total size is unknown.
    int percent() const;
    // True once the transfer can no longer make progress on its own. Paused
    // is deliberately NOT finished: the engine still owns the transfer and
    // both resume and cancel remain possible.
    bool is_finished() const {
        return state == DownloadState::Complete ||
               state == DownloadState::Cancelled ||
               state == DownloadState::Interrupted;
    }
    // True while the row still belongs to a transfer the engine may own
    // (running or paused). Used to decide which rows "clear finished" keeps.
    bool is_active() const {
        return state == DownloadState::InProgress ||
               state == DownloadState::Paused;
    }
    // True when a fresh attempt can be started from the stored URL.
    bool is_retryable() const {
        return state == DownloadState::Cancelled ||
               state == DownloadState::Interrupted;
    }
};

class DownloadService {
public:
    explicit DownloadService(storage::Database& db) : db_(db) {}

    // Record the start of a download. Returns the new row id, or 0 on failure.
    int64_t begin(std::string_view url, std::string_view target_path,
                  std::string_view mime, int64_t total_bytes);

    // Progress update for an in-flight download. A new progress callback also
    // moves a recoverably interrupted row back to InProgress; completed and
    // cancelled rows are never reopened by a late callback.
    bool update_progress(int64_t id, int64_t received, int64_t total_bytes);

    // Correct the recorded destination. The engine can report a download
    // before its file exists (an empty path, or the ".crdownload" temporary),
    // so the final path may only be known later.
    bool set_target_path(int64_t id, std::string_view path);

    // Terminal transition (complete/cancelled/interrupted).
    //
    // May be called more than once for the same row: Chromium resumes a
    // recoverable interruption on its own, so a row marked Interrupted can
    // legitimately end up Complete. Complete is monotonic (a late interrupt
    // callback cannot downgrade it), and an interrupt reported after all
    // advertised bytes arrived is normalized to Complete.
    bool finish(int64_t id, DownloadState state);

    // User-initiated pause/resume. Pause only accepts a running row; resume
    // only a paused one. Neither is terminal — the engine-side transfer is
    // still alive — so finished_at is untouched and a later finish() (a
    // cancel, or the eventual complete) still lands. Returns false when the
    // row is not in the required source state (nothing is written).
    bool pause(int64_t id);
    bool resume(int64_t id);

    // Most recent downloads, newest first.
    std::vector<DownloadItem> recent(size_t limit) const;

    std::optional<DownloadItem> find(int64_t id) const;

    // Open a completed download with the platform's default application.
    bool open_file(int64_t id);

    // Show a completed download in the platform file manager.
    bool reveal_in_folder(int64_t id);

    bool remove(int64_t id);

    // Remove every row that is no longer in progress or paused. Returns rows
    // deleted. A paused transfer is still live engine-side and must survive.
    int clear_finished();

    // Remove every row. Returns rows deleted.
    int clear();

private:
    storage::Database& db_;
};

} // namespace aurora::downloads

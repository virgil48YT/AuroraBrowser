// Shared destructive-data path for the menu and clear-on-exit.
#pragma once

#include "core/IncognitoPolicy.hpp"
#include "downloads/DownloadService.hpp"
#include "history/HistoryService.hpp"

namespace aurora::privacy {

struct ClearedRows {
    int history = 0;
    int downloads = 0;
};

inline ClearedRows clear_browsing_data(
    const core::IncognitoPolicy& policy,
    history::HistoryService* history_service,
    downloads::DownloadService* download_service,
    bool clear_history, bool clear_downloads) {
    if (!policy.may_clear_profile_data()) return {};
    ClearedRows rows;
    if (clear_history && history_service) rows.history = history_service->clear();
    if (clear_downloads && download_service) rows.downloads = download_service->clear();
    return rows;
}

} // namespace aurora::privacy

// Aurora — the set of profile services the window needs.
//
// MainWindow used to own nothing but its tab model, which is why the internal
// pages could not be implemented: History/Downloads/Bookmarks/Settings all
// live in SQLite and the window had no way to reach them. Rather than making
// the window own the Database (main.cpp does, and it must outlive the window),
// startup hands it this bundle of borrowed service pointers.
//
// Every pointer is optional: a window created without services (e.g. the
// "New window" menu action before a profile is attached) still runs, and the
// pages report that no profile is attached instead of crashing.
#pragma once

#include <string>

namespace aurora {
namespace history { class HistoryService; }
namespace bookmarks { class BookmarkService; }
namespace downloads { class DownloadService; }
namespace extensions { class ExtensionService; }
namespace settings { class Settings; }
namespace session { class SessionService; }
namespace adblock { class AdblockService; }
namespace searchengines { class SearchEngineService; }
} // namespace aurora

namespace aurora::ui {

struct ServiceBundle {
    history::HistoryService* history = nullptr;
    bookmarks::BookmarkService* bookmarks = nullptr;
    downloads::DownloadService* downloads = nullptr;
    extensions::ExtensionService* extensions = nullptr;
    settings::Settings* prefs = nullptr;
    session::SessionService* session = nullptr;
    // Aurora Shields. The service owns the per-site exception list (SQLite);
    // `adblock_rules` is the raw filter-list text the engine is built from —
    // handed over as a string because the window only ever forwards it to the
    // webview controller, which rebuilds its engine from it.
    adblock::AdblockService* adblock = nullptr;
    std::string adblock_rules;
    // Custom search engines. Owns its own worker thread and database
    // connection; windows only read its in-memory snapshot to resolve the
    // default engine. Kept in incognito windows (read-only use: a private
    // window searches with the profile's default engine but cannot edit the
    // list — the settings page disables management there).
    searchengines::SearchEngineService* search_engines = nullptr;
};

} // namespace aurora::ui

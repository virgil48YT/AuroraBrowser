// Aurora — feeds the omnibox dropdown.
//
// Owns everything the omnibox itself deliberately does not: reading history,
// bookmarks and open tabs, asking the default search engine for completions,
// and merging it all through omnibox::build_suggestions(). Local rows are
// produced right after each burst of keystrokes so the list appears
// instantly; the engine's completions arrive a moment later and the list is
// updated in place. Until then, the previous completions that still match
// the typed text are kept, so the dropdown never collapses and reopens
// between keystrokes.
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "omnibox/OmniboxSuggest.hpp"
#include "searchengines/SearchEngine.hpp"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace aurora::bookmarks { class BookmarkService; }
namespace aurora::history { class HistoryService; }
namespace aurora::settings { class Settings; }
namespace aurora::core { class TabManager; }

namespace aurora::ui {

class Omnibox;

class OmniboxSuggestController : public QObject {
    Q_OBJECT

public:
    struct Sources {
        history::HistoryService* history = nullptr;      // null in private windows
        bookmarks::BookmarkService* bookmarks = nullptr; // null in private windows
        settings::Settings* prefs = nullptr;
        const core::TabManager* tabs = nullptr;
        // The user's default engine, resolved from the in-memory snapshot.
        std::function<searchengines::ResolvedEngine()> engine;
    };

    OmniboxSuggestController(Omnibox* omnibox, Sources sources, QObject* parent = nullptr);
    ~OmniboxSuggestController() override;

private:
    void on_query(const QString& text);
    void run_query();
    void rebuild(const QString& query);
    void request_remote(const QString& query);
    void on_remote_finished(QNetworkReply* reply, const QString& query,
                            const QString& cache_key);
    bool remote_enabled() const;
    omnibox::Inputs gather_local(const QString& query) const;

    QPointer<Omnibox> omnibox_;
    Sources src_;
    QNetworkAccessManager* net_ = nullptr;
    QTimer* debounce_ = nullptr;
    // Coalesces a burst of keystrokes (key repeat, fast typing) into one
    // rebuild of the list: zero-interval, so it runs as soon as the queued
    // input has been processed.
    QTimer* coalesce_ = nullptr;
    // True while an engine request for current_query_ is debounced or in
    // flight: the dropdown is kept as-is instead of collapsing meanwhile.
    bool remote_pending_ = false;
    QPointer<QNetworkReply> inflight_;
    QString pending_query_;
    QString current_query_;
    QHash<QString, QStringList> remote_cache_;  // "<engine>\n<query>" -> completions
    QStringList remote_current_;                // completions for current_query_
};

} // namespace aurora::ui

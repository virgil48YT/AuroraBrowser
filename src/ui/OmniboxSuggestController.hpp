// Aurora — feeds the omnibox dropdown.
//
// Owns everything the omnibox itself deliberately does not: reading history,
// bookmarks and open tabs, asking the default search engine for completions,
// and merging it all through omnibox::build_suggestions(). Local rows are
// produced synchronously on every keystroke so the list appears instantly;
// the engine's completions arrive a moment later and the list is rebuilt.
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
    QPointer<QNetworkReply> inflight_;
    QString pending_query_;
    QString current_query_;
    QHash<QString, QStringList> remote_cache_;  // "<engine>\n<query>" -> completions
    QStringList remote_current_;                // completions for current_query_
};

} // namespace aurora::ui

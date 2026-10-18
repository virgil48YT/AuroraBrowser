#include "ui/OmniboxSuggestController.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_set>

#include "bookmarks/BookmarkService.hpp"
#include "core/InternalUrl.hpp"
#include "history/HistoryService.hpp"
#include "settings/Settings.hpp"
#include "tabs/TabManager.hpp"
#include "ui/Omnibox.hpp"

namespace aurora::ui {

namespace {

constexpr int kDebounceMs = 80;     // wait for a pause in typing before asking the engine
constexpr int kTimeoutMs = 2500;    // the dropdown never waits longer than this
constexpr int kMaxCache = 64;
constexpr size_t kHistoryFtsRows = 25;
constexpr size_t kHistoryRecentRows = 400;

int64_t to_epoch(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

} // namespace

OmniboxSuggestController::OmniboxSuggestController(Omnibox* omnibox, Sources sources,
                                                   QObject* parent)
    : QObject(parent), omnibox_(omnibox), src_(std::move(sources)) {
    net_ = new QNetworkAccessManager(this);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kDebounceMs);
    connect(debounce_, &QTimer::timeout, this, [this] { request_remote(pending_query_); });

    coalesce_ = new QTimer(this);
    coalesce_->setSingleShot(true);
    coalesce_->setInterval(0);
    connect(coalesce_, &QTimer::timeout, this, &OmniboxSuggestController::run_query);

    if (omnibox_) {
        connect(omnibox_, &Omnibox::query_changed, this, &OmniboxSuggestController::on_query);
    }
}

OmniboxSuggestController::~OmniboxSuggestController() {
    if (inflight_) {
        inflight_->disconnect(this);
        inflight_->abort();
    }
}

bool OmniboxSuggestController::remote_enabled() const {
    if (!src_.prefs) return settings::PrefDefaults::kSearchSuggestions;
    return src_.prefs->get_bool(settings::PrefKeys::kSearchSuggestions,
                                settings::PrefDefaults::kSearchSuggestions);
}

omnibox::Inputs OmniboxSuggestController::gather_local(const QString& query) const {
    omnibox::Inputs in;
    in.query = query.toStdString();
    in.now_epoch = to_epoch(std::chrono::system_clock::now());
    if (src_.engine) in.engine_name = src_.engine().name;

    if (src_.history) {
        // The full-text index finds token-prefix matches ("git" -> "github");
        // the recent window adds substring matches the index cannot see
        // ("hub" -> "github"). Both feed the same ranker, so merge by URL.
        std::unordered_set<std::string> seen;
        auto add = [&](const history::Visit& v) {
            if (!seen.insert(v.url).second) return;
            omnibox::HistoryCandidate h;
            h.url = v.url;
            h.title = v.title;
            h.visit_count = v.visit_count;
            h.typed_count = v.typed_count;
            h.last_visit_epoch = to_epoch(v.last_visit);
            in.history.push_back(std::move(h));
        };
        for (const auto& v : src_.history->search(in.query, kHistoryFtsRows)) add(v);
        for (const auto& v : src_.history->recent(kHistoryRecentRows)) add(v);
    }

    if (src_.bookmarks) {
        for (const auto& b : src_.bookmarks->all_bookmarks()) {
            if (b.is_folder || b.url.empty()) continue;
            in.bookmarks.push_back({b.url, b.title});
        }
    }

    if (src_.tabs) {
        const auto active = src_.tabs->active();
        for (const auto& t : src_.tabs->tabs()) {
            const std::string& url = t.url.raw();
            // Internal pages (aurora://...) have their own entry points and
            // would only clutter the list.
            if (url.empty() || !t.url.is_valid() || core::is_internal_url(url)) continue;
            in.tabs.push_back({url, t.title, t.id.value(), t.id == active});
        }
    }
    return in;
}

void OmniboxSuggestController::on_query(const QString& text) {
    current_query_ = text.trimmed();

    if (inflight_) {
        inflight_->disconnect(this);
        inflight_->abort();
        inflight_.clear();
    }
    debounce_->stop();

    if (current_query_.isEmpty()) {
        coalesce_->stop();
        remote_current_.clear();
        remote_pending_ = false;
        return;
    }
    // One rebuild per burst of keystrokes, not one per key.
    coalesce_->start();
}

void OmniboxSuggestController::run_query() {
    const QString query = current_query_;
    if (query.isEmpty()) return;

    // Anything already fetched for this exact text is reused immediately, so
    // backspacing over a word does not flicker the list or re-ask the engine.
    const auto engine = src_.engine ? src_.engine() : searchengines::ResolvedEngine{};
    const QString key = QString::fromStdString(engine.key) + QLatin1Char('\n') + query;
    const auto cached = remote_cache_.constFind(key);
    if (cached != remote_cache_.constEnd()) {
        remote_current_ = cached.value();
    } else {
        // No answer for this exact text yet: keep the previous completions
        // that still extend what is typed (Chrome does the same). Dropping
        // them all made the list shrink to the lone default row, which hid
        // the dropdown on every keystroke and showed it again when the
        // engine replied — the disappear/reappear flicker.
        QStringList still_matching;
        for (const QString& r : remote_current_) {
            if (r.startsWith(query, Qt::CaseInsensitive) &&
                r.compare(query, Qt::CaseInsensitive) != 0) {
                still_matching << r;
            }
        }
        remote_current_ = still_matching;
    }

    remote_pending_ = cached == remote_cache_.constEnd() && remote_enabled() &&
                      omnibox::should_request_remote(query.toStdString()) &&
                      !omnibox::suggest_url_template(engine.key).empty();

    rebuild(query);

    if (remote_pending_) {
        pending_query_ = query;
        debounce_->start();
    }
}

void OmniboxSuggestController::rebuild(const QString& query) {
    if (!omnibox_) return;
    omnibox::Inputs in = gather_local(query);
    for (const auto& r : remote_current_) in.remote.push_back(r.toStdString());
    auto rows = omnibox::build_suggestions(in);
    omnibox_->set_suggestions(query, std::move(rows), QString::fromStdString(in.engine_name),
                              remote_pending_);
}

void OmniboxSuggestController::request_remote(const QString& query) {
    if (query != current_query_ || query.isEmpty()) return;
    // Whatever happens below, this query's list must not stay "pending"
    // forever: if no request goes out, settle the list with what we have.
    const auto settle = [this, &query] {
        remote_pending_ = false;
        rebuild(query);
    };
    if (!remote_enabled() || !src_.engine) return settle();
    const auto engine = src_.engine();
    const std::string tmpl = omnibox::suggest_url_template(engine.key);
    if (tmpl.empty()) return settle();

    // The query is the only thing sent. Percent-encode it fully so nothing in
    // the typed text can alter the request.
    const QByteArray encoded = QUrl::toPercentEncoding(query);
    QString url_text = QString::fromStdString(tmpl);
    url_text.replace(QStringLiteral("%s"), QString::fromLatin1(encoded));
    const QUrl url(url_text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) return settle();

    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "AuroraBrowser");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // No cookies out, none stored: completions must not link keystrokes to a
    // logged-in identity.
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::PreferCache);
    req.setTransferTimeout(kTimeoutMs);

    const QString cache_key = QString::fromStdString(engine.key) + QLatin1Char('\n') + query;
    QNetworkReply* reply = net_->get(req);
    inflight_ = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, query, cache_key] { on_remote_finished(reply, query, cache_key); });
}

void OmniboxSuggestController::on_remote_finished(QNetworkReply* reply, const QString& query,
                                                  const QString& cache_key) {
    reply->deleteLater();
    if (inflight_ == reply) inflight_.clear();
    // The user has typed on; this answer is for text that is gone.
    if (query != current_query_) return;
    if (reply->error() != QNetworkReply::NoError) {
        // No completions after all: settle the list with the local rows (and
        // any carried-over completions) instead of leaving it stale.
        remote_pending_ = false;
        rebuild(query);
        return;
    }

    // Bounded read: a misbehaving endpoint cannot make us buffer a huge body.
    const QByteArray body = reply->read(64 * 1024 + 1);
    const auto parsed = omnibox::parse_remote_suggestions(
        std::string_view(body.constData(), static_cast<size_t>(body.size())));

    QStringList list;
    for (const auto& s : parsed) list << QString::fromStdString(s);

    if (remote_cache_.size() >= kMaxCache) remote_cache_.clear();
    remote_cache_.insert(cache_key, list);

    remote_current_ = list;
    remote_pending_ = false;
    rebuild(query);
}

} // namespace aurora::ui

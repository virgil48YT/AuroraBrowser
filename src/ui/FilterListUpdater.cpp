#include "ui/FilterListUpdater.hpp"

#include <QDateTime>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadPool>
#include <QUrl>

#include <utility>

#include "adblock/AdblockEngine.hpp"
#include "settings/Settings.hpp"

namespace aurora::ui {

namespace {
// EasyList is a few MB; on a slow link that is still well under a minute.
// The timeout is per stalled transfer, not total, so a steady download of a
// large list is never cut off.
constexpr int kTimeoutMs = 60000;
} // namespace

FilterListUpdater::FilterListUpdater(const QString& profile_dir,
                                     settings::Settings* prefs,
                                     std::vector<std::string> rule_paths,
                                     std::string initial_rules,
                                     QObject* parent)
    : QObject(parent),
      updater_(profile_dir.toStdString()),
      prefs_(prefs),
      rule_paths_(std::move(rule_paths)),
      rules_(std::move(initial_rules)),
      net_(new QNetworkAccessManager(this)),
      pool_(std::make_unique<QThreadPool>()) {
    // One install at a time is all ListUpdater allows anyway.
    pool_->setMaxThreadCount(1);
}

FilterListUpdater::~FilterListUpdater() = default;

QString FilterListUpdater::source_url() const {
    return QString::fromStdString(updater_.source_url());
}

adblock::UpdateRecord FilterListUpdater::last_update() const {
    if (!prefs_) return {};
    return adblock::ListUpdater::last_update(*prefs_);
}

void FilterListUpdater::start() {
    if (busy_) {
        emit finished(false,
                      QStringLiteral("A filter list update is already running"));
        return;
    }
    busy_ = true;
    emit busy_changed(true);
    updater_.update(
        [this](const std::string& url, adblock::ListUpdater::FetchDone done) {
            fetch(url, std::move(done));
        },
        [this](const adblock::ListUpdater::Result& result) {
            // Runs on the worker (or inline on the UI thread for a failed
            // fetch). Re-read the combined rules here too: it is file I/O and
            // must reflect exactly what load_rules() will see on next launch.
            std::string new_rules;
            if (result.ok) new_rules = adblock::load_rules(rule_paths_);
            QMetaObject::invokeMethod(
                this,
                [this, result, new_rules = std::move(new_rules)]() mutable {
                    complete(result, std::move(new_rules));
                },
                Qt::QueuedConnection);
        });
}

void FilterListUpdater::fetch(const std::string& url,
                              adblock::ListUpdater::FetchDone done) {
    QNetworkRequest req{QUrl(QString::fromStdString(url))};
    req.setTransferTimeout(kTimeoutMs);
    // Same policy as WeatherService: follow redirects, but never from
    // https:// down to http:// — a list that decides what the engine blocks
    // must not be swappable by anyone on the path.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "AuroraBrowser");
    req.setRawHeader("Accept", "text/plain");
    QNetworkReply* reply = net_->get(req);

    // Abort a response that grows past any plausible filter list instead of
    // buffering it all in memory first.
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64 /*total*/) {
                if (received > static_cast<qint64>(adblock::kMaxFilterListBytes))
                    reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, done = std::move(done)]() mutable {
                reply->deleteLater();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                if (reply->error() != QNetworkReply::NoError ||
                    (status != 0 && status != 200)) {
                    const std::string error =
                        status != 0 && status != 200
                            ? "HTTP " + std::to_string(status)
                            : reply->errorString().toStdString();
                    done(false, std::string(), error);
                    return;
                }
                const QByteArray body = reply->readAll();
                // Validation and the file swap leave the UI thread: the
                // engine parse of a full list is too slow to run inline.
                pool_->start([done = std::move(done),
                              bytes = body.toStdString()]() mutable {
                    done(true, std::move(bytes), std::string());
                });
            });
}

void FilterListUpdater::complete(const adblock::ListUpdater::Result& result,
                                 std::string new_rules) {
    if (result.ok) {
        rules_ = std::move(new_rules);
        ++generation_;
        if (prefs_) {
            adblock::ListUpdater::record_success(
                *prefs_, result, QDateTime::currentSecsSinceEpoch());
        }
    }
    busy_ = false;
    emit busy_changed(false);
    emit finished(result.ok, QString::fromStdString(result.message));
}

} // namespace aurora::ui

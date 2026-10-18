#include "ui/FaviconCache.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QUrl>

namespace aurora::ui {

namespace {

// Icons are small; anything larger is not a favicon and is refused so a
// hostile site cannot make us hold a bitmap in memory per host.
constexpr qint64 kMaxIconBytes = 512 * 1024;

// The tab strip, dock and lists draw favicons at 16px; that is the size of
// the QIcon handed out by icon_for_url().
constexpr int kIconSize = 16;

// Larger surfaces (homepage tiles and cards) ask for up to this size through
// pixmap_for_url(); sources are kept (and persisted) up to this resolution.
constexpr int kLargeIconSize = 64;

QPixmap cap_size(const QPixmap& pm, int px) {
    if (pm.width() <= px && pm.height() <= px) return pm;
    return pm.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// A host whose icon could not be fetched is remembered as "no icon" so the
// request is not repeated on every repaint — but only for this long. Sites go
// offline, redirects settle, and a browser that gives up permanently after one
// bad moment shows a blank tab for the rest of the session.
constexpr qint64 kNegativeRetryMs = 60 * 1000;

// A persisted icon is served immediately, but refreshed in the background once
// it is older than this: sites do change their favicon, and the previous
// behaviour (serve the file forever) is exactly why deleting the favicons
// directory by hand "fixed" a wrong icon.
constexpr qint64 kDiskRefreshMs = 7LL * 24 * 60 * 60 * 1000;

} // namespace

FaviconCache& FaviconCache::instance() {
    static FaviconCache cache;
    return cache;
}

FaviconCache::FaviconCache(QObject* parent) : QObject(parent) {
    net_ = new QNetworkAccessManager(this);
}

void FaviconCache::set_cache_dir(const QString& dir) {
    if (dir.isEmpty()) {
        cache_dir_.clear();
        return;
    }
    QDir base(dir);
    if (!base.exists("favicons") && !base.mkpath("favicons")) {
        cache_dir_.clear();  // unwritable profile: memory-only cache
        return;
    }
    cache_dir_ = base.filePath("favicons");
}

QString FaviconCache::host_of(const QString& url) {
    const QUrl parsed(url);
    if (!parsed.isValid()) return QString();
    const auto scheme = parsed.scheme();
    // Only network schemes have a fetchable icon; aurora://, file:// and
    // about: pages use the built-in page icons instead.
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return QString();
    }
    // The port is part of the key: two servers on the same host but different
    // ports (localhost development, staging on :8443) are different sites and
    // must not share an icon.
    QString key = parsed.host();
    if (parsed.port() != -1) {
        key += QStringLiteral(":") + QString::number(parsed.port());
    }
    return key;
}

void FaviconCache::register_favicon(const QString& page_url,
                                    const QString& favicon_url) {
    const auto host = host_of(page_url);
    if (host.isEmpty() || favicon_url.isEmpty()) return;
    const auto known = icon_urls_.constFind(host);
    if (known != icon_urls_.constEnd() && known.value() == favicon_url) return;
    icon_urls_[host] = favicon_url;
    // The engine has told us the page's REAL icon URL, and it differs from
    // whatever we last used for this host (a /favicon.ico guess, or the icon of
    // a page that was mistakenly attributed to this host). Fetch it and let it
    // replace what we have — the previous "keep whatever is cached" shortcut is
    // why a wrong icon survived until the user deleted the cache directory.
    failed_at_ms_.remove(host);
    fetch(host, favicon_url);
}

QString FaviconCache::disk_path(const QString& host) const {
    if (cache_dir_.isEmpty()) return QString();
    // Hash the host so the file name is always a safe, fixed-length token.
    const auto digest = QCryptographicHash::hash(host.toUtf8(),
                                                 QCryptographicHash::Sha1);
    return QDir(cache_dir_).filePath(QString::fromLatin1(digest.toHex()) +
                                     QStringLiteral(".png"));
}

bool FaviconCache::load_from_disk(const QString& host, bool* stale) {
    if (stale) *stale = false;
    const auto path = disk_path(host);
    if (path.isEmpty()) return false;
    QPixmap pm;
    if (!pm.load(path)) return false;
    large_[host] = cap_size(pm, kLargeIconSize);
    icons_[host] = QIcon(cap_size(pm, kIconSize));
    const QFileInfo info(path);
    const auto age = info.lastModified().msecsTo(QDateTime::currentDateTime());
    if (stale && age > kDiskRefreshMs) *stale = true;
    return true;
}

QString FaviconCache::icon_url_for(const QString& host,
                                   const QString& page_url) const {
    const auto known = icon_urls_.constFind(host);
    if (known != icon_urls_.constEnd()) return known.value();
    // Guess /favicon.ico on the page's OWN origin. Hardcoding https and
    // dropping the port (as this once did) asked the wrong server for the
    // icon: an http://host:8099 page turned into https://host/, which either
    // fails or — worse, behind a proxy — answers with a completely unrelated
    // site's icon.
    const QUrl page(page_url);
    QUrl guess;
    guess.setScheme(page.scheme());
    guess.setHost(page.host());
    if (page.port() != -1) guess.setPort(page.port());
    guess.setPath(QStringLiteral("/favicon.ico"));
    return guess.toString();
}

QIcon FaviconCache::icon_for_url(const QString& page_url) {
    const auto host = host_of(page_url);
    if (host.isEmpty()) return QIcon();

    const auto cached = icons_.constFind(host);
    if (cached != icons_.constEnd() && !cached.value().isNull()) {
        return cached.value();
    }
    // A null cached entry means "we looked and found nothing". Honour it for
    // kNegativeRetryMs, then try again rather than blanking this site forever.
    if (cached != icons_.constEnd()) {
        const auto failed = failed_at_ms_.constFind(host);
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (failed != failed_at_ms_.constEnd() &&
            now - failed.value() < kNegativeRetryMs) {
            return QIcon();
        }
    }

    // Not in memory: try the profile's icon directory once.
    if (!disk_checked_.contains(host)) {
        disk_checked_.insert(host);
        bool stale = false;
        if (load_from_disk(host, &stale)) {
            // A stale file is still shown at once — but refreshed behind it, so
            // a site that changed its icon updates on its own.
            if (stale) fetch(host, icon_url_for(host, page_url));
            return icons_.value(host);
        }
    }

    if (!in_flight_.contains(host)) fetch(host, icon_url_for(host, page_url));
    return QIcon();
}

QPixmap FaviconCache::pixmap_for_url(const QString& page_url, int px,
                                     qreal dpr) {
    // Goes through icon_for_url() so the disk lookup / fetch / negative-cache
    // rules are exactly the same as for the strip icon.
    const QIcon icon = icon_for_url(page_url);
    if (icon.isNull() || px <= 0) return QPixmap();
    if (dpr <= 0.0) dpr = 1.0;
    QPixmap source = large_.value(host_of(page_url));
    if (source.isNull()) source = icon.pixmap(QSize(kIconSize, kIconSize));
    if (source.isNull()) return QPixmap();
    const int device_px = qMax(1, qRound(px * dpr));
    // Always scale to the requested size: QIcon::pixmap() never upscales,
    // which is why a 16px favicon used to sit tiny inside a 40px slot.
    QPixmap out = source.scaled(device_px, device_px, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    out.setDevicePixelRatio(dpr);
    return out;
}

void FaviconCache::fetch(const QString& host, const QString& icon_url) {
    const QUrl url(icon_url);
    if (!url.isValid() || url.host().isEmpty()) return;
    // A request for this host is already pending. Only skip when it is for the
    // SAME icon URL: if the engine has just reported a different (real) icon,
    // that one must be issued now, and the older reply discards itself when it
    // sees it has been superseded.
    if (in_flight_.contains(host) &&
        in_flight_urls_.value(host) == icon_url) {
        return;
    }
    in_flight_.insert(host);
    in_flight_urls_[host] = icon_url;

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(3);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this,
            [this, host, icon_url, reply] {
        reply->deleteLater();
        in_flight_.remove(host);
        // Discard a reply for an icon URL that is no longer the host's: the
        // engine reported a better one while this request was in flight, and
        // letting the older reply land would both show and PERSIST the wrong
        // icon under this host's key.
        const auto issued = in_flight_urls_.constFind(host);
        const bool superseded = issued != in_flight_urls_.constEnd() &&
                                issued.value() != icon_url;
        in_flight_urls_.remove(host);
        if (superseded) return;
        QPixmap pm;
        if (reply->error() == QNetworkReply::NoError &&
            reply->bytesAvailable() <= kMaxIconBytes) {
            pm.loadFromData(reply->readAll());
        }
        if (!pm.isNull()) {
            const QPixmap large = cap_size(pm, kLargeIconSize);
            large_[host] = large;
            icons_[host] = QIcon(cap_size(large, kIconSize));
            const auto path = disk_path(host);
            if (!path.isEmpty()) {
                // Best effort: a failed write only costs a refetch next run.
                // The large copy is persisted so tiles stay crisp next run.
                (void)large.save(path, "PNG");
            }
            failed_at_ms_.remove(host);
        } else {
            // Remember the failure as "no icon" so the fetch is not repeated on
            // every repaint — with a timestamp, so it IS retried later.
            icons_[host] = QIcon();
            large_.remove(host);
            failed_at_ms_[host] = QDateTime::currentMSecsSinceEpoch();
        }
        emit icon_ready(host);
    });
}

} // namespace aurora::ui

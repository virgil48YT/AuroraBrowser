// Aurora — shared favicon cache.
//
// One process-wide cache so every surface that shows a site — the tab strip,
// aurora://history and aurora://bookmarks — draws the same icon and each icon
// is downloaded at most once per run.
//
// Keying is by host, not by page URL: history and bookmarks rows only know a
// page URL, and every page on a host shares its icon in practice. When the
// engine reports a real <link rel="icon"> URL for a page we use it; otherwise
// we fall back to the well-known /favicon.ico for the host.
//
// Icons are also written to a per-profile directory, so history and bookmarks
// show icons for sites that were visited in an earlier run without refetching
// them.
#pragma once

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSet>
#include <QString>

class QNetworkAccessManager;

namespace aurora::ui {

class FaviconCache : public QObject {
    Q_OBJECT
public:
    // The shared instance. Created on first use, parented to nothing (it lives
    // for the process); safe to call before or after set_cache_dir().
    static FaviconCache& instance();

    // Where icons are persisted (a subdirectory is created inside it). Called
    // once at startup with the profile directory. Empty disables disk caching.
    void set_cache_dir(const QString& dir);

    // Record the icon URL the engine discovered for a page, so later lookups
    // for that host fetch the real icon instead of guessing /favicon.ico.
    void register_favicon(const QString& page_url, const QString& favicon_url);

    // The icon for a page URL's host. Returns a null QIcon when the icon is
    // not known yet and starts a fetch; icon_ready() fires when it arrives.
    QIcon icon_for_url(const QString& page_url);

    // Host component of a URL ("" when it has none, e.g. aurora:// pages).
    static QString host_of(const QString& url);

signals:
    // A host's icon became available (or was resolved to "no icon").
    void icon_ready(const QString& host);

private:
    explicit FaviconCache(QObject* parent = nullptr);

    void fetch(const QString& host, const QString& icon_url);
    QString disk_path(const QString& host) const;
    // Loads the persisted icon for `host`. Sets *stale when the file on disk is
    // older than the refresh window, so the caller can show it immediately and
    // refetch in the background (a site that changed its icon used to keep the
    // old one until the user deleted the cache directory by hand).
    bool load_from_disk(const QString& host, bool* stale);
    // The icon URL to request for `host`: the one the engine reported, else
    // /favicon.ico on the page's own origin.
    QString icon_url_for(const QString& host, const QString& page_url) const;

    QNetworkAccessManager* net_ = nullptr;
    QString cache_dir_;
    // Host -> icon. A null QIcon value means "looked up, none available", so a
    // missing icon is never fetched twice.
    QHash<QString, QIcon> icons_;
    // Host -> icon URL reported by the engine.
    QHash<QString, QString> icon_urls_;
    // Icon URL each in-flight request was issued for, so a reply that lands
    // after a newer icon URL was registered for the same host is discarded
    // instead of overwriting the newer icon (and persisting the older one).
    QHash<QString, QString> in_flight_urls_;
    QSet<QString> in_flight_;
    // Hosts already looked for on disk, so a miss doesn't stat on every paint.
    QSet<QString> disk_checked_;
    // When a host's fetch last failed. A failure is remembered as "no icon" so
    // the fetch is not retried on every paint, but NOT forever: a transient
    // failure (offline, slow server, a redirect that had not settled) used to
    // mean that site never showed an icon again for the life of the process.
    QHash<QString, qint64> failed_at_ms_;
};

} // namespace aurora::ui

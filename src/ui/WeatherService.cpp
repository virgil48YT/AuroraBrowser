#include "ui/WeatherService.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

namespace aurora::ui {

namespace {

constexpr int kRefreshMs = 20 * 60 * 1000;
constexpr int kRetryMs = 60 * 1000;
constexpr int kTimeoutMs = 10 * 1000;

bool uses_fahrenheit(const QString& country_code) {
    static const QStringList kCountries = {
        QStringLiteral("US"), QStringLiteral("LR"), QStringLiteral("MM"),
        QStringLiteral("BS"), QStringLiteral("BZ"), QStringLiteral("KY"),
        QStringLiteral("PW")};
    return kCountries.contains(country_code.toUpper());
}

} // namespace

WeatherService& WeatherService::instance() {
    static WeatherService service;
    return service;
}

WeatherService::WeatherService(QObject* parent) : QObject(parent) {
    net_ = new QNetworkAccessManager(this);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
}

QString WeatherService::describe(int code, bool is_day) {
    switch (code) {
        case 0: return is_day ? QStringLiteral("Sunny") : QStringLiteral("Clear");
        case 1: return is_day ? QStringLiteral("Mostly sunny")
                              : QStringLiteral("Mostly clear");
        case 2: return QStringLiteral("Partly cloudy");
        case 3: return QStringLiteral("Overcast");
        case 45: case 48: return QStringLiteral("Fog");
        case 51: case 53: case 55: return QStringLiteral("Drizzle");
        case 56: case 57: return QStringLiteral("Freezing drizzle");
        case 61: return QStringLiteral("Light rain");
        case 63: return QStringLiteral("Rain");
        case 65: return QStringLiteral("Heavy rain");
        case 66: case 67: return QStringLiteral("Freezing rain");
        case 71: return QStringLiteral("Light snow");
        case 73: return QStringLiteral("Snow");
        case 75: return QStringLiteral("Heavy snow");
        case 77: return QStringLiteral("Snow grains");
        case 80: case 81: case 82: return QStringLiteral("Rain showers");
        case 85: case 86: return QStringLiteral("Snow showers");
        case 95: return QStringLiteral("Thunderstorm");
        case 96: case 99: return QStringLiteral("Thunderstorm with hail");
        default: return QStringLiteral("Unknown");
    }
}

icons::Name WeatherService::icon_for(int code, bool is_day) {
    switch (code) {
        case 0: case 1:
            return is_day ? icons::Name::Sun : icons::Name::Moon;
        case 2: return icons::Name::CloudSun;
        case 3: return icons::Name::WeatherCloud;
        case 45: case 48: return icons::Name::WeatherFog;
        case 71: case 73: case 75: case 77: case 85: case 86:
            return icons::Name::WeatherSnow;
        case 95: case 96: case 99: return icons::Name::WeatherStorm;
        default:
            if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
                return icons::Name::WeatherRain;
            }
            return icons::Name::CloudSun;
    }
}

void WeatherService::use_location(const QString& name, double lat, double lon,
                                  bool fahrenheit) {
    if (manual_ && have_location_ && info_.valid &&
        qFuzzyCompare(lat_ + 1000.0, lat + 1000.0) &&
        qFuzzyCompare(lon_ + 1000.0, lon + 1000.0)) {
        refresh_if_stale();
        return;
    }
    manual_ = true;
    have_location_ = true;
    lat_ = lat;
    lon_ = lon;
    fahrenheit_ = fahrenheit;
    place_ = name;
    ++generation_;
    in_flight_ = false;
    refresh();
}

void WeatherService::use_automatic_location() {
    if (!manual_ && have_location_) return;
    manual_ = false;
    have_location_ = false;
    ++generation_;
    in_flight_ = false;
    refresh();
}

void WeatherService::refresh_if_stale() {
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - last_ok_ms_;
    if (!info_.valid || age > kRefreshMs / 2) refresh();
}

void WeatherService::refresh() {
    if (in_flight_) return;
    timer_->stop();
    in_flight_ = true;
    if (have_location_) {
        fetch_conditions();
    } else {
        locate_by_ip();
    }
}

void WeatherService::schedule_retry() {
    in_flight_ = false;
    timer_->start(kRetryMs);
}

void WeatherService::finish_get(
    QNetworkReply* reply, const std::function<void(const QByteArray&)>& on_ok,
    const std::function<void()>& on_fail) {
    const quint64 gen = generation_;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, gen, on_ok, on_fail] {
                reply->deleteLater();
                if (gen != generation_) return;   // location changed meanwhile
                if (reply->error() != QNetworkReply::NoError) {
                    on_fail();
                    return;
                }
                on_ok(reply->readAll());
            });
}

namespace {

QNetworkRequest make_request(const QUrl& url) {
    QNetworkRequest req(url);
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "AuroraBrowser");
    req.setRawHeader("Accept", "application/json");
    return req;
}

} // namespace

void WeatherService::locate_by_ip() {
    QNetworkReply* reply = net_->get(make_request(
        QUrl(QStringLiteral("https://ipwho.is/"
                            "?fields=success,city,latitude,longitude,"
                            "country_code"))));
    finish_get(
        reply,
        [this](const QByteArray& body) {
            const QJsonObject o = QJsonDocument::fromJson(body).object();
            if (!o.value(QStringLiteral("success")).toBool(false) ||
                !o.contains(QStringLiteral("latitude"))) {
                locate_by_ip_fallback();
                return;
            }
            lat_ = o.value(QStringLiteral("latitude")).toDouble();
            lon_ = o.value(QStringLiteral("longitude")).toDouble();
            place_ = o.value(QStringLiteral("city")).toString();
            fahrenheit_ = uses_fahrenheit(
                o.value(QStringLiteral("country_code")).toString());
            have_location_ = true;
            fetch_conditions();
        },
        [this] { locate_by_ip_fallback(); });
}

void WeatherService::locate_by_ip_fallback() {
    QNetworkReply* reply = net_->get(make_request(
        QUrl(QStringLiteral("https://get.geojs.io/v1/ip/geo.json"))));
    finish_get(
        reply,
        [this](const QByteArray& body) {
            const QJsonObject o = QJsonDocument::fromJson(body).object();
            bool ok_lat = false, ok_lon = false;
            const double lat =
                o.value(QStringLiteral("latitude")).toString().toDouble(&ok_lat);
            const double lon =
                o.value(QStringLiteral("longitude")).toString().toDouble(&ok_lon);
            if (!ok_lat || !ok_lon) {
                schedule_retry();
                return;
            }
            lat_ = lat;
            lon_ = lon;
            place_ = o.value(QStringLiteral("city")).toString();
            fahrenheit_ = uses_fahrenheit(
                o.value(QStringLiteral("country_code")).toString());
            have_location_ = true;
            fetch_conditions();
        },
        [this] { schedule_retry(); });
}

void WeatherService::fetch_conditions() {
    QUrl url(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("latitude"), QString::number(lat_, 'f', 4));
    q.addQueryItem(QStringLiteral("longitude"), QString::number(lon_, 'f', 4));
    q.addQueryItem(QStringLiteral("current"),
                   QStringLiteral("temperature_2m,weather_code,is_day"));
    q.addQueryItem(QStringLiteral("timezone"), QStringLiteral("auto"));
    if (fahrenheit_) {
        q.addQueryItem(QStringLiteral("temperature_unit"),
                       QStringLiteral("fahrenheit"));
    }
    url.setQuery(q);
    finish_get(
        net_->get(make_request(url)),
        [this](const QByteArray& body) {
            const QJsonObject cur = QJsonDocument::fromJson(body).object()
                .value(QStringLiteral("current")).toObject();
            if (!cur.contains(QStringLiteral("temperature_2m"))) {
                schedule_retry();
                return;
            }
            info_.valid = true;
            info_.temperature =
                cur.value(QStringLiteral("temperature_2m")).toDouble();
            info_.code = cur.value(QStringLiteral("weather_code")).toInt();
            info_.is_day = cur.value(QStringLiteral("is_day")).toInt(1) != 0;
            info_.fahrenheit = fahrenheit_;
            info_.place = place_;
            in_flight_ = false;
            last_ok_ms_ = QDateTime::currentMSecsSinceEpoch();
            timer_->start(kRefreshMs);
            emit updated();
        },
        [this] { schedule_retry(); });
}

void WeatherService::search_city(const QString& query) {
    QUrl url(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), query.trimmed());
    q.addQueryItem(QStringLiteral("count"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("language"), QStringLiteral("en"));
    url.setQuery(q);
    QNetworkReply* reply = net_->get(make_request(url));
    const quint64 gen = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, query, gen] {
        reply->deleteLater();
        if (gen != generation_) return;
        const QJsonArray results = QJsonDocument::fromJson(reply->readAll())
            .object().value(QStringLiteral("results")).toArray();
        if (reply->error() != QNetworkReply::NoError || results.isEmpty()) {
            emit city_not_found(query);
            return;
        }
        const QJsonObject r = results.first().toObject();
        const QString name = r.value(QStringLiteral("name")).toString();
        const double lat = r.value(QStringLiteral("latitude")).toDouble();
        const double lon = r.value(QStringLiteral("longitude")).toDouble();
        const bool f = uses_fahrenheit(
            r.value(QStringLiteral("country_code")).toString());
        use_location(name, lat, lon, f);
        emit city_resolved(name, lat, lon, f);
    });
}

} // namespace aurora::ui

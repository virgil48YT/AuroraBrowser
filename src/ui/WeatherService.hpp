// Aurora — homepage weather.
//
// Real conditions for the weather chip on aurora://home. The location comes
// from the user's pinned city when they set one, otherwise from a coarse
// IP lookup (city level, no GPS, nothing stored on a server by Aurora); the
// conditions come from Open-Meteo, which needs no account or API key.
//
// One process-wide instance so every window and every new tab shows the same
// numbers instantly and the network is hit at most once per refresh period.
// Until a first successful fetch the chip stays hidden — the page never shows
// a placeholder temperature.
#pragma once

#include <QObject>
#include <QString>

#include <functional>

#include "ui/Icons.hpp"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace aurora::ui {

struct WeatherInfo {
    bool valid = false;
    double temperature = 0.0;
    bool fahrenheit = false;
    int code = 0;          // WMO weather interpretation code
    bool is_day = true;
    QString place;         // city name, may be empty
};

class WeatherService : public QObject {
    Q_OBJECT
public:
    static WeatherService& instance();

    const WeatherInfo& current() const { return info_; }

    // Short label for the current conditions ("Partly cloudy").
    static QString describe(int wmo_code, bool is_day);
    static icons::Name icon_for(int wmo_code, bool is_day);

    // Pin a specific place (lat/lon in degrees). Fetches immediately.
    void use_location(const QString& name, double lat, double lon,
                      bool fahrenheit);
    // Go back to locating by IP.
    void use_automatic_location();
    // Look a city up by name and pin the first match; emits
    // city_resolved() on success or city_not_found() otherwise.
    void search_city(const QString& query);
    void refresh();
    // refresh(), unless the numbers on screen are already fresh.
    void refresh_if_stale();

signals:
    void updated();
    void city_resolved(const QString& name, double lat, double lon,
                       bool fahrenheit);
    void city_not_found(const QString& query);

private:
    explicit WeatherService(QObject* parent = nullptr);

    void locate_by_ip();
    void locate_by_ip_fallback();
    void fetch_conditions();
    void schedule_retry();
    void finish_get(QNetworkReply* reply,
                    const std::function<void(const QByteArray&)>& on_ok,
                    const std::function<void()>& on_fail);

    QNetworkAccessManager* net_ = nullptr;
    QTimer* timer_ = nullptr;
    WeatherInfo info_;

    bool manual_ = false;
    bool have_location_ = false;
    double lat_ = 0.0;
    double lon_ = 0.0;
    bool fahrenheit_ = false;
    QString place_;
    bool in_flight_ = false;
    qint64 last_ok_ms_ = 0;    // wall clock of the last successful fetch
    quint64 generation_ = 0;   // drops replies that predate a location change
};

} // namespace aurora::ui

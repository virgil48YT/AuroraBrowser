#include "ui/ShortcutList.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include "settings/Settings.hpp"

namespace aurora::ui {

QString normalize_shortcut_url(const QString& input) {
    QString text = input.trimmed();
    if (text.isEmpty() || text.contains(QLatin1Char(' '))) return {};
    if (!text.contains(QStringLiteral("://"))) {
        text.prepend(QStringLiteral("https://"));
    }
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() ||
        (scheme != QStringLiteral("http") &&
         scheme != QStringLiteral("https"))) {
        return {};
    }
    return url.toString();
}

QString default_shortcut_title(const QString& url) {
    QString host = QUrl(url).host();
    if (host.startsWith(QStringLiteral("www."))) host.remove(0, 4);
    return host.isEmpty() ? url : host;
}

QVector<Shortcut> load_shortcuts(const settings::Settings* prefs,
                                 std::string_view key) {
    QVector<Shortcut> out;
    if (!prefs) return out;
    const std::string raw = prefs->get_string(key, "[]");
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(raw));
    if (!doc.isArray()) return out;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        Shortcut s;
        s.url = normalize_shortcut_url(o.value(QStringLiteral("url")).toString());
        if (s.url.isEmpty()) continue;
        s.title = o.value(QStringLiteral("title")).toString().trimmed();
        if (s.title.isEmpty()) s.title = default_shortcut_title(s.url);
        out.append(s);
    }
    return out;
}

void save_shortcuts(settings::Settings* prefs, std::string_view key,
                    const QVector<Shortcut>& list) {
    if (!prefs) return;
    QJsonArray arr;
    for (const Shortcut& s : list) {
        QJsonObject o;
        o.insert(QStringLiteral("title"), s.title);
        o.insert(QStringLiteral("url"), s.url);
        arr.append(o);
    }
    const QByteArray json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    prefs->set_string(key, std::string_view(json.constData(),
                                            static_cast<size_t>(json.size())));
}

} // namespace aurora::ui

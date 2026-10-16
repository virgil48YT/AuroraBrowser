// Aurora — user-managed shortcut lists.
//
// Two places let the user collect their own sites: the tiles under the
// homepage search pill and the pinned-site buttons on the left dock. Neither
// ships with presets; both start empty and persist as a JSON array in the
// profile's preferences (one key per list).
#pragma once

#include <QString>
#include <QVector>

#include <string_view>

namespace aurora::settings { class Settings; }

namespace aurora::ui {

struct Shortcut {
    QString title;
    QString url;
};

// Turn whatever the user typed into an openable http(s) URL, or an empty
// string when it is not one ("github.com" becomes "https://github.com").
QString normalize_shortcut_url(const QString& input);

// A readable default label for a URL: the host without a leading "www.".
QString default_shortcut_title(const QString& url);

// Load/save a list from the preferences store. A null store loads an empty
// list and saves nothing (windows created without a profile).
QVector<Shortcut> load_shortcuts(const settings::Settings* prefs,
                                 std::string_view key);
void save_shortcuts(settings::Settings* prefs, std::string_view key,
                    const QVector<Shortcut>& list);

} // namespace aurora::ui

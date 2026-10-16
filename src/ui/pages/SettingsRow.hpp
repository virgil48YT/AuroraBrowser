// Aurora — reusable Chrome-style row for the settings page.
//
// One settings entry laid out the way chrome://settings does it: the title
// plus an optional muted explanatory subtitle on the left, the control
// (checkbox, combo, spinbox, ...) flush right. The row also carries the
// lower-cased search terms the page filter matches against, so filtering
// only shows or hides rows and the cards that own them — it never touches
// the controls or the preferences behind them.
#pragma once

#include <QWidget>

class QHBoxLayout;
class QLabel;

namespace aurora::ui {

class SettingsRow : public QWidget {
    Q_OBJECT
public:
    SettingsRow(const QString& title, const QString& subtitle,
                QWidget* control, QWidget* parent = nullptr);

    // Lower-cased haystack the settings filter matches against
    // (title + subtitle + anything added with add_search_text()).
    QString search_terms() const { return search_terms_; }
    void add_search_text(const QString& text);

    // Append an extra widget on the right, after the primary control (used
    // for the shortcut key hints on the About card).
    void add_accessory(QWidget* widget);

    // Insert a full-width leading widget into the row, before the title
    // column (used by the Startup card's radio options, which read as
    // whole rows rather than as title/control pairs).
    void prepend_leading(QWidget* widget);

private:
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QHBoxLayout* layout_ = nullptr;
    QString search_terms_;
};

} // namespace aurora::ui

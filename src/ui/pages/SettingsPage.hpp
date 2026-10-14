// Aurora — settings page (aurora://settings).
//
// Real typed preferences persisted through settings::Settings the moment the
// user changes a control (no Apply button, no shadow state): default search
// engine (which is also the home / new-tab page — there is no separate startup
// URL option), theme, restore-session toggle, downloads directory and the two
// privacy switches the engine reads.
#pragma once

#include "ui/pages/InternalPageWidget.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace aurora::settings { class Settings; }

namespace aurora::ui {

class SettingsPage : public InternalPageWidget {
    Q_OBJECT
public:
    explicit SettingsPage(settings::Settings* prefs, QWidget* parent = nullptr);

    // Reload every control from the store (values may have changed elsewhere).
    void refresh() override;

signals:
    // "system" | "light" | "dark" — the window re-applies its stylesheet.
    void theme_preference_changed(const QString& theme);
    // The default search engine identifier changed.
    void search_engine_changed(const QString& engine_id);
    // Aurora Shields was switched on/off globally. The window pushes the new
    // state to the engine immediately (no restart needed).
    void adblock_enabled_changed(bool enabled);

private:
    // Guard so programmatic control updates in refresh() do not write back.
    bool loading_ = false;

    settings::Settings* prefs_ = nullptr;
    QComboBox* search_engine_ = nullptr;
    QComboBox* theme_ = nullptr;
    QCheckBox* restore_session_ = nullptr;
    QSpinBox* suspend_minutes_ = nullptr;
    QLineEdit* downloads_path_ = nullptr;
    QPushButton* downloads_browse_ = nullptr;
    QCheckBox* downloads_ask_where_ = nullptr;
    QCheckBox* block_third_party_cookies_ = nullptr;
    QCheckBox* send_dnt_ = nullptr;
    QCheckBox* adblock_enabled_ = nullptr;
    QCheckBox* reduced_motion_ = nullptr;
};

} // namespace aurora::ui

// Aurora — settings page (aurora://settings).
//
// Real typed preferences persisted through settings::Settings the moment the
// user changes a control (no Apply button, no shadow state): default search
// engine (which is also the home / new-tab page — there is no separate startup
// URL option), theme, restore-session toggle, downloads directory and the two
// privacy switches the engine reads.
//
// Custom search engines (name + https:// template with one %s) are listed and
// managed here too. The page never touches SQLite for them: it reads
// SearchEngineService's in-memory snapshot and posts add/edit/delete to the
// service's worker thread, then refreshes when the result is marshalled back
// to the UI thread.
#pragma once

#include <cstdint>

#include "ui/pages/InternalPageWidget.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace aurora::settings { class Settings; }
namespace aurora::searchengines {
class SearchEngineService;
struct MutationResult;
} // namespace aurora::searchengines

namespace aurora::ui {

class SettingsPage : public InternalPageWidget {
    Q_OBJECT
public:
    // `engines` may be null (no profile); custom-engine management is then
    // disabled and only the built-in engines are offered.
    explicit SettingsPage(settings::Settings* prefs,
                          searchengines::SearchEngineService* engines = nullptr,
                          QWidget* parent = nullptr);

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
    void report_change(const QString& message);

    // Custom search engines. reload_search_engines() rebuilds the default-
    // engine picker and the custom-engine list from the service's in-memory
    // snapshot (no SQLite) and re-selects the stored preference.
    void reload_search_engines();
    bool search_engines_editable() const;
    void update_engine_buttons();
    // Add (id == 0) or edit an engine through a validating dialog.
    void edit_search_engine(int64_t id);
    void delete_selected_search_engine();
    int64_t selected_search_engine() const;
    // Runs on the UI thread once the worker reports a mutation's result.
    void on_search_engine_saved(const searchengines::MutationResult& result,
                                const QString& success_message, bool removed);

    // Guard so programmatic control updates in refresh() do not write back.
    bool loading_ = false;

    settings::Settings* prefs_ = nullptr;
    searchengines::SearchEngineService* engines_ = nullptr;
    QComboBox* search_engine_ = nullptr;
    QTreeWidget* engine_list_ = nullptr;
    QPushButton* engine_add_ = nullptr;
    QPushButton* engine_edit_ = nullptr;
    QPushButton* engine_delete_ = nullptr;
    QLabel* engine_note_ = nullptr;
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

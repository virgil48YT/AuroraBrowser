// Aurora — settings page (aurora://settings).
//
// Real typed preferences persisted through settings::Settings the moment the
// user changes a control (no Apply button, no shadow state): default search
// engine (which is also the home / new-tab page — there is no separate
// startup URL option), theme, restore-session toggle, downloads directory
// and the privacy switches the engine reads.
//
// The layout follows chrome://settings: a fixed left rail with one entry per
// section (scrolling the matching card into view), a live filter field above
// a scrolling column of rounded cards, and rows with the title plus an
// explanatory subtitle on the left and the control on the right. The filter
// only shows/hides rows and cards — it never touches the preferences.
//
// Custom search engines (name + https:// template with one %s) are managed in
// the Search card: one SettingsRow per engine (Edit / Delete), plus an "Add"
// row. The page never touches SQLite for them: it reads SearchEngineService's
// in-memory snapshot and posts add/edit/delete to the service's worker
// thread, then rebuilds the rows when the result is marshalled back to the UI
// thread.
#pragma once

#include <cstdint>

#include <QList>
#include <QPointer>
#include <QString>

#include "ui/Icons.hpp"
#include "ui/ThemeState.hpp"
#include "ui/pages/InternalPageWidget.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPropertyAnimation;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace aurora::settings { class Settings; }
namespace aurora::searchengines {
class SearchEngineService;
struct MutationResult;
} // namespace aurora::searchengines

namespace aurora::ui {

class SettingsRow;

class SettingsPage : public InternalPageWidget {
    Q_OBJECT
public:
    // `engines` may be null (no profile); custom-engine management is then
    // disabled and only the built-in engines are offered.
    explicit SettingsPage(settings::Settings* prefs,
                          searchengines::SearchEngineService* engines = nullptr,
                          QWidget* parent = nullptr);
    ~SettingsPage() override;

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
    // One card on the page plus the sidebar entry that scrolls to it.
    struct Section {
        QWidget* wrapper = nullptr;   // header + card block (hidden whole by the filter)
        QWidget* card = nullptr;      // the rounded card itself
        QVBoxLayout* body = nullptr;  // row list inside the card
        QString title;
        icons::Name icon = icons::Name::Settings;
        QToolButton* button = nullptr;  // sidebar entry, lazily created
    };

    void report_change(const QString& message);

    // Custom search engines. reload_search_engines() rebuilds the default-
    // engine picker and the per-engine rows of the Search card from the
    // service's in-memory snapshot (no SQLite), re-selects the stored
    // preference and re-applies the filter.
    void reload_search_engines();
    bool search_engines_editable() const;
    // Add (id == 0) or edit an engine through a validating dialog.
    void edit_search_engine(int64_t id);
    void delete_search_engine(int64_t id, const QString& name);
    // Runs on the UI thread once the worker reports a mutation's result.
    void on_search_engine_saved(const searchengines::MutationResult& result,
                                const QString& success_message, bool removed);

    // Adds a Chrome-style card (header above a rounded group of rows) and
    // appends its nav entry. Returns the vbox rows are added to.
    QVBoxLayout* add_section(const QString& title, icons::Name icon);
    // Adds a row to a section body and indexes its text for the filter.
    SettingsRow* add_row(QVBoxLayout* body, const QString& title,
                         const QString& subtitle, QWidget* control);
    void add_divider(QWidget* parent, QVBoxLayout* into);

    // Filter / nav behaviour. Re-applied on text change and when sections
    // are (re)populated; empty sections disappear entirely while filtering.
    void apply_filter();
    void select_section(int index, bool scroll_to);
    // Eases the content to the section's card unless the reduced-motion
    // preference is on (instant jump then).
    void scroll_to_section(const Section& section);
    void update_sidebar_mode();
    void apply_sidebar_icons();

    void resizeEvent(QResizeEvent* event) override;

    // Guard so programmatic control updates in refresh() do not write back.
    bool loading_ = false;

    settings::Settings* prefs_ = nullptr;
    searchengines::SearchEngineService* engines_ = nullptr;

    // Chrome-like chrome: left section rail, live filter, scrolling cards.
    QWidget* sidebar_ = nullptr;
    QVBoxLayout* sidebar_layout_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QLabel* no_results_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QVBoxLayout* cards_column_ = nullptr;
    QPointer<QPropertyAnimation> scroll_anim_;
    QList<Section> sections_;
    ThemeObserverId theme_observer_ = 0;
    int active_section_ = -1;
    bool sidebar_compact_ = false;

    // Settings controls. Identical set, keys and write-behind behaviour as
    // before the restyle; only their presentation changed.
    QComboBox* search_engine_ = nullptr;
    // Search card body, the "Add search engine" button, and the rows (plus
    // their dividers) rebuilt for each custom engine on reload.
    QVBoxLayout* search_body_ = nullptr;
    QPushButton* engine_add_ = nullptr;
    QList<QWidget*> engine_rows_;
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

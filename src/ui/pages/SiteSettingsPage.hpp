// Aurora — site settings page (aurora://site-settings).
//
// Everything Aurora remembers per site, in one place, the way
// chrome://settings/content lists it:
//
//   * permission answers (microphone / camera / notifications), stored by
//     permissions::PermissionStore as "perm.<kind>.<origin>" prefs;
//   * custom zoom levels, stored by settings::SiteZoomStore as
//     "zoom.<origin>" prefs;
//   * Aurora Shields exceptions (ad blocking turned off for a site), stored
//     by adblock::AdblockService in the adblock_exceptions table.
//
// Entries are grouped into one card per site — adblock::site_key() of the
// origin, the same "host without www." the shield shows — so a site with a
// camera grant, a zoom level and Shields down reads as one block. Each entry
// can be cleared on its own, or the whole site at once. Clearing is the only
// write this page performs: granting happens at the prompt, zooming in the
// page, and Shields from the toolbar shield, so there is one place to set
// each thing and this page to review and undo them.
//
// The page reads the stores on every refresh() (each time it is shown). It
// never talks to the engine: a removed Shields exception is announced with
// adblock_exceptions_changed() and the window re-pushes the engine snapshot,
// the same path the toolbar shield uses.
#pragma once

#include <QList>
#include <QString>

#include "ui/pages/InternalPageWidget.hpp"

class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

namespace aurora::settings { class Settings; }
namespace aurora::adblock { class AdblockService; }

namespace aurora::ui {

class SettingsRow;

class SiteSettingsPage : public InternalPageWidget {
    Q_OBJECT
public:
    // Either service may be null (no profile; private windows have no
    // AdblockService and a read-only prefs view). The page then lists what
    // it can and disables what it cannot change.
    explicit SiteSettingsPage(settings::Settings* prefs,
                              adblock::AdblockService* adblock,
                              QWidget* parent = nullptr);

    void refresh() override;

signals:
    // A Shields exception was removed; the window pushes the new exception
    // set to the engine and repaints the toolbar shield.
    void adblock_exceptions_changed();

private:
    // True when this window may clear entries (a normal-window profile).
    bool editable() const;
    // Drop every card and rebuild them from the stores.
    void rebuild();
    void apply_filter();

    settings::Settings* prefs_ = nullptr;
    adblock::AdblockService* adblock_ = nullptr;

    QLineEdit* filter_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QVBoxLayout* cards_column_ = nullptr;
    QLabel* no_results_ = nullptr;
    // One wrapper (header + card) per site, with its lower-cased search
    // text, so the filter only toggles visibility.
    struct Card {
        QWidget* wrapper = nullptr;
        QString terms;
    };
    QList<Card> cards_;
};

} // namespace aurora::ui

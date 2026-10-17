#include "ui/pages/SiteSettingsPage.hpp"

#include <map>
#include <string>
#include <vector>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "adblock/AdblockService.hpp"
#include "permissions/PermissionStore.hpp"
#include "settings/Settings.hpp"
#include "settings/SiteZoomStore.hpp"
#include "ui/Theme.hpp"
#include "ui/pages/SettingsRow.hpp"

namespace aurora::ui {

namespace {

QString kind_label(permissions::PermissionKind kind) {
    switch (kind) {
        case permissions::PermissionKind::AudioCapture:
            return QStringLiteral("Microphone");
        case permissions::PermissionKind::VideoCapture:
            return QStringLiteral("Camera");
        case permissions::PermissionKind::Notification:
            return QStringLiteral("Notifications");
    }
    return QStringLiteral("Permission");
}

// Everything remembered for one site card. Permission and zoom entries keep
// their full origin (scheme and port matter to the stores); Shields
// exceptions are keyed by the site itself.
struct SiteEntries {
    std::vector<permissions::RememberedPermission> permissions;
    std::vector<settings::SiteZoomStore::Entry> zooms;
    bool shields_down = false;
};

} // namespace

SiteSettingsPage::SiteSettingsPage(settings::Settings* prefs,
                                   adblock::AdblockService* adblock,
                                   QWidget* parent)
    : InternalPageWidget(QStringLiteral("Site settings"), parent),
      prefs_(prefs),
      adblock_(adblock) {
    set_subtitle(QStringLiteral(
        "Permissions, zoom levels and Shields exceptions Aurora remembers for "
        "each site."));

    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("PageSearch"));
    filter_->setPlaceholderText(QStringLiteral("Search sites"));
    filter_->setClearButtonEnabled(true);
    toolbar_row()->addWidget(filter_, 1);

    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    no_results_ = new QLabel(QStringLiteral("No sites match your search"), body);
    no_results_->setObjectName(QStringLiteral("PageEmptyState"));
    no_results_->setAlignment(Qt::AlignCenter);
    no_results_->setWordWrap(true);
    no_results_->hide();
    body_layout->addWidget(no_results_, 1);

    // Same scroll/card chrome as aurora://settings (StylesheetBuilder styles
    // these object names from the active Theme), so the two pages read as
    // one settings surface.
    scroll_ = new QScrollArea(body);
    scroll_->setObjectName(QStringLiteral("SettingsScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    body_layout->addWidget(scroll_, 1);

    auto* container = new QWidget(scroll_);
    container->setObjectName(QStringLiteral("SettingsContainer"));
    cards_column_ = new QVBoxLayout(container);
    cards_column_->setContentsMargins(0, 0, Spacing::md, 0);
    cards_column_->setSpacing(Spacing::xl);
    scroll_->setWidget(container);
    set_body(body);

    connect(filter_, &QLineEdit::textChanged, this,
            [this](const QString&) { apply_filter(); });
}

bool SiteSettingsPage::editable() const {
    return prefs_ && !prefs_->is_read_only();
}

void SiteSettingsPage::refresh() { rebuild(); }

void SiteSettingsPage::rebuild() {
    // Tear down the previous cards (and the trailing stretch / banner).
    cards_.clear();
    while (QLayoutItem* item = cards_column_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    if (!prefs_) {
        set_empty_state(true,
                        QStringLiteral("No profile is attached to this window, "
                                       "so site settings are unavailable."));
        return;
    }

    // Gather every store into site -> entries. std::map keeps the cards in
    // alphabetical order, which is how a user scans for a site.
    std::map<std::string, SiteEntries> sites;
    for (auto& perm : permissions::PermissionStore(*prefs_).list()) {
        const std::string site = adblock::site_key(perm.origin);
        if (site.empty()) continue;
        sites[site].permissions.push_back(std::move(perm));
    }
    for (auto& zoom : settings::SiteZoomStore(*prefs_).list()) {
        const std::string site = adblock::site_key(zoom.origin);
        if (site.empty()) continue;
        sites[site].zooms.push_back(std::move(zoom));
    }
    if (adblock_) {
        for (const auto& site : adblock_->list_exception_sites()) {
            if (!site.empty()) sites[site].shields_down = true;
        }
    }

    if (sites.empty()) {
        set_empty_state(true,
                        QStringLiteral("No site has stored permissions, zoom "
                                       "levels or Shields exceptions yet."));
        filter_->setEnabled(false);
        return;
    }
    set_empty_state(false);
    filter_->setEnabled(true);

    QWidget* container = scroll_->widget();
    if (prefs_->is_read_only()) {
        auto* warning = new QLabel(
            QStringLiteral("This is an incognito window - site settings can "
                           "only be cleared from a normal window"),
            container);
        warning->setObjectName(QStringLiteral("PageWarning"));
        warning->setWordWrap(true);
        cards_column_->addWidget(warning);
    }

    const bool can_edit = editable();
    // Every clear re-reads the stores and rebuilds, queued so the clicked
    // button is not deleted inside its own clicked() handler.
    const auto rebuild_later = [this] {
        QMetaObject::invokeMethod(this, [this] { rebuild(); },
                                  Qt::QueuedConnection);
    };

    for (const auto& [site_key, entries] : sites) {
        const QString site = QString::fromStdString(site_key);
        QString terms = site.toLower();

        auto* wrapper = new QWidget(container);
        wrapper->setObjectName(QStringLiteral("SettingsSection"));
        auto* wrapper_layout = new QVBoxLayout(wrapper);
        wrapper_layout->setContentsMargins(0, 0, 0, 0);
        wrapper_layout->setSpacing(Spacing::sm);

        auto* header_row = new QHBoxLayout;
        header_row->setContentsMargins(Spacing::sm, 0, 0, 0);
        header_row->setSpacing(Spacing::sm);
        auto* header = new QLabel(site, wrapper);
        header->setObjectName(QStringLiteral("SettingsSectionHeader"));
        header_row->addWidget(header, 1);
        auto* clear_site = new QPushButton(QStringLiteral("Clear all"), wrapper);
        clear_site->setObjectName(QStringLiteral("PageDangerButton"));
        clear_site->setToolTip(
            QStringLiteral("Forget everything Aurora remembers for %1").arg(site));
        clear_site->setEnabled(can_edit);
        header_row->addWidget(clear_site, 0);
        wrapper_layout->addLayout(header_row);

        auto* card = new QWidget(wrapper);
        card->setObjectName(QStringLiteral("SettingsCard"));
        auto* card_body = new QVBoxLayout(card);
        card_body->setContentsMargins(0, Spacing::xs, 0, Spacing::xs);
        card_body->setSpacing(0);
        wrapper_layout->addWidget(card);

        bool first_row = true;
        const auto add_row = [&](const QString& title, const QString& subtitle,
                                 const QString& button_text) {
            if (!first_row) {
                auto* divider = new QFrame(card);
                divider->setObjectName(QStringLiteral("SettingsRowDivider"));
                divider->setFrameShape(QFrame::NoFrame);
                divider->setFixedHeight(Metrics::hairline);
                card_body->addWidget(divider);
            }
            first_row = false;
            auto* button = new QPushButton(button_text, card);
            button->setObjectName(QStringLiteral("PageButton"));
            button->setEnabled(can_edit);
            card_body->addWidget(new SettingsRow(title, subtitle, button, card));
            terms += QLatin1Char(' ') + title.toLower() + QLatin1Char(' ') +
                     subtitle.toLower();
            return button;
        };

        for (const auto& perm : entries.permissions) {
            const QString origin = QString::fromStdString(perm.origin);
            const bool allowed =
                perm.decision == permissions::PermissionDecision::Allow;
            auto* button = add_row(
                kind_label(perm.kind),
                QStringLiteral("%1 · %2")
                    .arg(allowed ? QStringLiteral("Allowed")
                                 : QStringLiteral("Blocked"),
                         origin),
                QStringLiteral("Clear"));
            connect(button, &QPushButton::clicked, this,
                    [this, perm, rebuild_later] {
                        if (!editable()) return;
                        // Back to Ask: the site prompts again next time.
                        permissions::PermissionStore(*prefs_).remember(
                            perm.origin, perm.kind,
                            permissions::PermissionDecision::Ask);
                        emit status_message(
                            QStringLiteral("%1 permission cleared for %2")
                                .arg(kind_label(perm.kind),
                                     QString::fromStdString(perm.origin)));
                        rebuild_later();
                    });
        }
        for (const auto& zoom : entries.zooms) {
            const QString origin = QString::fromStdString(zoom.origin);
            auto* button = add_row(
                QStringLiteral("Zoom"),
                QStringLiteral("%1% · %2")
                    .arg(settings::SiteZoomStore::percent(zoom.level))
                    .arg(origin),
                QStringLiteral("Reset"));
            connect(button, &QPushButton::clicked, this,
                    [this, origin_key = zoom.origin, origin, rebuild_later] {
                        if (!editable()) return;
                        // Open tabs keep their current size until their next
                        // navigation, which then applies the default.
                        settings::SiteZoomStore(*prefs_).clear(origin_key);
                        emit status_message(
                            QStringLiteral("Zoom reset for %1").arg(origin));
                        rebuild_later();
                    });
        }
        if (entries.shields_down) {
            auto* button = add_row(
                QStringLiteral("Aurora Shields"),
                QStringLiteral("Off for this site — ads and trackers are not "
                               "blocked"),
                QStringLiteral("Turn on"));
            button->setEnabled(can_edit && adblock_ != nullptr);
            connect(button, &QPushButton::clicked, this,
                    [this, site_key = site_key, site, rebuild_later] {
                        if (!editable() || !adblock_) return;
                        adblock_->set_site_allowed(site_key, false);
                        emit adblock_exceptions_changed();
                        emit status_message(
                            QStringLiteral("Aurora Shields turned back on for %1")
                                .arg(site));
                        rebuild_later();
                    });
        }

        connect(clear_site, &QPushButton::clicked, this,
                [this, site_key = site_key, entries = entries, site,
                 rebuild_later] {
                    if (!editable()) return;
                    permissions::PermissionStore perms(*prefs_);
                    for (const auto& perm : entries.permissions)
                        perms.remember(perm.origin, perm.kind,
                                       permissions::PermissionDecision::Ask);
                    settings::SiteZoomStore zoom(*prefs_);
                    for (const auto& z : entries.zooms) zoom.clear(z.origin);
                    if (entries.shields_down && adblock_) {
                        adblock_->set_site_allowed(site_key, false);
                        emit adblock_exceptions_changed();
                    }
                    emit status_message(
                        QStringLiteral("Cleared site settings for %1").arg(site));
                    rebuild_later();
                });

        cards_column_->addWidget(wrapper);
        cards_.append(Card{wrapper, terms});
    }
    cards_column_->addStretch(1);
    apply_filter();
}

void SiteSettingsPage::apply_filter() {
    const QString needle = filter_->text().trimmed().toLower();
    bool any_visible = false;
    for (const Card& card : cards_) {
        const bool match = needle.isEmpty() || card.terms.contains(needle);
        card.wrapper->setVisible(match);
        any_visible = any_visible || match;
    }
    const bool nothing = !cards_.isEmpty() && !any_visible;
    scroll_->setVisible(!nothing);
    no_results_->setVisible(nothing);
}

} // namespace aurora::ui

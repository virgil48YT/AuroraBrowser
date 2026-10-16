#include "ui/pages/SettingsPage.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

#include "settings/Settings.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"
#include "ui/pages/SettingsRow.hpp"

namespace aurora::ui {

namespace {

// Identifier stored in preferences, label shown to the user.
struct Choice {
    const char* id;
    const char* label;
};

constexpr Choice kEngines[] = {
    {"duckduckgo", "DuckDuckGo"},
    {"google", "Google"},
    {"bing", "Bing"},
    {"startpage", "Startpage"},
};

constexpr Choice kThemes[] = {
    {"system", "Match system"},
    {"light", "Light"},
    {"dark", "Dark"},
};

// Window width (px) below which the section rail collapses from icon + label
// to icon-only, like Chrome's settings sidebar in narrow windows.
constexpr int kSidebarCompactBelow = 720;
constexpr int kSidebarIconPx = 16;

// Build version baked in by CMake (PROJECT_VERSION). Settings.cpp/.hpp live
// in a Qt-free module, so the macro is consumed here on the UI side; the
// fallback matches the version in CMakeLists.txt.
#ifdef AURORA_VERSION_STRING
constexpr const char* kVersion = AURORA_VERSION_STRING;
#else
constexpr const char* kVersion = "0.1.0";
#endif

// Shown on the About Aurora card. Display-only mirror of the bindings in
// MainWindow::bind_shortcuts(); update both together when a binding changes.
// History intentionally absent: the browser menu shows "Ctrl+H" as a hint,
// but no window shortcut actually owns it.
struct ShortcutLine { const char* keys; const char* action; };
constexpr ShortcutLine kShortcuts[] = {
    {"Ctrl+T", "Open a new tab"},
    {"Ctrl+W", "Close the current tab"},
    {"Ctrl+Shift+T", "Reopen the last closed tab"},
    {"Ctrl+Tab", "Switch to the next tab"},
    {"Ctrl+Shift+Tab", "Switch to the previous tab"},
    {"Ctrl+L", "Focus the address bar"},
    {"Ctrl+R", "Reload the page"},
    {"F5", "Reload the page (alternative)"},
    {"Ctrl+F", "Find on the page"},
    {"Ctrl+D", "Bookmark this page"},
    {"Ctrl+J", "Open Downloads"},
    {"Ctrl+Shift+I", "Open developer tools"},
    {"F11", "Toggle fullscreen"},
    {"Ctrl+Shift+N", "Open an incognito window"},
};

void fill(QComboBox* box, const Choice* choices, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        box->addItem(QString::fromUtf8(choices[i].label),
                     QString::fromUtf8(choices[i].id));
    }
}

void select_id(QComboBox* box, const QString& id) {
    const int index = box->findData(id);
    box->setCurrentIndex(index >= 0 ? index : 0);
}

} // namespace

SettingsPage::SettingsPage(settings::Settings* prefs, QWidget* parent)
    : InternalPageWidget(QStringLiteral("Settings"), parent), prefs_(prefs) {
    set_subtitle(QStringLiteral(
        "Preferences are saved as you change them and apply to every window "
        "on this profile."));

    // Live filter box in the page's actions row; like chrome://settings it
    // only narrows what is visible, it never changes stored preferences.
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("PageSearch"));
    filter_->setPlaceholderText(QStringLiteral("Search settings"));
    filter_->setClearButtonEnabled(true);
    toolbar_row()->addWidget(filter_, 1);

    // --- Body: fixed section rail on the left, scrolling cards on the right.
    auto* body = new QWidget(this);
    auto* body_row = new QHBoxLayout(body);
    body_row->setContentsMargins(0, 0, 0, 0);
    body_row->setSpacing(Spacing::xl);

    sidebar_ = new QWidget(body);
    sidebar_->setObjectName(QStringLiteral("SettingsSidebar"));
    sidebar_layout_ = new QVBoxLayout(sidebar_);
    sidebar_layout_->setContentsMargins(0, 0, 0, 0);
    sidebar_layout_->setSpacing(Spacing::xxs);
    body_row->addWidget(sidebar_, 0);

    // Centered placeholder shown when the filter matches nothing; the cards
    // and sidebar collapse around it instead of leaving a blank area.
    no_results_ = new QLabel(
        QStringLiteral("No settings match your search"), body);
    no_results_->setObjectName(QStringLiteral("PageEmptyState"));
    no_results_->setAlignment(Qt::AlignCenter);
    no_results_->setWordWrap(true);
    no_results_->hide();
    body_row->addWidget(no_results_, 1);

    scroll_ = new QScrollArea(body);
    scroll_->setObjectName(QStringLiteral("SettingsScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    body_row->addWidget(scroll_, 1);

    auto* container = new QWidget(scroll_);
    container->setObjectName(QStringLiteral("SettingsContainer"));
    cards_column_ = new QVBoxLayout(container);
    cards_column_->setContentsMargins(0, 0, Spacing::md, 0);
    cards_column_->setSpacing(Spacing::xl);

    if (prefs_ && prefs_->is_read_only()) {
        auto* warning = new QLabel(
            QStringLiteral("This is an incognito window - changes are not saved"),
            container);
        warning->setObjectName(QStringLiteral("PageWarning"));
        warning->setWordWrap(true);
        cards_column_->addWidget(warning);
    }

    // === Search =============================================================
    QVBoxLayout* search_body = add_section(QStringLiteral("Search"),
                                           icons::Name::Search);
    search_engine_ = new QComboBox(container);
    search_engine_->setObjectName(QStringLiteral("PageCombo"));
    fill(search_engine_, kEngines, std::size(kEngines));
    SettingsRow* engine_row =
        add_row(search_body,
                QStringLiteral("Search engine"),
                QStringLiteral("Used in the omnibox; new tabs and the Home "
                               "button open its home page"),
                search_engine_);
    // Let the filter find the engine names themselves, not just the row.
    for (const auto& engine : kEngines) {
        engine_row->add_search_text(QString::fromUtf8(engine.label));
    }

    // === Privacy and security ===============================================
    QVBoxLayout* privacy_body =
        add_section(QStringLiteral("Privacy and security"), icons::Name::Lock);
    adblock_enabled_ = new QCheckBox(container);
    adblock_enabled_->setObjectName(QStringLiteral("PageCheck"));
    add_row(privacy_body,
            QStringLiteral("Aurora Shields"),
            QStringLiteral("Block ads and trackers. Per-site exceptions are "
                           "made from the shield in the toolbar; this switch "
                           "turns the whole feature off"),
            adblock_enabled_);
    add_divider(container, privacy_body);
    block_third_party_cookies_ = new QCheckBox(container);
    block_third_party_cookies_->setObjectName(QStringLiteral("PageCheck"));
    add_row(privacy_body,
            QStringLiteral("Block third-party cookies"),
            QStringLiteral("Sites you visit directly can still use cookies"),
            block_third_party_cookies_);
    add_divider(container, privacy_body);
    send_dnt_ = new QCheckBox(container);
    send_dnt_->setObjectName(QStringLiteral("PageCheck"));
    add_row(privacy_body,
            QStringLiteral("Send a “Do Not Track” request"),
            QStringLiteral("Websites may ignore the request"),
            send_dnt_);
    // Additional privacy rows use properties so refresh() can update them
    // without changing the existing controls or their write/read-only policy.
    struct PrivacyRow {
        const char* label;
        const char* subtitle;
        std::string_view key;
        bool fallback;
    };
    const PrivacyRow privacy_rows[] = {
        {"Use HTTPS-only mode (no insecure fallback)",
         "Refuse to load pages that are not encrypted",
         settings::PrefKeys::kHttpsOnly, settings::PrefDefaults::kHttpsOnly},
        {"Clear cookies when closing this window",
         "Stored cookies are removed when every window closes",
         settings::PrefKeys::kClearOnExitCookies,
         settings::PrefDefaults::kClearOnExitCookies},
        {"Clear browsing history when closing this window",
         "Visited pages are not kept between sessions",
         settings::PrefKeys::kClearOnExitHistory,
         settings::PrefDefaults::kClearOnExitHistory},
        {"Clear the download list when closing this window (keep files)",
         "The downloads page starts empty next time; files stay on disk",
         settings::PrefKeys::kClearOnExitDownloads,
         settings::PrefDefaults::kClearOnExitDownloads},
        {"Warn when a site's certificate fingerprint changes",
         "Alerts you when a previously seen site presents a new certificate",
         settings::PrefKeys::kCertAudit, settings::PrefDefaults::kCertAudit},
        {"Trim Referer headers to the referring origin",
         "Other sites see where you came from, not the exact page",
         settings::PrefKeys::kRefererTrim, settings::PrefDefaults::kRefererTrim},
    };
    for (const auto& row : privacy_rows) {
        add_divider(container, privacy_body);
        auto* box = new QCheckBox(container);
        box->setObjectName(QStringLiteral("PageCheck"));
        box->setProperty("auroraPrivacyKey",
                         QString::fromStdString(std::string(row.key)));
        box->setProperty("auroraPrivacyDefault", row.fallback);
        box->setEnabled(prefs_ != nullptr);
        box->setChecked(prefs_ ? prefs_->get_bool(row.key, row.fallback)
                               : row.fallback);
        add_row(privacy_body, QString::fromUtf8(row.label),
                QString::fromUtf8(row.subtitle), box);
        connect(box, &QCheckBox::toggled, this,
                [this, box, key = row.key](bool on) {
            if (loading_ || !prefs_ || !box->isEnabled()) return;
            prefs_->set_bool(key, on);
        });
    }

    // === Appearance =========================================================
    QVBoxLayout* appearance_body = add_section(QStringLiteral("Appearance"),
                                               icons::Name::Sun);
    theme_ = new QComboBox(container);
    theme_->setObjectName(QStringLiteral("PageCombo"));
    fill(theme_, kThemes, std::size(kThemes));
    SettingsRow* theme_row =
        add_row(appearance_body,
                QStringLiteral("Theme"),
                QStringLiteral("Aurora can follow the system light or dark "
                               "appearance"),
                theme_);
    for (const auto& theme_choice : kThemes) {
        theme_row->add_search_text(QString::fromUtf8(theme_choice.label));
    }
    add_divider(container, appearance_body);
    reduced_motion_ = new QCheckBox(container);
    reduced_motion_->setObjectName(QStringLiteral("PageCheck"));
    add_row(appearance_body,
            QStringLiteral("Reduce interface animation"),
            QStringLiteral("Turns off animated transitions across the browser, "
                           "including this page's scrolling"),
            reduced_motion_);

    // === Downloads ==========================================================
    QVBoxLayout* downloads_body = add_section(QStringLiteral("Downloads"),
                                              icons::Name::Downloads);
    auto* downloads_location = new QWidget(container);
    auto* downloads_location_layout = new QHBoxLayout(downloads_location);
    downloads_location_layout->setContentsMargins(0, 0, 0, 0);
    downloads_location_layout->setSpacing(Spacing::sm);
    downloads_path_ = new QLineEdit(downloads_location);
    downloads_path_->setObjectName(QStringLiteral("PageInput"));
    downloads_path_->setPlaceholderText(QStringLiteral("System download folder"));
    downloads_browse_ =
        new QPushButton(QStringLiteral("Choose…"), downloads_location);
    downloads_browse_->setObjectName(QStringLiteral("PageButton"));
    downloads_location_layout->addWidget(downloads_path_, 1);
    downloads_location_layout->addWidget(downloads_browse_);
    add_row(downloads_body,
            QStringLiteral("Location"),
            QStringLiteral("Folder files are saved to"),
            downloads_location);
    add_divider(container, downloads_body);
    downloads_ask_where_ = new QCheckBox(container);
    downloads_ask_where_->setObjectName(QStringLiteral("PageCheck"));
    add_row(downloads_body,
            QStringLiteral("Ask where to save each file before downloading"),
            QString(), downloads_ask_where_);

    // === Tabs and performance ===============================================
    QVBoxLayout* tabs_body =
        add_section(QStringLiteral("Tabs and performance"), icons::Name::NewTab);
    suspend_minutes_ = new QSpinBox(container);
    suspend_minutes_->setObjectName(QStringLiteral("TabSuspendMinutes"));
    suspend_minutes_->setRange(settings::PrefDefaults::kTabSuspendMinMinutes,
                               settings::PrefDefaults::kTabSuspendMaxMinutes);
    suspend_minutes_->setSuffix(QStringLiteral(" minutes"));
    suspend_minutes_->setSpecialValueText(QStringLiteral("Off"));
    add_row(tabs_body,
            QStringLiteral("Suspend inactive tabs after"),
            QStringLiteral("Suspended tabs free memory and reload when you "
                           "return to them"),
            suspend_minutes_);
    add_divider(container, tabs_body);
    restore_session_ = new QCheckBox(container);
    restore_session_->setObjectName(QStringLiteral("PageCheck"));
    add_row(tabs_body,
            QStringLiteral("Reopen the tabs from my last session on startup"),
            QString(), restore_session_);

    // === About Aurora =======================================================
    // No preference controls here: version info plus the keyboard shortcuts
    // (read-only, so it stays accurate and useful in incognito windows).
    QVBoxLayout* about_body = add_section(QStringLiteral("About Aurora"),
                                          icons::Name::Info);
    add_row(about_body,
            QStringLiteral("Aurora Browser"),
            QStringLiteral("Version %1 — built on the Chromium Embedded "
                           "Framework").arg(QString::fromUtf8(kVersion)),
            nullptr);
    for (const auto& shortcut : kShortcuts) {
        add_divider(container, about_body);
        auto* row = add_row(about_body, QString::fromUtf8(shortcut.action),
                            QString(), nullptr);
        const QString keys = QString::fromUtf8(shortcut.keys);
        auto* hint = new QLabel(keys, row);
        hint->setObjectName(QStringLiteral("SettingsRowHint"));
        row->add_accessory(hint);
        row->add_search_text(keys + QStringLiteral(" keyboard shortcut"));
    }

    if (!prefs_) {
        auto* warning = new QLabel(
            QStringLiteral("No profile is attached to this window, so changes "
                           "cannot be saved."),
            container);
        warning->setObjectName(QStringLiteral("PageWarning"));
        warning->setWordWrap(true);
        cards_column_->addWidget(warning);
        for (QWidget* w : {static_cast<QWidget*>(search_engine_),
                           static_cast<QWidget*>(theme_),
                           static_cast<QWidget*>(restore_session_),
                           static_cast<QWidget*>(suspend_minutes_),
                           static_cast<QWidget*>(downloads_path_),
                           static_cast<QWidget*>(downloads_browse_),
                           static_cast<QWidget*>(downloads_ask_where_),
                           static_cast<QWidget*>(block_third_party_cookies_),
                           static_cast<QWidget*>(send_dnt_),
                           static_cast<QWidget*>(adblock_enabled_),
                           static_cast<QWidget*>(reduced_motion_)}) {
            w->setEnabled(false);
        }
    }

    cards_column_->addStretch(1);
    scroll_->setWidget(container);
    set_body(body);

    // Sidebar entries, one per section, now that every card exists.
    for (int i = 0; i < sections_.size(); ++i) {
        Section& section = sections_[i];
        auto* button = new QToolButton(sidebar_);
        button->setObjectName(QStringLiteral("SettingsNavButton"));
        button->setText(section.title);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setToolTip(section.title);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        section.button = button;
        sidebar_layout_->addWidget(button);
        connect(button, &QToolButton::clicked, this,
                [this, i] { select_section(i, /*scroll_to=*/true); });
    }
    sidebar_layout_->addStretch(1);
    select_section(0, /*scroll_to=*/false);
    // Sidebar icons are stamped from the live palette, so re-render them on
    // every theme change — MainWindow re-publishes current_theme() before
    // notifying observers (same mechanism TabSpinner/PipWindow use).
    theme_observer_ = add_theme_observer(
        [this](const Theme&) { apply_sidebar_icons(); });
    apply_sidebar_icons();

    // Persist on change. `loading_` suppresses writes while refresh() is
    // populating the controls.
    connect(search_engine_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_ || !prefs_) return;
        const auto id = search_engine_->currentData().toString();
        prefs_->set_string(settings::PrefKeys::kSearchEngine, id.toStdString());
        emit search_engine_changed(id);
        report_change(QStringLiteral("Search engine set to %1")
                                .arg(search_engine_->currentText()));
    });
    connect(theme_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_ || !prefs_) return;
        const auto id = theme_->currentData().toString();
        prefs_->set_string(settings::PrefKeys::kTheme, id.toStdString());
        emit theme_preference_changed(id);
    });
    connect(restore_session_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_bool(settings::PrefKeys::kRestoreSession, on);
        report_change(on ? QStringLiteral("Session will be restored on "
                                               "startup")
                               : QStringLiteral("Startup will open a new tab"));
    });
    connect(suspend_minutes_, &QSpinBox::valueChanged, this, [this](int minutes) {
        if (loading_ || !prefs_) return;
        prefs_->set_int(settings::PrefKeys::kTabSuspendAfterMinutes, minutes);
        report_change(minutes
            ? QStringLiteral("Inactive tabs will suspend after %1 minutes")
                  .arg(minutes)
            : QStringLiteral("Automatic tab suspension turned off"));
    });
    connect(downloads_path_, &QLineEdit::editingFinished, this, [this] {
        if (loading_ || !prefs_) return;
        prefs_->set_downloads_path(
            downloads_path_->text().trimmed().toStdString());
        report_change(QStringLiteral("Download location saved"));
    });
    connect(downloads_browse_, &QPushButton::clicked, this, [this] {
        if (!prefs_) return;
        QString initial = downloads_path_->text().trimmed();
        if (initial.isEmpty()) {
            initial = QStandardPaths::writableLocation(
                QStandardPaths::DownloadLocation);
        }
        if (initial.isEmpty()) {
            initial = QDir::homePath() + QStringLiteral("/Downloads");
        }
        const QString chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose download location"), initial);
        if (chosen.isEmpty()) return;
        const QString clean_path = QDir::cleanPath(chosen);
        downloads_path_->setText(QDir::toNativeSeparators(clean_path));
        prefs_->set_downloads_path(clean_path.toStdString());
        report_change(QStringLiteral("Download location saved"));
    });
    connect(downloads_ask_where_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_downloads_ask_where(on);
        report_change(
            on ? QStringLiteral("Aurora will ask where to save downloads")
               : QStringLiteral("Downloads will be saved automatically"));
    });
    connect(block_third_party_cookies_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_bool(settings::PrefKeys::kBlockThirdPartyCookies, on);
    });
    connect(send_dnt_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_bool(settings::PrefKeys::kSendDoNotTrack, on);
    });
    connect(adblock_enabled_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_bool(settings::PrefKeys::kAdblockEnabled, on);
        emit adblock_enabled_changed(on);
        report_change(on
                                ? QStringLiteral("Aurora Shields will block ads "
                                                 "from the next page load")
                                : QStringLiteral("Aurora Shields turned off"));
    });
    connect(reduced_motion_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_bool(settings::PrefKeys::kReducedMotion, on);
    });

    connect(filter_, &QLineEdit::textChanged, this,
            [this](const QString&) { apply_filter(); });

    refresh();
}

SettingsPage::~SettingsPage() { remove_theme_observer(theme_observer_); }

QVBoxLayout* SettingsPage::add_section(const QString& title, icons::Name icon) {
    auto* wrapper = new QWidget(scroll_);
    wrapper->setObjectName(QStringLiteral("SettingsSection"));
    auto* wrapper_layout = new QVBoxLayout(wrapper);
    wrapper_layout->setContentsMargins(0, 0, 0, 0);
    wrapper_layout->setSpacing(Spacing::sm);

    auto* header = new QLabel(title, wrapper);
    header->setObjectName(QStringLiteral("SettingsSectionHeader"));
    wrapper_layout->addWidget(header);

    auto* card = new QWidget(wrapper);
    card->setObjectName(QStringLiteral("SettingsCard"));
    auto* card_body = new QVBoxLayout(card);
    card_body->setContentsMargins(0, Spacing::xs, 0, Spacing::xs);
    card_body->setSpacing(0);
    wrapper_layout->addWidget(card);

    cards_column_->addWidget(wrapper);
    sections_.append(Section{wrapper, card, card_body, title, icon, nullptr});
    return card_body;
}

SettingsRow* SettingsPage::add_row(QVBoxLayout* body, const QString& title,
                                   const QString& subtitle, QWidget* control) {
    auto* row = new SettingsRow(title, subtitle, control, body->parentWidget());
    body->addWidget(row);
    return row;
}

void SettingsPage::add_divider(QWidget* parent, QVBoxLayout* into) {
    auto* divider = new QFrame(parent);
    divider->setObjectName(QStringLiteral("SettingsRowDivider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedHeight(Metrics::hairline);
    into->addWidget(divider);
}

void SettingsPage::apply_filter() {
    const QString needle = filter_->text().trimmed().toLower();
    const bool filtering = !needle.isEmpty();

    bool any_visible = false;
    bool active_still_visible = false;
    int first_visible = -1;
    for (int i = 0; i < sections_.size(); ++i) {
        Section& section = sections_[i];
        const bool section_name_matches =
            filtering && section.title.toLower().contains(needle);

        int visible_rows = 0;
        for (int j = 0; j < section.body->count(); ++j) {
            QLayoutItem* item = section.body->itemAt(j);
            QWidget* widget = item ? item->widget() : nullptr;
            if (!widget) continue;
            if (qobject_cast<QFrame*>(widget)) {
                // Hide separators while filtering: matching rows are sparse
                // and hairlines between hidden neighbours read as strays.
                widget->setVisible(!filtering);
                continue;
            }
            auto* row = qobject_cast<SettingsRow*>(widget);
            bool match = true;
            if (filtering && row) {
                match = section_name_matches ||
                        row->search_terms().contains(needle);
            }
            widget->setVisible(match);
            if (match && row) ++visible_rows;
        }

        const bool section_visible = !filtering || visible_rows > 0;
        section.wrapper->setVisible(section_visible);
        if (section.button) section.button->setVisible(section_visible);
        if (section_visible) {
            any_visible = true;
            if (first_visible < 0) first_visible = i;
            if (i == active_section_) active_still_visible = true;
        }
    }

    // Highlight the first visible section unless the active one survived.
    if (any_visible && !active_still_visible && first_visible >= 0) {
        active_section_ = first_visible;
        if (sections_[first_visible].button) {
            sections_[first_visible].button->setChecked(true);
        }
        apply_sidebar_icons();
    }

    // Nothing matched: reuse the skeleton's inline empty state rather than
    // leaving a blank scroll area. Unlike the list pages the scroll area
    // always hosts the cards, so instead of replacing the body we show the
    // empty state text and collapse the section column.
    set_empty_state(false);
    sidebar_->setVisible(!filtering || any_visible);
    scroll_->setVisible(!filtering || any_visible);
    no_results_->setVisible(filtering && !any_visible);
}

void SettingsPage::select_section(int index, bool scroll_to) {
    if (index < 0 || index >= sections_.size()) return;
    active_section_ = index;
    Section& section = sections_[index];
    if (section.button && !section.button->isChecked()) {
        section.button->setChecked(true);
    }
    // Checked state drove the icon tint: restamp so the active entry uses
    // the accent color and the previous one falls back to muted.
    apply_sidebar_icons();
    if (scroll_to && scroll_ && section.wrapper->isVisible())
        scroll_to_section(section);
}

void SettingsPage::scroll_to_section(const Section& section) {
    // Chrome eases the content column to the tapped section. Honor the
    // "Reduce interface animation" preference exactly like the omnibox/tab
    // bar do: reduced-motion profiles get an instant jump.
    const bool reduce_motion =
        prefs_ && prefs_->get_bool(settings::PrefKeys::kReducedMotion, false);
    int target = section.wrapper->y();  // wrapper is a child of the scrolled widget
    target = qBound(0, target, scroll_->verticalScrollBar()->maximum());

    if (scroll_anim_) scroll_anim_->stop();
    if (reduce_motion) {
        scroll_->verticalScrollBar()->setValue(target);
        return;
    }
    auto* anim = new QPropertyAnimation(scroll_->verticalScrollBar(), "value",
                                        this);
    anim->setDuration(Motion::base);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(scroll_->verticalScrollBar()->value());
    anim->setEndValue(target);
    scroll_anim_ = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void SettingsPage::update_sidebar_mode() {
    const bool compact = width() < kSidebarCompactBelow;
    if (compact == sidebar_compact_) return;
    sidebar_compact_ = compact;
    for (Section& section : sections_) {
        if (!section.button) continue;
        section.button->setToolButtonStyle(
            compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
}

void SettingsPage::apply_sidebar_icons() {
    // QIcon does not follow palette changes, so the nav icons are stamped
    // from the live theme: muted at rest, accent on the active section. The
    // stylesheet supplies the button fill/hover; only the glyph is stateful
    // here.
    const Theme& theme = current_theme();
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    for (const Section& section : sections_) {
        if (!section.button) continue;
        const QColor color = section.button->isChecked()
            ? theme.colors.accent
            : theme.colors.text_muted;
        section.button->setIcon(
            icons::icon(section.icon, color, kSidebarIconPx, dpr));
    }
}

void SettingsPage::resizeEvent(QResizeEvent* event) {
    InternalPageWidget::resizeEvent(event);
    update_sidebar_mode();
}

void SettingsPage::report_change(const QString& message) {
    emit status_message(prefs_ && prefs_->is_read_only()
        ? QStringLiteral("Incognito changes are not saved") : message);
}

void SettingsPage::refresh() {
    if (!prefs_) return;
    loading_ = true;

    select_id(search_engine_,
              QString::fromStdString(prefs_->get_string(
                  settings::PrefKeys::kSearchEngine,
                  settings::PrefDefaults::kSearchEngine)));
    select_id(theme_, QString::fromStdString(prefs_->get_string(
                          settings::PrefKeys::kTheme,
                          settings::PrefDefaults::kTheme)));
    restore_session_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kRestoreSession,
                         settings::PrefDefaults::kRestoreSession));
    suspend_minutes_->setValue(static_cast<int>(prefs_->get_int(
        settings::PrefKeys::kTabSuspendAfterMinutes,
        settings::PrefDefaults::kTabSuspendAfterMinutes)));
    downloads_path_->setText(
        QString::fromStdString(prefs_->downloads_path()));
    downloads_ask_where_->setChecked(prefs_->downloads_ask_where());
    block_third_party_cookies_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kBlockThirdPartyCookies, true));
    send_dnt_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kSendDoNotTrack, false));
    adblock_enabled_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kAdblockEnabled, true));
    reduced_motion_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kReducedMotion, false));

    for (auto* box : findChildren<QCheckBox*>()) {
        const auto key = box->property("auroraPrivacyKey");
        if (!key.isValid()) continue;
        box->setChecked(prefs_->get_bool(
            key.toString().toStdString(),
            box->property("auroraPrivacyDefault").toBool()));
    }

    loading_ = false;
    apply_filter();
}

} // namespace aurora::ui

#include "ui/pages/SettingsPage.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "settings/Settings.hpp"
#include "ui/Theme.hpp"

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

QGroupBox* make_group(const QString& title, QFormLayout*& form) {
    auto* group = new QGroupBox(title);
    group->setObjectName("SettingsGroup");
    form = new QFormLayout(group);
    form->setContentsMargins(Spacing::lg, Spacing::lg, Spacing::lg, Spacing::lg);
    form->setSpacing(Spacing::md);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    return group;
}

} // namespace

SettingsPage::SettingsPage(settings::Settings* prefs, QWidget* parent)
    : InternalPageWidget(QStringLiteral("Settings"), parent), prefs_(prefs) {
    set_subtitle(QStringLiteral(
        "Preferences are saved as you change them and apply to every window "
        "on this profile."));

    // The form scrolls: the window can be short and settings only grow.
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("SettingsScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* container = new QWidget(scroll);
    container->setObjectName("SettingsContainer");
    auto* column = new QVBoxLayout(container);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(Spacing::lg);

    QFormLayout* startup_form = nullptr;
    auto* startup = make_group(QStringLiteral("Startup"), startup_form);
    auto* home_note = new QLabel(
        QStringLiteral("New tabs and the Home button open your search "
                       "engine's home page."),
        startup);
    home_note->setObjectName("PageHint");
    home_note->setWordWrap(true);
    startup_form->addRow(QString(), home_note);
    restore_session_ = new QCheckBox(
        QStringLiteral("Reopen the tabs from my last session on startup"), startup);
    restore_session_->setObjectName("PageCheck");
    startup_form->addRow(QString(), restore_session_);
    column->addWidget(startup);

    QFormLayout* tabs_form = nullptr;
    auto* tabs_group = make_group(QStringLiteral("Tabs"), tabs_form);
    suspend_minutes_ = new QSpinBox(tabs_group);
    suspend_minutes_->setObjectName("TabSuspendMinutes");
    suspend_minutes_->setRange(settings::PrefDefaults::kTabSuspendMinMinutes,
                               settings::PrefDefaults::kTabSuspendMaxMinutes);
    suspend_minutes_->setSuffix(QStringLiteral(" minutes"));
    suspend_minutes_->setSpecialValueText(QStringLiteral("Off"));
    tabs_form->addRow(QStringLiteral("Suspend inactive tabs after"),
                      suspend_minutes_);
    column->addWidget(tabs_group);

    QFormLayout* search_form = nullptr;
    auto* search = make_group(QStringLiteral("Search"), search_form);
    search_engine_ = new QComboBox(search);
    search_engine_->setObjectName("PageCombo");
    fill(search_engine_, kEngines, std::size(kEngines));
    search_form->addRow(QStringLiteral("Search engine used in the omnibox"),
                        search_engine_);
    column->addWidget(search);

    QFormLayout* appearance_form = nullptr;
    auto* appearance = make_group(QStringLiteral("Appearance"), appearance_form);
    theme_ = new QComboBox(appearance);
    theme_->setObjectName("PageCombo");
    fill(theme_, kThemes, std::size(kThemes));
    appearance_form->addRow(QStringLiteral("Theme"), theme_);
    reduced_motion_ = new QCheckBox(QStringLiteral("Reduce interface animation"),
                                    appearance);
    reduced_motion_->setObjectName("PageCheck");
    appearance_form->addRow(QString(), reduced_motion_);
    column->addWidget(appearance);

    QFormLayout* downloads_form = nullptr;
    auto* downloads = make_group(QStringLiteral("Downloads"), downloads_form);
    auto* downloads_location = new QWidget(downloads);
    auto* downloads_location_layout = new QHBoxLayout(downloads_location);
    downloads_location_layout->setContentsMargins(0, 0, 0, 0);
    downloads_location_layout->setSpacing(Spacing::sm);
    downloads_path_ = new QLineEdit(downloads_location);
    downloads_path_->setObjectName("PageInput");
    downloads_path_->setPlaceholderText(QStringLiteral("System download folder"));
    downloads_browse_ =
        new QPushButton(QStringLiteral("Choose…"), downloads_location);
    downloads_browse_->setObjectName("PageButton");
    downloads_location_layout->addWidget(downloads_path_, 1);
    downloads_location_layout->addWidget(downloads_browse_);
    downloads_form->addRow(QStringLiteral("Save files to"), downloads_location);
    downloads_ask_where_ = new QCheckBox(
        QStringLiteral("Ask where to save each file before downloading"),
        downloads);
    downloads_ask_where_->setObjectName("PageCheck");
    downloads_form->addRow(QString(), downloads_ask_where_);
    column->addWidget(downloads);

    QFormLayout* privacy_form = nullptr;
    auto* privacy = make_group(QStringLiteral("Privacy"), privacy_form);
    block_third_party_cookies_ =
        new QCheckBox(QStringLiteral("Block third-party cookies"), privacy);
    block_third_party_cookies_->setObjectName("PageCheck");
    privacy_form->addRow(QString(), block_third_party_cookies_);
    send_dnt_ = new QCheckBox(QStringLiteral("Send a “Do Not Track” request"),
                              privacy);
    send_dnt_->setObjectName("PageCheck");
    privacy_form->addRow(QString(), send_dnt_);
    // The global switch for Aurora Shields. Per-site exceptions are made from
    // the shield in the toolbar; this one turns the whole feature off (it
    // takes effect on the next page load).
    adblock_enabled_ = new QCheckBox(
        QStringLiteral("Block ads and trackers (Aurora Shields)"), privacy);
    adblock_enabled_->setObjectName("PageCheck");
    privacy_form->addRow(QString(), adblock_enabled_);
    column->addWidget(privacy);

    if (!prefs_) {
        auto* warning = new QLabel(
            QStringLiteral("No profile is attached to this window, so changes "
                           "cannot be saved."),
            container);
        warning->setObjectName("PageWarning");
        warning->setWordWrap(true);
        column->addWidget(warning);
        for (QWidget* w : {static_cast<QWidget*>(search_engine_),
                           static_cast<QWidget*>(theme_),
                           static_cast<QWidget*>(restore_session_),
                           static_cast<QWidget*>(suspend_minutes_),
                           static_cast<QWidget*>(downloads_path_),
                           static_cast<QWidget*>(downloads_browse_),
                           static_cast<QWidget*>(downloads_ask_where_),
                           static_cast<QWidget*>(block_third_party_cookies_),
                           static_cast<QWidget*>(send_dnt_),
                           static_cast<QWidget*>(reduced_motion_)}) {
            w->setEnabled(false);
        }
    }

    column->addStretch(1);
    scroll->setWidget(container);
    set_body(scroll);

    // Persist on change. `loading_` suppresses writes while refresh() is
    // populating the controls.
    connect(search_engine_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_ || !prefs_) return;
        const auto id = search_engine_->currentData().toString();
        prefs_->set_string(settings::PrefKeys::kSearchEngine, id.toStdString());
        emit search_engine_changed(id);
        emit status_message(QStringLiteral("Search engine set to %1")
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
        emit status_message(on ? QStringLiteral("Session will be restored on "
                                               "startup")
                               : QStringLiteral("Startup will open a new tab"));
    });
    connect(suspend_minutes_, &QSpinBox::valueChanged, this, [this](int minutes) {
        if (loading_ || !prefs_) return;
        prefs_->set_int(settings::PrefKeys::kTabSuspendAfterMinutes, minutes);
        emit status_message(minutes
            ? QStringLiteral("Inactive tabs will suspend after %1 minutes")
                  .arg(minutes)
            : QStringLiteral("Automatic tab suspension turned off"));
    });
    connect(downloads_path_, &QLineEdit::editingFinished, this, [this] {
        if (loading_ || !prefs_) return;
        prefs_->set_downloads_path(
            downloads_path_->text().trimmed().toStdString());
        emit status_message(QStringLiteral("Download location saved"));
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
        emit status_message(QStringLiteral("Download location saved"));
    });
    connect(downloads_ask_where_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_downloads_ask_where(on);
        emit status_message(
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
        emit status_message(on
                                ? QStringLiteral("Aurora Shields will block ads "
                                                 "from the next page load")
                                : QStringLiteral("Aurora Shields turned off"));
    });
    connect(reduced_motion_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_bool(settings::PrefKeys::kReducedMotion, on);
    });

    refresh();
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

    loading_ = false;
}

} // namespace aurora::ui

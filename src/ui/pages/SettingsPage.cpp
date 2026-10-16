#include "ui/pages/SettingsPage.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "searchengines/SearchEngine.hpp"
#include "searchengines/SearchEngineService.hpp"
#include "settings/Settings.hpp"
#include "ui/Theme.hpp"

namespace aurora::ui {

namespace {

// Identifier stored in preferences, label shown to the user.
struct Choice {
    const char* id;
    const char* label;
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

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// Case-insensitive name clash against the built-in labels and the other
// custom engines, for instant feedback in the dialog. The worker re-checks.
bool name_in_use(const QString& name, int64_t self_id,
                 const std::vector<searchengines::SearchEngine>& engines) {
    for (const auto& builtin : searchengines::builtin_engines()) {
        if (qs(builtin.name).compare(name, Qt::CaseInsensitive) == 0) return true;
    }
    for (const auto& engine : engines) {
        if (engine.id != self_id &&
            QString::fromStdString(engine.name).compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
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

SettingsPage::SettingsPage(settings::Settings* prefs,
                           searchengines::SearchEngineService* engines,
                           QWidget* parent)
    : InternalPageWidget(QStringLiteral("Settings"), parent),
      prefs_(prefs),
      engines_(engines) {
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

    if (prefs_ && prefs_->is_read_only()) {
        auto* warning = new QLabel(
            QStringLiteral("This is an incognito window - changes are not saved"),
            container);
        warning->setObjectName("PageWarning");
        warning->setWordWrap(true);
        column->addWidget(warning);
    }

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
    // Filled by reload_search_engines(): built-ins, then custom engines.
    search_form->addRow(QStringLiteral("Search engine used in the omnibox"),
                        search_engine_);

    engine_list_ = new QTreeWidget(search);
    engine_list_->setObjectName("PageList");
    engine_list_->setColumnCount(2);
    engine_list_->setHeaderLabels(
        {QStringLiteral("Custom engine"), QStringLiteral("Search URL")});
    engine_list_->setRootIsDecorated(false);
    engine_list_->setUniformRowHeights(true);
    engine_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    engine_list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    engine_list_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    engine_list_->setMinimumHeight(120);
    search_form->addRow(QStringLiteral("Custom search engines"), engine_list_);

    auto* engine_buttons = new QWidget(search);
    auto* engine_buttons_layout = new QHBoxLayout(engine_buttons);
    engine_buttons_layout->setContentsMargins(0, 0, 0, 0);
    engine_buttons_layout->setSpacing(Spacing::sm);
    engine_add_ = new QPushButton(QStringLiteral("Add…"), engine_buttons);
    engine_add_->setObjectName("PageButton");
    engine_edit_ = new QPushButton(QStringLiteral("Edit…"), engine_buttons);
    engine_edit_->setObjectName("PageButton");
    engine_delete_ = new QPushButton(QStringLiteral("Delete"), engine_buttons);
    engine_delete_->setObjectName("PageDangerButton");
    engine_buttons_layout->addWidget(engine_add_);
    engine_buttons_layout->addWidget(engine_edit_);
    engine_buttons_layout->addWidget(engine_delete_);
    engine_buttons_layout->addStretch(1);
    search_form->addRow(QString(), engine_buttons);

    engine_note_ = new QLabel(search);
    engine_note_->setObjectName("PageHint");
    engine_note_->setWordWrap(true);
    search_form->addRow(QString(), engine_note_);
    if (!engines_ || !prefs_) {
        engine_note_->setText(QStringLiteral(
            "Custom search engines need a profile; only the built-in engines "
            "are available in this window."));
    } else if (prefs_->is_read_only()) {
        engine_note_->setText(QStringLiteral(
            "Custom search engines can be used here but only managed from a "
            "normal (non-incognito) window."));
    } else if (!engines_->storage_available()) {
        engine_note_->setText(QStringLiteral(
            "Custom search engines could not be loaded from the profile, so "
            "they cannot be changed right now."));
    } else {
        engine_note_->setText(QStringLiteral(
            "A custom engine needs an https:// search URL with %s where the "
            "search terms go, e.g. https://example.com/search?q=%s. Its home "
            "page (for new tabs and Home) is the site's root."));
    }
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
    // Additional privacy rows use properties so refresh() can update them
    // without changing the existing controls or their write/read-only policy.
    struct PrivacyRow {
        const char* label;
        std::string_view key;
        bool fallback;
    };
    const PrivacyRow privacy_rows[] = {
        {"Use HTTPS-only mode (no insecure fallback)",
         settings::PrefKeys::kHttpsOnly, settings::PrefDefaults::kHttpsOnly},
        {"Clear cookies when closing this window",
         settings::PrefKeys::kClearOnExitCookies,
         settings::PrefDefaults::kClearOnExitCookies},
        {"Clear browsing history when closing this window",
         settings::PrefKeys::kClearOnExitHistory,
         settings::PrefDefaults::kClearOnExitHistory},
        {"Clear the download list when closing this window (keep files)",
         settings::PrefKeys::kClearOnExitDownloads,
         settings::PrefDefaults::kClearOnExitDownloads},
        {"Warn when a site's certificate fingerprint changes",
         settings::PrefKeys::kCertAudit, settings::PrefDefaults::kCertAudit},
        {"Trim Referer headers to the referring origin",
         settings::PrefKeys::kRefererTrim, settings::PrefDefaults::kRefererTrim},
    };
    for (const auto& row : privacy_rows) {
        auto* box = new QCheckBox(QString::fromUtf8(row.label), privacy);
        box->setObjectName("PageCheck");
        box->setProperty("auroraPrivacyKey",
                         QString::fromStdString(std::string(row.key)));
        box->setProperty("auroraPrivacyDefault", row.fallback);
        box->setEnabled(prefs_ != nullptr);
        box->setChecked(prefs_ ? prefs_->get_bool(row.key, row.fallback)
                               : row.fallback);
        privacy_form->addRow(QString(), box);
        connect(box, &QCheckBox::toggled, this, [this, box, key = row.key](bool on) {
            if (loading_ || !prefs_ || !box->isEnabled()) return;
            prefs_->set_bool(key, on);
        });
    }
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
                           static_cast<QWidget*>(adblock_enabled_),
                           static_cast<QWidget*>(reduced_motion_)}) {
            w->setEnabled(false);
        }
    }

    column->addStretch(1);
    scroll->setWidget(container);
    set_body(scroll);

    // Persist on change. `loading_` suppresses writes while refresh() is
    // populating the controls.
    connect(engine_add_, &QPushButton::clicked, this,
            [this] { edit_search_engine(0); });
    connect(engine_edit_, &QPushButton::clicked, this, [this] {
        if (const auto id = selected_search_engine()) edit_search_engine(id);
    });
    connect(engine_delete_, &QPushButton::clicked, this,
            &SettingsPage::delete_selected_search_engine);
    connect(engine_list_, &QTreeWidget::itemSelectionChanged, this,
            &SettingsPage::update_engine_buttons);
    connect(engine_list_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem*, int) {
                if (!search_engines_editable()) return;
                if (const auto id = selected_search_engine()) edit_search_engine(id);
            });
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

    refresh();
}

void SettingsPage::report_change(const QString& message) {
    emit status_message(prefs_ && prefs_->is_read_only()
        ? QStringLiteral("Incognito changes are not saved") : message);
}

bool SettingsPage::search_engines_editable() const {
    return engines_ && prefs_ && !prefs_->is_read_only() &&
           engines_->storage_available();
}

int64_t SettingsPage::selected_search_engine() const {
    const auto items = engine_list_->selectedItems();
    if (items.isEmpty()) return 0;
    return items.front()->data(0, Qt::UserRole).toLongLong();
}

void SettingsPage::update_engine_buttons() {
    const bool editable = search_engines_editable();
    const bool selected = selected_search_engine() != 0;
    engine_add_->setEnabled(editable);
    engine_edit_->setEnabled(editable && selected);
    engine_delete_->setEnabled(editable && selected);
}

void SettingsPage::reload_search_engines() {
    const bool was_loading = loading_;
    loading_ = true;  // rebuilding the combo must not write the preference

    const auto custom = engines_ ? engines_->snapshot()
                                 : std::vector<searchengines::SearchEngine>{};
    const int64_t keep_selected = selected_search_engine();

    search_engine_->clear();
    for (const auto& builtin : searchengines::builtin_engines()) {
        search_engine_->addItem(qs(builtin.name), qs(builtin.key));
    }
    for (const auto& engine : custom) {
        search_engine_->addItem(
            QString::fromStdString(engine.name),
            QString::fromStdString(searchengines::custom_engine_key(engine.id)));
    }

    engine_list_->clear();
    for (const auto& engine : custom) {
        auto* item = new QTreeWidgetItem(engine_list_);
        item->setText(0, QString::fromStdString(engine.name));
        item->setText(1, QString::fromStdString(engine.url_template));
        item->setData(0, Qt::UserRole, QVariant::fromValue<qlonglong>(engine.id));
        if (engine.id == keep_selected) item->setSelected(true);
    }
    if (custom.empty()) {
        auto* item = new QTreeWidgetItem(engine_list_);
        item->setText(0, QStringLiteral("No custom engines yet"));
        item->setFlags(Qt::NoItemFlags);
    }

    // A preference naming a deleted (or not yet loaded) custom engine shows
    // as DuckDuckGo, which is exactly what the omnibox falls back to.
    const std::string pref = prefs_
        ? prefs_->get_string(settings::PrefKeys::kSearchEngine,
                             settings::PrefDefaults::kSearchEngine)
        : std::string(settings::PrefDefaults::kSearchEngine);
    select_id(search_engine_,
              QString::fromStdString(
                  searchengines::resolve_default_engine(pref, custom).key));

    update_engine_buttons();
    loading_ = was_loading;
}

void SettingsPage::edit_search_engine(int64_t id) {
    if (!search_engines_editable()) return;
    const auto engines = engines_->snapshot();
    searchengines::SearchEngine current;
    if (id != 0) {
        bool found = false;
        for (const auto& engine : engines) {
            if (engine.id == id) {
                current = engine;
                found = true;
                break;
            }
        }
        if (!found) {
            reload_search_engines();
            return;
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle(id == 0 ? QStringLiteral("Add search engine")
                                  : QStringLiteral("Edit search engine"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(QString::fromStdString(current.name), &dialog);
    name->setObjectName("PageInput");
    name->setPlaceholderText(QStringLiteral("e.g. Kagi"));
    name->setMaxLength(static_cast<int>(searchengines::kMaxNameLength));
    auto* url = new QLineEdit(QString::fromStdString(current.url_template), &dialog);
    url->setObjectName("PageInput");
    url->setPlaceholderText(QStringLiteral("https://example.com/search?q=%s"));
    url->setMinimumWidth(360);
    auto* error = new QLabel(&dialog);
    error->setObjectName("PageWarning");
    error->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Search URL"), url);
    form->addRow(QString(), error);
    form->addRow(buttons);

    // Live validation with the same pure checks the worker applies.
    const auto validate = [&] {
        const QString n = QString::fromStdString(
            searchengines::normalize_input(name->text().toStdString()));
        const std::string t =
            searchengines::normalize_input(url->text().toStdString());
        QString message;
        if (const auto ne = searchengines::validate_name(n.toStdString());
            ne != searchengines::NameError::Ok) {
            message = qs(searchengines::describe(ne));
        } else if (name_in_use(n, id, engines)) {
            message = QStringLiteral("A search engine with that name already exists.");
        } else if (const auto te = searchengines::validate_template(t);
                   te != searchengines::TemplateError::Ok) {
            message = qs(searchengines::describe(te));
        }
        // Do not shout at an untouched, empty form.
        const bool pristine = name->text().isEmpty() && url->text().isEmpty();
        error->setText(pristine ? QString() : message);
        buttons->button(QDialogButtonBox::Save)->setEnabled(message.isEmpty());
    };
    connect(name, &QLineEdit::textChanged, &dialog, validate);
    connect(url, &QLineEdit::textChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validate();
    if (dialog.exec() != QDialog::Accepted) return;
    // The dialog's local event loop may have outlived the profile's
    // writability (or this page's service); re-check before posting.
    if (!search_engines_editable()) return;

    const std::string new_name = name->text().toStdString();
    const std::string new_template = url->text().toStdString();
    const QString label = QString::fromStdString(
        searchengines::normalize_input(new_name));

    // The callback runs on the service's worker thread: hop to the UI thread
    // through the application object, and drop the result if this page has
    // been destroyed in the meantime.
    QPointer<SettingsPage> self(this);
    const QString success = id == 0 ? QStringLiteral("Added search engine %1").arg(label)
                                    : QStringLiteral("Saved search engine %1").arg(label);
    auto done = [self, success](const searchengines::MutationResult& result) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, success, result] {
                if (self) self->on_search_engine_saved(result, success, false);
            },
            Qt::QueuedConnection);
    };
    if (id == 0) {
        engines_->add(new_name, new_template, std::move(done));
    } else {
        engines_->update(id, new_name, new_template, std::move(done));
    }
}

void SettingsPage::delete_selected_search_engine() {
    if (!search_engines_editable()) return;
    const auto items = engine_list_->selectedItems();
    if (items.isEmpty()) return;
    const int64_t id = items.front()->data(0, Qt::UserRole).toLongLong();
    if (id == 0) return;
    const QString label = items.front()->text(0);
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Delete search engine"),
        QStringLiteral("Delete the search engine \"%1\"?").arg(label),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes || !search_engines_editable()) return;

    QPointer<SettingsPage> self(this);
    const QString success = QStringLiteral("Deleted search engine %1").arg(label);
    engines_->remove(id, [self, success](const searchengines::MutationResult& result) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, success, result] {
                if (self) self->on_search_engine_saved(result, success, true);
            },
            Qt::QueuedConnection);
    });
}

void SettingsPage::on_search_engine_saved(
    const searchengines::MutationResult& result, const QString& success_message,
    bool removed) {
    if (!result.ok()) {
        emit status_message(QString::fromStdString(result.message()));
        reload_search_engines();
        return;
    }
    QString message = success_message;
    // Deleting the default engine: reset the preference explicitly instead of
    // leaving a dangling "custom:<id>" that silently means DuckDuckGo.
    if (removed && prefs_ && !prefs_->is_read_only() &&
        prefs_->get_string(settings::PrefKeys::kSearchEngine,
                           settings::PrefDefaults::kSearchEngine) ==
            searchengines::custom_engine_key(result.id)) {
        const QString fallback = qs(settings::PrefDefaults::kSearchEngine);
        prefs_->set_string(settings::PrefKeys::kSearchEngine,
                           settings::PrefDefaults::kSearchEngine);
        emit search_engine_changed(fallback);
        message += QStringLiteral(" — the omnibox now uses %1")
                       .arg(qs(searchengines::resolve_default_engine(
                                   settings::PrefDefaults::kSearchEngine, {})
                                   .name));
    }
    reload_search_engines();
    report_change(message);
}

void SettingsPage::refresh() {
    if (!prefs_) {
        reload_search_engines();
        return;
    }
    loading_ = true;

    reload_search_engines();
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
}

} // namespace aurora::ui

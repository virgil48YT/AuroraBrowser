#include "ui/pages/SettingsPage.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPixmap>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "searchengines/SearchEngine.hpp"
#include "searchengines/SearchEngineService.hpp"
#include "settings/Settings.hpp"
#include "ui/FilterListUpdater.hpp"
#include "ui/GlowFrame.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"
#include "ui/pages/SettingsRow.hpp"
#include "ui/ToggleSwitch.hpp"

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

// Settings > Language > Preferred language. The tag is stored in
// intl.language and becomes Accept-Language + the spell-check dictionary
// (see cef::refresh_privacy_preferences). "" keeps the engine default.
constexpr Choice kLanguages[] = {
    {"", "System default"},
    {"en-US", "English (United States)"},
    {"en-GB", "English (United Kingdom)"},
    {"ro-RO", "Română (Romanian)"},
    {"de-DE", "Deutsch (German)"},
    {"fr-FR", "Français (French)"},
    {"es-ES", "Español (Spanish)"},
    {"it-IT", "Italiano (Italian)"},
    {"pt-BR", "Português (Brasil)"},
    {"pt-PT", "Português (Portugal)"},
    {"nl-NL", "Nederlands (Dutch)"},
    {"pl-PL", "Polski (Polish)"},
    {"hu-HU", "Magyar (Hungarian)"},
    {"cs-CZ", "Čeština (Czech)"},
    {"sk-SK", "Slovenčina (Slovak)"},
    {"sv-SE", "Svenska (Swedish)"},
    {"da-DK", "Dansk (Danish)"},
    {"nb-NO", "Norsk bokmål (Norwegian)"},
    {"fi-FI", "Suomi (Finnish)"},
    {"el-GR", "Ελληνικά (Greek)"},
    {"tr-TR", "Türkçe (Turkish)"},
    {"bg-BG", "Български (Bulgarian)"},
    {"ru-RU", "Русский (Russian)"},
    {"uk-UA", "Українська (Ukrainian)"},
    {"ja-JP", "日本語 (Japanese)"},
    {"ko-KR", "한국어 (Korean)"},
    {"zh-CN", "中文 简体 (Chinese, Simplified)"},
    {"zh-TW", "中文 繁體 (Chinese, Traditional)"},
    {"hi-IN", "हिन्दी (Hindi)"},
    {"ar", "العربية (Arabic)"},
};

// The sidebar categories, in mockup order. Cards list the ids they belong to.
struct CategoryDef {
    const char* id;
    const char* title;
    icons::Name icon;
};
const CategoryDef kCategories[] = {
    {"general", "General", icons::Name::Settings},
    {"appearance", "Appearance", icons::Name::Palette},
    {"tabs", "Tabs", icons::Name::WindowTabs},
    {"browsing", "Browsing", icons::Name::Globe},
    {"search", "Search", icons::Name::Search},
    {"privacy", "Privacy & Security", icons::Name::Shield},
    {"downloads", "Downloads", icons::Name::Downloads},
    {"languages", "Languages", icons::Name::Language},
    {"extensions", "Extensions", icons::Name::Puzzle},
    {"advanced", "Advanced", icons::Name::Sliders},
};

// Page width (px) below which the sidebar card collapses from icon + label
// to icon-only, like Chrome's settings sidebar in narrow windows.
constexpr int kSidebarCompactBelow = 960;
constexpr int kSidebarWidth = 236;
constexpr int kSidebarCompactWidth = 76;
constexpr int kSidebarIconPx = 20;
constexpr int kCardIconPx = 22;

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
    {"Ctrl+P", "Print the page"},
    {"Ctrl+D", "Bookmark this page"},
    {"Ctrl+J", "Open Downloads"},
    {"Ctrl+Shift+I", "Open developer tools"},
    {"F11", "Toggle fullscreen"},
    {"Ctrl+Shift+N", "Open an incognito window"},
};

void select_id(QComboBox* box, const QString& id) {
    const int index = box->findData(id);
    box->setCurrentIndex(index >= 0 ? index : 0);
}

// The Startup card's single write path for the launch-mode preference.
void store_startup_mode(settings::Settings& prefs, settings::StartupMode mode) {
    prefs.set_string(settings::PrefKeys::kStartupMode,
                     settings::startup_mode_name(mode));
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

// A card's title bar: clicking anywhere on it collapses / expands the card
// (the chevron button on its right does the same).
class CardHeader final : public QWidget {
public:
    explicit CardHeader(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("SettingsCardHeader"));
        setCursor(Qt::PointingHandCursor);
    }
    std::function<void()> on_click;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (on_click && event->button() == Qt::LeftButton &&
            rect().contains(event->position().toPoint())) {
            on_click();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }
};

} // namespace

SettingsPage::SettingsPage(settings::Settings* prefs,
                           searchengines::SearchEngineService* engines,
                           QWidget* parent)
    : InternalPageWidget(QStringLiteral("Settings"), parent),
      prefs_(prefs),
      engines_(engines) {
    const ThemeColors& colors = current_theme().colors;
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;

    // --- Header: gear + title/subtitle on the left, the search field on the
    // right. The skeleton's own title label keeps the accessible name but is
    // hidden, like the History and Downloads pages.
    if (auto* t = page_title_label()) t->hide();
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("SettingsHeader"));
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(14);
    auto* gear = new QLabel(header);
    gear->setPixmap(icons::pixmap(icons::Name::Settings, 34, colors.accent, dpr));
    gear->setFixedSize(38, 38);
    header_row->addWidget(gear, 0, Qt::AlignVCenter);
    auto* titles = new QVBoxLayout;
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("Settings"), header);
    title->setObjectName(QStringLiteral("SettingsHeaderTitle"));
    titles->addWidget(title);
    auto* subtitle = new QLabel(
        QStringLiteral("Customize your browsing experience."), header);
    subtitle->setObjectName(QStringLiteral("SettingsHeaderSubtitle"));
    titles->addWidget(subtitle);
    header_row->addLayout(titles);
    header_row->addStretch(1);

    // Live filter in an omnibar-shaped glowing field; like chrome://settings
    // it only narrows what is visible, it never changes stored preferences.
    auto* search_wrap = new GlowFrame(header);
    search_wrap->setObjectName(QStringLiteral("SettingsSearchWrap"));
    {
        GlowFrame::Edge edge;
        edge.color1 = QColor(40, 190, 255, 230);   // cyan-blue
        edge.color2 = QColor(150, 90, 255, 215);   // violet
        edge.glow = QColor(70, 120, 255, 100);
        edge.glow_extent = 3;
        search_wrap->set_style(edge, QColor(5, 9, 30, 200), Radius::omnibox);
        search_wrap->setMinimumWidth(280);
        search_wrap->setMaximumWidth(420);
        search_wrap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* l = new QHBoxLayout(search_wrap);
        l->setContentsMargins(14, 8, 14, 8);
        l->setSpacing(10);
        auto* mag = new QLabel(search_wrap);
        mag->setPixmap(icons::pixmap(icons::Name::Search, 18, colors.text_muted, dpr));
        mag->setFixedSize(20, 20);
        l->addWidget(mag);
        filter_ = new QLineEdit(search_wrap);
        filter_->setObjectName(QStringLiteral("SettingsSearch"));
        filter_->setPlaceholderText(QStringLiteral("Search settings..."));
        // The placeholder must stay readable on the translucent field over
        // the aurora backdrop: muted, one step brighter than the faint grey.
        QPalette pal = filter_->palette();
        pal.setColor(QPalette::PlaceholderText, colors.text_muted);
        filter_->setPalette(pal);
        filter_->setClearButtonEnabled(true);
        l->addWidget(filter_, 1);
    }
    header_row->addWidget(search_wrap, 1, Qt::AlignVCenter);
    root_layout()->insertWidget(0, header);

    // --- Body: the sidebar card on the left, scrolling cards on the right.
    auto* body = new QWidget(this);
    body->setObjectName(QStringLiteral("SettingsBody"));
    // Pin the face the platform actually resolved for the UI font (the same
    // fix BrowserMenu uses): the app-wide CSS family list can land on a
    // partial fallback face on some Windows setups, which is what made the
    // settings text render oddly. Sizes/weights still come from the sheet.
    {
        QFont ui_font = QApplication::font();
        ui_font.setStyleHint(QFont::SansSerif, QFont::PreferDefault);
        const QString family = QFontInfo(ui_font).family();
        if (!family.isEmpty()) {
            body->setStyleSheet(
                QStringLiteral("QWidget#SettingsBody, QWidget#SettingsBody * {"
                               " font-family: \"%1\"; }")
                    .arg(family));
        }
    }
    auto* body_row = new QHBoxLayout(body);
    body_row->setContentsMargins(0, Spacing::xs, 0, 0);
    body_row->setSpacing(Spacing::lg + Spacing::xs);

    // Sidebar: a glass card with the AURORA brand and the category entries.
    auto* nav_card = new GlowFrame(body);
    nav_card->setObjectName(QStringLiteral("SettingsNavCard"));
    nav_card->apply_card_style(14, 3);
    nav_card->setFixedWidth(kSidebarWidth);
    sidebar_ = nav_card;
    sidebar_layout_ = new QVBoxLayout(nav_card);
    sidebar_layout_->setContentsMargins(14, 18, 14, 18);
    sidebar_layout_->setSpacing(Spacing::xs);
    {
        auto* brand = new QHBoxLayout;
        brand->setContentsMargins(Spacing::xs, 0, 0, Spacing::md);
        brand->setSpacing(10);
        auto* mark = new QLabel(nav_card);
        mark->setFixedSize(38, 38);
        mark->setAlignment(Qt::AlignCenter);
        QPixmap logo(QStringLiteral(":/brand/aurora_mark.png"));
        if (!logo.isNull()) {
            logo = logo.scaled(QSize(qRound(36 * dpr), qRound(36 * dpr)),
                               Qt::KeepAspectRatio, Qt::SmoothTransformation);
            logo.setDevicePixelRatio(dpr);
            mark->setPixmap(logo);
        }
        brand->addWidget(mark, 0, Qt::AlignVCenter);
        brand_text_ = new QLabel(QStringLiteral("AURORA"), nav_card);
        brand_text_->setObjectName(QStringLiteral("SettingsBrand"));
        brand->addWidget(brand_text_, 1, Qt::AlignVCenter);
        sidebar_layout_->addLayout(brand);
    }
    body_row->addWidget(nav_card, 0);

    // Centered placeholder shown when the filter matches nothing; the cards
    // collapse around it instead of leaving a blank area.
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
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body_row->addWidget(scroll_, 1);

    auto* container = new QWidget(scroll_);
    container->setObjectName(QStringLiteral("SettingsContainer"));
    cards_column_ = new QVBoxLayout(container);
    cards_column_->setContentsMargins(0, 0, Spacing::sm, 0);
    cards_column_->setSpacing(Spacing::md + 2);

    for (const auto& def : kCategories) {
        categories_.append(Category{QString::fromUtf8(def.id),
                                    QString::fromUtf8(def.title), def.icon,
                                    nullptr});
    }

    if (prefs_ && prefs_->is_read_only()) {
        auto* warning = new QLabel(
            QStringLiteral("This is an incognito window - changes are not saved"),
            container);
        warning->setObjectName(QStringLiteral("PageWarning"));
        warning->setWordWrap(true);
        cards_column_->addWidget(warning);
    }

    // === Startup (General) ==================================================
    // The radio group is kStartupMode ("home" | "continue" | "specific"),
    // which main.cpp's launch path reads through MainWindow::open_startup_tabs;
    // "specific" opens the page(s) kept under kStartupHomeUrl (several
    // addresses separated by commas or spaces). Picking an option persists
    // immediately like every other control on the page.
    QVBoxLayout* startup_body = add_section(
        QStringLiteral("Startup"), icons::Name::Power,
        {QStringLiteral("general")});
    const auto add_startup_row = [startup_body](QWidget* widget,
                                                const QString& terms) {
        // Radio options read as full-width rows of the card, not as
        // title/control pairs, so they are laid out directly (padded like a
        // SettingsRow) instead of through add_row(). A SettingsRow still
        // indexes the text for the filter.
        auto* row = new SettingsRow(QString(), QString(), nullptr,
                                    startup_body->parentWidget());
        row->prepend_leading(widget);
        row->add_search_text(terms);
        startup_body->addWidget(row);
        return row;
    };
    startup_open_home_ = new QRadioButton(
        QStringLiteral("Open the home page"), container);
    startup_open_home_->setObjectName(QStringLiteral("PageRadio"));
    add_startup_row(startup_open_home_,
                    QStringLiteral("startup open home page"));
    startup_continue_ = new QRadioButton(
        QStringLiteral("Continue where you left off"), container);
    startup_continue_->setObjectName(QStringLiteral("PageRadio"));
    add_startup_row(startup_continue_,
                    QStringLiteral("startup continue session restore"));
    startup_specific_ = new QRadioButton(
        QStringLiteral("Open a specific page or set of pages"), container);
    startup_specific_->setObjectName(QStringLiteral("PageRadio"));
    add_startup_row(startup_specific_,
                    QStringLiteral("startup specific page pages"));

    startup_url_ = new QLineEdit(container);
    startup_url_->setObjectName(QStringLiteral("PageInputInline"));
    startup_url_->setPlaceholderText(
        QStringLiteral("https://aurora.start — separate several pages with commas"));
    startup_url_->addAction(
        icons::icon(icons::Name::Globe, colors.text_muted, 16, dpr),
        QLineEdit::LeadingPosition);
    startup_set_page_ = new QPushButton(QStringLiteral("Set pages"),
                                        container);
    startup_set_page_->setObjectName(QStringLiteral("PageOutlineButton"));
    startup_set_page_->setCursor(Qt::PointingHandCursor);
    auto* specific_row = new QWidget(container);
    auto* specific_layout = new QHBoxLayout(specific_row);
    specific_layout->setContentsMargins(Spacing::xl + Spacing::xs, 0, 0,
                                        Spacing::sm);
    specific_layout->setSpacing(Spacing::sm);
    specific_layout->addWidget(startup_url_, 1);
    specific_layout->addWidget(startup_set_page_, 0);
    add_startup_row(specific_row,
                    QStringLiteral("startup specific page pages url set"));

    // === Default browser (General) ==========================================
    QVBoxLayout* default_body = add_section(
        QStringLiteral("Default browser"), icons::Name::Globe,
        {QStringLiteral("general")});
    default_browser_button_ =
        new QPushButton(QStringLiteral("Set as default"), container);
    default_browser_button_->setObjectName(QStringLiteral("PageOutlineButton"));
    default_browser_button_->setCursor(Qt::PointingHandCursor);
    default_browser_row_ = add_row(
        default_body, QStringLiteral("Make Aurora your default browser"),
        QStringLiteral("Links will open in Aurora instead of your default "
                       "browser."),
        default_browser_button_);
    default_browser_row_->add_search_text(
        QStringLiteral("default browser system links handler"));
    connect(default_browser_button_, &QPushButton::clicked, this,
            &SettingsPage::set_as_default_browser);

    // === Language (General, Languages) ======================================
    QVBoxLayout* language_body = add_section(
        QStringLiteral("Language"), icons::Name::Language,
        {QStringLiteral("general"), QStringLiteral("languages")});
    language_ = new QComboBox(container);
    language_->setObjectName(QStringLiteral("PageCombo"));
    language_->setMinimumWidth(280);
    language_->setCursor(Qt::PointingHandCursor);
    for (const auto& choice : kLanguages) {
        language_->addItem(QString::fromUtf8(choice.label),
                           QString::fromUtf8(choice.id));
    }
    SettingsRow* language_row = add_row(
        language_body, QStringLiteral("Preferred language"),
        QStringLiteral("Websites show this language first when they offer it, "
                       "and spell check uses its dictionary. Aurora's own "
                       "menus stay in English"),
        language_);
    for (const auto& choice : kLanguages) {
        language_row->add_search_text(QString::fromUtf8(choice.label));
    }
    language_row->add_search_text(
        QStringLiteral("languages accept-language locale"));
    add_divider(container, language_body);
    add_pref_toggle(language_body, QStringLiteral("Spell check"),
                    QStringLiteral("Underline misspelled words in text fields "
                                   "as you type"),
                    settings::PrefKeys::kSpellCheck,
                    settings::PrefDefaults::kSpellCheck)
        ->setProperty("auroraSearchExtra", QStringLiteral("spelling"));
    connect(language_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_ || !prefs_) return;
        prefs_->set_string(settings::PrefKeys::kLanguage,
                           language_->currentData().toString().toStdString());
        report_change(QStringLiteral("Preferred language set to %1")
                          .arg(language_->currentText()));
    });

    // === Performance (General, Advanced) ====================================
    QVBoxLayout* perf_body = add_section(
        QStringLiteral("Performance"), icons::Name::Gauge,
        {QStringLiteral("general"), QStringLiteral("advanced")});
    {
        QString hw_subtitle = QStringLiteral(
            "Use hardware acceleration when available. Takes effect the next "
            "time Aurora starts");
        if (!qEnvironmentVariableIsEmpty("AURORA_GPU")) {
            hw_subtitle += QStringLiteral(" (AURORA_GPU=%1 is set and decides "
                                          "for now)")
                               .arg(qEnvironmentVariable("AURORA_GPU"));
        }
        ToggleSwitch* hardware = add_pref_toggle(
            perf_body, QStringLiteral("Hardware acceleration"), hw_subtitle,
            settings::PrefKeys::kHardwareAcceleration,
            settings::PrefDefaults::kHardwareAcceleration);
        connect(hardware, &QCheckBox::toggled, this, [this](bool) {
            if (loading_ || !prefs_) return;
            report_change(QStringLiteral(
                "Restart Aurora to apply the hardware acceleration setting"));
        });
    }
    add_divider(container, perf_body);
    memory_saver_ = new ToggleSwitch(container);
    memory_saver_->setObjectName(QStringLiteral("PageCheck"));
    add_row(perf_body, QStringLiteral("Memory saver"),
            QStringLiteral("Reduces memory usage by putting inactive tabs to "
                           "sleep. Sleeping tabs reload when you return"),
            memory_saver_)
        ->add_search_text(QStringLiteral("suspend sleep inactive tabs ram"));
    add_divider(container, perf_body);
    suspend_minutes_ = new QSpinBox(container);
    suspend_minutes_->setObjectName(QStringLiteral("TabSuspendMinutes"));
    suspend_minutes_->setRange(settings::PrefDefaults::kTabSuspendMinMinutes,
                               settings::PrefDefaults::kTabSuspendMaxMinutes);
    suspend_minutes_->setSuffix(QStringLiteral(" minutes"));
    suspend_minutes_->setSpecialValueText(QStringLiteral("Off"));
    add_row(perf_body,
            QStringLiteral("Suspend inactive tabs after"),
            QStringLiteral("Suspended tabs free memory and reload when you "
                           "return to them"),
            suspend_minutes_);

    // === Tabs & browsing (General, Tabs) ====================================
    QVBoxLayout* tabs_body = add_section(
        QStringLiteral("Tabs & browsing"), icons::Name::WindowTabs,
        {QStringLiteral("general"), QStringLiteral("tabs")});
    add_pref_toggle(tabs_body, QStringLiteral("Open tabs in new window"),
                    QStringLiteral("Links that sites open in a new tab open "
                                   "in a new window instead"),
                    settings::PrefKeys::kOpenTabsInNewWindow,
                    settings::PrefDefaults::kOpenTabsInNewWindow);
    add_divider(container, tabs_body);
    add_pref_toggle(tabs_body, QStringLiteral("Show tab previews"),
                    QStringLiteral("Show a card with the page's address and "
                                   "memory use when you hover a tab"),
                    settings::PrefKeys::kTabHoverPreviews,
                    settings::PrefDefaults::kTabHoverPreviews);
    add_divider(container, tabs_body);
    restore_session_ = new ToggleSwitch(container);
    restore_session_->setObjectName(QStringLiteral("PageCheck"));
    add_row(tabs_body,
            QStringLiteral("Reopen the tabs from my last session on startup"),
            QString(), restore_session_);

    // === Appearance =========================================================
    // The mockup presents Dark / Light / Auto as preview chips rather than a
    // dropdown; the chips write the same kTheme preference the combo did
    // ("dark" / "light" / "system").
    QVBoxLayout* appearance_body = add_section(
        QStringLiteral("Appearance"), icons::Name::Palette,
        {QStringLiteral("appearance")});
    auto* chips_row = new QWidget(container);
    auto* chips_layout = new QHBoxLayout(chips_row);
    chips_layout->setContentsMargins(0, 0, 0, 0);
    chips_layout->setSpacing(Spacing::sm);
    const struct { const char* id; const char* label; } kChips[] = {
        {"dark", "Dark"}, {"light", "Light"}, {"system", "Auto"},
    };
    for (const auto& chip : kChips) {
        auto* button = new QToolButton(chips_row);
        button->setObjectName(QStringLiteral("ThemeChip"));
        button->setText(QString::fromUtf8(chip.label));
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("auroraThemeId", QString::fromUtf8(chip.id));
        chips_layout->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, id = chip.id] {
            if (loading_ || !prefs_) return;
            const QString pref = QString::fromUtf8(id);
            prefs_->set_string(settings::PrefKeys::kTheme,
                               pref.toStdString());
            emit theme_preference_changed(pref);
        });
        theme_chips_.append(button);
    }
    chips_layout->addStretch(1);
    SettingsRow* theme_row =
        add_row(appearance_body,
                QStringLiteral("Theme"),
                QStringLiteral("Choose how Aurora looks"),
                chips_row);
    for (const auto& theme_choice : kThemes) {
        theme_row->add_search_text(QString::fromUtf8(theme_choice.label));
    }
    theme_row->add_search_text(QStringLiteral("dark light auto"));
    add_divider(container, appearance_body);
    reduced_motion_ = new ToggleSwitch(container);
    reduced_motion_->setObjectName(QStringLiteral("PageCheck"));
    add_row(appearance_body,
            QStringLiteral("Reduce interface animation"),
            QStringLiteral("Turns off animated transitions across the browser, "
                           "including this page's scrolling"),
            reduced_motion_);

    // === Browsing ===========================================================
    QVBoxLayout* browsing_body = add_section(
        QStringLiteral("Browsing"), icons::Name::Globe,
        {QStringLiteral("browsing")});
    add_pref_toggle(browsing_body,
                    QStringLiteral("Night mode for web pages by default"),
                    QStringLiteral("Darkens page content (not the browser); "
                                   "switch a site with Ctrl+Alt+N"),
                    settings::PrefKeys::kNightModeDefault, false);
    add_divider(container, browsing_body);
    bookmark_bar_ = add_pref_toggle(
        browsing_body, QStringLiteral("Show bookmarks bar"),
        QStringLiteral("The bar under the address bar with your saved sites"),
        settings::PrefKeys::kShowBookmarkBar, true);
    connect(bookmark_bar_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        emit bookmark_bar_visibility_changed(on);
    });

    // === Search engine ======================================================
    QVBoxLayout* search_body = add_section(
        QStringLiteral("Search engine"), icons::Name::Search,
        {QStringLiteral("search")});
    search_body_ = search_body;
    search_engine_ = new QComboBox(container);
    search_engine_->setObjectName(QStringLiteral("PageCombo"));
    // Filled by reload_search_engines(): built-ins, then custom engines.
    SettingsRow* engine_row =
        add_row(search_body,
                QStringLiteral("Search engine"),
                QStringLiteral("Used in the omnibox; new tabs and the Home "
                               "button open its home page"),
                search_engine_);
    // Let the filter find the engine names themselves, not just the row.
    // Custom engines are findable through their own rows below.
    for (const auto& builtin : searchengines::builtin_engines()) {
        engine_row->add_search_text(qs(builtin.name));
    }
    add_divider(container, search_body);
    // Query completions from the engine while typing. Persisted by the same
    // generic loader/saver as the privacy toggles (auroraPrivacyKey).
    {
        auto* suggest = add_pref_toggle(
            search_body, QStringLiteral("Show search suggestions"),
            QStringLiteral("As you type, the address bar asks your search "
                           "engine for completions, so it sees what you type. "
                           "History, bookmark and open-tab matches stay on "
                           "this computer either way"),
            settings::PrefKeys::kSearchSuggestions,
            settings::PrefDefaults::kSearchSuggestions);
        suggest->setProperty(
            "auroraSearchExtra",
            QStringLiteral("autocomplete suggestions address bar omnibox "
                           "completions typing"));
        add_divider(container, search_body);
    }
    // Custom engines: an "Add" row whose subtitle states the template rule
    // (or why management is unavailable here), followed by one row per
    // engine, rebuilt by reload_search_engines().
    engine_add_ = new QPushButton(QStringLiteral("Add…"), container);
    engine_add_->setObjectName(QStringLiteral("PageButton"));
    QString engines_subtitle;
    if (!engines_ || !prefs_) {
        engines_subtitle = QStringLiteral(
            "Custom search engines need a profile; only the built-in engines "
            "are available in this window");
    } else if (prefs_->is_read_only()) {
        engines_subtitle = QStringLiteral(
            "Custom search engines can be used here but only managed from a "
            "normal (non-incognito) window");
    } else if (!engines_->storage_available()) {
        engines_subtitle = QStringLiteral(
            "Custom search engines could not be loaded from the profile, so "
            "they cannot be changed right now");
    } else {
        engines_subtitle = QStringLiteral(
            "Any https:// search URL with %s where the search terms go, e.g. "
            "https://example.com/search?q=%s. Its home page is the site's root");
    }
    SettingsRow* add_engine_row =
        add_row(search_body, QStringLiteral("Custom search engines"),
                engines_subtitle, engine_add_);
    add_engine_row->add_search_text(
        QStringLiteral("add search engine custom template url %s"));
    engine_add_->setEnabled(search_engines_editable());

    // === Privacy & Security =================================================
    QVBoxLayout* privacy_body = add_section(
        QStringLiteral("Privacy & Security"), icons::Name::Shield,
        {QStringLiteral("privacy")});
    adblock_enabled_ = new ToggleSwitch(container);
    adblock_enabled_->setObjectName(QStringLiteral("PageCheck"));
    add_row(privacy_body,
            QStringLiteral("Aurora Shields"),
            QStringLiteral("Block ads and trackers. Per-site exceptions are "
                           "made from the shield in the toolbar; this switch "
                           "turns the whole feature off"),
            adblock_enabled_);
    add_divider(container, privacy_body);
    block_third_party_cookies_ = new ToggleSwitch(container);
    block_third_party_cookies_->setObjectName(QStringLiteral("PageCheck"));
    add_row(privacy_body,
            QStringLiteral("Block third-party cookies"),
            QStringLiteral("Sites you visit directly can still use cookies"),
            block_third_party_cookies_);
    add_divider(container, privacy_body);
    send_dnt_ = new ToggleSwitch(container);
    send_dnt_->setObjectName(QStringLiteral("PageCheck"));
    add_row(privacy_body,
            QStringLiteral("Send a “Do Not Track” request"),
            QStringLiteral("Websites may ignore the request"),
            send_dnt_);
    add_divider(container, privacy_body);
    // Manual only: Aurora never downloads filter lists on its own (no timer,
    // no update at launch). The button stays disabled until the window
    // hands over the updater (set_filter_list_updater).
    filter_list_update_ = new QPushButton(QStringLiteral("Update now"), container);
    filter_list_update_->setObjectName(QStringLiteral("PageButton"));
    filter_list_update_->setEnabled(false);
    filter_list_row_ = add_row(
        privacy_body, QStringLiteral("Shields filter list"),
        QStringLiteral("Download the latest EasyList for Aurora Shields"),
        filter_list_update_);
    connect(filter_list_update_, &QPushButton::clicked, this, [this] {
        // One raw load of the guarded pointer (GCC cannot see through two
        // separate QPointer reads and warns -Wnull-dereference otherwise).
        if (FilterListUpdater* updater = filter_lists_.data();
            updater && !updater->busy())
            updater->start();
    });
    add_divider(container, privacy_body);
    site_settings_open_ = new QPushButton(QStringLiteral("Open"), container);
    site_settings_open_->setObjectName(QStringLiteral("PageButton"));
    add_row(privacy_body, QStringLiteral("Site settings"),
            QStringLiteral("Review and clear permissions, zoom levels and "
                           "Shields exceptions remembered for each site"),
            site_settings_open_);
    connect(site_settings_open_, &QPushButton::clicked, this,
            &SettingsPage::site_settings_requested);
    // The remaining privacy switches go through the generic loader/saver
    // (auroraPrivacyKey) so refresh() updates them without bespoke code.
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
        add_pref_toggle(privacy_body, QString::fromUtf8(row.label),
                        QString::fromUtf8(row.subtitle), row.key, row.fallback);
    }

    // === Downloads ==========================================================
    QVBoxLayout* downloads_body = add_section(
        QStringLiteral("Downloads"), icons::Name::Downloads,
        {QStringLiteral("downloads")});
    auto* downloads_location = new QWidget(container);
    auto* downloads_location_layout = new QHBoxLayout(downloads_location);
    downloads_location_layout->setContentsMargins(0, 0, 0, 0);
    downloads_location_layout->setSpacing(Spacing::sm);
    downloads_path_ = new QLineEdit(downloads_location);
    downloads_path_->setObjectName(QStringLiteral("PageInput"));
    downloads_path_->setPlaceholderText(QStringLiteral("System download folder"));
    downloads_path_->setMinimumWidth(240);
    downloads_browse_ =
        new QPushButton(QStringLiteral("Choose…"), downloads_location);
    downloads_browse_->setObjectName(QStringLiteral("PageButton"));
    downloads_location_layout->addWidget(downloads_path_, 1);
    downloads_location_layout->addWidget(downloads_browse_);
    add_row(downloads_body,
            QStringLiteral("Location"),
            QStringLiteral("Folder files are saved to"),
            downloads_location)
        ->add_search_text(QStringLiteral("download folder directory path"));
    add_divider(container, downloads_body);
    downloads_ask_where_ = new ToggleSwitch(container);
    downloads_ask_where_->setObjectName(QStringLiteral("PageCheck"));
    add_row(downloads_body,
            QStringLiteral("Ask where to save each file before downloading"),
            QString(), downloads_ask_where_);

    // === Extensions =========================================================
    QVBoxLayout* extensions_body = add_section(
        QStringLiteral("Extensions"), icons::Name::Puzzle,
        {QStringLiteral("extensions")});
    extensions_open_ = new QPushButton(QStringLiteral("Open"), container);
    extensions_open_->setObjectName(QStringLiteral("PageButton"));
    extensions_open_->setCursor(Qt::PointingHandCursor);
    add_row(extensions_body, QStringLiteral("Manage extensions"),
            QStringLiteral("Add, enable, disable or remove extensions on "
                           "aurora://extensions. Changes to the set apply the "
                           "next time Aurora starts"),
            extensions_open_)
        ->add_search_text(QStringLiteral("extensions add-ons plugins unpacked"));
    connect(extensions_open_, &QPushButton::clicked, this,
            &SettingsPage::extensions_requested);

    // === About Aurora (Advanced) ============================================
    // No preference controls here: version info plus the keyboard shortcuts
    // (read-only, so it stays accurate and useful in incognito windows).
    QVBoxLayout* about_body = add_section(
        QStringLiteral("About Aurora"), icons::Name::Info,
        {QStringLiteral("advanced")});
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
        cards_column_->insertWidget(0, warning);
        for (QWidget* w : {static_cast<QWidget*>(search_engine_),
                           static_cast<QWidget*>(startup_open_home_),
                           static_cast<QWidget*>(startup_continue_),
                           static_cast<QWidget*>(startup_specific_),
                           static_cast<QWidget*>(startup_url_),
                           static_cast<QWidget*>(startup_set_page_),
                           static_cast<QWidget*>(restore_session_),
                           static_cast<QWidget*>(suspend_minutes_),
                           static_cast<QWidget*>(memory_saver_),
                           static_cast<QWidget*>(language_),
                           static_cast<QWidget*>(downloads_path_),
                           static_cast<QWidget*>(downloads_browse_),
                           static_cast<QWidget*>(downloads_ask_where_),
                           static_cast<QWidget*>(block_third_party_cookies_),
                           static_cast<QWidget*>(send_dnt_),
                           static_cast<QWidget*>(adblock_enabled_),
                           static_cast<QWidget*>(filter_list_update_),
                           static_cast<QWidget*>(reduced_motion_)}) {
            w->setEnabled(false);
        }
    }

    cards_column_->addStretch(1);
    scroll_->setWidget(container);
    set_body(body);

    // Sidebar entries, one per category, now that every card exists.
    for (int i = 0; i < categories_.size(); ++i) {
        Category& category = categories_[i];
        auto* button = new QToolButton(sidebar_);
        button->setObjectName(QStringLiteral("SettingsNavButton"));
        // QToolButton treats '&' as a mnemonic marker, which turned
        // "Privacy & Security" into "Privacy _Security". Escape it, and give
        // the glyph a little air: QToolButton has no icon/text spacing
        // property, so the text carries a leading gap.
        QString nav_text = category.title;
        nav_text.replace(QLatin1Char('&'), QStringLiteral("&&"));
        button->setText(QStringLiteral("  ") + nav_text);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(kSidebarIconPx, kSidebarIconPx));
        button->setToolTip(category.title);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        category.button = button;
        sidebar_layout_->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, i] {
            // A category click leaves search mode: the field clears and the
            // category's own cards come back.
            if (!filter_->text().isEmpty()) filter_->clear();
            select_category(i, /*scroll_to=*/true);
        });
    }
    sidebar_layout_->addStretch(1);
    // Sidebar and card icons are stamped from the live palette, so re-render
    // them on every theme change — MainWindow re-publishes current_theme()
    // before notifying observers (same mechanism TabSpinner/PipWindow use).
    theme_observer_ = add_theme_observer(
        [this](const Theme&) { apply_sidebar_icons(); });
    select_category(0, /*scroll_to=*/false);

    // Persist on change. `loading_` suppresses writes while refresh() is
    // populating the controls.
    connect(engine_add_, &QPushButton::clicked, this,
            [this] { edit_search_engine(0); });
    connect(search_engine_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (loading_ || !prefs_) return;
        const auto id = search_engine_->currentData().toString();
        prefs_->set_string(settings::PrefKeys::kSearchEngine, id.toStdString());
        emit search_engine_changed(id);
        report_change(QStringLiteral("Search engine set to %1")
                                .arg(search_engine_->currentText()));
    });
    // Startup radio group. kStartupMode is what the launch path reads (see
    // settings::resolve_startup_mode); kRestoreSession is kept in step so the
    // "Reopen the tabs" switch below — and an older build sharing the
    // profile — agree with the radio choice.
    connect(startup_open_home_, &QRadioButton::toggled, this, [this](bool on) {
        if (loading_ || !prefs_ || !on) return;
        prefs_->set_bool(settings::PrefKeys::kRestoreSession, false);
        store_startup_mode(*prefs_, settings::StartupMode::Home);
        sync_startup_controls();
        report_change(QStringLiteral("Startup will open the home page"));
    });
    connect(startup_continue_, &QRadioButton::toggled, this, [this](bool on) {
        if (loading_ || !prefs_ || !on) return;
        prefs_->set_bool(settings::PrefKeys::kRestoreSession, true);
        store_startup_mode(*prefs_, settings::StartupMode::Continue);
        sync_startup_controls();
        report_change(QStringLiteral("Session will be restored on startup"));
    });
    connect(startup_specific_, &QRadioButton::toggled, this, [this](bool on) {
        if (loading_ || !prefs_ || !on) return;
        // A specific page replaces the session, so the legacy boolean goes
        // off too: an older build reading this profile then opens its home
        // page rather than restoring tabs the user opted out of.
        prefs_->set_bool(settings::PrefKeys::kRestoreSession, false);
        store_startup_mode(*prefs_, settings::StartupMode::Specific);
        sync_startup_controls();
        if (startup_url_) startup_url_->setFocus(Qt::ShortcutFocusReason);
        report_change(startup_url_ && startup_url_->text().trimmed().isEmpty()
            ? QStringLiteral("Enter the page(s) to open on startup")
            : QStringLiteral("Startup opens a specific page or set of pages"));
    });
    const auto save_startup_url = [this] {
        if (loading_ || !prefs_) return;
        const QString url = startup_url_->text().trimmed();
        prefs_->set_string(settings::PrefKeys::kStartupHomeUrl,
                           url.toStdString());
        // Typing a page is a clear statement of intent: select the option
        // instead of silently saving a URL nothing will open.
        if (!url.isEmpty() && !startup_specific_->isChecked()) {
            startup_specific_->setChecked(true);  // persists via toggled()
        }
        report_change(url.isEmpty() ? QStringLiteral("Startup pages cleared")
                                    : QStringLiteral("Startup pages saved"));
    };
    connect(startup_set_page_, &QPushButton::clicked, this, save_startup_url);
    connect(startup_url_, &QLineEdit::editingFinished, this,
            save_startup_url);
    connect(restore_session_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        prefs_->set_bool(settings::PrefKeys::kRestoreSession, on);
        // Mirror the switch into the Startup card. Turning it off only
        // leaves "continue" (to home); a specific page stays selected.
        const std::string mode = prefs_->get_string(
            settings::PrefKeys::kStartupMode, "");
        if (on) {
            store_startup_mode(*prefs_, settings::StartupMode::Continue);
        } else if (mode.empty() ||
                   mode == settings::startup_mode_name(
                               settings::StartupMode::Continue)) {
            store_startup_mode(*prefs_, settings::StartupMode::Home);
        }
        sync_startup_controls();
        report_change(on ? QStringLiteral("Session will be restored on "
                                               "startup")
                               : QStringLiteral("Startup will open a new tab"));
    });
    connect(suspend_minutes_, &QSpinBox::valueChanged, this, [this](int minutes) {
        if (loading_ || !prefs_) return;
        prefs_->set_int(settings::PrefKeys::kTabSuspendAfterMinutes, minutes);
        if (minutes > 0) last_suspend_minutes_ = minutes;
        sync_memory_saver();
        report_change(minutes
            ? QStringLiteral("Inactive tabs will suspend after %1 minutes")
                  .arg(minutes)
            : QStringLiteral("Automatic tab suspension turned off"));
    });
    // Memory saver is the on/off face of the same timer: on restores the
    // last delay (30 minutes by default), off stores 0.
    connect(memory_saver_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_ || !prefs_) return;
        const int minutes = on
            ? (last_suspend_minutes_ > 0
                   ? last_suspend_minutes_
                   : settings::PrefDefaults::kTabSuspendAfterMinutes)
            : 0;
        prefs_->set_int(settings::PrefKeys::kTabSuspendAfterMinutes, minutes);
        const bool was_loading = loading_;
        loading_ = true;
        suspend_minutes_->setValue(minutes);
        loading_ = was_loading;
        report_change(on ? QStringLiteral("Memory saver on: inactive tabs sleep "
                                          "after %1 minutes").arg(minutes)
                         : QStringLiteral("Memory saver off"));
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

QVBoxLayout* SettingsPage::add_section(const QString& title, icons::Name icon,
                                       const QStringList& categories) {
    // A glass card with the History panel's neon rim. The header row (icon,
    // title, chevron) collapses the body; GlowFrame paints the plate, so the
    // card and its children carry no stylesheet background.
    auto* card = new GlowFrame(scroll_);
    card->setObjectName(QStringLiteral("SettingsGlowCard"));
    card->apply_card_style(14, 3);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(Spacing::sm + 2, Spacing::sm,
                                    Spacing::sm + 2, Spacing::sm);
    card_layout->setSpacing(0);

    auto* header = new CardHeader(card);
    auto* header_row = new QHBoxLayout(header);
    header_row->setContentsMargins(Spacing::sm, Spacing::xs, Spacing::xs,
                                   Spacing::xs);
    header_row->setSpacing(Spacing::md);
    auto* icon_label = new QLabel(header);
    icon_label->setObjectName(QStringLiteral("SettingsSectionIcon"));
    icon_label->setFixedSize(kCardIconPx + 2, kCardIconPx + 2);
    icon_label->setAlignment(Qt::AlignCenter);
    header_row->addWidget(icon_label, 0, Qt::AlignVCenter);
    auto* label = new QLabel(title, header);
    label->setObjectName(QStringLiteral("SettingsSectionHeader"));
    header_row->addWidget(label, 1, Qt::AlignVCenter);
    auto* chevron = new QToolButton(header);
    chevron->setObjectName(QStringLiteral("SettingsCardChevron"));
    chevron->setAutoRaise(true);
    chevron->setFixedSize(26, 26);
    chevron->setIconSize(QSize(14, 14));
    chevron->setCursor(Qt::PointingHandCursor);
    chevron->setToolTip(QStringLiteral("Collapse"));
    header_row->addWidget(chevron, 0, Qt::AlignVCenter);
    card_layout->addWidget(header);

    auto* body_host = new QWidget(card);
    body_host->setObjectName(QStringLiteral("SettingsCardBody"));
    auto* card_body = new QVBoxLayout(body_host);
    card_body->setContentsMargins(0, 0, 0, Spacing::xs);
    card_body->setSpacing(0);
    card_layout->addWidget(body_host);

    cards_column_->addWidget(card);
    const int index = static_cast<int>(sections_.size());
    sections_.append(Section{card, card, card_body, body_host, chevron,
                             icon_label, title, icon, categories, false});
    header->on_click = [this, index] {
        set_collapsed(sections_[index], !sections_[index].collapsed);
    };
    connect(chevron, &QToolButton::clicked, this, [this, index] {
        set_collapsed(sections_[index], !sections_[index].collapsed);
    });
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

ToggleSwitch* SettingsPage::add_pref_toggle(QVBoxLayout* body,
                                            const QString& title,
                                            const QString& subtitle,
                                            std::string_view key,
                                            bool fallback) {
    auto* box = new ToggleSwitch(body->parentWidget());
    box->setObjectName(QStringLiteral("PageCheck"));
    box->setProperty("auroraPrivacyKey",
                     QString::fromStdString(std::string(key)));
    box->setProperty("auroraPrivacyDefault", fallback);
    box->setEnabled(prefs_ != nullptr);
    box->setChecked(prefs_ ? prefs_->get_bool(key, fallback) : fallback);
    add_row(body, title, subtitle, box);
    connect(box, &QCheckBox::toggled, this,
            [this, box, pref_key = std::string(key)](bool on) {
        if (loading_ || !prefs_ || !box->isEnabled()) return;
        prefs_->set_bool(pref_key, on);
    });
    return box;
}

void SettingsPage::set_collapsed(Section& section, bool collapsed) {
    section.collapsed = collapsed;
    const bool filtering = filter_ && !filter_->text().trimmed().isEmpty();
    if (section.body_host) section.body_host->setVisible(filtering || !collapsed);
    if (section.chevron) {
        section.chevron->setToolTip(collapsed ? QStringLiteral("Expand")
                                              : QStringLiteral("Collapse"));
    }
    apply_sidebar_icons();
}

void SettingsPage::apply_filter() {
    const QString needle = filter_->text().trimmed().toLower();
    const bool filtering = !needle.isEmpty();
    const QString active_id =
        active_category_ >= 0 && active_category_ < categories_.size()
            ? categories_[active_category_].id
            : QString();

    bool any_visible = false;
    for (Section& section : sections_) {
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
                // Rows built by add_pref_toggle can carry extra keywords on
                // their switch (auroraSearchExtra).
                if (!match) {
                    for (auto* box : row->findChildren<QCheckBox*>()) {
                        const QString extra =
                            box->property("auroraSearchExtra").toString();
                        if (!extra.isEmpty() &&
                            extra.toLower().contains(needle)) {
                            match = true;
                        }
                    }
                }
            }
            widget->setVisible(match);
            if (match && row) ++visible_rows;
        }

        // Searching looks across every category; otherwise the active
        // category decides which cards are on the page.
        const bool section_visible = filtering
            ? visible_rows > 0
            : section.categories.contains(active_id);
        section.wrapper->setVisible(section_visible);
        // Matches inside a collapsed card are shown while searching.
        if (section.body_host) {
            section.body_host->setVisible(filtering || !section.collapsed);
        }
        if (section_visible) any_visible = true;
    }

    // Nothing matched: show the centered empty-state text and collapse the
    // card column instead of leaving a blank scroll area.
    set_empty_state(false);
    scroll_->setVisible(!filtering || any_visible);
    no_results_->setVisible(filtering && !any_visible);
}

void SettingsPage::select_category(int index, bool scroll_to) {
    if (index < 0 || index >= categories_.size()) return;
    active_category_ = index;
    const QColor glow_color = current_theme().colors.accent2;
    for (int i = 0; i < categories_.size(); ++i) {
        QToolButton* button = categories_[i].button;
        if (!button) continue;
        if (i == index) {
            if (!button->isChecked()) button->setChecked(true);
            // The active entry glows like the mockup's: a soft purple halo
            // around its outlined pill (the outline itself is the sheet's).
            auto* glow = new QGraphicsDropShadowEffect(button);
            QColor halo = glow_color;
            halo.setAlpha(150);
            glow->setColor(halo);
            glow->setBlurRadius(22.0);
            glow->setOffset(0.0, 0.0);
            button->setGraphicsEffect(glow);
        } else {
            button->setGraphicsEffect(nullptr);
        }
    }
    // Checked state drives the icon tint: restamp so the active entry uses
    // the accent color and the previous one falls back to muted.
    apply_sidebar_icons();
    apply_filter();
    if (!scroll_to || !scroll_) return;
    // The category's first card is the top of its page.
    if (cards_column_) cards_column_->activate();
    for (const Section& section : sections_) {
        if (section.wrapper->isVisibleTo(scroll_->widget())) {
            scroll_to_section(section);
            return;
        }
    }
}

void SettingsPage::show_category(const QString& id) {
    for (int i = 0; i < categories_.size(); ++i) {
        if (categories_[i].id != id) continue;
        if (filter_ && !filter_->text().isEmpty()) filter_->clear();
        select_category(i, /*scroll_to=*/true);
        return;
    }
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
    if (sidebar_) {
        sidebar_->setFixedWidth(compact ? kSidebarCompactWidth : kSidebarWidth);
    }
    if (brand_text_) brand_text_->setVisible(!compact);
    for (Category& category : categories_) {
        if (!category.button) continue;
        category.button->setToolButtonStyle(
            compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
}

void SettingsPage::apply_sidebar_icons() {
    // QIcon does not follow palette changes, so the glyphs are stamped from
    // the live theme: nav entries muted at rest and accent when active, card
    // header glyphs in the accent, chevrons muted. The stylesheet supplies
    // the fills, outlines and hover washes.
    const Theme& theme = current_theme();
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    for (const Category& category : categories_) {
        if (!category.button) continue;
        const QColor color = category.button->isChecked()
            ? theme.colors.accent
            : theme.colors.text_muted;
        category.button->setIcon(
            icons::icon(category.icon, color, kSidebarIconPx, dpr));
    }
    for (const Section& section : sections_) {
        if (section.icon_label) {
            section.icon_label->setPixmap(icons::pixmap(
                section.icon, kCardIconPx, theme.colors.accent, dpr));
        }
        if (section.chevron) {
            section.chevron->setIcon(icons::icon(
                section.collapsed ? icons::Name::ChevronDown
                                  : icons::Name::ChevronUp,
                theme.colors.text_muted, 14, dpr));
        }
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

bool SettingsPage::search_engines_editable() const {
    return engines_ && prefs_ && !prefs_->is_read_only() &&
           engines_->storage_available();
}

void SettingsPage::reload_search_engines() {
    const bool was_loading = loading_;
    loading_ = true;  // rebuilding the combo must not write the preference

    const auto custom = engines_ ? engines_->snapshot()
                                 : std::vector<searchengines::SearchEngine>{};
    const bool editable = search_engines_editable();

    search_engine_->clear();
    for (const auto& builtin : searchengines::builtin_engines()) {
        search_engine_->addItem(qs(builtin.name), qs(builtin.key));
    }
    for (const auto& engine : custom) {
        search_engine_->addItem(
            QString::fromStdString(engine.name),
            QString::fromStdString(searchengines::custom_engine_key(engine.id)));
    }

    // Rebuild the per-engine rows at the end of the Search card. deleteLater
    // (not delete): a reload can run while one of these rows is on the stack.
    for (QWidget* widget : engine_rows_) {
        search_body_->removeWidget(widget);
        widget->hide();
        widget->deleteLater();
    }
    engine_rows_.clear();
    QWidget* card = search_body_->parentWidget();
    const auto add_engine_divider = [&] {
        add_divider(card, search_body_);
        engine_rows_.append(
            search_body_->itemAt(search_body_->count() - 1)->widget());
    };
    for (const auto& engine : custom) {
        const auto id = engine.id;
        const QString name = QString::fromStdString(engine.name);
        auto* controls = new QWidget(card);
        auto* controls_layout = new QHBoxLayout(controls);
        controls_layout->setContentsMargins(0, 0, 0, 0);
        controls_layout->setSpacing(Spacing::sm);
        auto* edit = new QPushButton(QStringLiteral("Edit…"), controls);
        edit->setObjectName(QStringLiteral("PageButton"));
        edit->setEnabled(editable);
        auto* remove = new QPushButton(QStringLiteral("Delete"), controls);
        remove->setObjectName(QStringLiteral("PageDangerButton"));
        remove->setEnabled(editable);
        controls_layout->addWidget(edit);
        controls_layout->addWidget(remove);
        // Queued: the dialog's nested event loop must not run inside the
        // clicked() emission of a button that a reload may delete.
        connect(edit, &QPushButton::clicked, this, [this, id] {
            QMetaObject::invokeMethod(
                this, [this, id] { edit_search_engine(id); },
                Qt::QueuedConnection);
        });
        connect(remove, &QPushButton::clicked, this, [this, id, name] {
            QMetaObject::invokeMethod(
                this, [this, id, name] { delete_search_engine(id, name); },
                Qt::QueuedConnection);
        });

        add_engine_divider();
        SettingsRow* row = add_row(search_body_, name,
                                   QString::fromStdString(engine.url_template),
                                   controls);
        row->add_search_text(QStringLiteral("custom search engine"));
        engine_rows_.append(row);
    }
    if (custom.empty()) {
        add_engine_divider();
        SettingsRow* row = add_row(
            search_body_, QStringLiteral("No custom search engines yet"),
            QString(), nullptr);
        row->add_search_text(QStringLiteral("custom search engine"));
        engine_rows_.append(row);
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

    engine_add_->setEnabled(editable);
    loading_ = was_loading;
    apply_filter();
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
    name->setObjectName(QStringLiteral("PageInput"));
    name->setPlaceholderText(QStringLiteral("e.g. Kagi"));
    name->setMaxLength(static_cast<int>(searchengines::kMaxNameLength));
    auto* url = new QLineEdit(QString::fromStdString(current.url_template), &dialog);
    url->setObjectName(QStringLiteral("PageInput"));
    url->setPlaceholderText(QStringLiteral("https://example.com/search?q=%s"));
    url->setMinimumWidth(360);
    auto* error = new QLabel(&dialog);
    error->setObjectName(QStringLiteral("PageWarning"));
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

void SettingsPage::delete_search_engine(int64_t id, const QString& name) {
    if (!search_engines_editable() || id == 0) return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Delete search engine"),
        QStringLiteral("Delete the search engine \"%1\"?").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes || !search_engines_editable()) return;

    QPointer<SettingsPage> self(this);
    const QString success = QStringLiteral("Deleted search engine %1").arg(name);
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

void SettingsPage::set_filter_list_updater(FilterListUpdater* updater) {
    if (filter_lists_) disconnect(filter_lists_, nullptr, this, nullptr);
    filter_lists_ = updater;
    if (filter_lists_) {
        connect(filter_lists_, &FilterListUpdater::busy_changed, this,
                [this](bool) { sync_filter_list_row(); });
        connect(filter_lists_, &FilterListUpdater::finished, this,
                [this](bool ok, const QString& message) {
                    // A failure stays visible on the row (the previous list
                    // is still in use); success shows the new timestamp.
                    sync_filter_list_row(ok ? QString() : message);
                });
    }
    sync_filter_list_row();
}

void SettingsPage::sync_filter_list_row(const QString& status) {
    if (!filter_list_row_) return;
    const bool writable = prefs_ && !prefs_->is_read_only();
    // One snapshot of the QPointer: every `filter_lists_->` re-reads it, which
    // is what GCC's -Wnull-dereference flagged after the null check.
    FilterListUpdater* const updater = filter_lists_.data();
    if (!updater) {
        filter_list_update_->setEnabled(false);
        filter_list_row_->set_subtitle(
            writable ? QStringLiteral("Filter list updates are unavailable "
                                      "without a profile")
                     : QStringLiteral("Filter lists can only be updated from "
                                      "a normal window"));
        return;
    }
    const bool busy = updater->busy();
    filter_list_update_->setEnabled(writable && !busy);
    filter_list_update_->setText(busy ? QStringLiteral("Updating…")
                                      : QStringLiteral("Update now"));
    if (busy) {
        filter_list_row_->set_subtitle(
            QStringLiteral("Downloading %1…").arg(updater->source_url()));
        return;
    }
    const adblock::UpdateRecord record = updater->last_update();
    QString line;
    if (record.last_success_unix > 0) {
        const QString when = QLocale().toString(
            QDateTime::fromSecsSinceEpoch(record.last_success_unix),
            QLocale::ShortFormat);
        line = QStringLiteral("Last updated %1 from %2")
                   .arg(when, QString::fromStdString(record.source_url));
    } else {
        line = QStringLiteral("Not updated from Aurora yet. Source: %1")
                   .arg(updater->source_url());
    }
    if (!status.isEmpty()) line = status + QStringLiteral(". ") + line;
    filter_list_row_->set_subtitle(line);
}

void SettingsPage::sync_startup_controls() {
    if (!prefs_) return;
    // The stored mode as the user picked it — NOT resolve_startup_mode(),
    // which degrades "specific" without a URL to Home for the launch path;
    // the card must keep showing the half-finished choice so it can be
    // completed. No stored mode: the legacy boolean decides, like at launch.
    settings::StartupMode mode = settings::StartupMode::Home;
    if (!settings::parse_startup_mode(
            prefs_->get_string(settings::PrefKeys::kStartupMode, ""), mode)) {
        mode = prefs_->get_bool(settings::PrefKeys::kRestoreSession,
                                settings::PrefDefaults::kRestoreSession)
                   ? settings::StartupMode::Continue
                   : settings::StartupMode::Home;
    }
    const bool was_loading = loading_;
    loading_ = true;
    startup_open_home_->setChecked(mode == settings::StartupMode::Home);
    startup_continue_->setChecked(mode == settings::StartupMode::Continue);
    startup_specific_->setChecked(mode == settings::StartupMode::Specific);
    restore_session_->setChecked(mode == settings::StartupMode::Continue);
    loading_ = was_loading;
}

void SettingsPage::sync_memory_saver() {
    if (!memory_saver_ || !suspend_minutes_) return;
    const bool was_loading = loading_;
    loading_ = true;
    memory_saver_->setChecked(suspend_minutes_->value() > 0);
    loading_ = was_loading;
}

// --- Default browser ---------------------------------------------------------
//
// No desktop OS lets an application silently take over the web-browser
// default any more, so "Set as default" does the part an app may do and hands
// the final choice to the system:
//   * Windows: registers Aurora per user (HKCU, no admin rights) as a
//     StartMenuInternet client with http/https/.html capabilities and an
//     "AuroraHTML" ProgID, then opens Settings > Default apps on Aurora's
//     entry, where the user confirms;
//   * Linux: runs `xdg-settings set default-web-browser aurora.desktop`
//     (the entry the packages install);
//   * macOS: opens System Settings > Desktop & Dock, whose "Default web
//     browser" menu lists Aurora once the app bundle has been launched.
// The status line is best effort: Windows reads the https UserChoice ProgID,
// Linux asks xdg-settings; macOS has no API here without extra frameworks.

void SettingsPage::set_as_default_browser() {
#if defined(Q_OS_WIN)
    const QDir app_dir(QCoreApplication::applicationDirPath());
    // Packaged builds start through the bootstrap launcher, which resolves
    // the engine's resources and forwards the URL to the browser.
    QString exe = app_dir.filePath(QStringLiteral("aurora_bootstrap.exe"));
    if (!QFileInfo::exists(exe)) exe = QCoreApplication::applicationFilePath();
    exe = QDir::toNativeSeparators(exe);
    const QString quoted = QLatin1Char('"') + exe + QLatin1Char('"');
    const QString open_command = quoted + QStringLiteral(" \"%1\"");
    const QString icon = exe + QStringLiteral(",0");
    const QString prog_id = QStringLiteral("AuroraHTML");

    QSettings classes(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\") +
                          prog_id,
                      QSettings::NativeFormat);
    classes.setValue(QStringLiteral("Default"),
                     QStringLiteral("Aurora HTML Document"));
    classes.setValue(QStringLiteral("DefaultIcon/Default"), icon);
    classes.setValue(QStringLiteral("shell/open/command/Default"), open_command);
    classes.sync();

    QSettings client(QStringLiteral("HKEY_CURRENT_USER\\Software\\Clients\\"
                                    "StartMenuInternet\\Aurora"),
                     QSettings::NativeFormat);
    client.setValue(QStringLiteral("Default"), QStringLiteral("Aurora"));
    client.setValue(QStringLiteral("DefaultIcon/Default"), icon);
    client.setValue(QStringLiteral("shell/open/command/Default"), quoted);
    client.setValue(QStringLiteral("Capabilities/ApplicationName"),
                    QStringLiteral("Aurora"));
    client.setValue(QStringLiteral("Capabilities/ApplicationDescription"),
                    QStringLiteral("Aurora web browser"));
    client.setValue(QStringLiteral("Capabilities/ApplicationIcon"), icon);
    for (const char* scheme : {"http", "https"}) {
        client.setValue(QStringLiteral("Capabilities/URLAssociations/") +
                            QLatin1String(scheme),
                        prog_id);
    }
    for (const char* ext : {".htm", ".html", ".xhtml"}) {
        client.setValue(QStringLiteral("Capabilities/FileAssociations/") +
                            QLatin1String(ext),
                        prog_id);
    }
    client.sync();

    QSettings registered(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\RegisteredApplications"),
        QSettings::NativeFormat);
    registered.setValue(
        QStringLiteral("Aurora"),
        QStringLiteral("Software\\Clients\\StartMenuInternet\\Aurora\\Capabilities"));
    registered.sync();

    if (classes.status() != QSettings::NoError ||
        client.status() != QSettings::NoError ||
        registered.status() != QSettings::NoError) {
        default_browser_row_->set_subtitle(QStringLiteral(
            "Aurora could not register itself as a browser for this user"));
        return;
    }
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("ms-settings:defaultapps?registeredAppUser=Aurora")));
    default_browser_row_->set_subtitle(QStringLiteral(
        "Aurora is registered. In Windows Settings > Default apps, choose "
        "Aurora for web links (http and https)."));
    report_change(QStringLiteral("Choose Aurora in Windows Settings to finish"));
#elif defined(Q_OS_MACOS)
    QDesktopServices::openUrl(QUrl(QStringLiteral(
        "x-apple.systempreferences:com.apple.Desktop-Settings.extension")));
    default_browser_row_->set_subtitle(QStringLiteral(
        "In System Settings > Desktop & Dock, pick Aurora as the default web "
        "browser."));
#else
    if (default_browser_probe_) return;  // a set/check is still running
    auto* process = new QProcess(this);
    default_browser_probe_ = process;
    connect(process, &QProcess::finished, this,
            [this, process](int code, QProcess::ExitStatus status) {
                const QString error =
                    QString::fromLocal8Bit(process->readAllStandardError())
                        .trimmed();
                default_browser_probe_ = nullptr;
                process->deleteLater();
                if (status == QProcess::NormalExit && code == 0) {
                    report_change(
                        QStringLiteral("Aurora is now your default browser"));
                    QTimer::singleShot(0, this,
                                       [this] { check_default_browser(); });
                    return;
                }
                default_browser_row_->set_subtitle(
                    error.isEmpty()
                        ? QStringLiteral("xdg-settings could not make Aurora "
                                         "the default (is aurora.desktop "
                                         "installed?)")
                        : QStringLiteral("xdg-settings: %1").arg(error));
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) return;
                default_browser_probe_ = nullptr;
                process->deleteLater();
                default_browser_row_->set_subtitle(QStringLiteral(
                    "xdg-settings is not installed, so Aurora cannot change "
                    "the default browser here. Use your desktop's settings."));
            });
    process->start(QStringLiteral("xdg-settings"),
                   {QStringLiteral("set"), QStringLiteral("default-web-browser"),
                    QStringLiteral("aurora.desktop")});
#endif
}

void SettingsPage::check_default_browser() {
    if (!default_browser_row_ || !default_browser_button_) return;
    const auto show_state = [this](bool is_default) {
        default_browser_button_->setEnabled(!is_default);
        default_browser_button_->setText(is_default
                                             ? QStringLiteral("Default")
                                             : QStringLiteral("Set as default"));
        default_browser_row_->set_subtitle(
            is_default ? QStringLiteral("Aurora is your default browser.")
                       : QStringLiteral("Links will open in Aurora instead of "
                                        "your default browser."));
    };
#if defined(Q_OS_WIN)
    const QSettings choice(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\"
                       "Associations\\UrlAssociations\\https\\UserChoice"),
        QSettings::NativeFormat);
    show_state(choice.value(QStringLiteral("ProgId")).toString() ==
         QStringLiteral("AuroraHTML"));
#elif defined(Q_OS_MACOS)
    Q_UNUSED(show_state);
#else
    if (default_browser_probe_) return;
    auto* process = new QProcess(this);
    default_browser_probe_ = process;
    connect(process, &QProcess::finished, this,
            [this, process, show_state](int code, QProcess::ExitStatus status) {
                const QString out =
                    QString::fromLocal8Bit(process->readAllStandardOutput())
                        .trimmed();
                default_browser_probe_ = nullptr;
                process->deleteLater();
                if (status == QProcess::NormalExit && code == 0) {
                    show_state(out.compare(QStringLiteral("yes"),
                                     Qt::CaseInsensitive) == 0);
                }
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) return;
                default_browser_probe_ = nullptr;
                process->deleteLater();
            });
    process->start(QStringLiteral("xdg-settings"),
                   {QStringLiteral("check"),
                    QStringLiteral("default-web-browser"),
                    QStringLiteral("aurora.desktop")});
#endif
}

void SettingsPage::refresh() {
    check_default_browser();
    if (!prefs_) {
        // No profile: still offer the built-in engines (disabled).
        reload_search_engines();
        return;
    }
    loading_ = true;

    // Fills the engine picker and selects the stored kSearchEngine value.
    reload_search_engines();
    // Theme chips: check whichever of Dark / Light / Auto matches the
    // stored preference (system maps to Auto).
    const QString theme_pref = QString::fromStdString(prefs_->get_string(
        settings::PrefKeys::kTheme, settings::PrefDefaults::kTheme));
    for (QToolButton* chip : theme_chips_) {
        chip->setChecked(chip->property("auroraThemeId").toString() ==
                         theme_pref);
    }
    sync_startup_controls();
    startup_url_->setText(QString::fromStdString(
        prefs_->get_string(settings::PrefKeys::kStartupHomeUrl, "")));
    const int minutes = static_cast<int>(prefs_->get_int(
        settings::PrefKeys::kTabSuspendAfterMinutes,
        settings::PrefDefaults::kTabSuspendAfterMinutes));
    suspend_minutes_->setValue(minutes);
    if (minutes > 0) last_suspend_minutes_ = minutes;
    sync_memory_saver();
    select_id(language_, QString::fromStdString(prefs_->get_string(
                             settings::PrefKeys::kLanguage,
                             settings::PrefDefaults::kLanguage)));
    downloads_path_->setText(
        QString::fromStdString(prefs_->downloads_path()));
    downloads_ask_where_->setChecked(prefs_->downloads_ask_where());
    block_third_party_cookies_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kBlockThirdPartyCookies,
                         settings::PrefDefaults::kBlockThirdPartyCookies));
    send_dnt_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kSendDoNotTrack,
                         settings::PrefDefaults::kSendDoNotTrack));
    adblock_enabled_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kAdblockEnabled, true));
    reduced_motion_->setChecked(
        prefs_->get_bool(settings::PrefKeys::kReducedMotion, false));

    // Every switch built by add_pref_toggle (privacy, language, performance,
    // tabs, browsing, search suggestions).
    for (auto* box : findChildren<QCheckBox*>()) {
        const auto key = box->property("auroraPrivacyKey");
        if (!key.isValid()) continue;
        box->setChecked(prefs_->get_bool(
            key.toString().toStdString(),
            box->property("auroraPrivacyDefault").toBool()));
    }

    sync_filter_list_row();

    loading_ = false;
    apply_filter();
}

} // namespace aurora::ui

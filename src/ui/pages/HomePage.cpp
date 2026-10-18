#include "ui/pages/HomePage.hpp"

#include <QContextMenuEvent>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QRadialGradient>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

#include <QSet>
#include <QUrl>

#include "history/HistoryService.hpp"
#include "settings/Settings.hpp"
#include "ui/FaviconCache.hpp"
#include "ui/GlowFrame.hpp"
#include "ui/Icons.hpp"
#include "ui/PageBackground.hpp"
#include "ui/ShortcutDialog.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"
#include "ui/WeatherService.hpp"

namespace aurora::ui {

namespace {

// The Aurora logo (emblem + wordmark) from the brand artwork, scaled to fit
// and drawn crisply at any device pixel ratio. The artwork is a transparent
// PNG, so it sits directly on the cosmic backdrop.
class BrandLabel : public QWidget {
public:
    explicit BrandLabel(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TranslucentBackground, true);
        logo_.load(QStringLiteral(":/brand/aurora_logo_full.png"));
        set_width(400);
    }

    void set_width(int width) {
        const qreal aspect = logo_.isNull()
            ? 0.78 : static_cast<qreal>(logo_.height()) / logo_.width();
        setFixedSize(width, qRound(width * aspect));
    }

    void paintEvent(QPaintEvent*) override {
        if (logo_.isNull()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawPixmap(rect(), logo_);
    }

private:
    QPixmap logo_;
};

// Reuse History's actual painted frame, including its outer aura, rather
// than approximating a glow with a flat QSS border. Only homepage instances
// get these sizes and theme-aware fills; History itself is unchanged.
class HomeGlass : public GlowFrame {
public:
    enum class Kind { Card, Shortcut, Search };

    HomeGlass(QWidget* parent, Kind kind) : GlowFrame(parent), kind_(kind) {
        apply_theme();
    }

    void apply_theme() {
        const Theme& theme = current_theme();
        const auto& c = theme.colors;
        apply_card_style(18, 6);
        Edge rim = edge();
        QColor plate = fill();
        if (!theme.is_dark) {
            plate = c.card_bg;
            plate.setAlpha(242);
            rim.color1 = c.accent;
            rim.color2 = c.accent2;
            rim.glow = c.accent2;
            rim.glow.setAlpha(85);
        }
        int radius = 18;
        if (kind_ == Kind::Search) {
            rim.color1 = c.accent;
            rim.color2 = c.accent2;
            rim.glow = c.accent2;
            rim.glow.setAlpha(145);
            rim.glow_extent = 8;
            radius = 999;
            if (theme.is_dark) plate = QColor(5, 9, 30, 220);
        } else if (kind_ == Kind::Shortcut) {
            rim.color1.setAlpha(105);
            rim.color2.setAlpha(125);
            rim.glow.setAlpha(65);
            rim.glow_extent = 4;
            radius = 16;
        }
        set_style(rim, plate, radius);
    }

private:
    Kind kind_;
};

// Let rows use the card's actual width instead of truncating every title at
// a fixed 190px. The full value stays available in the tooltip.
class HomeElidedLabel : public QLabel {
public:
    HomeElidedLabel(const QString& text, QWidget* parent)
        : QLabel(text, parent) {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
        setToolTip(text);
        setTextFormat(Qt::PlainText);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setFont(font());
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                   fontMetrics().elidedText(text(), Qt::ElideRight,
                                            contentsRect().width()));
    }
};

// Circular colored letter badge used as the shortcut-tile icon: the first
// letter of the site's name on a brand-tinted disc, like the reference's
// YouTube / GitHub / Discord / Gmail / Drive tiles.
QPixmap letter_badge(const QString& label, const QColor& color, int px,
                     qreal dpr) {
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(1, 1, px - 2, px - 2));
    QFont f;
    f.setPixelSize(px * 0.42);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(QStringLiteral("#F1F5F9")));
    p.drawText(QRectF(0, 0, px, px), Qt::AlignCenter,
               label.left(1).toUpper());
    return pm;
}

// Brand colors for well-known sites, used only for the letter badge shown
// until a real favicon has loaded; anything else gets a calm slate.
QColor brand_color(const QString& label) {
    const QString l = label.toLower();
    if (l.contains(QStringLiteral("youtube")))  return QColor(QStringLiteral("#FF0033"));
    if (l.contains(QStringLiteral("github")))   return QColor(QStringLiteral("#3B4252"));
    if (l.contains(QStringLiteral("discord")))  return QColor(QStringLiteral("#5865F2"));
    if (l.contains(QStringLiteral("gmail")))    return QColor(QStringLiteral("#EA4335"));
    if (l.contains(QStringLiteral("drive")))    return QColor(QStringLiteral("#0F9D58"));
    if (l.contains(QStringLiteral("reddit")))   return QColor(QStringLiteral("#FF4500"));
    if (l.contains(QStringLiteral("stack")))    return QColor(QStringLiteral("#F48024"));
    return QColor(QStringLiteral("#33415C"));
}

constexpr int kTilesPerRow = 6;
constexpr int kMaxShortcuts = 18;
constexpr int kRowsPerCard = 3;

bool is_internal_url(const QString& url) {
    return url.startsWith(QStringLiteral("aurora://")) ||
           url.startsWith(QStringLiteral("about:")) ||
           url.startsWith(QStringLiteral("chrome://"));
}

} // namespace

HomePage::HomePage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("HomePage"));
    // paintEvent fills the whole rect; the sheet only supplies the tokens.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);

    build_ui();

    // Live clock: the block shows the mockup's HH:mm + weekday, updated
    // often enough that a minutes flip is never stale for long.
    clock_timer_ = new QTimer(this);
    clock_timer_->setInterval(20000);
    const auto tick = [this] {
        if (clock_label_) {
            clock_label_->setText(QTime::currentTime().toString(
                QStringLiteral("HH:mm")));
        }
        if (date_label_) {
            date_label_->setText(QDate::currentDate().toString(
                QStringLiteral("dddd, MMMM d")));
        }
    };
    connect(clock_timer_, &QTimer::timeout, this, tick);
    clock_timer_->start();
    tick();

    render_history_rows();

    // Keep every glyph in step with the live theme.
    theme_observer_ = add_theme_observer([this](const Theme&) {
        for (GlowFrame* frame : findChildren<GlowFrame*>()) {
            static_cast<HomeGlass*>(frame)->apply_theme();
        }
        apply_icons();
        update();
    });
    apply_icons();

    // Site icons land asynchronously; refresh the rows and tiles so the
    // letter badges get swapped for real favicons as they arrive.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString&) {
                render_history_rows();
                apply_icons();
            });

    // Live weather: show whatever is already known, follow later updates.
    connect(&WeatherService::instance(), &WeatherService::updated, this,
            [this] { update_weather(); });
    update_weather();
}

HomePage::~HomePage() {
    if (theme_observer_ != 0) remove_theme_observer(theme_observer_);
}

void HomePage::set_query_url_resolver(SearchUrlFor fn) {
    query_url_for_ = std::move(fn);
}

void HomePage::set_history_service(history::HistoryService* history) {
    history_ = history;
    render_history_rows();
}

void HomePage::set_prefs(settings::Settings* prefs) {
    prefs_ = prefs;
    shortcuts_ = load_shortcuts(prefs_, settings::PrefKeys::kHomeShortcuts);
    rebuild_shortcuts();
    apply_icons();

    // A city the user pinned earlier takes over from the IP lookup.
    if (!prefs_) {
        WeatherService::instance().refresh_if_stale();
        return;
    }
    const QJsonObject loc = QJsonDocument::fromJson(QByteArray::fromStdString(
        prefs_->get_string(settings::PrefKeys::kWeatherLocation, "auto")))
        .object();
    if (loc.contains(QStringLiteral("lat")) &&
        loc.contains(QStringLiteral("lon"))) {
        WeatherService::instance().use_location(
            loc.value(QStringLiteral("name")).toString(),
            loc.value(QStringLiteral("lat")).toDouble(),
            loc.value(QStringLiteral("lon")).toDouble(),
            loc.value(QStringLiteral("f")).toBool(false));
    } else {
        WeatherService::instance().refresh_if_stale();
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void HomePage::build_ui() {
    auto* page_layout = new QVBoxLayout(this);
    page_layout->setContentsMargins(0, 0, 0, 0);
    content_scroll_ = new QScrollArea(this);
    content_scroll_->setObjectName(QStringLiteral("HomeScroll"));
    content_scroll_->setFrameShape(QFrame::NoFrame);
    content_scroll_->setWidgetResizable(true);
    content_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_scroll_->viewport()->setAutoFillBackground(false);
    content_ = new QWidget;
    content_->setObjectName(QStringLiteral("HomeContent"));
    content_scroll_->setWidget(content_);
    page_layout->addWidget(content_scroll_);

    auto* root = new QVBoxLayout(content_);
    root->setContentsMargins(32, 32, 32, 32);
    root->setSpacing(0);

    // The weather and clock share the brand's top band, as in the reference,
    // rather than taking a full extra row above it.
    header_ = new QWidget(content_);
    header_->setObjectName(QStringLiteral("HomeHeader"));
    auto* top_row = new QHBoxLayout(header_);
    top_row->setContentsMargins(8, 0, 8, 0);
    top_row->addWidget(build_weather_chip(), 0, Qt::AlignTop);
    top_row->addStretch(1);
    top_row->addWidget(build_clock_block(), 0, Qt::AlignTop);

    root->addStretch(1);

    // --- Center column: brand, pill, tiles --------------------------------
    auto* column = new QWidget(content_);
    column_ = column;
    column->setObjectName(QStringLiteral("HomeColumn"));
    column->setAttribute(Qt::WA_TranslucentBackground, true);
    column->setFixedWidth(1060);
    auto* column_layout = new QVBoxLayout(column);
    column_layout->setContentsMargins(0, 0, 0, 0);
    column_layout->setSpacing(20);

    brand_ = new BrandLabel(column);
    column_layout->addWidget(brand_, 0, Qt::AlignHCenter);

    auto* subtitle = new QLabel(
        QStringLiteral("B R O W S E .   F O C U S .   A U R O R A ."),
        column);
    subtitle->setObjectName(QStringLiteral("HomeSubtitle"));
    subtitle->setAlignment(Qt::AlignCenter);
    column_layout->addWidget(subtitle);

    // Search pill: leading search glyph, the field, then the trailing
    // sparkle — all inside one capsule host.
    auto* pill = new HomeGlass(column, HomeGlass::Kind::Search);
    search_host_ = pill;
    pill->setFixedSize(840, 80);
    pill->setObjectName(QStringLiteral("HomeSearchPillHost"));
    auto* pill_layout = new QHBoxLayout(pill);
    pill_layout->setContentsMargins(14, 6, 14, 6);
    pill_layout->setSpacing(0);

    search_btn_ = new QToolButton(pill);
    search_btn_->setObjectName(QStringLiteral("HomeSparkleButton"));
    search_btn_->setCursor(Qt::PointingHandCursor);
    search_btn_->setToolTip(QStringLiteral("Search"));
    search_btn_->setIconSize(QSize(26, 26));
    connect(search_btn_, &QToolButton::clicked, this, [this] {
        search_->setFocus(Qt::ShortcutFocusReason);
    });
    pill_layout->addSpacing(Spacing::sm);
    pill_layout->addWidget(search_btn_, 0, Qt::AlignVCenter);

    search_ = new QLineEdit(pill);
    search_->setObjectName(QStringLiteral("HomeSearchPill"));
    search_->setPlaceholderText(QStringLiteral("Search or enter address"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumHeight(36);
    connect(search_, &QLineEdit::returnPressed, this, &HomePage::submit);
    pill_layout->addWidget(search_, 1);

    sparkle_btn_ = new QToolButton(pill);
    sparkle_btn_->setObjectName(QStringLiteral("HomeSparkleButton"));
    sparkle_btn_->setCursor(Qt::PointingHandCursor);
    sparkle_btn_->setToolTip(QStringLiteral("Search"));
    sparkle_btn_->setIconSize(QSize(28, 28));
    connect(sparkle_btn_, &QToolButton::clicked, this, &HomePage::submit);
    pill_layout->addWidget(sparkle_btn_, 0, Qt::AlignVCenter);
    pill_layout->addSpacing(Spacing::sm);

    column_layout->addWidget(pill, 0, Qt::AlignHCenter);

    // --- Shortcut tiles: the user's own sites, none by default ------------
    tiles_host_ = new QWidget(column);
    tiles_host_->setObjectName(QStringLiteral("HomeShortcutTile"));
    tiles_layout_ = new QVBoxLayout(tiles_host_);
    tiles_layout_->setContentsMargins(0, 0, 0, 0);
    tiles_layout_->setSpacing(Spacing::md);
    column_layout->addWidget(tiles_host_);
    rebuild_shortcuts();

    root->addWidget(column, 0, Qt::AlignHCenter);
    root->addSpacing(24);
    root->addStretch(1);

    // --- Bottom cards -------------------------------------------------------
    cards_row_ = new QWidget(content_);
    cards_row_->setObjectName(QStringLiteral("HomeCardsRow"));
    cards_row_->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* cards_layout = new QHBoxLayout(cards_row_);
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(20);
    cards_row_->setFixedWidth(1440);

    QWidget* resume = build_history_card(
        cards_row_, QStringLiteral("Continue where you left off"));
    resume_rows_ = resume->findChild<QVBoxLayout*>(
        QStringLiteral("HomeRows"));
    cards_layout->addWidget(resume, 4);

    QWidget* recent = build_history_card(
        cards_row_, QStringLiteral("Recently visited"));
    recent_rows_ = recent->findChild<QVBoxLayout*>(
        QStringLiteral("HomeRows"));
    cards_layout->addWidget(recent, 4);

    cards_layout->addWidget(build_actions_card(cards_row_), 2);

    root->addWidget(cards_row_, 0, Qt::AlignHCenter);
    header_->raise();
    update_home_layout();
}

QWidget* HomePage::build_weather_chip() {
    auto* chip = new QWidget(this);
    weather_chip_ = chip;
    chip->setAttribute(Qt::WA_TranslucentBackground, true);
    chip->setContextMenuPolicy(Qt::CustomContextMenu);
    chip->setCursor(Qt::PointingHandCursor);
    chip->setToolTip(QStringLiteral("Right-click to change location"));
    connect(chip, &QWidget::customContextMenuRequested, this,
            [this, chip](const QPoint& pos) {
                show_weather_menu(chip->mapToGlobal(pos));
            });
    auto* row = new QHBoxLayout(chip);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(Spacing::sm);
    weather_icon_ = new QLabel(chip);
    weather_icon_->setFixedSize(48, 48);
    row->addWidget(weather_icon_, 0, Qt::AlignVCenter);
    auto* texts = new QVBoxLayout;
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(0);
    weather_temp_ = new QLabel(chip);
    weather_temp_->setObjectName(QStringLiteral("HomeWeatherTemp"));
    texts->addWidget(weather_temp_);
    weather_sub_ = new QLabel(chip);
    weather_sub_->setObjectName(QStringLiteral("HomeWeatherSub"));
    texts->addWidget(weather_sub_);
    row->addLayout(texts);
    // Hidden until real conditions arrive: never a made-up temperature.
    chip->hide();
    return chip;
}

void HomePage::update_weather() {
    if (!weather_chip_) return;
    const WeatherInfo& w = WeatherService::instance().current();
    if (!w.valid) {
        weather_chip_->hide();
        return;
    }
    weather_temp_->setText(QStringLiteral("%1°%2")
        .arg(qRound(w.temperature))
        .arg(w.fahrenheit ? QStringLiteral("F") : QStringLiteral("C")));
    QString sub = WeatherService::describe(w.code, w.is_day);
    if (!w.place.isEmpty()) sub += QStringLiteral(" · ") + w.place;
    weather_sub_->setText(sub);
    apply_icons();
    weather_chip_->show();
}

void HomePage::show_weather_menu(const QPoint& global_pos) {
    QMenu menu(this);
    QAction* refresh = menu.addAction(QStringLiteral("Refresh"));
    QAction* city = menu.addAction(QStringLiteral("Change city…"));
    QAction* automatic =
        menu.addAction(QStringLiteral("Use my location (automatic)"));
    QAction* chosen = menu.exec(global_pos);
    if (chosen == refresh) {
        WeatherService::instance().refresh();
    } else if (chosen == city) {
        set_weather_city();
    } else if (chosen == automatic) {
        if (prefs_) {
            prefs_->set_string(settings::PrefKeys::kWeatherLocation, "auto");
        }
        WeatherService::instance().use_automatic_location();
    }
}

void HomePage::set_weather_city() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Weather location"),
        QStringLiteral("City name:"), QLineEdit::Normal, QString(), &ok)
        .trimmed();
    if (!ok || name.isEmpty()) return;
    auto& weather = WeatherService::instance();
    // One-shot: persist the pin the moment the lookup resolves.
    auto* resolved = new QMetaObject::Connection;
    auto* missing = new QMetaObject::Connection;
    *resolved = connect(&weather, &WeatherService::city_resolved, this,
        [this, resolved, missing](const QString& n, double lat, double lon,
                                  bool f) {
            disconnect(*resolved);
            disconnect(*missing);
            delete resolved;
            delete missing;
            if (!prefs_) return;
            QJsonObject o;
            o.insert(QStringLiteral("name"), n);
            o.insert(QStringLiteral("lat"), lat);
            o.insert(QStringLiteral("lon"), lon);
            o.insert(QStringLiteral("f"), f);
            const QByteArray json =
                QJsonDocument(o).toJson(QJsonDocument::Compact);
            prefs_->set_string(settings::PrefKeys::kWeatherLocation,
                               std::string_view(json.constData(),
                                   static_cast<size_t>(json.size())));
        });
    *missing = connect(&weather, &WeatherService::city_not_found, this,
        [this, resolved, missing](const QString& q) {
            disconnect(*resolved);
            disconnect(*missing);
            delete resolved;
            delete missing;
            emit status_message(
                QStringLiteral("Couldn't find a city named \"%1\"").arg(q));
        });
    weather.search_city(name);
}

QWidget* HomePage::build_clock_block() {
    auto* block = new QWidget(this);
    block->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* column = new QVBoxLayout(block);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    clock_label_ = new QLabel(block);
    clock_label_->setObjectName(QStringLiteral("HomeClock"));
    clock_label_->setAlignment(Qt::AlignRight);
    column->addWidget(clock_label_);
    date_label_ = new QLabel(block);
    date_label_->setObjectName(QStringLiteral("HomeDate"));
    date_label_->setAlignment(Qt::AlignRight);
    column->addWidget(date_label_);
    return block;
}

QWidget* HomePage::build_shortcut_tile(QWidget* parent, const QString& label,
                                       const QString& target_url, bool is_add,
                                       int index) {
    auto* frame = new HomeGlass(parent, HomeGlass::Kind::Shortcut);
    frame->setObjectName(QStringLiteral("HomeShortcutFrame"));
    frame->setFixedSize(tile_width_, tile_height_);
    auto* frame_layout = new QVBoxLayout(frame);
    frame_layout->setContentsMargins(7, 7, 7, 7);
    auto* tile = new QToolButton(frame);
    tile->setObjectName(QStringLiteral("HomeShortcut"));
    tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tile->setCursor(Qt::PointingHandCursor);
    tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    tile->setIconSize(QSize(48, 48));
    frame_layout->addWidget(tile);
    tile->setProperty("auroraTileAdd", is_add);
    tile->setProperty("auroraTileLabel", label);
    tile->setProperty("auroraTileUrl", target_url);
    // Long names must not stretch the tile: elide to the tile's width.
    tile->ensurePolished();
    const QFontMetrics fm(tile->font());
    tile->setText(fm.elidedText(label, Qt::ElideRight, tile_width_ - 26));
    tile->setToolTip(is_add ? QStringLiteral("Add a shortcut") : target_url);
    connect(tile, &QToolButton::clicked, this, [this, target_url, is_add] {
        if (is_add) {
            add_shortcut();
        } else if (!target_url.isEmpty()) {
            emit open_url_requested(target_url);
        }
    });
    if (!is_add) {
        tile->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tile, &QToolButton::customContextMenuRequested, this,
                [this, tile, index](const QPoint& pos) {
                    QMenu menu(this);
                    QAction* edit = menu.addAction(QStringLiteral("Edit…"));
                    QAction* remove = menu.addAction(QStringLiteral("Remove"));
                    QAction* chosen = menu.exec(tile->mapToGlobal(pos));
                    if (chosen == edit) edit_shortcut(index);
                    else if (chosen == remove) remove_shortcut(index);
                });
    }
    shortcut_buttons_.append(tile);
    return frame;
}

void HomePage::rebuild_shortcuts() {
    if (!tiles_layout_) return;
    // Tear down the old rows (tiles are owned by tiles_host_).
    shortcut_buttons_.clear();
    while (QLayoutItem* item = tiles_layout_->takeAt(0)) {
        if (QLayout* row = item->layout()) {
            while (QLayoutItem* inner = row->takeAt(0)) {
                if (QWidget* w = inner->widget()) w->deleteLater();
                delete inner;
            }
        }
        delete item;
    }

    // The final slot is always the "Add shortcut" tile (until the cap).
    const int total = shortcuts_.size() +
                      (shortcuts_.size() < kMaxShortcuts ? 1 : 0);
    for (int start = 0; start < total; start += tiles_per_row_) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(24);
        row->addStretch(1);
        for (int i = start; i < qMin(total, start + tiles_per_row_); ++i) {
            if (i < shortcuts_.size()) {
                row->addWidget(build_shortcut_tile(
                    tiles_host_, shortcuts_[i].title, shortcuts_[i].url,
                    /*is_add=*/false, i));
            } else {
                row->addWidget(build_shortcut_tile(
                    tiles_host_, QStringLiteral("Add shortcut"), QString(),
                    /*is_add=*/true, -1));
            }
        }
        row->addStretch(1);
        tiles_layout_->addLayout(row);
    }
    apply_icons();
}

void HomePage::add_shortcut() {
    Shortcut result;
    if (!prompt_shortcut(this, QStringLiteral("Add shortcut"), {}, &result)) {
        return;
    }
    shortcuts_.append(result);
    save_shortcuts(prefs_, settings::PrefKeys::kHomeShortcuts, shortcuts_);
    rebuild_shortcuts();
}

void HomePage::edit_shortcut(int index) {
    if (index < 0 || index >= shortcuts_.size()) return;
    Shortcut result;
    if (!prompt_shortcut(this, QStringLiteral("Edit shortcut"),
                         shortcuts_[index], &result)) {
        return;
    }
    shortcuts_[index] = result;
    save_shortcuts(prefs_, settings::PrefKeys::kHomeShortcuts, shortcuts_);
    rebuild_shortcuts();
}

void HomePage::remove_shortcut(int index) {
    if (index < 0 || index >= shortcuts_.size()) return;
    shortcuts_.removeAt(index);
    save_shortcuts(prefs_, settings::PrefKeys::kHomeShortcuts, shortcuts_);
    rebuild_shortcuts();
}

QWidget* HomePage::build_history_card(QWidget* parent, const QString& heading) {
    auto* card = new HomeGlass(parent, HomeGlass::Kind::Card);
    card->setObjectName(QStringLiteral("HomeCard"));
    card->setMinimumWidth(0);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(26, 22, 26, 22);
    card_layout->setSpacing(6);

    auto* header_row = new QHBoxLayout;
    header_row->setContentsMargins(0, 0, 0, 0);
    auto* title = new HomeElidedLabel(heading, card);
    title->setObjectName(QStringLiteral("HomeCardTitle"));
    header_row->addWidget(title, 1);
    auto* show_all = new QToolButton(card);
    show_all->setObjectName(QStringLiteral("HomeActionRow"));
    show_all->setProperty("accent", true);   // the cyan "Show all" link
    show_all->setText(QStringLiteral("Show all"));
    show_all->setCursor(Qt::PointingHandCursor);
    connect(show_all, &QToolButton::clicked, this, [this] {
        emit open_url_requested(QStringLiteral("aurora://history"));
    });
    header_row->addWidget(show_all);
    card_layout->addLayout(header_row);

    auto* divider = new QFrame(card);
    divider->setObjectName(QStringLiteral("HomeCardDivider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedHeight(Metrics::hairline);
    card_layout->addWidget(divider);
    card_layout->addSpacing(Spacing::xs);

    auto* rows = new QVBoxLayout;
    rows->setObjectName(QStringLiteral("HomeRows"));
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(10);
    card_layout->addLayout(rows);
    card_layout->addStretch(1);

    return card;
}

QWidget* HomePage::build_actions_card(QWidget* parent) {
    auto* card = new HomeGlass(parent, HomeGlass::Kind::Card);
    card->setObjectName(QStringLiteral("HomeCard"));
    card->setMinimumWidth(0);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(26, 22, 26, 22);
    card_layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("Quick actions"), card);
    title->setObjectName(QStringLiteral("HomeCardTitle"));
    card_layout->addWidget(title);

    auto* divider = new QFrame(card);
    divider->setObjectName(QStringLiteral("HomeCardDivider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedHeight(Metrics::hairline);
    card_layout->addWidget(divider);
    card_layout->addSpacing(Spacing::xs);

    const struct {
        icons::Name icon;
        const char* label;
        const char* target;
    } kActions[] = {
        {icons::Name::StarOutline, "Bookmarks", "aurora://bookmarks"},
        {icons::Name::History, "History", "aurora://history"},
        {icons::Name::Downloads, "Downloads", "aurora://downloads"},
        {icons::Name::Puzzle, "Extensions", "aurora://extensions"},
        {icons::Name::Settings, "Settings", "aurora://settings"},
    };
    for (const auto& action : kActions) {
        auto* row = new QToolButton(card);
        row->setObjectName(QStringLiteral("HomeActionRow"));
        row->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        row->setText(QString::fromUtf8(action.label));
        row->setIconSize(QSize(24, 24));
        row->setMinimumHeight(36);
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("auroraActionIcon",
                         static_cast<int>(action.icon));
        const QString target = QString::fromUtf8(action.target);
        connect(row, &QToolButton::clicked, this, [this, target] {
            emit open_url_requested(target);
        });
        card_layout->addWidget(row);
        action_buttons_.append(row);
    }
    card_layout->addStretch(1);
    return card;
}

// ---------------------------------------------------------------------------
// History rows
// ---------------------------------------------------------------------------

void HomePage::render_history_rows() {
    const auto clear = [](QVBoxLayout* rows) {
        while (QLayoutItem* item = rows ? rows->takeAt(0) : nullptr) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
    };
    clear(resume_rows_);
    clear(recent_rows_);
    if (!resume_rows_ || !recent_rows_) return;

    // Real history only. "Continue where you left off" is the newest pages;
    // "Recently visited" is the next newest, from sites not already shown
    // above, so the two cards never repeat each other.
    QVector<HistoryEntry> resume;
    QVector<HistoryEntry> recent;
    if (history_) {
        QSet<QString> seen_urls;
        QSet<QString> resume_hosts;
        for (const auto& v : history_->recent(80)) {
            HistoryEntry entry;
            entry.url = QString::fromStdString(v.url);
            if (entry.url.isEmpty() || is_internal_url(entry.url) ||
                seen_urls.contains(entry.url)) {
                continue;
            }
            seen_urls.insert(entry.url);
            entry.title = v.title.empty()
                ? FaviconCache::host_of(entry.url)
                : QString::fromStdString(v.title);
            entry.when_epoch = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    v.last_visit.time_since_epoch()).count());
            const QString host = FaviconCache::host_of(entry.url);
            if (resume.size() < kRowsPerCard) {
                resume.append(entry);
                resume_hosts.insert(host);
            } else if (!resume_hosts.contains(host) &&
                       recent.size() < kRowsPerCard) {
                recent.append(entry);
                resume_hosts.insert(host);   // one row per site in this card
            }
            if (resume.size() >= kRowsPerCard &&
                recent.size() >= kRowsPerCard) {
                break;
            }
        }
    }

    const auto fill = [this](QVBoxLayout* rows,
                             const QVector<HistoryEntry>& list) {
        if (list.isEmpty()) {
            auto* empty = new QLabel(
                QStringLiteral("Pages you visit will show up here."),
                rows->parentWidget());
            empty->setObjectName(QStringLiteral("HomeRowSub"));
            empty->setWordWrap(true);
            rows->addWidget(empty);
            return;
        }
        for (const HistoryEntry& entry : list) render_entry(rows, entry);
    };
    fill(resume_rows_, resume);
    fill(recent_rows_, recent);
}

void HomePage::render_entry(QVBoxLayout* rows, const HistoryEntry& entry) {
    auto* row = new QWidget(rows->parentWidget());
    row->setObjectName(QStringLiteral("HomeRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setCursor(Qt::PointingHandCursor);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(Spacing::xs, Spacing::xs, Spacing::xs,
                                   Spacing::xs);
    row_layout->setSpacing(Spacing::md);

    // Favicon: the shared cache when it has one, a letter badge otherwise.
    auto* icon_label = new QLabel(row);
    icon_label->setFixedSize(40, 40);
    icon_label->setAlignment(Qt::AlignCenter);
    QIcon site = FaviconCache::instance().icon_for_url(entry.url);
    if (!site.isNull()) {
        icon_label->setPixmap(site.pixmap(QSize(40, 40), devicePixelRatioF()));
    } else {
        const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
        const QString host = FaviconCache::host_of(entry.url);
        icon_label->setPixmap(letter_badge(
            host.isEmpty() ? entry.title : host, brand_color(host), 40, dpr));
    }
    row_layout->addWidget(icon_label, 0, Qt::AlignTop);

    auto* texts = new QVBoxLayout;
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(Spacing::xxs);
    auto* title = new HomeElidedLabel(entry.title, row);
    title->setObjectName(QStringLiteral("HomeRowTitle"));
    texts->addWidget(title);
    auto* sub = new HomeElidedLabel(FaviconCache::host_of(entry.url), row);
    sub->setObjectName(QStringLiteral("HomeRowSub"));
    texts->addWidget(sub);
    row_layout->addLayout(texts, 1);

    auto* when = new QLabel(time_ago(entry.when_epoch), row);
    when->setObjectName(QStringLiteral("HomeRowSub"));
    when->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row_layout->addWidget(when, 0, Qt::AlignTop);

    // Whole-row click opens the entry.
    row->installEventFilter(this);
    row->setProperty("auroraRowUrl", entry.url);
    rows->addWidget(row);
}

bool HomePage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        if (auto* row = qobject_cast<QWidget*>(watched)) {
            const QString url =
                row->property("auroraRowUrl").toString();
            if (!url.isEmpty()) {
                emit open_url_requested(url);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

QString HomePage::time_ago(std::int64_t epoch_seconds) const {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 delta = qMax<qint64>(0, now - epoch_seconds);
    if (delta < 90) return QStringLiteral("Just now");
    if (delta < 60 * 60) {
        return QStringLiteral("%1m ago").arg(qMax<qint64>(1, delta / 60));
    }
    if (delta < 24 * 60 * 60) {
        return QStringLiteral("%1h ago").arg(delta / 3600);
    }
    if (delta < 48 * 60 * 60) return QStringLiteral("Yesterday");
    return QStringLiteral("%1d ago").arg(delta / 86400);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void HomePage::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // The full-bleed aurora wallpaper is the page's backdrop. Scaled once,
    // repainted cheaply: only the size changes matter (resizeEvent clears
    // the cache and paintEvent lazily re-scales on next frame).
    if (bg_source_.isNull()) {
        bg_source_ = load_page_wallpaper();
        bg_source_.setDevicePixelRatio(devicePixelRatioF());
    }
    if (!bg_source_.isNull() && bg_.isNull()) {
        bg_ = scale_page_wallpaper(bg_source_, size(), devicePixelRatioF());
    }
    if (!bg_.isNull() && bg_.devicePixelRatio() != devicePixelRatioF()) {
        bg_ = scale_page_wallpaper(bg_source_, size(), devicePixelRatioF());
    }
    if (!bg_.isNull()) {
        const int x = (width() - bg_.width() / devicePixelRatioF()) / 2;
        const int y = (height() - bg_.height() / devicePixelRatioF()) / 2;
        p.drawPixmap(x, y, bg_);
    } else {
        // Without the image the page must still read as a calm dark surface
        // and never flash black between states.
        p.fillRect(rect(), current_theme().colors.background);
    }

    QWidget::paintEvent(event);
}

void HomePage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    bg_ = {};  // repaint lazily via paintEvent's empty-cache check
    update_home_layout();
}

void HomePage::update_home_layout() {
    if (!column_ || !cards_row_) return;
    // Leave room for a vertical scrollbar without changing the grid width
    // when it appears. Crucially, these are explicit widths: AlignHCenter
    // alone makes Qt shrink a widget back to its content's sizeHint.
    const int available = qMax(240, width() - 84);
    const qreal scale = qBound<qreal>(0.72, height() / 1040.0, 1.0);
    column_->setFixedWidth(qMin(available, 1060));
    search_host_->setFixedSize(qMin(available, 840), qRound(80 * scale));
    static_cast<BrandLabel*>(brand_)->set_width(
        qMin(available, qRound(400 * scale)));
    cards_row_->setFixedWidth(qMin(available, 1640));
    cards_row_->setMinimumHeight(qRound(300 * scale));

    // Stack cards only when there is no longer room for readable columns.
    auto* cards = static_cast<QBoxLayout*>(cards_row_->layout());
    const auto direction = available < 1050
        ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (cards->direction() != direction) cards->setDirection(direction);

    const int tile_width = qRound(144 * scale);
    const int tile_height = qRound(126 * scale);
    const int per_row = qBound(1, (available + 24) / (tile_width + 24),
                              kTilesPerRow);
    const bool reflow = per_row != tiles_per_row_;
    tile_width_ = tile_width;
    tile_height_ = tile_height;
    tiles_per_row_ = per_row;
    if (reflow) {
        rebuild_shortcuts();
    } else {
        for (QToolButton* tile : shortcut_buttons_) {
            tile->parentWidget()->setFixedSize(tile_width_, tile_height_);
            tile->setText(QFontMetrics(tile->font()).elidedText(
                tile->property("auroraTileLabel").toString(), Qt::ElideRight,
                tile_width_ - 26));
        }
    }

    header_->setGeometry(qMax(0, (width() - available) / 2), 32, available,
                       header_->sizeHint().height());
    // On narrow windows the corner widgets get their own band, so neither
    // can cover the centered logo. Extra shortcuts/cards scroll, not shrink.
    auto* root = static_cast<QVBoxLayout*>(content_->layout());
    root->setContentsMargins(32, available < 850
        ? header_->sizeHint().height() + 52 : 32, 32, 32);
}

void HomePage::apply_icons() {
    const Theme& theme = current_theme();
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    if (search_btn_) {
        search_btn_->setIcon(icons::icon(icons::Name::Search,
                                         theme.colors.text_muted, 26, dpr));
    }
    if (sparkle_btn_) {
        sparkle_btn_->setIcon(icons::icon(icons::Name::Sparkle,
                                          theme.colors.accent2, 28, dpr));
    }
    if (weather_icon_) {
        const WeatherInfo& w = WeatherService::instance().current();
        const icons::Name glyph = w.valid
            ? WeatherService::icon_for(w.code, w.is_day)
            : icons::Name::CloudSun;
        weather_icon_->setPixmap(icons::pixmap(
            glyph, 48, theme.colors.accent, dpr));
    }
    for (QToolButton* tile : shortcut_buttons_) {
        if (tile->property("auroraTileAdd").toBool()) {
            tile->setIcon(icons::icon(icons::Name::Plus,
                                      theme.colors.text_muted, 36, dpr));
            continue;
        }
        const QString label = tile->property("auroraTileLabel").toString();
        const QString url = tile->property("auroraTileUrl").toString();
        const QIcon site = FaviconCache::instance().icon_for_url(url);
        if (!site.isNull()) {
            tile->setIcon(site);
        } else {
            tile->setIcon(QIcon(letter_badge(
                label, brand_color(FaviconCache::host_of(url)), 48, dpr)));
        }
    }
    for (QToolButton* row : action_buttons_) {
        const auto icon = static_cast<icons::Name>(
            row->property("auroraActionIcon").toInt());
        row->setIcon(icons::icon(icon, theme.colors.text_muted, 24, dpr));
    }
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

void HomePage::submit() {
    const QString query = search_->text().trimmed();
    if (query.isEmpty()) return;

    QString target = query_url_for_ ? query_url_for_(query) : QString();
    // No resolver wired yet (page created before the window finished its
    // search-engine setup): fall back to the raw text and let the caller's
    // own omnibox parsing route it. Never block input on chrome state.
    if (target.isEmpty()) target = query;
    search_->clear();
    emit status_message(QStringLiteral("Searching for %1").arg(query));
    emit open_url_requested(target);
}

} // namespace aurora::ui

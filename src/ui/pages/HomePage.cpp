#include "ui/pages/HomePage.hpp"

#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QProgressBar>
#include <QRadialGradient>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

#include "history/HistoryService.hpp"
#include "ui/FaviconCache.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

// One soft radial wash of color, saturated only at the core: beyond it the
// gradient fades to fully transparent so overlapping blobs additively stack
// into the nebula instead of drawing hard circles.
void paint_blob(QPainter& p, const QPointF& center, qreal radius,
                const QColor& color, int alpha) {
    if (radius <= 0.0) return;
    QColor core(color);
    core.setAlpha(alpha);
    QColor edge(color);
    edge.setAlpha(0);
    QRadialGradient g(center, radius);
    g.setColorAt(0.0, core);
    g.setColorAt(0.45, QColor(core.red(), core.green(), core.blue(),
                              alpha / 2));
    g.setColorAt(1.0, edge);
    p.fillRect(QRectF(center.x() - radius, center.y() - radius,
                      radius * 2.0, radius * 2.0), g);
}

// The glowing gradient-ribbon "A" logo plus the widely-tracked "A U R O R A"
// wordmark, both drawn against a cyan → purple → magenta gradient. Painted
// here (not in the stylesheet) because QSS cannot gradient-fill text.
class BrandLabel : public QWidget {
public:
    explicit BrandLabel(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setFixedSize(420, 150);
    }

    void paintEvent(QPaintEvent*) override {
        const ThemeColors& c = current_theme().colors;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QLinearGradient grad(rect().topLeft(), rect().bottomRight());
        grad.setColorAt(0.0, c.accent);
        grad.setColorAt(0.55, c.accent2);
        grad.setColorAt(1.0, c.accent3);

        // --- Ribbon "A" -------------------------------------------------
        // Two leaning wings meeting at an apex: an up-then-down chevron
        // with a soft inner fold, heavily rounded by the pen caps.
        const qreal cx = width() / 2.0;
        const qreal top = 6.0;
        const qreal base = 66.0;
        const qreal wing = 30.0;
        QPainterPath a;
        a.moveTo(cx - wing, base);
        a.cubicTo(cx - wing * 0.55, base - 26, cx - 7, top + 22, cx, top);
        a.cubicTo(cx + 7, top + 22, cx + wing * 0.55, base - 26,
                  cx + wing, base);
        // Glow pass, then the crisp pass.
        QColor glow(c.accent2);
        glow.setAlpha(90);
        p.setPen(QPen(glow, 15.0, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(a);
        p.setPen(QPen(QBrush(grad), 6.5, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(a);

        // --- "A U R O R A" ----------------------------------------------
        QFont f = font();
        f.setPixelSize(40);
        f.setWeight(QFont::Light);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 16);
        QPainterPath text;
        const QString word = QStringLiteral("AURORA");
        text.addText(0, 0, f, word);
        const QRectF tb = text.boundingRect();
        text.translate((width() - tb.width()) / 2.0 - tb.x(),
                       112.0 - tb.y());
        p.setPen(Qt::NoPen);
        p.setBrush(QBrush(grad));
        p.drawPath(text);
    }
};

// The search pill's capsule host: paints the glassy fill and the cyan →
// purple gradient border (QSS cannot gradient-stroke a border), with the
// field and its icons as transparent children on top.
class PillHost : public QWidget {
public:
    explicit PillHost(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_StyledBackground, false);
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
    }

    void paintEvent(QPaintEvent*) override {
        const ThemeColors& c = current_theme().colors;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Inset 2px so the outer glow pass has room inside the widget bounds.
        const QRectF r = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
        const qreal rad = r.height() / 2.0;

        QLinearGradient grad(r.topLeft(), r.bottomRight());
        grad.setColorAt(0.0, c.accent);
        grad.setColorAt(1.0, c.accent2);

        // Soft outer aura, then the capsule.
        QColor aura = c.accent2;
        aura.setAlpha(46);
        p.setPen(Qt::NoPen);
        p.setBrush(aura);
        p.drawRoundedRect(r.adjusted(-1.5, -1.5, 1.5, 1.5), rad + 1.5,
                          rad + 1.5);

        QColor fill(c.surface_alt);
        fill.setAlpha(216);
        p.setBrush(fill);
        p.setPen(QPen(QBrush(grad), 1.6));
        p.drawRoundedRect(r, rad, rad);
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

// Brand colors for the well-known tiles (only used for the letter badges).
QColor brand_color(const QString& label) {
    if (label == QStringLiteral("YouTube"))  return QColor(QStringLiteral("#FF0033"));
    if (label == QStringLiteral("GitHub"))   return QColor(QStringLiteral("#3B4252"));
    if (label == QStringLiteral("Discord"))  return QColor(QStringLiteral("#5865F2"));
    if (label == QStringLiteral("Gmail"))    return QColor(QStringLiteral("#EA4335"));
    if (label == QStringLiteral("Drive"))    return QColor(QStringLiteral("#0F9D58"));
    if (label == QStringLiteral("Reddit"))   return QColor(QStringLiteral("#FF4500"));
    if (label == QStringLiteral("Stack Overflow"))
        return QColor(QStringLiteral("#F48024"));
    if (label == QStringLiteral("Vercord Web") ||
        label == QStringLiteral("Vencord Web"))
        return QColor(QStringLiteral("#8B5CF6"));
    return QColor(QStringLiteral("#33415C"));
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

    // Static fallback rows (also what an empty history shows). The times are
    // relative to now so the time-ago labels always read naturally.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    resume_entries_ = {
        {QStringLiteral("https://www.youtube.com"),
         QStringLiteral("YouTube — Project Hail Mary — Chapter 12"),
         now - 2 * 60},
        {QStringLiteral("https://github.com"),
         QStringLiteral("GitHub — aurora-browser/aurora"),
         now - 60 * 60},
        {QStringLiteral("https://docs.google.com"),
         QStringLiteral("Google Docs — Aurora Design Brief"),
         now - 26 * 60 * 60},
    };
    recent_entries_ = {
        {QStringLiteral("https://www.reddit.com"),
         QStringLiteral("Reddit — r/webdev"), now - 45 * 60},
        {QStringLiteral("https://stackoverflow.com"),
         QStringLiteral("Stack Overflow — Qt6 rounded corners"),
         now - 2 * 60 * 60},
        {QStringLiteral("https://vencord.dev"),
         QStringLiteral("Vencord Web"), now - 26 * 60 * 60},
    };
    render_history_rows();

    // Keep every glyph in step with the live theme.
    theme_observer_ = add_theme_observer([this](const Theme&) {
        apply_icons();
        update();
    });
    apply_icons();

    // Site icons land asynchronously; refresh the rows so the letter badges
    // get swapped for real favicons as they arrive.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString&) { render_history_rows(); });
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

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void HomePage::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(Spacing::xl, Spacing::lg, Spacing::xl,
                             Spacing::lg);
    root->setSpacing(0);

    // --- Top overlay row: weather chip (left), clock + date (right) -------
    auto* top_row = new QHBoxLayout;
    top_row->setContentsMargins(Spacing::sm, 0, Spacing::sm, 0);
    top_row->addWidget(build_weather_chip(), 0, Qt::AlignTop);
    top_row->addStretch(1);
    top_row->addWidget(build_clock_block(), 0, Qt::AlignTop);
    root->addLayout(top_row);

    root->addStretch(4);

    // --- Center column: brand, pill, tiles --------------------------------
    auto* column = new QWidget(this);
    column->setObjectName(QStringLiteral("HomeColumn"));
    column->setAttribute(Qt::WA_TranslucentBackground, true);
    column->setMaximumWidth(760);
    auto* column_layout = new QVBoxLayout(column);
    column_layout->setContentsMargins(0, 0, 0, 0);
    column_layout->setSpacing(Spacing::xl);

    auto* brand = new BrandLabel(column);
    column_layout->addWidget(brand, 0, Qt::AlignHCenter);

    auto* subtitle = new QLabel(
        QStringLiteral("B R O W S E .   F O C U S .   A U R O R A ."),
        column);
    subtitle->setObjectName(QStringLiteral("HomeSubtitle"));
    subtitle->setAlignment(Qt::AlignCenter);
    column_layout->addWidget(subtitle);

    // Search pill: leading search glyph, the field, then the trailing
    // sparkle — all inside one capsule host.
    auto* pill = new PillHost(column);
    pill->setObjectName(QStringLiteral("HomeSearchPillHost"));
    pill->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* pill_layout = new QHBoxLayout(pill);
    pill_layout->setContentsMargins(0, 0, 0, 0);
    pill_layout->setSpacing(0);

    search_btn_ = new QToolButton(pill);
    search_btn_->setObjectName(QStringLiteral("HomeSparkleButton"));
    search_btn_->setCursor(Qt::PointingHandCursor);
    search_btn_->setToolTip(QStringLiteral("Search"));
    search_btn_->setIconSize(QSize(18, 18));
    connect(search_btn_, &QToolButton::clicked, this, [this] {
        search_->setFocus(Qt::ShortcutFocusReason);
    });
    pill_layout->addSpacing(Spacing::sm);
    pill_layout->addWidget(search_btn_, 0, Qt::AlignVCenter);

    search_ = new QLineEdit(pill);
    search_->setObjectName(QStringLiteral("HomeSearchPill"));
    search_->setPlaceholderText(QStringLiteral("Search or enter address"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumHeight(52);
    connect(search_, &QLineEdit::returnPressed, this, &HomePage::submit);
    pill_layout->addWidget(search_, 1);

    sparkle_btn_ = new QToolButton(pill);
    sparkle_btn_->setObjectName(QStringLiteral("HomeSparkleButton"));
    sparkle_btn_->setCursor(Qt::PointingHandCursor);
    sparkle_btn_->setToolTip(QStringLiteral("Search"));
    sparkle_btn_->setIconSize(QSize(18, 18));
    connect(sparkle_btn_, &QToolButton::clicked, this, &HomePage::submit);
    pill_layout->addWidget(sparkle_btn_, 0, Qt::AlignVCenter);
    pill_layout->addSpacing(Spacing::sm);

    column_layout->addWidget(pill);

    // --- Shortcut tiles ----------------------------------------------------
    auto* tiles = new QWidget(column);
    tiles->setObjectName(QStringLiteral("HomeShortcutTile"));
    auto* tiles_layout = new QHBoxLayout(tiles);
    tiles_layout->setContentsMargins(0, 0, 0, 0);
    tiles_layout->setSpacing(Spacing::md);
    const struct {
        const char* label;
        const char* url;
    } kShortcuts[] = {
        {"YouTube", "https://www.youtube.com"},
        {"GitHub", "https://github.com"},
        {"Discord", "https://discord.com"},
        {"Gmail", "https://mail.google.com"},
        {"Drive", "https://drive.google.com"},
    };
    for (const auto& tile : kShortcuts) {
        tiles_layout->addWidget(build_shortcut_tile(
            tiles, QString::fromUtf8(tile.label),
            QString::fromUtf8(tile.url), /*is_add=*/false));
    }
    tiles_layout->addWidget(build_shortcut_tile(
        tiles, QStringLiteral("Add shortcut"), QString(), /*is_add=*/true));
    column_layout->addWidget(tiles);

    root->addWidget(column, 0, Qt::AlignHCenter);
    root->addStretch(3);

    // --- Bottom cards -------------------------------------------------------
    cards_row_ = new QWidget(this);
    cards_row_->setObjectName(QStringLiteral("HomeCardsRow"));
    cards_row_->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* cards_layout = new QHBoxLayout(cards_row_);
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(Spacing::lg);
    cards_row_->setMaximumWidth(1120);

    QWidget* resume = build_history_card(
        cards_row_, QStringLiteral("Continue where you left off"),
        /*with_progress=*/true);
    resume_rows_ = resume->findChild<QVBoxLayout*>(
        QStringLiteral("HomeRows"));
    cards_layout->addWidget(resume, 1);

    QWidget* recent = build_history_card(
        cards_row_, QStringLiteral("Recently visited"),
        /*with_progress=*/false);
    recent_rows_ = recent->findChild<QVBoxLayout*>(
        QStringLiteral("HomeRows"));
    cards_layout->addWidget(recent, 1);

    cards_layout->addWidget(build_actions_card(cards_row_), 1);

    root->addWidget(cards_row_, 0, Qt::AlignHCenter);
}

QWidget* HomePage::build_weather_chip() {
    auto* chip = new QWidget(this);
    chip->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* row = new QHBoxLayout(chip);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(Spacing::sm);
    weather_icon_ = new QLabel(chip);
    weather_icon_->setFixedSize(22, 22);
    row->addWidget(weather_icon_, 0, Qt::AlignVCenter);
    auto* texts = new QVBoxLayout;
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(0);
    auto* temp = new QLabel(QStringLiteral("14°C"), chip);
    temp->setObjectName(QStringLiteral("HomeWeatherTemp"));
    texts->addWidget(temp);
    auto* sub = new QLabel(QStringLiteral("Partly cloudy"), chip);
    sub->setObjectName(QStringLiteral("HomeWeatherSub"));
    texts->addWidget(sub);
    row->addLayout(texts);
    return chip;
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
                                       const QString& target_url,
                                       bool is_add) {
    auto* tile = new QToolButton(parent);
    tile->setObjectName(QStringLiteral("HomeShortcut"));
    tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    tile->setText(label);
    tile->setCursor(Qt::PointingHandCursor);
    tile->setFixedSize(106, 84);
    tile->setIconSize(QSize(30, 30));
    tile->setProperty("auroraTileAdd", is_add);
    tile->setProperty("auroraTileLabel", label);
    connect(tile, &QToolButton::clicked, this, [this, target_url, is_add] {
        if (is_add) {
            emit status_message(QStringLiteral(
                "Custom shortcuts are not available yet"));
            return;
        }
        if (!target_url.isEmpty()) emit open_url_requested(target_url);
    });
    shortcut_buttons_.append(tile);
    return tile;
}

QWidget* HomePage::build_history_card(QWidget* parent, const QString& heading,
                                      bool with_progress) {
    auto* card = new QWidget(parent);
    card->setObjectName(QStringLiteral("HomeCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(Spacing::lg, Spacing::md, Spacing::lg,
                                    Spacing::md);
    card_layout->setSpacing(Spacing::xs);

    auto* header_row = new QHBoxLayout;
    header_row->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(heading, card);
    title->setObjectName(QStringLiteral("HomeCardTitle"));
    header_row->addWidget(title);
    header_row->addStretch(1);
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
    rows->setSpacing(Spacing::xs);
    card_layout->addLayout(rows);
    card_layout->addStretch(1);

    card->setProperty("withProgress", with_progress);
    return card;
}

QWidget* HomePage::build_actions_card(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName(QStringLiteral("HomeCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(Spacing::lg, Spacing::md, Spacing::lg,
                                    Spacing::md);
    card_layout->setSpacing(Spacing::xs);

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
        row->setIconSize(QSize(16, 16));
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

    QVector<HistoryEntry> resume = resume_entries_;
    QVector<HistoryEntry> recent = recent_entries_;
    if (history_) {
        const auto visits = history_->recent(6);
        if (!visits.empty()) {
            resume.clear();
            recent.clear();
            for (const auto& v : visits) {
                HistoryEntry entry;
                entry.url = QString::fromStdString(v.url);
                entry.title = v.title.empty()
                    ? entry.url : QString::fromStdString(v.title);
                entry.when_epoch = static_cast<std::int64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        v.last_visit.time_since_epoch()).count());
                recent.append(entry);
            }
            resume = recent;
        }
    }

    // The progress strip is a visual "how far in" hint derived from the row's
    // age: newer rows are further along. Real page-progress is not tracked.
    int index = 0;
    for (const HistoryEntry& entry : resume) {
        render_entry(resume_rows_, entry, /*with_progress=*/true);
        if (++index >= 3) break;
    }
    index = 0;
    for (const HistoryEntry& entry : recent) {
        render_entry(recent_rows_, entry, /*with_progress=*/false);
        if (++index >= 3) break;
    }
}

void HomePage::render_entry(QVBoxLayout* rows, const HistoryEntry& entry,
                            bool with_progress) {
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
    icon_label->setFixedSize(28, 28);
    icon_label->setAlignment(Qt::AlignCenter);
    QIcon site = FaviconCache::instance().icon_for_url(entry.url);
    if (!site.isNull()) {
        icon_label->setPixmap(site.pixmap(28, 28));
    } else {
        const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
        const QString name = entry.title.section(
            QStringLiteral("—"), 0, 0).trimmed();
        icon_label->setPixmap(letter_badge(
            name.isEmpty() ? entry.title : name,
            brand_color(name.isEmpty() ? entry.title : name), 28, dpr));
    }
    row_layout->addWidget(icon_label, 0, Qt::AlignTop);

    auto* texts = new QVBoxLayout;
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(Spacing::xxs);
    auto* title = new QLabel(entry.title, row);
    title->setObjectName(QStringLiteral("HomeRowTitle"));
    texts->addWidget(title);
    auto* sub = new QLabel(FaviconCache::host_of(entry.url), row);
    sub->setObjectName(QStringLiteral("HomeRowSub"));
    texts->addWidget(sub);
    if (with_progress) {
        auto* bar = new QProgressBar(row);
        bar->setObjectName(QStringLiteral("HomeRowProgress"));
        bar->setRange(0, 100);
        // Deterministic pseudo-progress: spread the rows so the column
        // doesn't show three identical bars.
        const int ordinal = rows->count() + 1;
        bar->setValue(30 + ordinal * 20 > 92 ? 92 : 30 + ordinal * 20);
        bar->setProperty("auroraBarOrdinal", ordinal);
        texts->addSpacing(Spacing::xxs);
        texts->addWidget(bar);
    }
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
    const Theme& t = current_theme();
    const ThemeColors& c = t.colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Deep cosmic base: navy falling off to near-black at the bottom corners.
    QLinearGradient base(rect().topLeft(), rect().bottomLeft());
    base.setColorAt(0.0, QColor(11, 17, 32));
    base.setColorAt(0.55, c.background);
    base.setColorAt(1.0, QColor(4, 6, 14));
    p.fillRect(rect(), base);

    // Aurora glows: cyan bloom on the left edge, magenta on the right, a
    // quiet violet core overhead. Subtle: the content owns the page.
    const qreal w = width(), h = height();
    paint_blob(p, QPointF(w * -0.02, h * 0.35), qMax(w, h) * 0.52,
               c.accent, 66);
    paint_blob(p, QPointF(w * 0.16, h * 0.72), qMax(w, h) * 0.44,
               c.accent, 40);
    paint_blob(p, QPointF(w * 1.02, h * 0.30), qMax(w, h) * 0.52,
               c.accent2, 72);
    paint_blob(p, QPointF(w * 0.88, h * 0.74), qMax(w, h) * 0.42,
               c.accent3, 46);
    paint_blob(p, QPointF(w * 0.5, h * 0.12), qMax(w, h) * 0.30,
               c.accent2, 22);

    // A faint starfield of pinned dots keeps the navy from reading as a flat
    // fill. Deterministic positions: repaint-stable, no allocations.
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 64; ++i) {
        const quint32 x = static_cast<quint32>(i * 2654435761u);
        const quint32 y = static_cast<quint32>((i + 31) * 2246822519u);
        const qreal sx = (x % 1000) / 1000.0 * w;
        const qreal sy = (y % 1000) / 1000.0 * h;
        QColor star(200, 220, 255);
        star.setAlpha(14 + (x >> 24) % 26);
        p.setBrush(star);
        const qreal r = 0.6 + ((y >> 24) % 3) * 0.4;
        p.drawEllipse(QPointF(sx, sy), r, r);
    }

    QWidget::paintEvent(event);
}

void HomePage::apply_icons() {
    const Theme& theme = current_theme();
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    if (search_btn_) {
        search_btn_->setIcon(icons::icon(icons::Name::Search,
                                         theme.colors.text_muted, 18, dpr));
    }
    if (sparkle_btn_) {
        sparkle_btn_->setIcon(icons::icon(icons::Name::Sparkle,
                                          theme.colors.accent2, 18, dpr));
    }
    if (weather_icon_) {
        weather_icon_->setPixmap(icons::pixmap(
            icons::Name::CloudSun, 22, theme.colors.text_muted, dpr));
    }
    for (QToolButton* tile : shortcut_buttons_) {
        const bool is_add = tile->property("auroraTileAdd").toBool();
        const QString label =
            tile->property("auroraTileLabel").toString();
        if (is_add) {
            tile->setIcon(icons::icon(icons::Name::Plus,
                                      theme.colors.text_muted, 22, dpr));
        } else {
            tile->setIcon(QIcon(letter_badge(label, brand_color(label), 30,
                                             dpr)));
        }
    }
    for (QToolButton* row : action_buttons_) {
        const auto icon = static_cast<icons::Name>(
            row->property("auroraActionIcon").toInt());
        row->setIcon(icons::icon(icon, theme.colors.text_muted, 16, dpr));
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

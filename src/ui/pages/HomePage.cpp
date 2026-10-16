#include "ui/pages/HomePage.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/Url.hpp"
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

} // namespace

HomePage::HomePage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("HomePage"));
    // paintEvent fills the whole rect; the sheet only supplies the tokens.
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(Spacing::xl, Spacing::xl, Spacing::xl, Spacing::xl);
    root->setSpacing(Spacing::md);

    root->addStretch(2);

    // Brand block: the wordmark, then the big centered search pill — the
    // mockup's top-center search bar presented as the new-tab centerpiece, so
    // a fresh tab is one keystroke from a search.
    auto* column = new QWidget(this);
    column->setObjectName(QStringLiteral("HomeColumn"));
    column->setAttribute(Qt::WA_TranslucentBackground, true);
    // Wide but bounded: the pill reads as a search bar, not an edge-to-edge
    // input, whatever the window's width.
    column->setMaximumWidth(640);
    auto* column_layout = new QVBoxLayout(column);
    column_layout->setContentsMargins(0, 0, 0, 0);
    column_layout->setSpacing(Spacing::xl);

    auto* wordmark = new QLabel(QStringLiteral("aurora"), column);
    wordmark->setObjectName(QStringLiteral("HomeWordmark"));
    wordmark->setAlignment(Qt::AlignCenter);
    wordmark->setAttribute(Qt::WA_TranslucentBackground, true);
    column_layout->addWidget(wordmark);

    auto* pill = new QWidget(column);
    pill->setObjectName(QStringLiteral("HomeSearchPillHost"));
    pill->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* pill_layout = new QHBoxLayout(pill);
    pill_layout->setContentsMargins(0, 0, 0, 0);
    pill_layout->setSpacing(0);

    search_ = new QLineEdit(pill);
    search_->setObjectName(QStringLiteral("HomeSearchPill"));
    search_->setPlaceholderText(
        QStringLiteral("Search or type an address"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumHeight(54);
    connect(search_, &QLineEdit::returnPressed, this, &HomePage::submit);
    connect(search_, &QLineEdit::textChanged, this,
            &HomePage::on_query_changed);
    pill_layout->addWidget(search_, 1);

    search_btn_ = new QToolButton(pill);
    search_btn_->setObjectName(QStringLiteral("HomeSearchButton"));
    search_btn_->setCursor(Qt::PointingHandCursor);
    search_btn_->setToolTip(QStringLiteral("Search"));
    search_btn_->setIconSize(QSize(20, 20));
    search_btn_->setIcon(icons::icon(icons::Name::Search,
                                     current_theme().colors.background, 20));
    connect(search_btn_, &QToolButton::clicked, this, &HomePage::submit);
    pill_layout->addWidget(search_btn_, 0, Qt::AlignVCenter);
    // A quiet trailing gutter so the orb floats inside the pill rather than
    // butting against its border.
    pill_layout->addSpacing(Spacing::xs);

    column_layout->addWidget(pill);
    root->addWidget(column, 0, Qt::AlignHCenter);
    root->addStretch(3);

    // Rest state: the page starts with the pill unfocused and a large
    // circular orb floating bottom-right (the mockup's "tap to search"
    // affordance). Typing (or clicking the orb) moves intent to the pill.
    orb_ = new QToolButton(this);
    orb_->setObjectName(QStringLiteral("HomeSearchOrb"));
    orb_->setCursor(Qt::PointingHandCursor);
    orb_->setToolTip(QStringLiteral("Search"));
    orb_->setIconSize(QSize(24, 24));
    orb_->setIcon(icons::icon(icons::Name::Search,
                              current_theme().colors.background, 24));
    orb_->setFixedSize(64, 64);
    // The shared sheet sizes both orbs at 44px for the in-pill gutter; the
    // floating orb is the rest-state hero and overrides to the mockup's big
    // circle. Same object name, explicit radius, so it stays a perfect disc.
    orb_->setStyleSheet(QStringLiteral(
        "QToolButton#HomeSearchOrb { min-width: 64px; max-width: 64px;"
        "  min-height: 64px; max-height: 64px; border-radius: 32px; }"));
    // Children of the page itself (not its layout) so it can float anywhere
    // in the widget's rect; position_orb() owns its geometry.
    connect(orb_, &QToolButton::clicked, this, [this] {
        search_->setFocus(Qt::ShortcutFocusReason);
    });
    orb_->show();

    // Keep both orb tints in step with the live theme.
    theme_observer_ = add_theme_observer([this](const Theme& theme) {
        if (search_btn_) {
            search_btn_->setIcon(icons::icon(icons::Name::Search,
                                             theme.colors.background, 20));
        }
        if (orb_) {
            orb_->setIcon(icons::icon(icons::Name::Search,
                                      theme.colors.background, 24));
        }
        update();
    });
}

HomePage::~HomePage() {
    if (theme_observer_ != 0) remove_theme_observer(theme_observer_);
}

void HomePage::set_query_url_resolver(SearchUrlFor fn) {
    query_url_for_ = std::move(fn);
}

void HomePage::paintEvent(QPaintEvent* event) {
    const Theme& t = current_theme();
    const ThemeColors& c = t.colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Base glass: a very subtle vertical falloff from the card tint toward
    // the window's transparent black so the bottom edge never band-steps.
    QLinearGradient base(rect().topLeft(), rect().bottomLeft());
    QColor top = c.card_bg;
    QColor bottom = c.background;
    base.setColorAt(0.0, top);
    base.setColorAt(1.0, bottom);
    p.fillRect(rect(), base);

    // The aurora nebula from the mockup: teal bloom lower-left, violet upper
    // right, a rose accent pulled from the accent/danger midpoint so the
    // third lobe stays on-palette without a new token.
    const qreal w = width(), h = height();
    paint_blob(p, QPointF(w * 0.82, h * 0.16), qMax(w, h) * 0.42,
               c.accent2, 74);
    paint_blob(p, QPointF(w * 0.12, h * 0.88), qMax(w, h) * 0.46,
               c.accent, 60);
    QColor rose(c.accent2.red() + (c.danger.red() - c.accent2.red()) / 3,
                c.accent2.green() + (c.danger.green() - c.accent2.green()) / 3,
                c.accent2.blue() + (c.danger.blue() - c.accent2.blue()) / 3);
    paint_blob(p, QPointF(w * 0.94, h * 0.66), qMax(w, h) * 0.30, rose, 44);
    paint_blob(p, QPointF(w * 0.30, h * 0.24), qMax(w, h) * 0.26,
               c.accent2, 30);

    // A faint bottom-edge vignette keeps the content card grounded on the
    // transparent-black window beneath.
    QLinearGradient vignette(QPointF(0, h * 0.72), QPointF(0, h));
    vignette.setColorAt(0.0, QColor(0, 0, 0, 0));
    vignette.setColorAt(1.0, QColor(0, 0, 0, 90));
    p.fillRect(rect(), vignette);

    QWidget::paintEvent(event);
}

void HomePage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    position_orb();
}

void HomePage::position_orb() {
    if (!orb_) return;
    const int margin = Spacing::xl;
    orb_->move(width() - orb_->width() - margin,
               height() - orb_->height() - margin);
}

void HomePage::on_query_changed(const QString& text) {
    // While the pill is empty the page sits in its rest state: orb prominent,
    // pill inert. Any content suppresses the floating orb so the layout is
    // unambiguous about where the next keystroke goes.
    if (orb_) orb_->setVisible(text.trimmed().isEmpty());
}

void HomePage::submit() {
    const QString query = search_->text().trimmed();
    if (query.isEmpty()) return;

    QString target = query_url_for_
                         ? query_url_for_(query)
                         : QString();
    // No resolver wired yet (page created before the window finished its
    // search-engine setup): fall back to the raw text and let the caller's
    // own omnibox parsing route it. Never block input on chrome state.
    if (target.isEmpty()) target = query;
    search_->clear();
    emit status_message(QStringLiteral("Searching for %1").arg(query));
    emit open_url_requested(target);
}

} // namespace aurora::ui

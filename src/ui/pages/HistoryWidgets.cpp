// Aurora — implementation for the history-page helper widgets.
//
// All of this is ordinary widget work: these classes exist so HistoryPage
// stays focused on the data flow (service -> buckets -> rows), and they are
// the only pieces that need custom painting or dedicated signals.
#include "ui/pages/HistoryWidgets.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QStyle>
#include <QVBoxLayout>

#include <chrono>

#include "history/HistoryService.hpp"
#include "ui/FaviconCache.hpp"
#include "ui/GlowFrame.hpp"
#include "ui/Icons.hpp"
#include "ui/PageBackground.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

inline qreal current_dpr(const QWidget* w) {
    return w && w->screen() ? w->screen()->devicePixelRatio() : 1.0;
}

} // namespace

// RoundCheck ----------------------------------------------------------------
RoundCheck::RoundCheck(QWidget* parent) : QCheckBox(parent) {
    setObjectName(QStringLiteral("HistoryCheck"));
    setFixedSize(20, 20);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void RoundCheck::paintEvent(QPaintEvent*) {
    const ThemeColors& c = current_theme().colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(2, 2, -2, -2);
    if (isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        p.drawRoundedRect(box, 5, 5);
        // Tick, in the dark surface color so it reads on the cyan fill.
        QPainterPath tick;
        tick.moveTo(box.left() + box.width() * 0.26, box.top() + box.height() * 0.54);
        tick.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
        tick.lineTo(box.left() + box.width() * 0.76, box.top() + box.height() * 0.30);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c.background, 1.8, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(tick);
    } else {
        QColor edge = underMouse() ? c.accent : c.text_muted;
        if (!underMouse()) edge.setAlpha(170);
        p.setPen(QPen(edge, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box.adjusted(0.7, 0.7, -0.7, -0.7), 5, 5);
    }
}

// ElidedLabel ---------------------------------------------------------------
ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {
    // Ignored: the layout decides the width, the text adapts to it.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

QSize ElidedLabel::minimumSizeHint() const {
    return {0, QLabel::minimumSizeHint().height()};
}

void ElidedLabel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // The sheet's `color` lands in the palette when the label is polished.
    p.setPen(palette().color(foregroundRole()));
    p.setFont(font());
    const QRect area = contentsRect();
    p.drawText(area, int(alignment()),
               fontMetrics().elidedText(text(), Qt::ElideRight, area.width()));
}

// FilterRadio ---------------------------------------------------------------
FilterRadio::FilterRadio(QWidget* parent) : QWidget(parent) {
    setFixedSize(16, 16);
    // Clicks belong to the row that hosts this dot.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void FilterRadio::setChecked(bool on) {
    if (checked_ == on) return;
    checked_ = on;
    update();
}

void FilterRadio::paintEvent(QPaintEvent*) {
    const ThemeColors& c = current_theme().colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = rect().adjusted(1.25, 1.25, -1.25, -1.25);
    // Slate ring at rest, cyan ring when this row is the active filter.
    p.setPen(QPen(checked_ ? c.accent : c.toggle_border_off, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r);
    if (checked_) {
        // The mocks draw a filled blue core dot on the active row, not a
        // hollow ring.
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        const qreal d = r.width() * 0.52;
        p.drawEllipse(QRectF(r.center() - QPointF(d / 2, d / 2),
                             QSizeF(d, d)));
    }
}

// FilterRow -----------------------------------------------------------------
FilterRow::FilterRow(const QString& label, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("HistoryFilterItem"));
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 7, 10, 7);
    row->setSpacing(10);
    bar_ = new QFrame(this);
    bar_->setObjectName(QStringLiteral("HistoryFilterBar"));
    bar_->setFixedWidth(3);
    row->addWidget(bar_);
    radio_ = new FilterRadio(this);
    row->addWidget(radio_);
    auto* text = new QLabel(label, this);
    row->addWidget(text, 1);
    setSelected(false);
}

void FilterRow::setSelected(bool on) {
    setProperty("selected", on ? "true" : "false");
    bar_->setVisible(on);
    radio_->setChecked(on);
    // The [selected="true"] selector only paints after a re-polish.
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void FilterRow::mousePressEvent(QMouseEvent*) { emit pressed(); }

// HistoryRow ----------------------------------------------------------------
HistoryRow::HistoryRow(QString url, QWidget* parent)
    : QWidget(parent), url_(std::move(url)) {
    setObjectName(QStringLiteral("HistoryRow"));
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(12, 7, 12, 7);
    row->setSpacing(12);

    check_ = new RoundCheck(this);
    row->addWidget(check_);

    favicon_ = new QLabel(this);
    favicon_->setFixedSize(24, 24);
    favicon_->setAlignment(Qt::AlignCenter);
    row->addWidget(favicon_);

    // Fixed stretch ratio + eliding labels: every row's domain and time sit
    // in the same columns whatever the title length.
    title_ = new ElidedLabel(this);
    title_->setObjectName(QStringLiteral("HistoryRowTitle"));
    row->addWidget(title_, 5);

    domain_ = new ElidedLabel(this);
    domain_->setObjectName(QStringLiteral("HistoryRowDomain"));
    row->addWidget(domain_, 3);

    time_ = new QLabel(this);
    time_->setObjectName(QStringLiteral("HistoryRowTime"));
    time_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    time_->setFixedWidth(76);
    row->addWidget(time_);
}

void HistoryRow::set_visit(const history::Visit& v) {
    title_->setText(v.title.empty() ? QString::fromStdString(v.url)
                                    : QString::fromStdString(v.title));
    domain_->setText(FaviconCache::host_of(QString::fromStdString(v.url)));
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          v.last_visit.time_since_epoch())
                          .count();
    time_->setText(format_history_time(secs));
    setProperty(kHistoryIdProperty, QVariant::fromValue(v.id));
    // The favicon cache resolves asynchronously; icon_ready re-stamps the
    // row later, so paint a neutral globe until then instead of a blank.
    const QIcon icon =
        FaviconCache::instance().icon_for_url(QString::fromStdString(v.url));
    if (icon.isNull()) {
        favicon_->setPixmap(icons::pixmap(icons::Name::Globe, 16,
                                          current_theme().colors.text_muted,
                                          current_dpr(this)));
    } else {
        favicon_->setPixmap(icon.pixmap(22));
    }
}

void HistoryRow::refresh_icon(const QString& host) {
    if (FaviconCache::host_of(url_) != host) return;
    const QIcon icon = FaviconCache::instance().icon_for_url(url_);
    if (!icon.isNull()) favicon_->setPixmap(icon.pixmap(22));
}

qint64 HistoryRow::id() const {
    return property(kHistoryIdProperty).toLongLong();
}
bool HistoryRow::checked() const { return check_->isChecked(); }

void HistoryRow::mouseDoubleClickEvent(QMouseEvent*) {
    if (!url_.isEmpty()) emit activated(url_);
}

// HistoryGroupCard -----------------------------------------------------------
HistoryGroupCard::HistoryGroupCard(const QString& title, QWidget* parent)
    : GlowFrame(parent) {
    setObjectName(QStringLiteral("HistoryGroupCard"));
    apply_card_style(14, 3);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 14, 18, 12);
    outer->setSpacing(4);

    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("HistoryGroupHeader"));
    header_->setCursor(Qt::PointingHandCursor);
    auto* head = new QHBoxLayout(header_);
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(10);

    auto* title_label = new QLabel(title, header_);
    title_label->setObjectName(QStringLiteral("HistoryGroupTitle"));
    head->addWidget(title_label);

    count_label_ = new QLabel(header_);
    count_label_->setObjectName(QStringLiteral("HistoryGroupCount"));
    count_label_->setAlignment(Qt::AlignCenter);
    count_label_->setFixedHeight(22);
    count_label_->setMinimumWidth(38);
    head->addWidget(count_label_);

    head->addStretch(1);

    chevron_ = new QLabel(header_);
    chevron_->setFixedSize(18, 18);
    head->addWidget(chevron_);
    update_chevron();

    outer->addWidget(header_);

    rows_host_ = new QWidget(this);
    rows_host_->setObjectName(QStringLiteral("HistoryGroupRows"));
    rows_ = new QVBoxLayout(rows_host_);
    rows_->setContentsMargins(0, 6, 0, 0);
    rows_->setSpacing(1);
    outer->addWidget(rows_host_);

    // The whole header strip toggles the group, like the mock's cards.
    header_->installEventFilter(this);
}

void HistoryGroupCard::set_visits(const QVector<history::Visit>& visits) {
    while (QLayoutItem* item = rows_->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    row_widgets_.clear();
    for (const auto& v : visits) {
        auto* row = new HistoryRow(QString::fromStdString(v.url), rows_host_);
        row->set_visit(v);
        connect(row, &HistoryRow::activated, this,
                &HistoryGroupCard::row_activated);
        rows_->addWidget(row);
        row_widgets_.push_back(row);
    }
    count_label_->setText(QString::number(visits.size()));
}

void HistoryGroupCard::set_collapsed(bool collapsed) {
    if (collapsed_ == collapsed) return;
    collapsed_ = collapsed;
    rows_host_->setVisible(!collapsed_);
    update_chevron();
}

QVector<qint64> HistoryGroupCard::selected_ids() const {
    QVector<qint64> ids;
    for (const auto* row : row_widgets_)
        if (row->checked()) ids.push_back(row->id());
    return ids;
}

void HistoryGroupCard::refresh_icon(const QString& host) {
    for (auto* row : row_widgets_) row->refresh_icon(host);
}

bool HistoryGroupCard::eventFilter(QObject* watched, QEvent* event) {
    if (watched == header_ && event->type() == QEvent::MouseButtonRelease) {
        toggle();
        return true;
    }
    return GlowFrame::eventFilter(watched, event);
}

void HistoryGroupCard::toggle() { set_collapsed(!collapsed_); }

void HistoryGroupCard::update_chevron() {
    chevron_->setPixmap(icons::pixmap(
        collapsed_ ? icons::Name::ChevronRight : icons::Name::ChevronDown,
        16, current_theme().colors.text_muted, current_dpr(this)));
}

} // namespace aurora::ui

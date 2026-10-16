#include "ui/TabHoverCard.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>

#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {
constexpr int kCardWidth = 280;
constexpr int kGapBelowTab = 4;
} // namespace

TabHoverCard::TabHoverCard(QWidget* parent)
    : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint |
                         Qt::WindowDoesNotAcceptFocus) {
    setObjectName(QStringLiteral("TabHoverCard"));
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFixedWidth(kCardWidth);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(Spacing::md, Spacing::sm + Spacing::xxs,
                               Spacing::md, Spacing::sm + Spacing::xxs);
    layout->setSpacing(Spacing::xxs);

    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("TabHoverTitle"));
    address_ = new QLabel(this);
    address_->setObjectName(QStringLiteral("TabHoverAddress"));
    memory_ = new QLabel(this);
    memory_->setObjectName(QStringLiteral("TabHoverMemory"));
    layout->addWidget(title_);
    layout->addWidget(address_);
    layout->addSpacing(Spacing::xs);
    layout->addWidget(memory_);

    apply_style();
}

void TabHoverCard::apply_style() {
    const ThemeColors& c = current_theme().colors;
    setStyleSheet(QStringLiteral(
        "QFrame#TabHoverCard { background: %1; border: 1px solid %2;"
        " border-radius: %3px; }"
        "QLabel { background: transparent; border: none; }"
        "QLabel#TabHoverTitle { color: %4; font-weight: 600; }"
        "QLabel#TabHoverAddress { color: %5; }"
        "QLabel#TabHoverMemory { color: %5; }")
        .arg(c.surface_raised.name(), c.border_subtle.name())
        .arg(Radius::lg)
        .arg(c.text.name(), c.text_muted.name()));
}

QString TabHoverCard::format_memory(qint64 bytes) {
    if (bytes < 0) return {};
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(qRound(mb));
}

void TabHoverCard::set_content(const QString& title, const QString& address,
                               const QString& memory_text) {
    apply_style();   // follow the live theme
    const int text_w = kCardWidth - 2 * Spacing::md;
    title_->setText(QFontMetrics(title_->font()).elidedText(
        title.isEmpty() ? QStringLiteral("New Tab") : title, Qt::ElideRight,
        text_w));
    address_->setText(QFontMetrics(address_->font()).elidedText(
        address, Qt::ElideMiddle, text_w));
    address_->setVisible(!address.isEmpty());
    set_memory_text(memory_text);
}

void TabHoverCard::set_memory_text(const QString& memory_text) {
    memory_->setText(memory_text);
    memory_->setVisible(!memory_text.isEmpty());
    adjustSize();
}

void TabHoverCard::show_below(const QRect& tab_rect_global) {
    adjustSize();
    QPoint pos(tab_rect_global.center().x() - width() / 2,
               tab_rect_global.bottom() + kGapBelowTab);
    const QScreen* screen = QGuiApplication::screenAt(tab_rect_global.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        pos.setX(qBound(area.left() + 4, pos.x(), area.right() - width() - 4));
        pos.setY(qMin(pos.y(), area.bottom() - height() - 4));
    }
    move(pos);
    show();
    raise();
}

} // namespace aurora::ui

#include "ui/ShieldButton.hpp"

#include <QCheckBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/Icons.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/ThemeState.hpp"
#include "ui/ToggleSwitch.hpp"

namespace aurora::ui {

namespace {

constexpr int kPopupWidth = 264;
// Transparent margin around the panel; the stylesheet paints the rounded
// corners, the popup window itself stays rectangular.
constexpr int kPopupMargin = 10;

} // namespace

ShieldButton::ShieldButton(QWidget* parent)
    : IconButton(icons::Name::Shield, parent) {
    setToolTip(QStringLiteral("Shields"));
    setIconPx(18);
    setCursor(Qt::PointingHandCursor);
    // The panel is opened by a normal click (clicked -> toggle_popup); no
    // popup mode is set, because that only affects the built-in menu arrow
    // behavior and there is no QMenu here.
}

void ShieldButton::set_site_blocking_enabled(bool enabled) {
    if (site_enabled_ == enabled) return;
    site_enabled_ = enabled;
    // The glyph carries the whole story at a glance: an intact shield while
    // blocking, a slashed one while the site is allowed.
    setIconName(enabled ? icons::Name::Shield : icons::Name::ShieldOff);
    refresh_popup();
}

void ShieldButton::set_blocked_count(uint64_t count) {
    if (blocked_ == count) return;
    blocked_ = count;
    update();
    refresh_popup();
}

void ShieldButton::set_site(const QString& site) {
    site_ = site;
    setEnabled(!site.isEmpty());
    setToolTip(site.isEmpty()
                   ? QStringLiteral("Shields")
                   : QStringLiteral("Shields — %1").arg(site));
    // Clearing the site (internal page, empty tab) also resets the glyph:
    // otherwise the button would keep the slashed shield of the last site
    // the user turned shields off on, on a page where the toggle does not
    // apply at all.
    if (site.isEmpty()) set_site_blocking_enabled(true);
    refresh_popup();
}

void ShieldButton::set_engine_available(bool available) {
    engine_available_ = available;
    refresh_popup();
}

void ShieldButton::toggle_popup() {
    if (popup_ && popup_->isVisible()) {
        popup_->close();
        return;
    }
    if (!popup_) build_popup();
    refresh_popup();
    fit_and_place_popup();
    popup_->show();
    popup_->raise();
}

void ShieldButton::close_popup() {
    if (popup_) popup_->close();
}

void ShieldButton::paintEvent(QPaintEvent* e) {
    IconButton::paintEvent(e);

    // Badge: requests blocked on this page. Drawn in the button's bottom-right
    // corner, over whatever the theme paints there, in the theme's destructive
    // color so it reads as "N things stopped". Zero hides it — a badge that
    // says "0" is noise, not information.
    if (blocked_ == 0 || !isEnabled()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QString text = blocked_ > 99 ? QStringLiteral("99+")
                                       : QString::number(blocked_);
    QFont f = font();
    f.setPointSizeF(qMax<qreal>(7.5, f.pointSizeF() * 0.6));
    f.setBold(true);
    p.setFont(f);

    const QFontMetrics fm(f);
    const qreal pad = 3.0;
    const qreal text_w = fm.horizontalAdvance(text);
    const qreal badge_w = qMax<qreal>(fm.height(), text_w + 2 * pad);
    const qreal badge_h = fm.height();
    // The glyph lives in the button centre; anchor the badge to the icon's
    // bottom-right so the two read as one mark.
    const QRectF icon = QRectF(rect()).adjusted(
        (width() - iconPx()) / 2.0, (height() - iconPx()) / 2.0,
        -(width() - iconPx()) / 2.0, -(height() - iconPx()) / 2.0);
    const QPointF center(icon.right() - badge_w / 2 + 5,
                         icon.bottom() - badge_h / 2 + 5);
    QRectF badge(center.x() - badge_w / 2, center.y() - badge_h / 2,
                 badge_w, badge_h);

    const QColor bg = current_theme().colors.danger;
    const QColor fg = QColor(Qt::white);

    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(badge, badge_h / 2, badge_h / 2);
    p.setPen(fg);
    p.drawText(badge, Qt::AlignCenter, text);
}

void ShieldButton::build_popup() {
    popup_ = new QWidget(window(), Qt::Popup | Qt::FramelessWindowHint);
    popup_->setAttribute(Qt::WA_TranslucentBackground);
    popup_->setObjectName(QStringLiteral("ShieldPopup"));

    auto* outer = new QVBoxLayout(popup_);
    outer->setContentsMargins(kPopupMargin, kPopupMargin,
                              kPopupMargin, kPopupMargin);
    outer->setSpacing(0);

    auto* panel = new QWidget(popup_);
    panel->setObjectName(QStringLiteral("ShieldPanel"));
    outer->addWidget(panel);

    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("Aurora Shields"), panel);
    title->setObjectName(QStringLiteral("ShieldTitle"));
    lay->addWidget(title);

    site_label_ = new QLabel(panel);
    site_label_->setObjectName(QStringLiteral("ShieldSite"));
    lay->addWidget(site_label_);

    count_label_ = new QLabel(panel);
    count_label_->setObjectName(QStringLiteral("ShieldCount"));
    count_label_->setWordWrap(true);
    lay->addWidget(count_label_);

    auto* separator = new QFrame(panel);
    separator->setObjectName(QStringLiteral("ShieldSeparator"));
    separator->setFrameShape(QFrame::HLine);
    lay->addWidget(separator);

    toggle_ = new ToggleSwitch(QStringLiteral("Block ads on this site"), panel);
    toggle_->setObjectName(QStringLiteral("ShieldToggle"));
    lay->addWidget(toggle_);

    unavailable_note_ = new QLabel(panel);
    unavailable_note_->setObjectName(QStringLiteral("ShieldNote"));
    unavailable_note_->setWordWrap(true);
    unavailable_note_->setText(
        QStringLiteral("The ad-block engine is not built into this build, so "
                       "nothing is being blocked."));
    unavailable_note_->hide();
    lay->addWidget(unavailable_note_);

    // The panel closes on any toggle change — same interaction as Brave: flip
    // the shield, land back on the page. The host decides whether to reload.
    connect(toggle_, &QCheckBox::toggled, this, [this](bool checked) {
        site_enabled_ = checked;
        setIconName(checked ? icons::Name::Shield : icons::Name::ShieldOff);
        emit site_blocking_toggled(checked);
        if (popup_) QTimer::singleShot(120, popup_, &QWidget::close);
    });

    // A click outside dismisses (Qt::Popup does this) — nothing else to wire.
}

void ShieldButton::refresh_popup() {
    if (!popup_) return;

    if (site_label_) {
        site_label_->setText(site_.isEmpty() ? QStringLiteral("This page")
                                             : site_);
    }
    if (count_label_) {
        if (blocked_ == 0) {
            count_label_->setText(
                site_enabled_
                    ? QStringLiteral("No ads or trackers blocked on this page")
                    : QStringLiteral("Shields are down for this site"));
        } else {
            count_label_->setText(site_enabled_
                                      ? QStringLiteral("%1 ads and trackers "
                                                       "blocked on this page")
                                            .arg(blocked_)
                                      : QStringLiteral("%1 requests would have "
                                                       "been blocked")
                                            .arg(blocked_));
        }
    }
    if (toggle_) {
        // Suppress the toggled() signal while syncing from host state: the
        // user did not touch anything, so no site_blocking_toggled emission.
        QSignalBlocker block(toggle_);
        toggle_->setChecked(site_enabled_);
        toggle_->setEnabled(engine_available_);
    }
    if (unavailable_note_) {
        unavailable_note_->setVisible(!engine_available_);
    }
}

void ShieldButton::fit_and_place_popup() {
    popup_->adjustSize();
    // The panel is inset by the transparent margin, so right-aligning the
    // WINDOW with the button would leave the panel visually off by that
    // margin — compensate the same way DownloadsPopover does.
    QPoint pos = dropdown_pos(this, popup_->size(),
                              /*align_right=*/true, Spacing::xs);
    pos.setX(pos.x() + kPopupMargin);
    popup_->move(pos);
}

} // namespace aurora::ui

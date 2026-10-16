#include "ui/LeftDock.hpp"

#include <QFontMetrics>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/FaviconCache.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

LeftDock::LeftDock(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("LeftDock"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(52);

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(Spacing::sm, Spacing::sm, Spacing::sm,
                                Spacing::sm);
    layout_->setSpacing(Spacing::xs);

    // The Aurora emblem at the top of the rail, from the brand artwork.
    auto* logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("DockLogo"));
    logo->setAlignment(Qt::AlignCenter);
    logo->setFixedSize(34, 30);
    const qreal logo_dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    QPixmap mark(QStringLiteral(":/brand/aurora_mark.png"));
    if (!mark.isNull()) {
        mark = mark.scaled(QSize(28, 24) * logo_dpr, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
        mark.setDevicePixelRatio(logo_dpr);
        logo->setPixmap(mark);
    }
    layout_->addWidget(logo, 0, Qt::AlignHCenter);
    layout_->addSpacing(Spacing::xs);

    auto* home = add_item(icons::Name::Home, QStringLiteral("Home"), false);
    connect(home, &QToolButton::clicked, this, &LeftDock::home_requested);
    auto* history = add_item(icons::Name::History, QStringLiteral("History"),
                             false);
    connect(history, &QToolButton::clicked, this, &LeftDock::history_requested);
    auto* downloads = add_item(icons::Name::Downloads,
                               QStringLiteral("Downloads"), false);
    connect(downloads, &QToolButton::clicked, this,
            &LeftDock::downloads_requested);
    auto* bookmarks = add_item(icons::Name::Bookmarks,
                               QStringLiteral("Bookmarks"), false);
    connect(bookmarks, &QToolButton::clicked, this,
            &LeftDock::bookmarks_requested);
    auto* extensions = add_item(icons::Name::Puzzle,
                                QStringLiteral("Extensions"), false);
    connect(extensions, &QToolButton::clicked, this,
            &LeftDock::extensions_requested);

    // "+": add a site to the rail. The window decides what the button offers.
    add_button_ = add_item(icons::Name::Plus,
                           QStringLiteral("Add a site to the sidebar"), false);
    connect(add_button_, &QToolButton::clicked, this, [this] {
        emit add_requested(add_button_->mapToGlobal(
            QPoint(add_button_->width(), 0)));
    });

    // Pinned sites live in their own host so they can be rebuilt in place.
    pins_host_ = new QWidget(this);
    pins_host_->setAttribute(Qt::WA_TranslucentBackground, true);
    pins_layout_ = new QVBoxLayout(pins_host_);
    pins_layout_->setContentsMargins(0, Spacing::xs, 0, 0);
    pins_layout_->setSpacing(Spacing::xs);
    layout_->addWidget(pins_host_, 0, Qt::AlignHCenter);
    pins_host_->hide();

    layout_->addStretch(1);

    auto* settings = add_item(icons::Name::Settings,
                              QStringLiteral("Settings"), true);
    connect(settings, &QToolButton::clicked, this,
            &LeftDock::settings_requested);

    theme_observer_ = add_theme_observer(
        [this](const Theme&) { apply_icons(); apply_pin_icons(); });
    apply_icons();

    // Pinned-site favicons arrive asynchronously.
    connect(&FaviconCache::instance(), &FaviconCache::icon_ready, this,
            [this](const QString&) { apply_pin_icons(); });
}

LeftDock::~LeftDock() {
    if (theme_observer_ != 0) remove_theme_observer(theme_observer_);
}

QToolButton* LeftDock::add_item(icons::Name icon, const QString& tooltip,
                                bool glow) {
    auto* button = new QToolButton(this);
    button->setObjectName(glow ? QStringLiteral("DockSettingsGlow")
                               : QStringLiteral("DockItem"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tooltip);
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(34, 34);
    layout_->addWidget(button, 0, Qt::AlignHCenter);
    entries_.append({icon, button, glow});
    return button;
}

void LeftDock::set_pins(const QVector<Shortcut>& pins) {
    pins_ = pins;
    rebuild_pins();
}

void LeftDock::rebuild_pins() {
    pin_buttons_.clear();
    while (QLayoutItem* item = pins_layout_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    for (int i = 0; i < pins_.size(); ++i) {
        auto* button = new QToolButton(pins_host_);
        button->setObjectName(QStringLiteral("DockItem"));
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(pins_[i].title);
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(34, 34);
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        const QString url = pins_[i].url;
        connect(button, &QToolButton::clicked, this,
                [this, url] { emit pin_open_requested(url); });
        connect(button, &QToolButton::customContextMenuRequested, this,
                [this, button, i](const QPoint& pos) {
                    QMenu menu(this);
                    QAction* edit = menu.addAction(QStringLiteral("Edit…"));
                    QAction* remove = menu.addAction(
                        QStringLiteral("Remove from sidebar"));
                    QAction* chosen = menu.exec(button->mapToGlobal(pos));
                    if (chosen == edit) emit pin_edit_requested(i);
                    else if (chosen == remove) emit pin_remove_requested(i);
                });
        pins_layout_->addWidget(button, 0, Qt::AlignHCenter);
        pin_buttons_.append(button);
    }
    pins_host_->setVisible(!pins_.isEmpty());
    apply_pin_icons();
}

void LeftDock::apply_pin_icons() {
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    for (int i = 0; i < pin_buttons_.size() && i < pins_.size(); ++i) {
        const QIcon site = FaviconCache::instance().icon_for_url(pins_[i].url);
        if (!site.isNull()) {
            pin_buttons_[i]->setIcon(site);
            continue;
        }
        // Until the favicon loads: the site's initial on a quiet disc.
        const int px = 20;
        QPixmap pm(QSize(px, px) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(current_theme().colors.accent2.darker(160));
        p.drawEllipse(QRectF(0, 0, px, px));
        QFont f = font();
        f.setPixelSize(11);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(QColor(QStringLiteral("#F1F5F9")));
        p.drawText(QRectF(0, 0, px, px), Qt::AlignCenter,
                   pins_[i].title.left(1).toUpper());
        p.end();
        pin_buttons_[i]->setIcon(QIcon(pm));
    }
}

void LeftDock::apply_icons() {
    const Theme& theme = current_theme();
    const qreal dpr = screen() ? screen()->devicePixelRatio() : 1.0;
    for (const Entry& entry : entries_) {
        entry.button->setIcon(icons::icon(
            entry.icon,
            entry.glow ? theme.colors.accent : theme.colors.text_muted, 18,
            dpr));
    }
}

} // namespace aurora::ui

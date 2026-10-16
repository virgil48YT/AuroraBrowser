#include "ui/LeftDock.hpp"

#include <QLabel>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

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

    // The aurora dot: a small brand bead at the top of the rail, marking the
    // dock. Painted gradient via the stylesheet (see #DockHomeDot).
    auto* dot = new QLabel(this);
    dot->setObjectName(QStringLiteral("DockHomeDot"));
    dot->setFixedSize(10, 10);
    dot->setAlignment(Qt::AlignCenter);
    layout_->addWidget(dot, 0, Qt::AlignHCenter);
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

    layout_->addStretch(1);

    auto* settings = add_item(icons::Name::Settings,
                              QStringLiteral("Settings"), true);
    connect(settings, &QToolButton::clicked, this,
            &LeftDock::settings_requested);

    theme_observer_ = add_theme_observer(
        [this](const Theme&) { apply_icons(); });
    apply_icons();
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

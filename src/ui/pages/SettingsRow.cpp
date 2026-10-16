#include "ui/pages/SettingsRow.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "ui/Theme.hpp"

namespace aurora::ui {

SettingsRow::SettingsRow(const QString& title, const QString& subtitle,
                         QWidget* control, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("SettingsRow"));
    // The card's stylesheet paints the background; the row must be
    // transparent so its hairline separator is the only visible edge.
    setAttribute(Qt::WA_StyledBackground, false);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(Spacing::lg, 0, Spacing::lg, 0);
    layout_->setSpacing(Spacing::lg);

    if (!title.isEmpty()) {
        auto* text_column = new QVBoxLayout;
        text_column->setContentsMargins(0, Spacing::md - Spacing::xxs, 0,
                                        Spacing::md - Spacing::xxs);
        text_column->setSpacing(Spacing::xxs);

        title_ = new QLabel(title, this);
        title_->setObjectName(QStringLiteral("SettingsRowTitle"));
        title_->setWordWrap(true);
        text_column->addWidget(title_);

        if (!subtitle.isEmpty()) {
            subtitle_ = new QLabel(subtitle, this);
            subtitle_->setObjectName(QStringLiteral("SettingsRowSubtitle"));
            subtitle_->setWordWrap(true);
            text_column->addWidget(subtitle_);
        } else {
            // A single-line row is denser in Chrome: trim the top/bottom air
            // so the control sits on one line-height, not two.
            text_column->setContentsMargins(0, Spacing::sm, 0, Spacing::sm);
        }
        layout_->addLayout(text_column, 1);
    }

    if (control) {
        layout_->addWidget(control, 0, Qt::AlignVCenter);
    }

    search_terms_ = title.toLower();
    if (!subtitle.isEmpty()) {
        search_terms_ += QLatin1Char(' ') + subtitle.toLower();
    }
}

void SettingsRow::add_search_text(const QString& text) {
    if (text.isEmpty()) return;
    if (!search_terms_.isEmpty()) search_terms_ += QLatin1Char(' ');
    search_terms_ += text.toLower();
}

void SettingsRow::add_accessory(QWidget* widget) {
    if (!layout_ || !widget) return;
    layout_->addWidget(widget, 0, Qt::AlignVCenter);
}

void SettingsRow::prepend_leading(QWidget* widget) {
    if (!layout_ || !widget) return;
    layout_->insertWidget(0, widget, 1);
}

} // namespace aurora::ui

#include "ui/FindBar.hpp"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QToolButton>

#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

FindBar::FindBar(QWidget* parent) : QWidget(parent) {
    setObjectName("FindBar");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(6);

    input_ = new QLineEdit(this);
    input_->setObjectName("FindInput");
    input_->setPlaceholderText("Find in page");
    input_->setClearButtonEnabled(true);
    input_->setMinimumWidth(260);
    layout->addWidget(input_);

    count_label_ = new QLabel(this);
    count_label_->setObjectName("FindCount");
    layout->addWidget(count_label_);

    prev_ = new IconButton(icons::Name::ChevronUp, this);
    prev_->setToolTip("Previous match (Shift+Enter)");
    layout->addWidget(prev_);

    next_ = new IconButton(icons::Name::ChevronDown, this);
    next_->setToolTip("Next match (Enter)");
    layout->addWidget(next_);

    close_ = new IconButton(icons::Name::Close, this);
    close_->setToolTip("Close (Esc)");
    layout->addWidget(close_);

    connect(input_, &QLineEdit::textChanged, this, [this] { do_search(true); });
    connect(input_, &QLineEdit::returnPressed, this, [this] {
        do_search(!(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier));
    });
    connect(next_, &QToolButton::clicked, this, [this] { do_search(true); });
    connect(prev_, &QToolButton::clicked, this, [this] { do_search(false); });
    connect(close_, &QToolButton::clicked, this, &FindBar::close_requested);
}

void FindBar::focus_search() {
    input_->setFocus();
    input_->selectAll();
}

void FindBar::set_match_count(int current, int total) {
    current_match_ = current;
    total_matches_ = total;
    count_label_->setText(total > 0
        ? QString("%1 of %2").arg(current).arg(total)
        : QString());
    input_->setToolTip(total > 0
        ? QString("%1 of %2").arg(current).arg(total)
        : QString("No matches"));
}

void FindBar::do_search(bool forward) {
    auto text = input_->text();
    if (text.isEmpty()) {
        count_label_->clear();
        return;
    }
    emit search_requested(text, forward);
}

void FindBar::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        emit close_requested();
        return;
    }
    QWidget::keyPressEvent(e);
}

void FindBar::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    focus_search();
}

void FindBar::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    input_->clear();
    count_label_->clear();
}

} // namespace aurora::ui

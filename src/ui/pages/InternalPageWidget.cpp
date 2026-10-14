#include "ui/pages/InternalPageWidget.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QShowEvent>
#include <QVBoxLayout>

#include "ui/Theme.hpp"

namespace aurora::ui {

namespace {

// Desktop, not mobile. The content column fills the window's width minus a
// comfortable gutter and is only capped on very wide displays -- the previous
// skeleton split the window three ways between two elastic gutters and the
// column, so the card ended up ~a third of the screen with the rest empty
// (and page titles / rows truncated inside it).
constexpr int kMaxContentWidth = 1400;  // only bites on ultrawide displays
// Side gutter. Wide enough to breathe, small enough that the content still
// owns the window. Deliberately no minimum width on the column: the window's
// own 400px minimum must stay achievable, and the body scrolls when narrow.
constexpr int kSideGutter = 56;
constexpr int kTopGutter = 28;

} // namespace

InternalPageWidget::InternalPageWidget(const QString& title, QWidget* parent)
    : QWidget(parent) {
    setObjectName("InternalPage");

    // The title is no longer drawn (see below), but it still identifies the
    // page for accessibility tooling and for anything inspecting the widget.
    setAccessibleName(title);

    // Outer row: a fixed gutter plus a column that takes ALL remaining width.
    // The gutters are zero-stretch: they only reclaim space once the column
    // hits kMaxContentWidth, which keeps the content centred on ultrawides
    // while filling the window everywhere else.
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(kSideGutter, kTopGutter, kSideGutter,
                              Spacing::xl);
    outer->setSpacing(0);
    outer->addStretch(0);

    auto* column = new QWidget(this);
    column->setObjectName("PageColumn");
    column->setMaximumWidth(kMaxContentWidth);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    outer->addWidget(column, 1);
    outer->addStretch(0);

    root_ = new QVBoxLayout(column);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(Spacing::md);

    // --- Header ------------------------------------------------------------
    // No large page title and no subtitle: the tab label and the omnibox
    // already say which page this is, and a 24px heading plus a paragraph of
    // explanation pushed the actual content down the window for no gain.
    // set_subtitle() is kept as an accepted no-op so the pages that call it
    // keep compiling; subtitle_ stays null.
    subtitle_ = nullptr;

    // The actions row is now the top of the page.
    toolbar_host_ = new QWidget(column);
    toolbar_host_->setObjectName("PageToolbar");
    toolbar_ = new QHBoxLayout(toolbar_host_);
    toolbar_->setContentsMargins(0, 0, 0, 0);
    toolbar_->setSpacing(Spacing::sm);
    root_->addWidget(toolbar_host_);

    // Hairline between the actions row and the body, so the controls read as
    // a header band rather than floating on top of the list.
    divider_ = new QFrame(column);
    divider_->setObjectName("PageDivider");
    divider_->setFrameShape(QFrame::NoFrame);
    divider_->setFixedHeight(Metrics::hairline);
    root_->addWidget(divider_);

    empty_label_ = new QLabel(column);
    empty_label_->setObjectName("PageEmptyState");
    empty_label_->setAlignment(Qt::AlignCenter);
    empty_label_->setWordWrap(true);
    empty_label_->hide();
    root_->addWidget(empty_label_, 1);
}

void InternalPageWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // A page with no toolbar controls (there is at least one today, but the
    // base class must not assume it) would otherwise leave an empty row's
    // worth of spacing under the header.
    const bool has_controls = toolbar_ && toolbar_->count() > 0;
    if (toolbar_host_) toolbar_host_->setVisible(has_controls);
    // With no title above it, a lone hairline over the body would read as a
    // stray line; it only makes sense as the underline of an actions row.
    if (divider_) divider_->setVisible(has_controls);
}

QHBoxLayout* InternalPageWidget::toolbar_row() { return toolbar_; }

void InternalPageWidget::set_subtitle(const QString& text) {
    // Intentionally a no-op: the skeleton no longer renders a title/subtitle
    // block. Retained so the pages can keep documenting themselves in code
    // without a header band eating the top of the window.
    Q_UNUSED(text);
}

void InternalPageWidget::set_body(QWidget* body) {
    body_ = body;
    // Insert above the empty-state label so only one of the two is visible.
    root_->insertWidget(root_->count() - 1, body, 1);
}

void InternalPageWidget::set_empty_state(bool visible, const QString& message) {
    if (!message.isEmpty()) empty_label_->setText(message);
    empty_label_->setVisible(visible);
    if (body_) body_->setVisible(!visible);
}

} // namespace aurora::ui

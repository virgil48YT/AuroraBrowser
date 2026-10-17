// Aurora — base class for the native internal pages.
//
// The internal pages (home, history, downloads, bookmarks, settings,
// extensions) are
// real Qt widgets swapped into the content stack in place of the web-content
// host. They share one layout skeleton, so every page has the same rhythm:
//
//   * a content column that fills the window minus a fixed side gutter and is
//     capped only on ultrawide displays (the old three-way elastic split left
//     the content at about a third of the width with dead space either side);
//   * an actions row at the top (search field, buttons) with a hairline under
//     it, hidden when a page adds no controls. There is deliberately no large
//     page title or subtitle: the tab label already names the page;
//   * the body, which fills the rest;
//   * an inline empty-state card so a page with no rows explains itself
//     instead of showing a blank rectangle.
//
// The look of all of it lives in StylesheetBuilder under the #Page* object
// names, so the pages stay layout-only.
//
// Pages talk to the window through signals only, so they never reach into
// MainWindow and stay testable in isolation.
#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>

class QFrame;
class QHBoxLayout;
class QLabel;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QVBoxLayout;

namespace aurora::ui {

class InternalPageWidget : public QWidget {
    Q_OBJECT
public:
    explicit InternalPageWidget(const QString& title, QWidget* parent = nullptr);

    // Re-read the underlying service and rebuild the view. Called every time
    // the page is shown, so it never displays stale rows.
    virtual void refresh() = 0;

signals:
    // The user activated a link on the page (double-clicked a history row,
    // a bookmark, or a download source). The window opens it in a new
    // foreground tab so the internal tool remains available.
    void open_url_requested(const QString& url);

    // Free-form feedback for the status bar ("12 items removed").
    void status_message(const QString& text);

protected:
    // Header block above the body: the page title (bold, one step up) and an
    // optional one-line subtitle. Both live over the toolbar row; an empty
    // subtitle hides only its label, never the title.
    void set_subtitle(const QString& text);
    // Row of controls at the top of the page (search field, action buttons).
    QHBoxLayout* toolbar_row();
    // Header bits for pages that want to restyle/re-arrange the top block
    // (the history page adds a leading icon next to its title, so it needs
    // both the label and the column it lives in).
    QLabel* page_title_label() const { return title_label_; }
    QVBoxLayout* root_layout() const { return root_; }
    // Add the main content widget; it expands to fill the page.
    void set_body(QWidget* body);
    // Show/hide the inline empty state with a given message.
    void set_empty_state(bool visible, const QString& message = QString());

    void showEvent(QShowEvent* event) override;

    // Paint + resize hooks for the shared aurora wallpaper (see .cpp).
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QVBoxLayout* root_ = nullptr;
    QPixmap bg_;            // scaled wallpaper cache; rebuilt on resize
    QPixmap bg_source_;     // ".../brand/aurora_bg.jpg" loaded once
    QLabel* title_label_ = nullptr;
    QWidget* toolbar_host_ = nullptr;
    QHBoxLayout* toolbar_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QFrame* divider_ = nullptr;
    QLabel* empty_label_ = nullptr;
    QWidget* body_ = nullptr;
};

} // namespace aurora::ui

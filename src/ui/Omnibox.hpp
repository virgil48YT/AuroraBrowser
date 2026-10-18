// Aurora — the omnibox / address bar.
//
// A premium first-class browser control: a wide rounded field painted by us
// (cyan -> violet gradient edge, as in the mockup), recessed one step darker
// than the toolbar plate, with a frameless
// transparent QLineEdit for the URL text, a painted leading security/search
// indicator, and a trailing bookmark star. The focus glow animates via a
// qreal property so focusing the bar feels like a gentle engagement, never a
// hard border swap — and, crucially, it ALWAYS animates back to zero.
//
// Focus bookkeeping is defensive by design. The web view's native CEF child
// window is not part of Qt's focus chain, so a click into the page can leave
// Qt believing the line edit is still focused — which is exactly how the
// "focus ring stuck on" bug happened. Four independent mechanisms guarantee
// the ring drops the moment the bar is not focused:
//   1. QApplication::focusChanged (nullptr `now` counts as focus-left),
//   2. an app-wide event filter watching MouseButtonPress / FocusOut,
//   3. a short poll timer that runs only while the ring is lit, and
//   4. on Windows, a native WM_KILLFOCUS guard on the top-level window.
#pragma once

#include <QColor>
#include <QLineEdit>
#include <QPointer>
#include <QPropertyAnimation>
#include <QRectF>
#include <QWidget>

#include <vector>

#include "core/Url.hpp"
#include "omnibox/OmniboxSuggest.hpp"

class QAbstractNativeEventFilter;
class QKeyEvent;
class QTimer;

namespace aurora::ui {

class IconButton;
class OmniboxPopup;

class Omnibox : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal focusProgress READ focusProgress WRITE setFocusProgress)

public:
    enum class Security { None, Secure, Standard, NotSecure };

    explicit Omnibox(QWidget* parent = nullptr);
    ~Omnibox() override;

    void set_current_url(const QString& url, bool select_all);
    QString text() const;
    void focus_search();
    void setSecurity(Security s);

    // Trailing bookmark star. Hidden until the window wires up a bookmarks
    // service and a current page; tinted accent while the page is bookmarked.
    void set_bookmarked(bool bookmarked);
    void set_star_visible(bool visible);

    // Internal aurora:// pages are not bookmarkable, so the star is shown
    // faint and inert there instead of silently doing nothing on click.
    void set_star_enabled(bool enabled);

    // True while the user is typing in the address field. The window must not
    // overwrite the text with an engine-reported URL while this is true (that
    // was the "address bar clears itself mid-typing" bug), and QWidget's own
    // hasFocus() is not enough because the real focus lives on the inner edit.
    bool editing() const;

    // Called by the inner edit when focus changes — drives the focus glow.
    void animate_focus(bool focused);

    // Re-evaluates the glow against ground truth (which widget actually holds
    // focus) and animates it down when it is lit without focus. Safe to call
    // at any time; cheap no-op when the state is already consistent.
    void recheck_focus();

    // Called from the Windows native-event guard when the top-level window
    // receives WM_KILLFOCUS in favor of a window Qt does not track (the CEF
    // child window). Synchronizes Qt's focus record and drops the glow.
    void on_native_focus_lost();

    // --- Suggestion dropdown -------------------------------------------------
    // The window answers query_changed() by calling set_suggestions() with
    // rows for that exact query (it may do so a moment later, e.g. once the
    // search engine replies). Rows for a query the user has since typed past
    // are dropped here, so a slow reply can never repaint stale results.
    // `more_pending` says a fuller answer for this query is on its way (the
    // engine's completions): an open dropdown is then kept as it is rather
    // than closed for a moment and reopened — the typing flicker.
    void set_suggestions(const QString& query,
                         std::vector<omnibox::Suggestion> rows,
                         const QString& engine_name,
                         bool more_pending = false);
    void hide_suggestions();
    bool suggestions_visible() const;

    // Forwarded from the inner edit's key handler. Returns true when the key
    // was consumed (navigating or dismissing the dropdown).
    bool handle_edit_key(QKeyEvent* e);

    qreal focusProgress() const { return focus_progress_; }
    void setFocusProgress(qreal v);

    QSize sizeHint() const override;

signals:
    void navigate_requested(const core::Url& url);
    void bookmark_requested();
    // The user is typing: `text` is the current contents of the address bar.
    void query_changed(const QString& text);
    // A suggested search query was chosen: search for it verbatim, even when
    // it happens to look like an address.
    void search_requested(const QString& text);
    // An "open tab" row was chosen.
    void switch_tab_requested(quint64 tab_id);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void apply_layout();
    QRectF leading_icon_rect() const;
    void activate_row(int index);
    void set_edit_text_quietly(const QString& text);
    QColor interp(const QColor& a, const QColor& b, qreal t) const;

    QLineEdit* edit_ = nullptr;
    IconButton* star_btn_ = nullptr;
    QTimer* focus_watch_ = nullptr;
    OmniboxPopup* popup_ = nullptr;
    QString typed_text_;          // what the user typed, before arrowing over rows
    bool programmatic_edit_ = false;
    bool painted_empty_ = true;   // emptiness the leading glyph was drawn for
    QPointer<QPropertyAnimation> focus_anim_;
#if defined(Q_OS_WIN)
    QAbstractNativeEventFilter* native_guard_ = nullptr;
#endif
    qreal focus_progress_ = 0.0;
    Security security_ = Security::None;
    QString last_url_;
};

} // namespace aurora::ui

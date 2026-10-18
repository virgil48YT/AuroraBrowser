#include "ui/Omnibox.hpp"

#include <QApplication>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#if defined(Q_OS_WIN)
#include <QAbstractNativeEventFilter>
// NOMINMAX so windows.h's min/max macros don't poison headers pulled in
// after it (this TU is part of the UI target, which has no target-wide
// NOMINMAX — only aurora_cef does).
#define NOMINMAX
#include <windows.h>
#endif

#include <cmath>

#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"
#include "ui/OmniboxPopup.hpp"
#include "ui/ThemeState.hpp"
#include "core/InternalUrl.hpp"
#include "core/Url.hpp"

namespace aurora::ui {

namespace {

// Corner radius of the address field (a rounded rectangle, see paintEvent).
constexpr qreal kOmniboxRadius = 10.0;

// A transparent, borderless line edit that delegates focus to the omnibox
// container so the focus glow animates around it.
class NakedEdit : public QLineEdit {
public:
    explicit NakedEdit(QWidget* p) : QLineEdit(p) {
        setFrame(false);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        // No clear button: the mockup's bar carries only the trailing star,
        // and select-all-on-focus already makes replacing the text one step.
        setClearButtonEnabled(false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

protected:
    void focusInEvent(QFocusEvent* e) override {
        QLineEdit::focusInEvent(e);
        if (auto* w = qobject_cast<Omnibox*>(parentWidget())) {
            w->animate_focus(true);
            // Select-all on focus mirrors Chrome/Firefox navigation UX.
            QTimer::singleShot(0, this, [this] { if (hasFocus()) selectAll(); });
        }
    }
    void focusOutEvent(QFocusEvent* e) override {
        QLineEdit::focusOutEvent(e);
        if (auto* w = qobject_cast<Omnibox*>(parentWidget())) {
            w->animate_focus(false);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        // Browser behavior: the FIRST click into an unfocused address bar
        // selects everything so the user can type over it; only later clicks
        // (or drags) place the caret and adjust the selection.
        const bool had_focus = hasFocus();
        QLineEdit::mousePressEvent(e);
        if (!had_focus && hasFocus()) {
            QTimer::singleShot(0, this, [this] {
                if (hasFocus() && !text().isEmpty()) selectAll();
            });
        }
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (auto* w = qobject_cast<Omnibox*>(parentWidget())) {
            if (w->handle_edit_key(e)) return;
        }
        if (e->key() == Qt::Key_Escape) {
            clearFocus();
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};

#if defined(Q_OS_WIN)
// Watches the native message stream for the top-level window losing keyboard
// focus to a window Qt does not track — above all the web content's native
// CEF child window. Qt's own focus bookkeeping can keep crediting the line
// edit in that case, which is exactly how the focus ring got stuck on.
class NativeFocusGuard : public QAbstractNativeEventFilter {
public:
    explicit NativeFocusGuard(Omnibox* box) : box_(box) {}

    bool nativeEventFilter(const QByteArray& type, void* message,
                           qintptr* result) override {
        Q_UNUSED(type);
        Q_UNUSED(result);
        const auto* msg = static_cast<const MSG*>(message);
        if (box_ && msg->message == WM_KILLFOCUS && box_->window() &&
            msg->hwnd == reinterpret_cast<HWND>(box_->window()->winId())) {
            box_->on_native_focus_lost();
        }
        return false;  // observation only — never consume the message
    }

private:
    Omnibox* box_;
};
#endif

} // namespace

Omnibox::Omnibox(QWidget* parent) : QWidget(parent) {
    setObjectName("Omnibox");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(Metrics::omnibox_height);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::StrongFocus);

    edit_ = new NakedEdit(this);
    edit_->setObjectName("OmniboxEdit");
    edit_->setPlaceholderText("Search or enter address");

    // Suggestion dropdown. Created once, filled by set_suggestions(); the
    // omnibox itself never touches history, bookmarks or the network.
    popup_ = new OmniboxPopup(this);
    popup_->hide();
    connect(popup_, &OmniboxPopup::row_activated, this,
            [this](int i) { activate_row(i); });

    // Trailing bookmark star (Chrome-style). Hidden until the window wires up
    // a bookmarks service and a current page.
    star_btn_ = new IconButton(icons::Name::StarOutline, this);
    star_btn_->setObjectName("OmniboxStar");
    star_btn_->setToolTip("Bookmark this page (Ctrl+D)");
    star_btn_->setFixedSize(26, 26);
    star_btn_->setIconPx(15);
    star_btn_->hide();
    connect(star_btn_, &IconButton::clicked, this, &Omnibox::bookmark_requested);

    // Mechanism 1/4 against the stuck focus ring: the line edit's
    // focusOutEvent fires for Qt-internal focus moves, but NOT when focus
    // jumps to the native CEF child window (Qt stops tracking it). Watch
    // application-wide focus changes: a nullptr `now` means focus left Qt
    // entirely, which for the omnibox is still a focus-out.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* now) {
        auto inside = [this](QWidget* w) {
            return w && (w == this || this->isAncestorOf(w));
        };
        if (inside(now)) return;  // focus still within the omnibox
        if (inside(old) || now == nullptr) recheck_focus();
    });

    // Mechanism 2/4: any click anywhere in the app, and any widget losing
    // focus, is a reason to re-evaluate a lit ring (deferred one turn so the
    // press can deliver focus first).
    qApp->installEventFilter(this);

    // Mechanism 3/4: a short poll that runs ONLY while the ring is lit, so a
    // focus jump no Qt signal reports still clears the ring within ~120 ms.
    focus_watch_ = new QTimer(this);
    focus_watch_->setInterval(120);
    connect(focus_watch_, &QTimer::timeout, this, &Omnibox::recheck_focus);

    // Mechanism 4/4 (Windows): the native focus guard. Qt never sees the CEF
    // child window, but Windows still tells our top-level HWND about it.
#if defined(Q_OS_WIN)
    native_guard_ = new NativeFocusGuard(this);
    qApp->installNativeEventFilter(native_guard_);
#endif

    // App-level deactivation: drop the glow while another application is
    // active, and restore it if the edit still owns focus on return.
    connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
        if (!edit_) return;
        if (s == Qt::ApplicationActive) {
            if (edit_->hasFocus()) animate_focus(true);
        } else if (focus_progress_ > 0.0) {
            animate_focus(false);
        }
    });

    connect(edit_, &QLineEdit::returnPressed, this, [this] {
        if (popup_ && popup_->isVisible() && popup_->selected() >= 0) {
            activate_row(popup_->selected());
            return;
        }
        hide_suggestions();
        const std::string typed = edit_->text().toStdString();
        // aurora://history|downloads|bookmarks|settings|extensions address the
        // browser's own native pages. They are navigable even though they are
        // not web URLs, so they are checked before the URL/search heuristic.
        if (core::parse_internal_url(typed).has_value()) {
            emit navigate_requested(core::Url(typed));
            edit_->clearFocus();
            return;
        }
        auto url = core::parse_omnibox_input(typed);
        if (url.is_valid()) {
            emit navigate_requested(url);
            edit_->clearFocus();
        }
    });
    connect(edit_, &QLineEdit::textChanged, this, [this](const QString& t) {
        // The leading indicator (search vs security glyph) only depends on
        // whether the field is empty. Repaint just that glyph, and only when
        // the emptiness flips: repainting the whole bar (glow, gradient edge)
        // on every keystroke was part of the typing flicker. The line edit
        // repaints its own text.
        if (t.isEmpty() != painted_empty_) {
            painted_empty_ = t.isEmpty();
            update(leading_icon_rect().toAlignedRect().adjusted(-2, -2, 2, 2));
        }
        // Cleared (including via the clear button, which does not emit
        // textEdited): nothing left to suggest for.
        if (!programmatic_edit_ && t.trimmed().isEmpty()) {
            typed_text_.clear();
            hide_suggestions();
        }
    });
    // textEdited fires only for the user's own typing, never for setText(), so
    // arrowing over rows (which rewrites the text) cannot re-trigger a lookup.
    connect(edit_, &QLineEdit::textEdited, this, [this](const QString& t) {
        typed_text_ = t;
        if (popup_) popup_->set_selected(-1);
        if (t.trimmed().isEmpty()) {
            hide_suggestions();
            return;
        }
        emit query_changed(t);
    });

    apply_layout();
}

Omnibox::~Omnibox() {
#if defined(Q_OS_WIN)
    if (native_guard_) {
        qApp->removeNativeEventFilter(native_guard_);
        delete native_guard_;
        native_guard_ = nullptr;
    }
#endif
    qApp->removeEventFilter(this);
}

void Omnibox::apply_layout() {
    // Left gutter: the painted leading indicator. Right: the bookmark star.
    // The edit itself carries only a small text inset (see the stylesheet),
    // so the text sits close to the glyph like in a real browser.
    QHBoxLayout* l = new QHBoxLayout(this);
    l->setContentsMargins(36, 0, 8, 0);
    l->setSpacing(4);
    l->addWidget(edit_);
    l->addWidget(star_btn_);
}

QSize Omnibox::sizeHint() const {
    return {520, Metrics::omnibox_height};
}

QColor Omnibox::interp(const QColor& a, const QColor& b, qreal t) const {
    t = qBound(0.0, t, 1.0);
    const auto mix = [t](int x, int y) {
        return static_cast<int>(std::lround(x + (y - x) * t));
    };
    return QColor(mix(a.red(), b.red()),
                  mix(a.green(), b.green()),
                  mix(a.blue(), b.blue()),
                  mix(a.alpha(), b.alpha()));
}

void Omnibox::setFocusProgress(qreal v) {
    focus_progress_ = v;
    // The poll timer exists purely to catch focus jumps Qt cannot see; it
    // runs only while the ring is (partially) lit and stops at zero.
    if (focus_watch_) {
        if (v > 0.0) {
            if (!focus_watch_->isActive()) focus_watch_->start();
        } else {
            focus_watch_->stop();
        }
    }
    update();
}

void Omnibox::animate_focus(bool focused) {
    if (!focused) hide_suggestions();
    // Never stack competing animations: stop the in-flight one first. It was
    // started with DeleteWhenStopped, so stop() also frees it and clears the
    // QPointer.
    if (focus_anim_) focus_anim_->stop();
    auto* a = new QPropertyAnimation(this, "focusProgress", this);
    a->setDuration(focused ? Motion::base : Motion::fast);
    a->setEasingCurve(QEasingCurve::OutCubic);
    a->setStartValue(focus_progress_);
    a->setEndValue(focused ? 1.0 : 0.0);
    focus_anim_ = a;
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void Omnibox::recheck_focus() {
    if (!edit_) return;
    if (!edit_->hasFocus() && !hasFocus() && focus_progress_ > 0.0)
        animate_focus(false);
}

void Omnibox::on_native_focus_lost() {
    // The OS just told us keyboard focus left our window for a window Qt
    // does not track (the CEF child). Sync Qt's focus record with reality
    // and drop the glow.
    if (edit_ && edit_->hasFocus()) edit_->clearFocus();
    if (focus_progress_ > 0.0) animate_focus(false);
}

bool Omnibox::eventFilter(QObject* obj, QEvent* e) {
    Q_UNUSED(obj);
    const auto t = e->type();
    // The dropdown is a separate top-level window, so it does not follow the
    // main window around: close it when the window moves, resizes or loses
    // activation instead of leaving it floating.
    // Only the browser window's own deactivation counts: any other window
    // (a tooltip, the hover card) deactivating must not close the list.
    if (popup_ && popup_->isVisible() && obj == window() &&
        (t == QEvent::Move || t == QEvent::Resize ||
         t == QEvent::WindowDeactivate)) {
        hide_suggestions();
    }
    if ((t == QEvent::MouseButtonPress || t == QEvent::FocusOut) &&
        focus_progress_ > 0.0) {
        // Defer one event-loop turn: the press we are seeing may be the one
        // that (re)focuses the edit, so judge only after it settles.
        QTimer::singleShot(0, this, [this] { recheck_focus(); });
    }
    return QWidget::eventFilter(obj, e);
}

void Omnibox::set_current_url(const QString& url, bool select_all) {
    // Invariant: setting the URL (e.g. while the engine reports load
    // progress) must NEVER light the focus glow — this method intentionally
    // performs no focus work at all.
    last_url_ = url;
    typed_text_.clear();
    hide_suggestions();
    set_edit_text_quietly(url);
    painted_empty_ = edit_->text().isEmpty();
    if (select_all) edit_->selectAll();
    // Derive the security indicator from the URL scheme.
    if (url.startsWith("https://", Qt::CaseInsensitive)) {
        security_ = Security::Secure;
    } else if (url.startsWith("http://", Qt::CaseInsensitive)) {
        security_ = Security::Standard;
    } else if (url.startsWith("about:", Qt::CaseInsensitive) ||
               url.startsWith("aurora:", Qt::CaseInsensitive)) {
        security_ = Security::Standard;
    } else {
        security_ = Security::None;
    }
    update();
}

void Omnibox::set_edit_text_quietly(const QString& text) {
    programmatic_edit_ = true;
    edit_->setText(text);
    programmatic_edit_ = false;
}

void Omnibox::set_suggestions(const QString& query,
                              std::vector<omnibox::Suggestion> rows,
                              const QString& engine_name,
                              bool more_pending) {
    if (!popup_) return;
    // Only show results for what is in the box right now, and only while the
    // user is still typing there.
    if (!edit_->hasFocus() || query.trimmed() != typed_text_.trimmed() ||
        typed_text_.trimmed().isEmpty()) {
        return;
    }
    // A lone default row is just the typed text again; not worth a dropdown.
    if (rows.size() < 2) {
        // ...but while the engine's completions for this text are still on
        // their way, keep an open list on screen as it is. Closing it now
        // and reopening it a moment later is the disappear/reappear flicker
        // (and every hide/show of the native popup window makes the window
        // beneath it repaint). The reply, or its failure, settles the list.
        if (more_pending && popup_->isVisible()) return;
        hide_suggestions();
        return;
    }
    // Same popup window, updated in place: new rows, resized if needed,
    // shown only if it was not already visible.
    popup_->set_rows(std::move(rows), engine_name);
    popup_->present();
}

void Omnibox::hide_suggestions() {
    if (popup_ && (popup_->isVisible() || popup_->has_rows())) popup_->clear_rows();
}

bool Omnibox::suggestions_visible() const {
    return popup_ && popup_->isVisible();
}

bool Omnibox::handle_edit_key(QKeyEvent* e) {
    if (!popup_) return false;
    const int key = e->key();
    const bool plain = !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                           Qt::MetaModifier | Qt::ShiftModifier));
    const bool open = popup_->isVisible();

    if (key == Qt::Key_Escape && open) {
        // First Escape closes the list and puts back what the user typed.
        set_edit_text_quietly(typed_text_);
        edit_->setCursorPosition(typed_text_.size());
        hide_suggestions();
        return true;
    }
    if ((key == Qt::Key_Down || key == Qt::Key_Up) && plain) {
        if (!open) {
            // Down on a closed list asks for suggestions again.
            const QString t = edit_->text();
            if (key == Qt::Key_Down && !t.trimmed().isEmpty() && edit_->hasFocus()) {
                typed_text_ = t;
                emit query_changed(t);
                return true;
            }
            return false;
        }
        const int idx = popup_->move_selection(key == Qt::Key_Down ? 1 : -1);
        const auto* row = popup_->row(idx);
        // Preview the highlighted row in the bar; arrowing back past the first
        // row restores the typed text.
        if (!row || row->kind == omnibox::Kind::Search ||
            (row->kind == omnibox::Kind::Navigate && idx == 0)) {
            set_edit_text_quietly(typed_text_);
        } else {
            set_edit_text_quietly(QString::fromStdString(row->fill));
        }
        edit_->setCursorPosition(edit_->text().size());
        return true;
    }
    return false;
}

void Omnibox::activate_row(int index) {
    const omnibox::Suggestion* row = popup_ ? popup_->row(index) : nullptr;
    if (!row) return;
    // Copy: hiding the popup invalidates the row storage.
    const omnibox::Suggestion s = *row;
    hide_suggestions();
    edit_->clearFocus();

    switch (s.kind) {
        case omnibox::Kind::OpenTab:
            emit switch_tab_requested(static_cast<quint64>(s.tab_id));
            return;
        case omnibox::Kind::RemoteSearch:
            emit search_requested(QString::fromStdString(s.fill));
            return;
        default: break;
    }
    const std::string target = s.fill;
    if (core::parse_internal_url(target).has_value()) {
        emit navigate_requested(core::Url(target));
        return;
    }
    const auto url = core::parse_omnibox_input(target);
    if (url.is_valid()) emit navigate_requested(url);
}

void Omnibox::setSecurity(Security s) {
    security_ = s;
    update();
}

void Omnibox::set_bookmarked(bool bookmarked) {
    if (!star_btn_) return;
    star_btn_->setIconName(bookmarked ? icons::Name::StarFilled : icons::Name::StarOutline);
    // An invalid color resets to the state-derived neutral tint.
    star_btn_->setIconColor(bookmarked ? current_theme().colors.accent : QColor());
    if (star_btn_->isEnabled()) {
        star_btn_->setToolTip(bookmarked ? "Edit bookmark (Ctrl+D)"
                                        : "Bookmark this page (Ctrl+D)");
    }
}

void Omnibox::set_star_visible(bool visible) {
    if (star_btn_) star_btn_->setVisible(visible);
}

void Omnibox::set_star_enabled(bool enabled) {
    if (!star_btn_) return;
    star_btn_->setEnabled(enabled);
    if (!enabled) {
        star_btn_->setToolTip("This page can't be bookmarked");
    }
}

QString Omnibox::text() const {
    return edit_->text();
}

bool Omnibox::editing() const {
    return edit_ && edit_->hasFocus();
}

void Omnibox::focus_search() {
    edit_->setFocus();
    edit_->selectAll();
}

static icons::Name security_icon(Omnibox::Security s) {
    switch (s) {
        case Omnibox::Security::Secure: return icons::Name::Lock;
        case Omnibox::Security::Standard: return icons::Name::Globe;
        case Omnibox::Security::NotSecure: return icons::Name::Info;
        default: return icons::Name::Search;
    }
}

void Omnibox::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const auto& c = current_theme().colors;
    // Inset 2px on every side so the soft outer glow has room to paint
    // inside the widget bounds (Qt clips painting to the widget rect).
    const QRectF r = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
    // Rounded rectangle, not a stadium: the mockup's address bar is a wide
    // field with softly rounded corners that spans the toolbar, with the
    // leading glyph and the trailing star integrated inside it.
    const qreal rad = qMin<qreal>(kOmniboxRadius, r.height() / 2.0);

    const bool hovered = underMouse() && focus_progress_ <= 0.0;

    // Recessed fill (a step DARKER than the toolbar); a gentle hover lift;
    // a calm lift while focused. Contrast comes from tone, not hard borders.
    QColor bg = c.omnibox_bg;
    if (focus_progress_ > 0.0) {
        bg = interp(c.omnibox_bg, c.surface_raised, focus_progress_ * 0.45);
    } else if (hovered) {
        bg = interp(c.omnibox_bg, c.surface_hover, 0.35);
    }
    if (property("incognito").toBool()) bg = incognito_tint(bg);

    // A faint outer glow that only really appears on focus, so the idle bar
    // reads as a calm, integrated field rather than a neon tube.
    const qreal aura = 0.03 + 0.09 * focus_progress_;
    if (aura > 0.0) {
        QColor halo = c.accent2;
        halo.setAlphaF(static_cast<float>(aura));
        QPainterPath outer;
        outer.addRoundedRect(r.adjusted(-2.0, -2.0, 2.0, 2.0), rad + 2.0,
                             rad + 2.0);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawPath(outer);
    }

    QPainterPath path;
    path.addRoundedRect(r, rad, rad);
    // Edge: a left-to-right cyan → violet gradient like the mockup. Idle it
    // is softened toward the fill; focus lifts it to the full accents.
    QLinearGradient edge_grad(r.left(), 0.0, r.right(), 0.0);
    const qreal edge_t = 0.55 + 0.45 * focus_progress_;
    QColor edge_start = interp(c.border_subtle, c.accent, edge_t);
    QColor edge_end = interp(c.border_subtle, c.accent2, edge_t);
    edge_grad.setColorAt(0.0, edge_start);
    edge_grad.setColorAt(1.0, edge_end);
    QPen edge_pen(QBrush(edge_grad), 1.0 + 0.3 * focus_progress_);
    p.setPen(edge_pen);
    p.setBrush(bg);
    p.drawPath(path);

    // Leading indicator: search glyph while the field is empty, otherwise the
    // security state of the current page.
    const bool empty = edit_->text().isEmpty();
    const icons::Name ic = empty ? icons::Name::Search : security_icon(security_);
    const QColor ic_color = (security_ == Security::NotSecure) ? c.error : c.text_muted;
    icons::paint(&p, ic, leading_icon_rect(), ic_color);
}

QRectF Omnibox::leading_icon_rect() const {
    return QRectF(13, (height() - 16) / 2.0, 16, 16);
}

} // namespace aurora::ui

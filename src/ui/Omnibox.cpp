#include "ui/Omnibox.hpp"

#include <QApplication>
#include <QCompleter>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringListModel>
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
#include "ui/ThemeState.hpp"
#include "core/InternalUrl.hpp"
#include "core/Url.hpp"

namespace aurora::ui {

namespace {

// A transparent, borderless line edit that delegates focus to the omnibox
// container so the focus glow animates around it.
class NakedEdit : public QLineEdit {
public:
    explicit NakedEdit(QWidget* p) : QLineEdit(p) {
        setFrame(false);
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setClearButtonEnabled(true);
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

    // Autocomplete populated off-thread by the window; the omnibox does no DB
    // work on the UI thread.
    auto* model = new QStringListModel(this);
    auto* completer = new QCompleter(model, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    edit_->setCompleter(completer);

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
    connect(edit_, &QLineEdit::textChanged, this, [this](const QString&) {
        update();  // refresh leading indicator (search vs security)
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
    l->setContentsMargins(34, 0, 5, 0);
    l->setSpacing(2);
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
    edit_->setText(url);
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
    // Inset 3px on every side so the focus glow has room to paint inside the
    // widget bounds (Qt clips painting to the widget rect).
    const QRectF r = QRectF(rect()).adjusted(3.0, 3.0, -3.0, -3.0);
    // True stadium pill: fully rounded ends. A capsule reads as the address
    // bar of a real browser; a moderate radius reads as a form field.
    const qreal rad = r.height() / 2.0;

    const bool hovered = underMouse() && focus_progress_ <= 0.0;

    // Recessed fill (a step DARKER than the toolbar); a gentle hover lift;
    // a calm lift while focused. Contrast comes from tone, not hard borders.
    QColor bg = c.omnibox_bg;
    if (focus_progress_ > 0.0) {
        bg = interp(c.omnibox_bg, c.surface_raised, focus_progress_ * 0.55);
    } else if (hovered) {
        bg = interp(c.omnibox_bg, c.surface_hover, 0.45);
    }
    if (property("incognito").toBool()) bg = incognito_tint(bg);

    // Aurora neon: a soft outer aura ALWAYS hugs the pill (the mockup's
    // glow), and focusing deepens it. The halo is the same gradient family
    // as the edge, painted twice at growing spread so it fades like light.
    const qreal aura = 0.05 + 0.07 * focus_progress_;
    QColor halo = c.accent2;
    halo.setAlphaF(static_cast<float>(aura));
    QPainterPath outer;
    outer.addRoundedRect(r.adjusted(-3.0, -3.0, 3.0, 3.0), rad + 3.0, rad + 3.0);
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawPath(outer);
    QColor halo2 = c.accent;
    halo2.setAlphaF(static_cast<float>(aura * 0.8));
    QPainterPath outer2;
    outer2.addRoundedRect(r.adjusted(-1.5, -1.5, 1.5, 1.5), rad + 1.5, rad + 1.5);
    p.setBrush(halo2);
    p.drawPath(outer2);

    QPainterPath path;
    path.addRoundedRect(r, rad, rad);
    // Aurora neon: the capsule's edge is the cyan → purple gradient, not a
    // flat slate. The focus animation still lifts it toward full accent; the
    // gradient simply rides along underneath.
    QLinearGradient edge_grad(r.topLeft(), r.bottomRight());
    QColor edge_start = interp(c.accent, c.accent_hover, focus_progress_ * 0.2);
    QColor edge_end = interp(c.accent2, c.accent3, focus_progress_ * 0.2);
    edge_grad.setColorAt(0.0, edge_start);
    edge_grad.setColorAt(1.0, edge_end);
    QPen edge_pen(QBrush(edge_grad), 1.2);
    p.setPen(edge_pen);
    p.setBrush(bg);
    p.drawPath(path);

    // Faint inner accent wash at full focus.
    if (focus_progress_ > 0.05) {
        QColor glow = c.accent;
        glow.setAlphaF(static_cast<float>(0.05 * focus_progress_));
        QPainterPath inner;
        inner.addRoundedRect(r.adjusted(1, 1, -1, -1), rad - 1, rad - 1);
        p.fillPath(inner, glow);
    }

    // Leading indicator: search glyph while the field is empty, otherwise the
    // security state of the current page.
    const bool empty = edit_->text().isEmpty();
    const icons::Name ic = empty ? icons::Name::Search : security_icon(security_);
    const QColor ic_color = (security_ == Security::NotSecure) ? c.error : c.text_muted;
    const qreal icon_y = (height() - 16) / 2.0;
    icons::paint(&p, ic, QRectF(11, icon_y, 16, 16), ic_color);
}

} // namespace aurora::ui

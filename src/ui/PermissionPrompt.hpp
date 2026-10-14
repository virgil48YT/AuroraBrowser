// Aurora — the mic/camera (and notification) permission prompt.
//
// A small styled popup hung under the omnibox, shown when a page requests a
// capability: "example.com wants to use your microphone [Allow] [Block]
// (x) Remember this decision". Policy:
//   * DEFAULT-DENY. Closing/dismissing the popup, clicking outside it, or
//     pressing Escape all DENY. Only an explicit Allow click grants, and it
//     grants exactly the capabilities the page asked for (the engine-side
//     responder was already masked to the request).
//   * PER-ORIGIN, PER-KIND memory via the "Remember" checkbox, persisted by
//     permissions::PermissionStore (Settings prefs table, no schema change).
//   * NEVER auto-grants: a remembered deny answers immediately; anything
//     unknown shows this prompt.
//
// Concurrent requests queue: the engine may fire several (a page asking for
// mic and camera back-to-back, two tabs at once). One popup at a time; each
// dismissed or answered request shows the next.
//
// Header-only on purpose: the app target lists its translation units
// explicitly in CMake, and a header-only widget needs no build-file change.
// (No Q_OBJECT either — AUTOMOC only mocs same-basename headers — so the
// few signal connections use function-pointer slots via lambdas.)
#pragma once

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <deque>
#include <optional>

#include "permissions/PermissionStore.hpp"
#include "ui/Icons.hpp"
#include "ui/PopupPlacement.hpp"
#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"
#include "webview/WebViewController.hpp"

namespace aurora::ui {

class PermissionPrompt : public QWidget {
public:
    explicit PermissionPrompt(QWidget* parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setObjectName(QStringLiteral("PermissionPrompt"));
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(kWidth);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(kShadowL, kShadowT, kShadowR, kShadowB);
        root->setSpacing(0);

        panel_ = new QWidget(this);
        panel_->setObjectName(QStringLiteral("PermissionPromptPanel"));
        panel_->setAttribute(Qt::WA_StyledBackground, true);
        root->addWidget(panel_);

        auto* pl = new QVBoxLayout(panel_);
        pl->setContentsMargins(Spacing::lg, Spacing::lg, Spacing::lg,
                               Spacing::lg);
        pl->setSpacing(Spacing::sm);

        icon_label_ = new QLabel(panel_);
        icon_label_->setObjectName(QStringLiteral("PermissionPromptIcon"));
        icon_label_->setFixedSize(32, 32);
        icon_label_->setAlignment(Qt::AlignCenter);
        pl->addWidget(icon_label_);

        title_ = new QLabel(panel_);
        title_->setObjectName(QStringLiteral("PermissionPromptTitle"));
        title_->setWordWrap(true);
        pl->addWidget(title_);

        remember_ = new QCheckBox(
            QStringLiteral("Remember this decision for this site"), panel_);
        remember_->setObjectName(QStringLiteral("PermissionPromptRemember"));
        pl->addWidget(remember_);

        auto* buttons = new QHBoxLayout();
        buttons->setSpacing(Spacing::sm);
        buttons->addStretch(1);
        block_ = new QPushButton(QStringLiteral("Block"), panel_);
        block_->setObjectName(QStringLiteral("PageButton"));
        allow_ = new QPushButton(QStringLiteral("Allow"), panel_);
        allow_->setObjectName(QStringLiteral("PageAccentButton"));
        allow_->setDefault(true);
        buttons->addWidget(block_);
        buttons->addWidget(allow_);
        pl->addLayout(buttons);

        QObject::connect(allow_, &QPushButton::clicked,
                         [this] { answer(true); });
        QObject::connect(block_, &QPushButton::clicked,
                         [this] { answer(false); });

        hide();
    }

    // Feed a request from the engine. A remembered per-origin decision
    // answers immediately (no popup); otherwise the request queues and the
    // first queued one shows under `anchor`. `store` may be null (window
    // without a profile): then nothing is remembered and every request
    // prompts.
    void enqueue(webview::PermissionRequest req,
                 permissions::PermissionStore* store, QWidget* anchor) {
        // A combined mic+camera request grants ONLY if every requested kind
        // is remembered as allow; a remembered deny for any of them denies
        // the whole request immediately; anything unknown prompts (one
        // prompt covering all undecided kinds).
        if (store) {
            bool all_allow = true;
            const auto check = [&](bool flag, permissions::PermissionKind k) {
                if (!flag) return;
                if (store->decision(req.origin, k) !=
                    permissions::PermissionDecision::Allow)
                    all_allow = false;
            };
            check(req.audio, permissions::PermissionKind::AudioCapture);
            check(req.video, permissions::PermissionKind::VideoCapture);
            check(req.notification, permissions::PermissionKind::Notification);
            if (all_allow) {
                req.respond(true);
                return;
            }
            const auto denied = [&](bool flag, permissions::PermissionKind k) {
                return flag &&
                       store->decision(req.origin, k) ==
                           permissions::PermissionDecision::Deny;
            };
            if (denied(req.audio, permissions::PermissionKind::AudioCapture) ||
                denied(req.video, permissions::PermissionKind::VideoCapture) ||
                denied(req.notification,
                       permissions::PermissionKind::Notification)) {
                req.respond(false);
                return;
            }
        }

        queued_.push_back(std::move(req));
        anchor_ = anchor;
        show_next();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        if (!panel_) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // The same soft three-ring shadow as the downloads popover.
        const QColor base = current_theme().colors.shadow_color;
        const QRectF panel_rect(panel_->geometry());
        p.setPen(Qt::NoPen);
        for (int i = 3; i >= 1; --i) {
            const qreal g = static_cast<qreal>(i) * 2.5;
            QColor ring(base);
            ring.setAlpha(qMax(6, base.alpha() / (2 * i + 2)));
            p.setBrush(ring);
            p.drawRoundedRect(panel_rect.adjusted(-g, -g + 3.0, g, g + 3.0),
                              Radius::xl + g, Radius::xl + g);
        }
    }

    // DEFAULT-DENY: any dismissal (click outside, Escape, window change)
    // answers the pending request with deny, remembered if the box is ticked.
    void hideEvent(QHideEvent* event) override {
        QWidget::hideEvent(event);
        if (pending_) answer(false);
    }

private:
    static constexpr int kWidth = 396 + 8 + 12;
    static constexpr int kShadowL = 8, kShadowT = 6, kShadowR = 12,
                         kShadowB = 14;

    static QString describe(const webview::PermissionRequest& req) {
        QString what;
        if (req.audio && req.video)
            what = QStringLiteral("use your microphone and camera");
        else if (req.audio)
            what = QStringLiteral("use your microphone");
        else if (req.video)
            what = QStringLiteral("use your camera");
        else if (req.notification)
            what = QStringLiteral("show notifications");
        const QString origin = QString::fromStdString(req.origin);
        return QStringLiteral("%1 wants to %2.").arg(origin, what);
    }

    void show_next() {
        if (isVisible() || queued_.empty() || pending_) return;
        pending_.emplace(std::move(queued_.front()));
        queued_.pop_front();

        const auto& req = *pending_;
        title_->setText(describe(req));
        remember_->setChecked(false);
        remember_->setVisible(store_ != nullptr);
        const qreal dpr = devicePixelRatioF();
        const QColor color = current_theme().colors.text;
        const auto icon = req.video      ? icons::Name::Camera
                          : req.audio    ? icons::Name::Mic
                                         : icons::Name::Bell;
        icon_label_->setPixmap(icons::pixmap(icon, 20, color, dpr));

        adjustSize();
        if (anchor_)
            move(dropdown_pos(anchor_, size(), /*align_right=*/false));
        show();
        raise();
    }

    void answer(bool allow) {
        if (!pending_) return;
        auto req = std::move(*pending_);
        pending_.reset();
        if (req.respond) req.respond(allow);
        // Persist only on an explicit click with the box ticked; dismissal
        // (default-deny) is never remembered — the site may ask again.
        if (remember_ && remember_->isChecked() && store_) {
            const auto decision = allow ? permissions::PermissionDecision::Allow
                                        : permissions::PermissionDecision::Deny;
            if (req.audio)
                store_->remember(req.origin,
                                 permissions::PermissionKind::AudioCapture,
                                 decision);
            if (req.video)
                store_->remember(req.origin,
                                 permissions::PermissionKind::VideoCapture,
                                 decision);
            if (req.notification)
                store_->remember(req.origin,
                                 permissions::PermissionKind::Notification,
                                 decision);
        }
        if (isVisible()) hide();  // triggers no re-answer: pending_ is cleared
        show_next();
    }

public:
    // Set once by the window; null-safe (nothing remembered without it).
    void set_store(permissions::PermissionStore* store) { store_ = store; }

private:

    QWidget* panel_ = nullptr;
    QLabel* icon_label_ = nullptr;
    QLabel* title_ = nullptr;
    QCheckBox* remember_ = nullptr;
    QPushButton* allow_ = nullptr;
    QPushButton* block_ = nullptr;
    QWidget* anchor_ = nullptr;
    permissions::PermissionStore* store_ = nullptr;
    // The request currently on screen (exactly one) and the queue behind it.
    std::optional<webview::PermissionRequest> pending_;
    std::deque<webview::PermissionRequest> queued_;
};

} // namespace aurora::ui

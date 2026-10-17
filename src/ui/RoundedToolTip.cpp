#include "ui/RoundedToolTip.hpp"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "ui/Theme.hpp"
#include "ui/ThemeState.hpp"

namespace aurora::ui {

namespace {

class RoundedTipWindow : public QWidget {
public:
    RoundedTipWindow()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint |
                               Qt::NoDropShadowWindowHint |
                               Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(Spacing::md, Spacing::xs + Spacing::xxs,
                                   Spacing::md, Spacing::xs + Spacing::xxs);
        label_ = new QLabel(this);
        label_->setTextFormat(Qt::AutoText);
        label_->setWordWrap(true);
        label_->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; border: none; }"));
        layout->addWidget(label_);
        hide_timer_.setSingleShot(true);
        hide_timer_.setInterval(10000);
        connect(&hide_timer_, &QTimer::timeout, this, &QWidget::hide);
    }

    void show_text(const QString& text, const QPoint& global) {
        const ThemeColors& c = current_theme().colors;
        label_->setStyleSheet(QStringLiteral(
            "QLabel { background: transparent; border: none; color: %1; }")
            .arg(c.text.name()));
        label_->setText(text);
        label_->setMaximumWidth(360);
        adjustSize();
        QPoint pos = global + QPoint(0, 22);
        const QScreen* screen = QGuiApplication::screenAt(global);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            if (pos.x() + width() > area.right() - 4)
                pos.setX(area.right() - width() - 4);
            if (pos.y() + height() > area.bottom() - 4)
                pos.setY(global.y() - height() - 6);
            pos.setX(qMax(area.left() + 4, pos.x()));
        }
        move(pos);
        show();
        raise();
        hide_timer_.start();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const ThemeColors& c = current_theme().colors;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(c.surface_raised);
        p.setPen(QPen(c.border_subtle, 1.0));
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.drawRoundedRect(r, Radius::lg, Radius::lg);
    }

private:
    QLabel* label_ = nullptr;
    QTimer hide_timer_;
};

class RoundedToolTipFilter : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject* obj, QEvent* e) override {
        switch (e->type()) {
            case QEvent::ToolTip: {
                auto* w = qobject_cast<QWidget*>(obj);
                // Menus show per-action tips through their own path; leave
                // those to Qt rather than guess at the action under the cursor.
                if (!w || qobject_cast<QMenu*>(w)) return false;
                const auto* he = static_cast<QHelpEvent*>(e);
                QString text;
                if (const auto* bar = qobject_cast<QTabBar*>(w)) {
                    const int idx = bar->tabAt(he->pos());
                    if (idx >= 0) text = bar->tabToolTip(idx);
                } else {
                    text = w->toolTip();
                }
                if (text.isEmpty()) {
                    hide_tip();
                    return false;
                }
                if (!tip_) tip_ = new RoundedTipWindow();
                owner_ = w;
                tip_->show_text(text, he->globalPos());
                return true;   // swallow: no square Qt tool tip on top
            }
            case QEvent::Leave:
                if (obj == owner_.data()) hide_tip();
                break;
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::Wheel:
            case QEvent::KeyPress:
            case QEvent::WindowDeactivate:
            case QEvent::ApplicationDeactivate:
                hide_tip();
                break;
            default:
                break;
        }
        return false;
    }

private:
    void hide_tip() {
        if (tip_ && tip_->isVisible()) tip_->hide();
        owner_.clear();
    }

    RoundedTipWindow* tip_ = nullptr;
    QPointer<QWidget> owner_;
};

} // namespace

void install_rounded_tooltips(QApplication* app) {
    if (!app) return;
    app->installEventFilter(new RoundedToolTipFilter(app));
}

} // namespace aurora::ui

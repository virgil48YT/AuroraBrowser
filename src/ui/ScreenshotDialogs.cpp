// Aurora — screenshot tool dialogs. See ScreenshotDialogs.hpp.
#include "ui/ScreenshotDialogs.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QRubberBand>
#include <QScreen>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace aurora::ui {

// --- RegionSelectDialog -------------------------------------------------------

RegionSelectDialog::RegionSelectDialog(const QImage& capture, QWidget* parent)
    : QDialog(parent), capture_(capture) {
    setWindowTitle(QStringLiteral("Select a region — drag, then press Enter"));
    auto* layout = new QVBoxLayout(this);
    // Fit the capture into ~90% of the screen; the selection is scaled back
    // to image pixels, so a HiDPI capture still crops at full resolution.
    QSize room(1280, 800);
    if (const QScreen* screen = parent ? parent->screen() : QGuiApplication::primaryScreen())
        room = screen->availableGeometry().size() * 0.9;
    QPixmap shown = QPixmap::fromImage(capture_);
    if (shown.width() > room.width() || shown.height() > room.height() - 60)
        shown = shown.scaled(room.width(), room.height() - 60, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    scale_ = shown.width() > 0 ? double(capture_.width()) / shown.width() : 1.0;

    canvas_ = new QLabel(this);
    canvas_->setPixmap(shown);
    canvas_->setFixedSize(shown.size());
    canvas_->setCursor(Qt::CrossCursor);
    canvas_->installEventFilter(this);
    layout->addWidget(canvas_, 0, Qt::AlignCenter);
    band_ = new QRubberBand(QRubberBand::Rectangle, canvas_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Capture region"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // Enable "Capture region" once there is a selection.
    setProperty("aurora_ok_button", QVariant::fromValue<QObject*>(
        buttons->button(QDialogButtonBox::Ok)));
}

bool RegionSelectDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched != canvas_) return QDialog::eventFilter(watched, event);
    auto* ok = qobject_cast<QPushButton*>(property("aurora_ok_button").value<QObject*>());
    switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* e = static_cast<QMouseEvent*>(event);
            if (e->button() != Qt::LeftButton) break;
            origin_ = e->position().toPoint();
            band_->setGeometry(QRect(origin_, QSize()));
            band_->show();
            return true;
        }
        case QEvent::MouseMove: {
            auto* e = static_cast<QMouseEvent*>(event);
            if (!(e->buttons() & Qt::LeftButton)) break;
            selection_ = QRect(origin_, e->position().toPoint()).normalized()
                             .intersected(canvas_->rect());
            band_->setGeometry(selection_);
            if (ok) ok->setEnabled(selection_.width() > 2 && selection_.height() > 2);
            return true;
        }
        case QEvent::MouseButtonRelease:
            return true;
        default:
            break;
    }
    return QDialog::eventFilter(watched, event);
}

QRect RegionSelectDialog::selection() const {
    if (selection_.width() <= 2 || selection_.height() <= 2) return {};
    const QRect r(static_cast<int>(std::lround(selection_.x() * scale_)),
                  static_cast<int>(std::lround(selection_.y() * scale_)),
                  static_cast<int>(std::lround(selection_.width() * scale_)),
                  static_cast<int>(std::lround(selection_.height() * scale_)));
    return r.intersected(capture_.rect());
}

QImage RegionSelectDialog::cropped() const {
    const QRect r = selection();
    return r.isEmpty() ? QImage() : capture_.copy(r);
}

// --- ScreenshotResultDialog ---------------------------------------------------

QString ScreenshotResultDialog::save_png(QWidget* parent, const QImage& image,
                                         const QString& suggested_name) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty()) dir = QDir::homePath();
    QString path = QFileDialog::getSaveFileName(
        parent, QStringLiteral("Save screenshot"),
        QDir(dir).filePath(suggested_name + QStringLiteral(".png")),
        QStringLiteral("PNG image (*.png)"));
    if (path.isEmpty()) return {};
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");
    if (!image.save(path, "PNG")) {
        QMessageBox::warning(parent, QStringLiteral("Save screenshot"),
                             QStringLiteral("Could not write %1")
                                 .arg(QDir::toNativeSeparators(path)));
        return {};
    }
    return path;
}

ScreenshotResultDialog::ScreenshotResultDialog(const QImage& image,
                                               const QString& suggested_name,
                                               QWidget* parent)
    : QDialog(parent), image_(image), name_(suggested_name) {
    setWindowTitle(QStringLiteral("Screenshot — %1 × %2")
                       .arg(image_.width())
                       .arg(image_.height()));
    auto* layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    auto* preview = new QLabel(scroll);
    QPixmap pm = QPixmap::fromImage(image_);
    if (pm.width() > 900) pm = pm.scaledToWidth(900, Qt::SmoothTransformation);
    preview->setPixmap(pm);
    scroll->setWidget(preview);
    scroll->setMinimumSize(std::min(pm.width() + 24, 924), std::min(pm.height() + 24, 640));
    layout->addWidget(scroll);

    auto* row = new QHBoxLayout;
    auto* status = new QLabel(QStringLiteral("Copied to the clipboard."), this);
    auto* copy = new QPushButton(QStringLiteral("Copy to clipboard"), this);
    auto* save = new QPushButton(QStringLiteral("Save…"), this);
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    save->setDefault(true);
    row->addWidget(status, 1);
    row->addWidget(copy);
    row->addWidget(save);
    row->addWidget(close);
    layout->addLayout(row);

    connect(copy, &QPushButton::clicked, this, [this, status] {
        QApplication::clipboard()->setImage(image_);
        status->setText(QStringLiteral("Copied to the clipboard."));
    });
    connect(save, &QPushButton::clicked, this, [this, status] {
        const QString path = save_png(this, image_, name_);
        if (!path.isEmpty())
            status->setText(QStringLiteral("Saved to %1").arg(QDir::toNativeSeparators(path)));
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

} // namespace aurora::ui

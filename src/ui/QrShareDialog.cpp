// Aurora — "Share as QR code" dialog. See QrShareDialog.hpp.
#include "ui/QrShareDialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "util/QrCode.hpp"

namespace aurora::ui {

namespace {
constexpr int kQuietZone = 4;  // modules of white border, per ISO/IEC 18004
}

QImage QrShareDialog::render(const QString& text, int scale) {
    const QByteArray utf8 = text.toUtf8();
    const auto matrix = util::encode_qr(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (!matrix) return {};
    const int side = (matrix->size + 2 * kQuietZone) * scale;
    // Always black on white, whatever the theme: inverted codes are not read
    // by every scanner app.
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (int y = 0; y < matrix->size; ++y)
        for (int x = 0; x < matrix->size; ++x)
            if (matrix->dark(x, y))
                p.drawRect((x + kQuietZone) * scale, (y + kQuietZone) * scale, scale, scale);
    p.end();
    return image;
}

QrShareDialog::QrShareDialog(const QString& url, const QString& title, QWidget* parent)
    : QDialog(parent), url_(url) {
    setWindowTitle(QStringLiteral("Share as QR code"));
    image_ = render(url_);

    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(title.isEmpty() ? url_ : title, this);
    heading->setWordWrap(true);
    QFont f = heading->font();
    f.setBold(true);
    heading->setFont(f);
    layout->addWidget(heading);

    auto* code = new QLabel(this);
    code->setAlignment(Qt::AlignCenter);
    // Shown at a fixed, comfortably scannable size; the copied/saved image is
    // the full-resolution render.
    code->setPixmap(QPixmap::fromImage(image_).scaled(
        280, 280, Qt::KeepAspectRatio, Qt::FastTransformation));
    code->setAccessibleName(QStringLiteral("QR code for ") + url_);
    layout->addWidget(code);

    auto* address = new QLabel(url_, this);
    address->setWordWrap(true);
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    address->setMaximumWidth(320);
    layout->addWidget(address);

    auto* buttons = new QHBoxLayout;
    auto* copy_image = new QPushButton(QStringLiteral("Copy image"), this);
    auto* copy_link = new QPushButton(QStringLiteral("Copy link"), this);
    auto* save = new QPushButton(QStringLiteral("Save…"), this);
    auto* close = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(copy_image);
    buttons->addWidget(copy_link);
    buttons->addWidget(save);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(copy_image, &QPushButton::clicked, this, [this, copy_image] {
        QApplication::clipboard()->setImage(image_);
        copy_image->setText(QStringLiteral("Copied"));
    });
    connect(copy_link, &QPushButton::clicked, this, [this, copy_link] {
        QApplication::clipboard()->setText(url_);
        copy_link->setText(QStringLiteral("Copied"));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save QR code"),
            QDir(dir).filePath(QStringLiteral("aurora-qr.png")),
            QStringLiteral("PNG image (*.png)"));
        if (path.isEmpty()) return;
        if (!image_.save(path, "PNG")) {
            QMessageBox::warning(this, QStringLiteral("Save QR code"),
                                 QStringLiteral("Could not write %1").arg(path));
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

void QrShareDialog::show_for(QWidget* parent, const QString& url, const QString& title) {
    const QString trimmed = url.trimmed();
    const bool web = trimmed.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
                     trimmed.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
    if (!web) {
        QMessageBox::information(parent, QStringLiteral("Share as QR code"),
                                 QStringLiteral("Only web pages (http/https) can be "
                                                "shared as a QR code."));
        return;
    }
    if (render(trimmed, 1).isNull()) {
        QMessageBox::information(parent, QStringLiteral("Share as QR code"),
                                 QStringLiteral("This address is too long to fit in a "
                                                "QR code."));
        return;
    }
    auto* dialog = new QrShareDialog(trimmed, title, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace aurora::ui

// Aurora — "Share as QR code" dialog.
//
// Shows the QR code of a tab's address so it can be opened on a phone, with
// Copy image / Copy link / Save PNG actions. The code is generated locally
// (util::encode_qr, the vendored Project Nayuki encoder): the URL never leaves
// the machine, unlike the web QR services some browsers use. Reachable from
// the tab context menu and the browser menu.
#pragma once

#include <QDialog>
#include <QImage>
#include <QString>

namespace aurora::ui {

class QrShareDialog : public QDialog {
    Q_OBJECT
public:
    QrShareDialog(const QString& url, const QString& title, QWidget* parent = nullptr);

    // Render `text` as a QR image with a 4-module quiet zone (the minimum the
    // spec requires for reliable scanning), `scale` pixels per module.
    // Null image when the text is too long to encode.
    static QImage render(const QString& text, int scale = 8);

    // Open a non-modal dialog for `url` (deleted on close). Internal and
    // empty addresses are rejected with a message instead.
    static void show_for(QWidget* parent, const QString& url, const QString& title);

private:
    QString url_;
    QImage image_;
};

} // namespace aurora::ui

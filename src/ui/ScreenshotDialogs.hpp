// Aurora — screenshot tool dialogs.
//
// The capture itself is the engine's (WebViewController::capture_screenshot:
// CEF's DevTools Page.captureScreenshot, visible area or full page). These
// dialogs are the Qt half:
//
//   * RegionSelectDialog — "Select region": the visible-area capture is shown
//     full-size and the user drags a rectangle over it. Selecting on a frozen
//     capture, rather than over the live page, is deliberate: the page is a
//     native CEF child window, which a Qt overlay cannot reliably cover.
//   * ScreenshotResultDialog — preview plus "Save…" (the same QFileDialog
//     plumbing as Save as PDF, defaulting to the Pictures folder) and
//     "Copy to clipboard".
#pragma once

#include <QDialog>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>

class QLabel;
class QRubberBand;

namespace aurora::ui {

class RegionSelectDialog : public QDialog {
    Q_OBJECT
public:
    RegionSelectDialog(const QImage& capture, QWidget* parent = nullptr);
    // The selection in image pixels (empty when nothing was selected).
    QRect selection() const;
    QImage cropped() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QImage capture_;
    QLabel* canvas_ = nullptr;
    QRubberBand* band_ = nullptr;
    QPoint origin_;
    QRect selection_;  // in canvas (= displayed) pixels
    double scale_ = 1.0;  // image px per displayed px
};

class ScreenshotResultDialog : public QDialog {
    Q_OBJECT
public:
    ScreenshotResultDialog(const QImage& image, const QString& suggested_name,
                           QWidget* parent = nullptr);

    // Ask for a path and write the PNG; returns the path or empty.
    static QString save_png(QWidget* parent, const QImage& image,
                            const QString& suggested_name);

private:
    QImage image_;
    QString name_;
};

} // namespace aurora::ui

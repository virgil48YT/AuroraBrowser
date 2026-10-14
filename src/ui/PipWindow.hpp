// Aurora — floating picture-in-picture page window.
//
// Shares the normal tab tear-off lifecycle: detach the model, host its page
// in a compact window, then attach it back at the original tab position.
// Controllers remain window-owned, so (as with tear-off) the page is restored
// at its current URL rather than promising preservation of renderer/DOM state.
// Only the Qt/CEF application builds this surface; core remains engine-free.
#pragma once

#if defined(AURORA_HAS_CEF)

#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include "core/Types.hpp"

class QToolButton;
class QStackedWidget;

namespace aurora::ui {

class MainWindow;

class PipWindow : public QWidget {
    Q_OBJECT
public:
    static bool can_open(MainWindow* source, core::TabId tab);
    // Returns null without changing the source when the tab cannot move.
    static PipWindow* open(MainWindow* source, core::TabId tab);
    ~PipWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    PipWindow(MainWindow* source, core::TabId tab, std::size_t index);
    bool take_page();
    bool restore_page(bool shutting_down = false);
    void configure_controller();
    void update_style();
    void place_in_corner();
    void layout_content();
    Qt::Edges resize_edges(const QPoint& point) const;
    QSize constrained_size(int content_width) const;

    QPointer<MainWindow> source_;
    // An unshown MainWindow owns the tab model, CefHostWidget and controller,
    // reusing normal history/download/profile callbacks. Only its content
    // stack is reparented into this QWidget. No CEF types cross this boundary.
    MainWindow* owner_ = nullptr;
    QStackedWidget* content_ = nullptr;
    QToolButton* return_button_ = nullptr;
    QToolButton* close_button_ = nullptr;
    core::TabId tab_;
    std::size_t original_index_ = 0;
    QPoint press_global_;
    QRect press_geometry_;
    Qt::Edges edges_;
    bool dragging_ = false;
    bool returning_ = false;
    bool restored_ = false;
    bool close_source_after_return_ = false;
    bool shutting_down_ = false;
    std::uint64_t theme_observer_id_ = 0;
};

} // namespace aurora::ui
#endif // AURORA_HAS_CEF

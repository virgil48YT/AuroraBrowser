// Aurora — tab hover card.
//
// The small card that drops under a tab after the pointer rests on it, like
// Chrome's: the page title, its address, and how much memory the tab's
// renderer is using. A tool-tip style top-level window that never takes focus
// or mouse input, so it cannot interrupt hovering, clicking or dragging tabs.
#pragma once

#include <QFrame>
#include <QRect>
#include <QString>

class QLabel;

namespace aurora::ui {

class TabHoverCard : public QFrame {
public:
    explicit TabHoverCard(QWidget* parent = nullptr);

    // `memory_text` is shown on the third line; pass an empty string to hide
    // that line (internal pages have nothing to measure).
    void set_content(const QString& title, const QString& address,
                     const QString& memory_text);
    void set_memory_text(const QString& memory_text);

    // Show centered under `tab_rect_global` (global coordinates), clamped to
    // the screen the tab is on.
    void show_below(const QRect& tab_rect_global);

    // "148 MB", "1.2 GB" — the figure shown on the memory line.
    static QString format_memory(qint64 bytes);

private:
    void apply_style();

    QLabel* title_ = nullptr;
    QLabel* address_ = nullptr;
    QLabel* memory_ = nullptr;
};

} // namespace aurora::ui

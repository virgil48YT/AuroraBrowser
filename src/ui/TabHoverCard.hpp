// Aurora — tab hover card.
//
// The card that drops under a tab after the pointer rests on it, like
// Chrome's: the page title (up to two lines), its site, a hairline, and a
// "Memory usage" row with a gauge glyph. A tool-tip style top-level window
// that never takes focus or mouse input, so it cannot interrupt hovering,
// clicking or dragging tabs. Corners are painted, over a translucent window.
#pragma once

#include <QRect>
#include <QString>
#include <QWidget>

namespace aurora::ui {

class TabHoverCard : public QWidget {
public:
    explicit TabHoverCard(QWidget* parent = nullptr);

    // `memory_text` is the whole bottom row ("Memory usage: 303 MB"); pass an
    // empty string to drop the hairline and row (internal pages have nothing
    // to measure).
    void set_content(const QString& title, const QString& address,
                     const QString& memory_text);
    void set_memory_text(const QString& memory_text);

    // Show centered under `tab_rect_global` (global coordinates), clamped to
    // the screen the tab is on.
    void show_below(const QRect& tab_rect_global);

    // "148 MB", "1.2 GB" — the figure shown on the memory line.
    static QString format_memory(qint64 bytes);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void relayout();

    QString title_;
    QString address_;
    QString memory_;
    QString title_line1_;
    QString title_line2_;
};

} // namespace aurora::ui

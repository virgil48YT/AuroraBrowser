// Aurora — gradient-outline card for the neon internal pages.
//
// The aurora-glass look outlines its cards (history groups, the filter
// panel, action buttons, the search field) with a soft gradient edge and a
// "glow" wash, over a semi-transparent dark fill. Qt stylesheets can tint a
// border with a gradient but cannot round it reliably cross-platform, and
// they cannot draw the outer glow pass at all, so that look is painted here
// in C++ instead of QSS.
//
// The frame draws only its own glass plate: children are laid out on top
// with margins, which is how a HistoryGroupCard keeps its rows clickable and
// its text crisp (child widgets are never inside a paint layer).
#pragma once

#include <QColor>
#include <QFrame>

namespace aurora::ui {

class GlowFrame : public QFrame {
    Q_OBJECT
public:
    // Edge gradient colors. color1 runs top-left, color2 bottom-right.
    struct Edge {
        QColor color1;
        QColor color2;
        QColor glow = {};      // outer aura; null = no glow pass
        int glow_extent = 0;   // the grey "padding" ring left for the aura
    };

    explicit GlowFrame(QWidget* parent = nullptr);

    // Installs the aurora cyan -> purple edge for ordinary cards/panels.
    void apply_card_style(int radius = 14, int glow_extent = 3);
    // The history panel's card look (same blue -> violet rim, blue halo and
    // navy glass) for floating menus such as the browser's three-dots menu.
    // A menu floats over arbitrary page content instead of the aurora
    // wallpaper, so the navy plate is near-opaque to keep rows legible; in
    // the light theme the plate falls back to `light_fill`.
    void apply_menu_style(bool dark, const QColor& light_fill, int radius = 14,
                          int glow_extent = 3);
    // Installs the purple -> magenta edge for destructive actions.
    void apply_danger_style(int radius = 10, int glow_extent = 3);
    // Installs an explicit edge + fill + radius (search field, free use).
    void set_style(const Edge& edge, const QColor& fill, int radius);

    Edge edge() const { return edge_; }
    QColor fill() const { return fill_; }
    int radius() const { return radius_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Edge edge_;
    QColor fill_ = QColor(QStringLiteral("#0E1626"));
    int radius_ = 14;
};

} // namespace aurora::ui

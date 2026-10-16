// Aurora — pill toggle switch.
//
// A QCheckBox that paints itself as a 34×20 capsule with a round knob that
// slides between the off (left) and on (right) positions. The old style-sheet
// version faked the knob with a radial gradient and drew it clipped; painting
// it directly keeps the knob a true circle at every DPI and lets it animate.
// It remains a QCheckBox, so existing toggled()/isChecked() code is unchanged.
#pragma once

#include <QCheckBox>

class QVariantAnimation;

namespace aurora::ui {

class ToggleSwitch : public QCheckBox {
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);
    explicit ToggleSwitch(const QString& text, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& pos) const override;

private:
    void init();
    qreal progress() const;   // 0 = off, 1 = on, fractional while sliding

    QVariantAnimation* anim_ = nullptr;
};

} // namespace aurora::ui

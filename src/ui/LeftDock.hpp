// Aurora — slim vertical launcher dock.
//
// The far-left rail from the aurora reference chrome: a transparent strip of
// icon-only app shortcuts (home / history / downloads / bookmarks /
// extensions) under a small aurora-dot badge, and the Settings gear pinned
// at the bottom with a persistent cyan→purple glow ring. The widget is
// layout-only; MainWindow owns what each button does.
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include "ui/Icons.hpp"
#include "ui/ThemeState.hpp"

class QToolButton;
class QVBoxLayout;

namespace aurora::ui {

class LeftDock : public QWidget {
    Q_OBJECT
public:
    explicit LeftDock(QWidget* parent = nullptr);
    ~LeftDock() override;

signals:
    void home_requested();
    void history_requested();
    void downloads_requested();
    void bookmarks_requested();
    void extensions_requested();
    void settings_requested();

private:
    // One 34px icon button: a flat square tile with the accent wash on hover,
    // or the glowing bordered pill for the bottom-pinned settings gear.
    QToolButton* add_item(icons::Name icon, const QString& tooltip, bool glow);
    void apply_icons();   // restamp every glyph from the live theme

    struct Entry {
        icons::Name icon;
        QToolButton* button = nullptr;
        bool glow = false;
    };

    QVBoxLayout* layout_ = nullptr;
    QList<Entry> entries_;
    ThemeObserverId theme_observer_ = 0;
};

} // namespace aurora::ui

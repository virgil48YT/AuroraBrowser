// Aurora — slim vertical launcher dock.
//
// The far-left rail from the aurora reference chrome: a transparent strip of
// icon-only app shortcuts (home / history / downloads / bookmarks /
// extensions) under a small aurora-dot badge, and the Settings gear pinned
// at the bottom with a persistent cyan→purple glow ring. Between them sits a
// "+" button for adding sites to the rail, followed by the user's pinned
// sites (favicon buttons). The widget is layout-only; MainWindow owns what
// each button does and where pins are stored.
#pragma once

#include <QList>
#include <QPoint>
#include <QString>
#include <QVector>
#include <QWidget>

#include "ui/Icons.hpp"
#include "ui/ShortcutList.hpp"
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
    // The "+" button: MainWindow shows its add menu at `global_pos`.
    void add_requested(const QPoint& global_pos);
    void pin_open_requested(const QString& url);
    void pin_edit_requested(int index);
    void pin_remove_requested(int index);

public:
    // Replace the pinned-site buttons (an empty list shows none).
    void set_pins(const QVector<Shortcut>& pins);

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

    void rebuild_pins();
    void apply_pin_icons();

    QVBoxLayout* layout_ = nullptr;
    QToolButton* add_button_ = nullptr;
    QWidget* pins_host_ = nullptr;
    QVBoxLayout* pins_layout_ = nullptr;
    QVector<Shortcut> pins_;
    QList<QToolButton*> pin_buttons_;
    QList<Entry> entries_;
    ThemeObserverId theme_observer_ = 0;
};

} // namespace aurora::ui

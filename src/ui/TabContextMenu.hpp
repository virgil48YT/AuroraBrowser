// Aurora — tab context menu.
//
// Right-click on a tab shows a context-aware menu with actions that
// operate on the clicked tab or the tab strip as a whole. All actions
// route through the TabManager so the model stays the single source of truth.
#pragma once

#include <QMenu>
#include <QPoint>
#include "tabs/TabManager.hpp"

namespace aurora::ui {

class TabContextMenu : public QMenu {
    Q_OBJECT
public:
    TabContextMenu(core::TabManager* manager, int tab_index, QWidget* parent = nullptr);

signals:
    void duplicate_tab(int index);
    void reopen_closed();
    void close_tab(int index);
    void close_other_tabs(int index);
    void pin_tab(int index);
    void mute_tab(int index);
    void reload_tab(int index);
    void suspend_tab(int index);
    void restore_tab(int index);
    void exclude_tab_from_suspension(int index);
    void copy_url(int index);

private:
    core::TabManager* manager_;
    int index_;
    void build();
    // Pins the BrowserMenu popover look (surface_raised panel, hairline
    // border, rounded corners, themed rows) onto this menu instead of
    // inheriting only the generic global QMenu block.
    void apply_style();
};

} // namespace aurora::ui

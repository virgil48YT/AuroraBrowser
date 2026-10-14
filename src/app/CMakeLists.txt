# The Qt browser executable. Built only when AURORA_BUILD_APP=ON and Qt6 is
# found (see the top-level CMakeLists.txt). The core library is linked
# statically so the binary is self-contained.

qt_add_executable(aurora_browser
    ../ui/MainWindow.cpp
    ../ui/Omnibox.cpp
    ../ui/TabBar.cpp
    ../ui/TabContextMenu.cpp
    ../ui/BrowserMenu.cpp
    ../ui/BookmarkBar.cpp
    ../ui/DownloadsPopover.cpp
    ../ui/ShieldButton.cpp
    ../ui/ExtensionsButton.cpp
    ../ui/FindBar.cpp
    ../ui/CefHostWidget.cpp
    ../ui/StylesheetBuilder.cpp
    ../ui/FaviconCache.cpp
    ../ui/Theme.cpp
    ../ui/ThemeState.cpp
    ../ui/Icons.cpp
    ../ui/IconButton.cpp
    ../ui/TabSpinner.cpp
    ../ui/pages/InternalPageWidget.cpp
    ../ui/pages/HistoryPage.cpp
    ../ui/pages/DownloadsPage.cpp
    ../ui/pages/BookmarksPage.cpp
    ../ui/pages/SettingsPage.cpp
    ../ui/pages/ExtensionsPage.cpp
    main.cpp
)

target_include_directories(aurora_browser PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aurora_browser PRIVATE
    aurora_core Qt::Widgets Qt::Gui Qt::Network Qt::Svg)

# Embed the Aurora app icon into the Windows executable so it shows in the
# taskbar, window title, and Explorer. The .rc is generated at configure time
# with the absolute icon path so the RC compiler resolves it from any build dir.
# Also build as a GUI-subsystem app so no console window appears on
# double-click; a diagnostic console can still be requested at runtime with
# --shell (see main.cpp -> AllocConsole).
if(WIN32)
    set(AURORA_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/icons/aurora.ico")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/aurora.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/aurora.rc
        @ONLY)
    target_sources(aurora_browser PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/aurora.rc)
    set_target_properties(aurora_browser PROPERTIES WIN32_EXECUTABLE TRUE)
    # MainWindow's frameless+Aero-Snap support calls GetWindowLongPtrW /
    # SetWindowLongPtrW / SetWindowPos / GetSystemMetrics directly. Qt6::Gui
    # already pulls user32 in transitively, but link it explicitly so the
    # symbols always resolve regardless of the Qt CMake config.
    target_link_libraries(aurora_browser PRIVATE user32)
endif()

if(AURORA_ENABLE_CEF)
    target_sources(aurora_browser PRIVATE ../ui/PipWindow.cpp ../ui/PipWindow.hpp)
    target_link_libraries(aurora_browser PRIVATE aurora_cef)
    target_compile_definitions(aurora_browser PRIVATE AURORA_HAS_CEF)
endif()

# macOS: bundle the app so CEF's framework and helper subprocesses resolve.
if(APPLE)
    set_target_properties(aurora_browser PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.aurora.browser")
endif()

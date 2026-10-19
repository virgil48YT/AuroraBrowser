# The Qt browser executable. Built only when AURORA_BUILD_APP=ON and Qt6 is
# found (see the top-level CMakeLists.txt). The core library is linked
# statically so the binary is self-contained.

set(AURORA_BROWSER_SOURCES
    ../ui/MainWindow.cpp
    ../ui/Omnibox.cpp
    ../ui/OmniboxPopup.cpp
    ../ui/OmniboxSuggestController.cpp
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
    ../ui/LeftDock.cpp
    ../ui/TabSpinner.cpp
    ../ui/ShortcutList.cpp
    ../ui/ToggleSwitch.cpp
    ../ui/GlowFrame.cpp
    ../ui/PageBackground.cpp
    ../ui/TabHoverCard.cpp
    ../ui/RoundedToolTip.cpp
    ../ui/ShortcutDialog.cpp
    ../ui/WeatherService.cpp
    ../ui/FilterListUpdater.cpp
    ../ui/pages/InternalPageWidget.cpp
    ../ui/pages/HistoryPage.cpp
    ../ui/pages/HistoryWidgets.cpp
    ../ui/pages/HomePage.cpp
    ../ui/pages/DownloadsPage.cpp
    ../ui/pages/BookmarksPage.cpp
    ../ui/pages/SettingsPage.cpp
    ../ui/pages/SettingsRow.cpp
    ../ui/pages/ExtensionsPage.cpp
    ../ui/pages/SiteSettingsPage.cpp
    main.cpp
)

# Windows + AURORA_WIN_SANDBOX (see the top-level CMakeLists.txt): the browser
# is aurora_browser.dll, exporting RunWinMain, and CEF's sandbox-linked
# bootstrap.exe is copied next to it as aurora_browser.exe. The executable is
# what users, the SFX and aurora_bootstrap start, so nothing downstream
# changes its name. Everywhere else it is an ordinary executable.
if(WIN32 AND AURORA_ENABLE_CEF AND AURORA_WIN_SANDBOX)
    qt_add_library(aurora_browser SHARED ${AURORA_BROWSER_SOURCES})
    target_compile_definitions(aurora_browser PRIVATE AURORA_WIN_BOOTSTRAP)
    # CommandLineToArgvW (shell32) and the LPAC ACL calls (advapi32).
    target_link_libraries(aurora_browser PRIVATE shell32 advapi32)
    set(AURORA_BROWSER_EXE "$<TARGET_FILE_DIR:aurora_browser>/aurora_browser.exe")
    add_custom_command(TARGET aurora_browser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEF_ROOT}/Release/bootstrap.exe" "${AURORA_BROWSER_EXE}"
        # Brand the bootstrap with Aurora's icon and grant the LPAC ACL the
        # sandboxed network service needs (both best effort; see the script).
        COMMAND ${CMAKE_COMMAND}
                -DRCEDIT=${CMAKE_SOURCE_DIR}/rcedit-x64.exe
                -DEXE=${AURORA_BROWSER_EXE}
                -DICON=${CMAKE_SOURCE_DIR}/assets/icons/aurora.ico
                -DLPAC_DIR=$<TARGET_FILE_DIR:aurora_browser>
                -P ${CMAKE_SOURCE_DIR}/cmake/FinishBootstrapExe.cmake
        COMMENT "Installing CEF bootstrap.exe as aurora_browser.exe (Windows sandbox)"
        VERBATIM)
else()
    qt_add_executable(aurora_browser ${AURORA_BROWSER_SOURCES})
endif()

# Brand artwork (logo + emblem) compiled into the executable so the window
# and the homepage never depend on loose files next to the .exe.
qt_add_resources(aurora_browser aurora_brand
    PREFIX "/brand"
    BASE "${CMAKE_SOURCE_DIR}/assets/brand"
    FILES
        "${CMAKE_SOURCE_DIR}/assets/brand/aurora_logo_full.png"
        "${CMAKE_SOURCE_DIR}/assets/brand/aurora_mark.png"
        "${CMAKE_SOURCE_DIR}/assets/brand/aurora_bg.jpg")

# The Public Suffix List (publicsuffix.org, MPL-2.0), compiled in so the
# third-party-cookie site boundaries never depend on a loose data file. Loaded
# once at startup into privacy::install_public_suffix_list (see main.cpp).
qt_add_resources(aurora_browser aurora_psl
    PREFIX "/psl"
    BASE "${CMAKE_SOURCE_DIR}/third_party/publicsuffix"
    FILES
        "${CMAKE_SOURCE_DIR}/third_party/publicsuffix/public_suffix_list.dat")

target_include_directories(aurora_browser PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aurora_browser PRIVATE
    aurora_core Qt::Widgets Qt::Gui Qt::Network Qt::Svg)

# Shown on the About section of aurora://settings; the macro is otherwise
# only compiled into the bootstrap/packaging tools.
target_compile_definitions(aurora_browser PRIVATE
    AURORA_VERSION_STRING="${PROJECT_VERSION}")

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
    if(NOT (AURORA_ENABLE_CEF AND AURORA_WIN_SANDBOX))
        set_target_properties(aurora_browser PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
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

# Linux: CEF's libcef.so (and its sibling libraries) are shipped next to the
# executable, so the binary looks for them in its own directory first. The
# packaging script (packaging/linux/package_linux.sh) relies on this.
if(UNIX AND NOT APPLE AND AURORA_ENABLE_CEF)
    set_target_properties(aurora_browser PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
endif()

# macOS: a real .app bundle. The executable is named "Aurora" because CEF
# derives the helper bundle names from it ("Aurora Helper (GPU).app", ...),
# and the bundle carries an .icns generated from the PNG brand icon.
if(APPLE)
    set(AURORA_MAC_BUNDLE_ID "com.aurora.browser")
    set(AURORA_MAC_NAME "Aurora")
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(AURORA_MAC_MIN_OS "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(AURORA_MAC_MIN_OS "13.3")
    endif()
    set(AURORA_ICNS "${CMAKE_CURRENT_BINARY_DIR}/Aurora.icns")
    add_custom_command(
        OUTPUT "${AURORA_ICNS}"
        COMMAND "${CMAKE_SOURCE_DIR}/packaging/macos/make_icns.sh"
                "${CMAKE_SOURCE_DIR}/assets/icons/aurora_256.png" "${AURORA_ICNS}"
        DEPENDS "${CMAKE_SOURCE_DIR}/assets/icons/aurora_256.png"
                "${CMAKE_SOURCE_DIR}/packaging/macos/make_icns.sh"
        COMMENT "Generating Aurora.icns"
        VERBATIM)
    target_sources(aurora_browser PRIVATE "${AURORA_ICNS}")
    set_source_files_properties("${AURORA_ICNS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    configure_file(
        "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    set_target_properties(aurora_browser PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "${AURORA_MAC_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${AURORA_MAC_BUNDLE_ID}")
endif()

# macOS + CEF: helper bundles and the framework. Chromium launches its
# renderer/GPU/plugin/alert subprocesses from separate helper apps inside
# Contents/Frameworks; each one loads the framework itself and enters
# CefExecuteProcess (mac_helper_main.cpp). The main bundle copies the
# framework and the helpers in after linking, mirroring CEF's cefsimple.
if(APPLE AND AURORA_ENABLE_CEF)
    set(AURORA_CEF_FRAMEWORK "${CEF_ROOT}/Release/Chromium Embedded Framework.framework")
    set(AURORA_BUNDLE_FRAMEWORKS "$<TARGET_BUNDLE_DIR:aurora_browser>/Contents/Frameworks")
    add_custom_command(TARGET aurora_browser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${AURORA_BUNDLE_FRAMEWORKS}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${AURORA_CEF_FRAMEWORK}"
                "${AURORA_BUNDLE_FRAMEWORKS}/Chromium Embedded Framework.framework"
        VERBATIM)
    foreach(_suffix_list ${CEF_HELPER_APP_SUFFIXES})
        # "<name suffix>:<target suffix>:<plist suffix>", e.g. " (GPU):_gpu:.gpu"
        string(REPLACE ":" ";" _suffix_list "${_suffix_list}")
        list(GET _suffix_list 0 _name_suffix)
        list(GET _suffix_list 1 _target_suffix)
        list(GET _suffix_list 2 _plist_suffix)
        set(_helper_target "aurora_helper${_target_suffix}")
        set(_helper_name "${AURORA_MAC_NAME} Helper${_name_suffix}")
        set(AURORA_HELPER_NAME "${_helper_name}")
        set(AURORA_HELPER_BUNDLE_ID "${AURORA_MAC_BUNDLE_ID}.helper${_plist_suffix}")
        set(_helper_plist "${CMAKE_CURRENT_BINARY_DIR}/helper${_target_suffix}-Info.plist")
        configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/helper-Info.plist.in"
                       "${_helper_plist}" @ONLY)
        add_executable(${_helper_target} MACOSX_BUNDLE mac_helper_main.cpp)
        target_include_directories(${_helper_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(${_helper_target} PRIVATE aurora_cef)
        set_target_properties(${_helper_target} PROPERTIES
            OUTPUT_NAME "${_helper_name}"
            MACOSX_BUNDLE_INFO_PLIST "${_helper_plist}")
        add_dependencies(aurora_browser ${_helper_target})
        add_custom_command(TARGET aurora_browser POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<TARGET_BUNDLE_DIR:${_helper_target}>"
                    "${AURORA_BUNDLE_FRAMEWORKS}/${_helper_name}.app"
            VERBATIM)
    endforeach()
endif()

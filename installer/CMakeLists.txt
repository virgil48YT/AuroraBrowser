# Aurora Browser Setup — the custom, visible self-extracting installer.
#
# This target is intentionally independent of Qt, CEF and aurora_core: the
# installer has to run before anything is extracted, so it must be a small,
# self-contained Win32 executable. The packaging script appends the ZIP payload
# to this .exe (see packaging/windows/build_installer.sh); the executable reads
# its own payload through the footer written after it.
#
# Built only when AURORA_BUILD_INSTALLER=ON (Windows only).

set(AURORA_SETUP_VERSION "${PROJECT_VERSION}")
set(AURORA_SETUP_VERSION_COMMA
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")

set(AURORA_SETUP_ASSET_DIR "${CMAKE_SOURCE_DIR}/assets")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/aurora_setup.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/aurora_setup.rc"
    @ONLY)

add_executable(aurora_setup_ui WIN32
    src/main.cpp
    src/win_util.cpp
    src/strings.cpp
    src/inflate.cpp
    src/payload.cpp
    src/install.cpp
    src/ui.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/aurora_setup.rc"
)

target_include_directories(aurora_setup_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(aurora_setup_ui PRIVATE cxx_std_20)
target_compile_definitions(aurora_setup_ui PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    AURORA_SETUP_VERSION="${AURORA_SETUP_VERSION}")
target_link_libraries(aurora_setup_ui PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    comctl32
    shell32
    shlwapi
    ole32
    uuid
    advapi32
    user32
    gdi32)

if(MSVC)
    # WIN32_EXECUTABLE selects /SUBSYSTEM:WINDOWS, whose default CRT entry
    # expects WinMain; make that explicit (the bootstrap uses the same pattern
    # for mainCRTStartup).
    target_link_options(aurora_setup_ui PRIVATE /ENTRY:WinMainCRTStartup)
    # The shared manifest makes the installer DPI-aware (PerMonitorV2) so the
    # Direct2D layout is sharp on scaled displays.
    target_sources(aurora_setup_ui PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/aurora_setup.manifest")
    set_target_properties(aurora_setup_ui PROPERTIES
        OUTPUT_NAME "aurora_setup_ui")
endif()

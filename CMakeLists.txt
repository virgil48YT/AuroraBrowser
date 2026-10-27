cmake_minimum_required(VERSION 3.24)

# Aurora — a premium desktop web browser built on Chromium/CEF.
#
# Build tiers:
#   1. Core library + unit tests  — always buildable with just a C++20
#      compiler and SQLite. This is what CI runs in a headless sandbox.
#   2. Full browser application    — built when Qt6 and CEF are found.
#
# Options:
#   AURORA_BUILD_TESTS   (ON)  Build the unit-test suite.
#   AURORA_ENABLE_CEF    (OFF) Enable the CEF integration layer. Requires
#                             CEF_ROOT to point at a CEF binary distribution.
#   AURORA_BUILD_APP     (OFF) Build the Qt browser executable. Requires Qt6.
#   AURORA_BUILD_INSTALLER (OFF) Build the Windows setup wizard.
#
# The core library never depends on Qt or CEF, so it can be compiled and
# tested on any machine with a C++20 compiler and SQLite.

# Numeric version override for release builds. The tag is the source of truth
# for a release (`v0.2.0`), while a plain development checkout keeps the
# version in this file. Keeping them in sync matters now that the in-app
# updater compares the compiled version with the latest release tag: a package
# built from a v0.2.0 tag must report 0.2.0, or it would offer itself forever.
set(AURORA_RELEASE_VERSION "" CACHE STRING
    "Numeric release version override (e.g. 0.2.0), set by the release workflow")
if(AURORA_RELEASE_VERSION AND
   NOT AURORA_RELEASE_VERSION MATCHES "^[0-9]+(\\.[0-9]+){0,2}$")
    message(FATAL_ERROR
        "AURORA_RELEASE_VERSION must be numeric (e.g. 0.2.0), got '${AURORA_RELEASE_VERSION}'")
endif()
set(AURORA_PROJECT_VERSION "0.1.0")
if(AURORA_RELEASE_VERSION)
    set(AURORA_PROJECT_VERSION "${AURORA_RELEASE_VERSION}")
endif()

project(Aurora
    VERSION ${AURORA_PROJECT_VERSION}
    DESCRIPTION "Aurora — a premium Chromium/CEF desktop browser"
    LANGUAGES C CXX)
# Objective-C++ is needed on macOS for the CEF/AppKit glue
# (src/cef/CefMacSupport.mm). Enabled here, at the top level, because CMake
# requires a language to be enabled before targets that use it are created.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Strict warnings for a safety-critical, long-lived codebase.
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
    # Warnings as errors only in CI; kept off-by-default locally to avoid
    # blocking development on platform-specific noise.
    option(AURORA_WERROR "Treat warnings as errors" OFF)
    if(AURORA_WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Wcast-align -Woverloaded-virtual
        -Wconversion -Wsign-conversion -Wnull-dereference
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
    option(AURORA_WERROR "Treat warnings as errors" OFF)
    if(AURORA_WERROR)
        add_compile_options(-Werror)
        # -Wnull-dereference is interprocedural, so at -O2/-O3 GCC reports it
        # inside inlined libstdc++ code (std::string's internal pointer) that
        # no caller can fix. Keep the diagnostic, but do not let a release
        # build fail on a warning that points into the standard library.
        add_compile_options(-Wno-error=null-dereference)
    endif()
endif()

# --- Output layout -----------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(AURORA_BUILD_TESTS "Build unit tests" ON)
option(AURORA_ENABLE_CEF  "Enable CEF integration layer" OFF)
option(AURORA_BUILD_APP   "Build the Qt browser executable" OFF)
option(AURORA_REQUIRE_H264_AAC
    "Require a CEF distribution with recorded Chrome FFmpeg/proprietary codec GN arguments" OFF)
if(AURORA_REQUIRE_H264_AAC AND NOT AURORA_ENABLE_CEF)
    message(FATAL_ERROR "AURORA_REQUIRE_H264_AAC requires AURORA_ENABLE_CEF=ON")
endif()
# The bootstrap launcher and the build-ID tool depend on nothing but
# aurora_core, so they are cheap and always built: the packaging step needs the
# tool, and the shipped SFX runs the launcher.
option(AURORA_BUILD_BOOTSTRAP "Build the bootstrap launcher (aurora_bootstrap)" ON)
option(AURORA_BUILD_TOOLS     "Build the build-ID packaging tool" ON)
option(AURORA_BUILD_INSTALLER "Build the Windows setup executable" OFF)
if(AURORA_BUILD_INSTALLER AND NOT WIN32)
    message(FATAL_ERROR "AURORA_BUILD_INSTALLER is Windows-only")
endif()

# Platform tag recorded in the build manifest. It has to match the CEF binary
# distribution's platform name (see scripts/fetch_cef.sh) because the manifest
# is how a package declares which runtime it carries.
if(WIN32)
    set(AURORA_PLATFORM "windows64")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        set(AURORA_PLATFORM "macosarm64")
    else()
        set(AURORA_PLATFORM "macosx64")
    endif()
else()
    set(AURORA_PLATFORM "linux64")
endif()

# --- Core library (no Qt, no CEF) -------------------------------------------
add_library(aurora_core STATIC)
target_sources(aurora_core
    PRIVATE
        src/core/Url.cpp
        src/core/InternalUrl.cpp
        src/core/ChromeNav.cpp
        src/core/BuildId.cpp
        src/core/CefLayout.cpp
        src/core/CefMaterialize.cpp
        src/updates/ReleaseVersion.cpp
        src/cef/GpuPolicy.cpp
        src/cef/MediaPolicy.cpp
        src/cef/SandboxPolicy.cpp
        src/util/Log.cpp
        src/util/Perf.cpp
        src/util/Sha256.cpp
        src/util/QrCode.cpp
        third_party/qrcodegen/qrcodegen.cpp
        src/storage/Database.cpp
        src/storage/Migration.cpp
        src/tabs/TabManager.cpp
        src/history/HistoryService.cpp
        src/bookmarks/BookmarkService.cpp
        src/bookmarks/BookmarkIo.cpp
        src/downloads/DownloadService.cpp
        src/extensions/ExtensionService.cpp
        src/extensions/ExtensionLoadList.cpp
        src/extensions/ZipExtractor.cpp
        src/session/SessionService.cpp
        src/session/SessionRestore.cpp
        src/settings/Settings.cpp
        src/searchengines/SearchEngine.cpp
        src/searchengines/SearchEngineService.cpp
        src/omnibox/OmniboxSuggest.cpp
        src/privacy/CertificateAudit.cpp
        src/privacy/CookieSite.cpp
        src/adblock/AdblockEngine.cpp
        src/adblock/AdblockService.cpp
        src/adblock/ListUpdater.cpp
        src/tor/TorBundle.cpp
)
target_include_directories(aurora_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Vendored QR encoder (Project Nayuki, MIT): third-party code, so it is built
# without Aurora's warning set and its header is a SYSTEM include.
target_include_directories(aurora_core SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen)
if(MSVC)
    set_source_files_properties(third_party/qrcodegen/qrcodegen.cpp PROPERTIES COMPILE_OPTIONS /W0)
else()
    set_source_files_properties(third_party/qrcodegen/qrcodegen.cpp PROPERTIES COMPILE_OPTIONS -w)
endif()
target_compile_features(aurora_core PUBLIC cxx_std_20)
# SearchEngineService owns a worker thread (std::thread), which needs the
# platform threads library on Linux/macOS toolchains.
find_package(Threads REQUIRED)
target_link_libraries(aurora_core PUBLIC Threads::Threads)

# --- Aurora Shields: the Rust adblock engine (brave/adblock-rust) ----------
# Built from third_party/adblock into a static library and linked into
# aurora_core. AUTO (the default) builds it when a cargo toolchain is found;
# OFF compiles the C++ side against an inert stub (AdblockEngine still links,
# blocks nothing, and the shield UI reports the engine as unavailable), so a
# machine without Rust still produces a fully working browser.
set(AURORA_ENABLE_ADBLOCK "auto" CACHE STRING "Build the Rust ad-block engine (ON/OFF/auto)")
set_property(CACHE AURORA_ENABLE_ADBLOCK PROPERTY STRINGS ON OFF auto)
if(AURORA_ENABLE_ADBLOCK STREQUAL "auto")
    find_program(CARGO_EXECUTABLE cargo)
    if(CARGO_EXECUTABLE)
        set(AURORA_ENABLE_ADBLOCK ON)
    else()
        set(AURORA_ENABLE_ADBLOCK OFF)
    endif()
endif()
if(AURORA_ENABLE_ADBLOCK)
    if(NOT CARGO_EXECUTABLE)
        find_program(CARGO_EXECUTABLE cargo)
    endif()
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR "AURORA_ENABLE_ADBLOCK=ON requires cargo (https://rustup.rs)")
    endif()
    set(AURORA_ADBLOCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/adblock")
    if(WIN32)
        set(AURORA_ADBLOCK_LIB "${AURORA_ADBLOCK_DIR}/target/x86_64-pc-windows-msvc/release/aurora_adblock.lib")
        # A proper CMake list (two items), expanded unquoted below so cargo
        # receives --target and the triple as separate arguments.
        set(AURORA_ADBLOCK_CARGO_ARGS --target x86_64-pc-windows-msvc)
    else()
        set(AURORA_ADBLOCK_LIB "${AURORA_ADBLOCK_DIR}/target/release/libaurora_adblock.a")
        set(AURORA_ADBLOCK_CARGO_ARGS)
    endif()
    add_custom_command(
        OUTPUT "${AURORA_ADBLOCK_LIB}"
        COMMAND "${CARGO_EXECUTABLE}" build --release ${AURORA_ADBLOCK_CARGO_ARGS}
        WORKING_DIRECTORY "${AURORA_ADBLOCK_DIR}"
        DEPENDS "${AURORA_ADBLOCK_DIR}/src/lib.rs"
                 "${AURORA_ADBLOCK_DIR}/Cargo.toml"
        COMMENT "Building aurora-adblock-ffi (brave/adblock-rust)"
        USES_TERMINAL
        VERBATIM)
    # A phony target so the static library is rebuilt on demand even before
    # any object that links it exists; aurora_core depends on it, which is
    # what orders the cargo build before the link step.
    add_custom_target(aurora_adblock_ffi ALL DEPENDS "${AURORA_ADBLOCK_LIB}")
    add_dependencies(aurora_core aurora_adblock_ffi)
    # Link by full path (a pre-built artifact, not a source file — which is
    # why this is target_link_libraries and not target_sources).
    target_link_libraries(aurora_core PUBLIC "${AURORA_ADBLOCK_LIB}")
    target_compile_definitions(aurora_core PUBLIC AURORA_ADBLOCK_FFI)
    if(NOT WIN32)
        # staticlib pulls in pthreads on Linux/macOS
        find_package(Threads REQUIRED)
        target_link_libraries(aurora_core PUBLIC Threads::Threads)
    else()
        # A Rust staticlib on MSVC embeds the Rust standard library, which
        # references these system import libraries. cargo prints the same
        # list when it links a staticlib by hand; CMake has to be told
        # explicitly or the final binary fails with a wall of LNK2019
        # __imp_* unresolved externals from the embedded std.
        target_link_libraries(aurora_core PUBLIC
            advapi32 ws2_32 userenv bcrypt ntdll)
    endif()
endif()

# SQLite is the only external dependency of the core library. We prefer the
# system amalgamation; fall back to a bundled sqlite3.c if present.
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    target_link_libraries(aurora_core PUBLIC SQLite::SQLite3)
    target_compile_definitions(aurora_core PUBLIC AURORA_HAVE_SYSTEM_SQLITE)
else()
    set(AURORA_BUNDLED_SQLITE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
    if(EXISTS "${AURORA_BUNDLED_SQLITE}")
        target_sources(aurora_core PRIVATE ${AURORA_BUNDLED_SQLITE})
        # Vendored third-party C: suppress its warnings so it never trips -Werror.
        if(MSVC)
            set_source_files_properties(${AURORA_BUNDLED_SQLITE} PROPERTIES COMPILE_OPTIONS /W0)
        else()
            set_source_files_properties(${AURORA_BUNDLED_SQLITE} PROPERTIES COMPILE_OPTIONS -w)
        endif()
        # PUBLIC so consumers that include storage/Database.hpp can find
        # sqlite3.h. SYSTEM so our strict warning set (-Wold-style-cast in
        # particular, which SQLITE_TRANSIENT's C-style cast trips) is not
        # applied to a vendored header we do not control. Without this a
        # bundled-SQLite build fails under -Werror while a system-SQLite build
        # (header already in a system directory) succeeds.
        target_include_directories(aurora_core SYSTEM PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
        target_compile_definitions(aurora_core PRIVATE
            SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1
            SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DEFAULT_WAL=1
            SQLITE_ENABLE_FTS5 SQLITE_ENABLE_JSON1)
    else()
        message(FATAL_ERROR
            "SQLite not found. Install libsqlite3-dev, set SQLite3_ROOT, "
            "or vendor sqlite3.c into third_party/sqlite/. "
            "Run scripts/fetch_deps.sh to fetch it automatically.")
    endif()
endif()

# --- CEF integration layer (compiled only when CEF is available) -------------
if(AURORA_ENABLE_CEF)
    set(CEF_ROOT "" CACHE PATH "Path to a CEF binary distribution")
    if(NOT CEF_ROOT OR NOT EXISTS "${CEF_ROOT}")
        message(FATAL_ERROR "AURORA_ENABLE_CEF requires CEF_ROOT to point at a "
            "CEF binary distribution. Run scripts/fetch_cef.sh first.")
    endif()
    # A custom CEF build supplies the codec implementations. CMake cannot
    # retrofit them into a stock libcef. Keep its actual GN args with the
    # distribution to make opting into a codec-required package explicit.
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CefCodecs.cmake")
    aurora_cef_codec_args_enabled("${CEF_ROOT}/codec-args.gn" AURORA_CEF_CODEC_ARGS_ENABLED)
    if(AURORA_REQUIRE_H264_AAC AND NOT AURORA_CEF_CODEC_ARGS_ENABLED)
        message(FATAL_ERROR
            "H.264/AAC requires a custom CEF built with "
            "ffmpeg_branding=Chrome proprietary_codecs=true. "
            "Place that build's args.gn at CEF_ROOT/codec-args.gn; "
            "see docs/twitch-playback.md. Stock CEF and runtime switches "
            "cannot provide missing decoders.")
    endif()
    if(AURORA_CEF_CODEC_ARGS_ENABLED)
        message(STATUS "CEF codec build arguments recorded: Chrome FFmpeg + proprietary codecs")
    else()
        message(STATUS "CEF H.264/AAC support unverified (no codec build arguments)")
    endif()
    # Use CEF's own CMake modules (cef_variables, cef_macros). find_package(CEF)
    # loads them and sets CEF_INCLUDE_PATH / CEF_LIBCEF_DLL_WRAPPER_PATH.
    list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
    find_package(CEF REQUIRED)
    if(APPLE)
        # CEF's cmake sets a 13.0 deployment target, but libc++'s
        # floating-point std::to_chars (used by std::format) needs 13.3.
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
    endif()
    if(MSVC)
        # CEF's cef_variables.cmake clears CMAKE_CXX_FLAGS when using the Ninja
        # generator (to quiet warnings in CEF's own project). That strips
        # /EHsc, so doctest (and the STL, and Qt) think exceptions are off.
        # Restore exception handling + RTTI for our own targets.
        string(APPEND CMAKE_CXX_FLAGS " /EHsc /GR /utf-8")
    endif()
    # Create the imported target for libcef (libcef.lib import library) so we
    # can link the C runtime symbols exported by libcef.dll.
    # macOS is different: libcef is never linked there. The framework is
    # loaded at runtime from the app bundle (CefScopedLibraryLoader, see
    # cef::load_library), so there is no import library to wrap.
    if(NOT APPLE AND NOT TARGET libcef_lib)
        ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
    endif()
    # Build the CEF C++ wrapper (libcef_dll_wrapper). It is a static lib that
    # implements the C++ API (CefBrowserHost, CefRefPtr, etc.); it does NOT link
    # libcef itself, so consumers must link libcef_lib too.
    add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_BINARY_DIR}/libcef_dll_wrapper")
    # The wrapper is third-party code and inherits Aurora's directory-level
    # warning flags, so with AURORA_WERROR its own style (variadic macros with
    # no argument, C-style casts) fails the build. Silence warnings for that
    # target only; -w / /W0 come after the inherited flags, so they win.
    if(TARGET libcef_dll_wrapper)
        if(MSVC)
            target_compile_options(libcef_dll_wrapper PRIVATE /W0)
        else()
            target_compile_options(libcef_dll_wrapper PRIVATE -w)
        endif()
    endif()
    # Windows Chromium sandbox. Since M138 CEF ships no cef_sandbox.lib that a
    # non-Chromium toolchain can link; the sandbox comes in a prebuilt
    # bootstrap.exe that loads "<its own name>.dll" and calls RunWinMain with
    # the sandbox_info pointer. AURORA_WIN_SANDBOX builds aurora_browser as a
    # DLL and copies bootstrap.exe in as aurora_browser.exe (src/app). Default
    # ON whenever the CEF distribution carries bootstrap.exe; OFF builds a
    # plain executable that runs unsandboxed (and logs that it does).
    if(WIN32)
        if(EXISTS "${CEF_ROOT}/Release/bootstrap.exe")
            set(_aurora_win_sandbox_default ON)
        else()
            set(_aurora_win_sandbox_default OFF)
        endif()
        option(AURORA_WIN_SANDBOX
            "Windows: build the browser as a DLL run by CEF's sandboxed bootstrap.exe"
            ${_aurora_win_sandbox_default})
        if(AURORA_WIN_SANDBOX AND NOT EXISTS "${CEF_ROOT}/Release/bootstrap.exe")
            message(FATAL_ERROR "AURORA_WIN_SANDBOX=ON but ${CEF_ROOT}/Release/bootstrap.exe is missing")
        endif()
        message(STATUS "Windows Chromium sandbox (bootstrap.exe): ${AURORA_WIN_SANDBOX}")
    endif()
    add_library(aurora_cef STATIC
        src/cef/CefAdapter.cpp
        src/cef/CefLifecycle.cpp
    )
    if(APPLE)
        # AppKit glue: AuroraApplication (CefAppProtocol) and NSView sizing.
        target_sources(aurora_cef PRIVATE src/cef/CefMacSupport.mm)
        target_link_libraries(aurora_cef PUBLIC aurora_core libcef_dll_wrapper
            "-framework AppKit" "-framework Cocoa" "-framework IOSurface")
    else()
        target_link_libraries(aurora_cef PUBLIC aurora_core libcef_dll_wrapper libcef_lib)
    endif()
    if(AURORA_CEF_CODEC_ARGS_ENABLED)
        target_compile_definitions(aurora_cef PRIVATE AURORA_CEF_CODEC_ARGS_ENABLED=1)
    endif()
    # The wrapper sets its include dir PRIVATE, so it does not propagate to
    # consumers. aurora_cef TUs include CEF headers as "include/cef_browser.h",
    # which needs CEF_ROOT (= CEF_INCLUDE_PATH) on the include path.
    # SYSTEM: CEF's headers do not compile clean under Aurora's warning set
    # (constructor parameters shadow members, unused parameters in interface
    # stubs), and they are not ours to fix. Our own CEF sources are still
    # checked with every warning enabled.
    target_include_directories(aurora_cef SYSTEM PRIVATE ${CEF_INCLUDE_PATH})
    # CEF headers use base-class brace-init that MSVC rejects under /permissive-;
    # relax conformance for the CEF translation units only.
    if(MSVC)
        target_compile_options(aurora_cef PRIVATE /permissive)
        # windows.h defines min/max as macros, which turns the
        # std::numeric_limits<...>::max() calls inside CEF's own headers
        # (cef_ref_counted.h) into syntax errors. CEF's headers pull in
        # windows.h themselves, so the guard has to be a target-wide define
        # rather than an include-order rule in our sources.
        target_compile_definitions(aurora_cef PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    # On X11 the embedded browser is a child window that does not follow its
    # parent's size; CEF offers no resize API, so the adapter moves the window
    # with Xlib on the display connection CEF itself opened. Only two Xlib
    # entry points are used and they are declared in the adapter, so no X11
    # headers are needed here — just the library.
    if(UNIX AND NOT APPLE)
        find_package(X11 REQUIRED)
        target_link_libraries(aurora_cef PRIVATE ${X11_LIBRARIES})
    endif()
endif()

# --- Bootstrap launcher ------------------------------------------------------
# The program the self-extracting installer runs. It verifies the build ID,
# materializes %LOCALAPPDATA%/Aurora/cef/<build_id>/ and then starts
# aurora_browser. Links aurora_core only: no Qt, no CEF, no SQLite on the
# critical path of every launch.
if(AURORA_BUILD_BOOTSTRAP)
    add_executable(aurora_bootstrap src/bootstrap/main.cpp)
    target_link_libraries(aurora_bootstrap PRIVATE aurora_core)
    target_compile_definitions(aurora_bootstrap PRIVATE
        AURORA_VERSION_STRING="${PROJECT_VERSION}"
        AURORA_PLATFORM_STRING="${AURORA_PLATFORM}")
    if(WIN32)
        # GUI subsystem: double-clicking the SFX must not flash a console. A
        # console is attached on demand by --shell (see src/bootstrap/main.cpp).
        set_target_properties(aurora_bootstrap PROPERTIES WIN32_EXECUTABLE TRUE)
        # WIN32_EXECUTABLE sets /subsystem:windows, which expects WinMain.
        # The bootstrap uses standard main(); point the CRT entry at
        # mainCRTStartup so /subsystem:windows still calls main().
        if(MSVC)
            target_link_options(aurora_bootstrap PRIVATE /ENTRY:mainCRTStartup)
        endif()
        # Same icon as the browser, since this is the executable the user sees.
        set(AURORA_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/aurora.ico")
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/aurora.rc.in
            ${CMAKE_CURRENT_BINARY_DIR}/aurora_bootstrap.rc
            @ONLY)
        target_sources(aurora_bootstrap PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/aurora_bootstrap.rc)
    endif()
    if(NOT MSVC)
        # std::filesystem needs an extra library on older GCC/Clang toolchains.
        find_package(Threads REQUIRED)
        target_link_libraries(aurora_bootstrap PRIVATE Threads::Threads)
    endif()
endif()

# --- Packaging tool ----------------------------------------------------------
if(AURORA_BUILD_TOOLS)
    add_executable(aurora_buildid tools/buildid_main.cpp)
    target_link_libraries(aurora_buildid PRIVATE aurora_core)
    target_compile_definitions(aurora_buildid PRIVATE
        AURORA_VERSION_STRING="${PROJECT_VERSION}"
        AURORA_PLATFORM_STRING="${AURORA_PLATFORM}")
endif()

# --- Custom Windows installer ------------------------------------------------
# A small native Win32/Direct2D setup UI. The packaging script appends the
# packaged app as a ZIP payload; see packaging/windows/build_installer.sh.
if(AURORA_BUILD_INSTALLER)
    add_subdirectory(installer)
endif()

# --- Tests -------------------------------------------------------------------
if(AURORA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Qt browser application --------------------------------------------------
if(AURORA_BUILD_APP)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Svg)
    qt_standard_project_setup()
    add_subdirectory(src/app)
endif()

message(STATUS "")
message(STATUS "Aurora ${PROJECT_VERSION} configuration:")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests:          ${AURORA_BUILD_TESTS}")
message(STATUS "  CEF layer:      ${AURORA_ENABLE_CEF}")
message(STATUS "  Require codecs: ${AURORA_REQUIRE_H264_AAC}")
message(STATUS "  Qt app:         ${AURORA_BUILD_APP}")
message(STATUS "  Bootstrap:      ${AURORA_BUILD_BOOTSTRAP}")
message(STATUS "  Build-ID tool:  ${AURORA_BUILD_TOOLS}")
message(STATUS "  Installer:      ${AURORA_BUILD_INSTALLER}")
message(STATUS "  Platform tag:   ${AURORA_PLATFORM}")
message(STATUS "")

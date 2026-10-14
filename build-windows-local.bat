@echo off
setlocal EnableDelayedExpansion
rem ===========================================================================
rem  Aurora - local Windows build (replaces .github/workflows/build-windows.yml)
rem
rem  Double-click or run with no args for a menu:
rem    1) Full app + CEF   -> saves Aurora.exe        to builds\app
rem    2) Qt shell         -> saves Aurora-QtShell.exe to builds\shell
rem
rem  Or run non-interactively:
rem    build-windows-local.bat cef | qt | all
rem
rem  Override paths if your install is not on the default PATH:
rem    set QT_ROOT=C:\Qt\6.8.0\msvc2022_64
rem    set PYTHON=python
rem    set SEVENZIP=C:\Program Files\7-Zip\7z.exe
rem    set CEF_ROOT=C:\path\to\codec-enabled-cef
rem
rem  Requires: Visual Studio 2022 (Desktop C++), Qt 6.5+, CMake, Ninja, Git,
rem            7-Zip, and Git Bash (ships with Git for Windows).
rem ===========================================================================

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
cd /d "%REPO%"

set "OUT_ROOT=%USERPROFILE%\Desktop\builds"
set "APP_OUT=%OUT_ROOT%\app"
set "SHELL_OUT=%OUT_ROOT%\shell"

set "MODE=%~1"
if "%MODE%"=="" goto :menu
goto :detect

:menu
echo.
echo === Aurora local build ===
echo   1) Full app + CEF  -^> %APP_OUT%
echo   2) Qt shell        -^> %SHELL_OUT%
echo.
choice /c 12 /n /m "Select 1 or 2: "
if errorlevel 2 set "MODE=qt"  & goto :detect
if errorlevel 1 set "MODE=cef" & goto :detect

:detect
echo.
echo [info] Mode: %MODE%
echo [info] Output: %APP_OUT% ^(cef^) / %SHELL_OUT% ^(qt^)
echo.

rem ---- start build timer (unix epoch seconds, crosses midnight safely) ----------
for /f %%t in ('powershell -NoProfile -Command "[DateTimeOffset]::Now.ToUnixTimeSeconds()"') do set "BUILD_START=%%t"

rem ---- locate Git Bash (for the .sh helper scripts) -------------------------
set "BASH_EXE="
for %%P in ( "%ProgramFiles%\Git\bin\bash.exe" "%ProgramFiles(x86)%\Git\bin\bash.exe" "%ProgramW6432%\Git\bin\bash.exe" ) do (
    if exist "%%~P" set "BASH_EXE=%%~P"
)
if "%BASH_EXE%"=="" (
    for /f "delims=" %%B in ('where bash.exe 2^>nul') do set "BASH_EXE=%%B"
)
if "%BASH_EXE%"=="" (
    echo ERROR: bash.exe not found. Install Git for Windows ^(git-scm.com^).
    pause & exit /b 1
)
echo [ok] bash: %BASH_EXE%

rem ---- Python (fetch_cef.sh uses python3; allow override) --------------------
set "PY=%PYTHON%"
if "%PY%"=="" (
    python3 --version >nul 2>&1
    if !errorlevel! equ 0 ( set "PY=python3" ) else (
        python --version >nul 2>&1
        if !errorlevel! equ 0 ( set "PY=python" ) else (
            echo ERROR: python not found. Install python.org Python, or set PYTHON.
            pause & exit /b 1
        )
    )
)
echo [ok] python: %PY%

rem ---- CMake + Ninja -------------------------------------------------------
where cmake >nul 2>&1 || ( echo ERROR: cmake not found on PATH. & pause & exit /b 1 )
where ninja >nul 2>&1 || ( echo ERROR: ninja not found on PATH. & pause & exit /b 1 )
echo [ok] cmake, ninja

rem ---- MSVC developer environment (Visual Studio 2022) --------------------
if not defined VSCMD_VER (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo ERROR: vswhere.exe not found. Install Visual Studio 2022 with the
        echo        "Desktop development with C++" workload.
        pause & exit /b 1
    )
    for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -property installationPath`) do set "VSINSTALL=%%I"
    if not exist "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat" (
        echo ERROR: MSVC x64 build tools not found. Install the C++ workload.
        pause & exit /b 1
    )
    call "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat" x64 >nul
    if !errorlevel! neq 0 ( echo ERROR: vcvars64 failed. & pause & exit /b 1 )
)
echo [ok] MSVC environment

rem ---- Qt6 ----------------------------------------------------------------
set "QT=%QT_ROOT%"
if "%QT%"=="" (
    for %%D in ( "%ProgramW6432%\Qt" "%ProgramFiles%\Qt" "C:\Qt" "%USERPROFILE%\Desktop" ) do (
        if exist "%%~D" if not defined QT for /f "delims=" %%V in ('dir /b /ad "%%~D" 2^>nul') do (
            if exist "%%~D\%%V\msvc2022_64" set "QT=%%~D\%%V\msvc2022_64"
        )
    )
)
if "%QT%"=="" (
    echo ERROR: Qt6 not found. Install Qt 6.5+ ^(msvc2022_64^), or set QT_ROOT.
    pause & exit /b 1
)
set "PATH=%QT%\bin;%PATH%"
if not exist "%QT%\bin\windeployqt.exe" (
    echo ERROR: windeployqt.exe not found under %QT%\bin. Bad Qt install?
    pause & exit /b 1
)
echo [ok] Qt6: %QT%

rem ---- 7-Zip (only needed for SFX packaging) ------------------------------
set "SZ=%SEVENZIP%"
if "%SZ%"=="" (
    for %%P in ( "%ProgramFiles%\7-Zip\7z.exe" "%ProgramFiles(x86)%\7-Zip\7z.exe" ) do if exist "%%~P" set "SZ=%%~P"
)
if "%SZ%"=="" ( where 7z >nul 2>&1 && for /f "delims=" %%Z in ('where 7z') do set "SZ=%%Z" )
if not "%SZ%"=="" echo [ok] 7z: %SZ%

rem =========================================================================
if /I "%MODE%"=="qt"  goto :build_qt
if /I "%MODE%"=="cef" goto :build_cef
if /I "%MODE%"=="all" ( call :build_qt && call :build_cef ) & goto :done
echo ERROR: unknown mode "%MODE%". & pause & exit /b 1

:build_qt
echo.
echo --- Building Qt6 shell (no CEF) ---
if exist build rmdir /s /q build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAURORA_BUILD_TESTS=ON -DAURORA_BUILD_APP=ON
if !errorlevel! neq 0 ( echo ERROR: configure failed. & pause & exit /b 1 )
cmake --build build --config Release
if !errorlevel! neq 0 ( echo ERROR: build failed. & pause & exit /b 1 )

echo --- Running unit tests ---
build\bin\aurora_tests.exe || ( echo ERROR: tests failed. & pause & exit /b 1 )

echo --- Packaging ---
if exist package rmdir /s /q package
mkdir package
copy build\bin\aurora_browser.exe package\ >nul
copy build\bin\aurora_bootstrap.exe package\ >nul
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw package\aurora_browser.exe
call :make_sfx package Aurora-QtShell.exe assets\icons\aurora.ico "%SHELL_OUT%"
goto :done

:build_cef
echo.
if defined CEF_ROOT (
    set "CEF_DIR=%CEF_ROOT%"
    set "CODEC_REQUIRE=-DAURORA_REQUIRE_H264_AAC=ON"
    echo --- Using supplied CEF: !CEF_DIR! ---
) else (
    echo --- Fetching stock CEF (Windows; H.264/AAC not guaranteed) ---
    "%BASH_EXE%" -c "cd '%REPO%' && PLATFORM=windows64 PYTHON='%PY%' ./scripts/fetch_cef.sh"
    if !errorlevel! neq 0 ( echo ERROR: fetch_cef failed. & pause & exit /b 1 )
    set "CEF_DIR=%REPO%\third_party\cef"
    set "CODEC_REQUIRE="
)

echo --- Configuring with CEF ---
if exist build rmdir /s /q build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DAURORA_BUILD_TESTS=ON -DAURORA_ENABLE_CEF=ON -DAURORA_BUILD_APP=ON ^
    -DCEF_ROOT="!CEF_DIR!" !CODEC_REQUIRE! -DCEF_RUNTIME_LIBRARY_FLAG=/MD
if !errorlevel! neq 0 ( echo ERROR: configure failed. & pause & exit /b 1 )
cmake --build build --config Release
if !errorlevel! neq 0 ( echo ERROR: build failed. & pause & exit /b 1 )

echo --- Running unit tests ---
build\bin\aurora_tests.exe || ( echo ERROR: tests failed. & pause & exit /b 1 )

echo --- Packaging with CEF resources ---
if exist package rmdir /s /q package
mkdir package
copy build\bin\aurora_browser.exe package\ >nul
copy build\bin\aurora_bootstrap.exe package\ >nul
windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw package\aurora_browser.exe
xcopy /e /y /i "!CEF_DIR!\Resources\*" package\ >nul 2>&1
xcopy /e /y /i "!CEF_DIR!\Release\*" package\ >nul 2>&1
del /q package\*.lib package\*.exp package\*.pdb 2>nul

echo --- Stamping build ID ---
build\bin\aurora_buildid.exe generate --root package --cef-root "!CEF_DIR!" --platform windows64 --out package\aurora_build.manifest
if !errorlevel! neq 0 ( echo ERROR: manifest generate failed. & pause & exit /b 1 )
build\bin\aurora_buildid.exe show --manifest package\aurora_build.manifest
build\bin\aurora_buildid.exe verify --manifest package\aurora_build.manifest --root package --full
if !errorlevel! neq 0 ( echo ERROR: manifest verify failed - package mismatch. & pause & exit /b 1 )
call :make_sfx package Aurora.exe assets\icons\aurora.ico "%APP_OUT%"
goto :done

:make_sfx
rem  %1 = package dir, %2 = output exe name, %3 = icon, %4 = output folder
set "PKG=%~1"
set "OUT=%~2"
set "ICO=%~3"
set "OUTDIR=%~4"
echo --- Building self-extracting .exe ---
if "%SZ%"=="" (
    echo WARNING: 7-Zip not found - skipping SFX. Packaging folder as zip instead.
    if not exist "%OUTDIR%" mkdir "%OUTDIR%"
    if exist "%SZ%" ( "%SZ%" a -tzip -mx9 "%OUTDIR%\%OUT:.exe=%.zip" "%PKG%\*" ) else (
        echo Cannot zip without 7z. Package folder left at: %REPO%\%PKG%
    )
    exit /b 0
)
rem rcedit is best-effort, exactly like CI.
curl -fsSL -o rcedit-x64.exe https://github.com/electron/rcedit/releases/latest/download/rcedit-x64.exe 2>nul
if exist rcedit-x64.exe ( set "RCEDIT=%REPO%\rcedit-x64.exe" ) else ( set "RCEDIT=rcedit-x64.exe" )
"%BASH_EXE%" -c "cd '%REPO%' && SEVENZIP='%SZ%' RCEDIT='%RCEDIT%' bash packaging/windows/build_sfx.sh '%PKG%' '%OUT%' '%ICO%'"
if !errorlevel! neq 0 (
    echo WARNING: SFX build failed - zipping the package folder instead.
    if not exist "%OUTDIR%" mkdir "%OUTDIR%"
    "%SZ%" a -tzip -mx9 "%OUTDIR%\%OUT:.exe=%.zip" "%PKG%\*"
    echo [ok] Wrote: %OUTDIR%\%OUT:.exe=%.zip
    exit /b 0
)
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
copy /y "%OUT%" "%OUTDIR%\%OUT%" >nul
echo [ok] Wrote: %OUTDIR%\%OUT%
exit /b 0

:done
echo.
if defined BUILD_START (
    for /f %%t in ('powershell -NoProfile -Command "[int64][DateTimeOffset]::Now.ToUnixTimeSeconds()-%BUILD_START%"') do set "ELAPSED_RAW=%%t"
    set /a "ELAPSED_MIN=ELAPSED_RAW/60"
    set /a "ELAPSED_SEC=ELAPSED_RAW - ELAPSED_MIN*60"
    echo Build time: !ELAPSED_MIN!m !ELAPSED_SEC!s
)
echo ============================================================
echo DONE.
echo   Full app + CEF : %APP_OUT%
echo   Qt shell       : %SHELL_OUT%
echo ============================================================
if "%~1"=="" pause
exit /b 0

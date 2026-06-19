@echo off
setlocal enabledelayedexpansion

REM Build script for browser-emulator on Windows with MSVC

set "SCRIPT_DIR=%~dp0"
set "BUILD_DIR=%SCRIPT_DIR%build"

echo Building browser-emulator for Windows with MSVC...

REM Check for cmake
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo Error: cmake is not in PATH
    echo Install CMake from https://cmake.org/download/ and ensure it is available in PATH.
    exit /b 1
)

REM Check for MSVC environment
if not defined VSCMD_VER (
    echo Error: MSVC environment not detected.
    echo Please run this script from a "Developer Command Prompt for VS"
    echo or call vcvarsall.bat first.
    exit /b 1
)

REM Create build directory
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

REM Configure with CMake
echo Configuring with CMake...
cmake .. ^
    -DBE_BUILD_TESTS=ON ^
    -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>"

if %ERRORLEVEL% NEQ 0 (
    echo CMake configuration failed.
    exit /b 1
)

REM Build Release
echo Building Release...
cmake --build . --config Release

if %ERRORLEVEL% NEQ 0 (
    echo Build failed.
    exit /b 1
)

echo.
echo Build complete!
echo.
echo To run tests:
echo   %BUILD_DIR%\tests\Release\browser-emulator-tests.exe

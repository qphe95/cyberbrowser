@echo off
setlocal enabledelayedexpansion

:: ============================================================================
:: Build cyberbrowser on Windows with MSVC using CMake.
:: This script finds Visual Studio, sets up the MSVC environment, then lets
:: CMake generate and build the project (the single source of truth).
:: ============================================================================

:: Find Visual Studio installation
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Please install Visual Studio or Build Tools.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo ERROR: Could not find Visual Studio installation.
    exit /b 1
)

echo Found Visual Studio at: %VSPATH%

:: Setup MSVC environment for x64
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo ERROR: Failed to setup MSVC environment.
    exit /b 1
)

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build-msvc"

if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%"

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

cmake --build . --config Release --target cyberbrowser-tests -j4
if errorlevel 1 exit /b 1

echo.
echo Build complete. Test binary: %BUILD%\tests\Release\cyberbrowser-tests.exe

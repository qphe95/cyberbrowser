@echo off
setlocal

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build-msvc"
set "DIST=%ROOT%\dist"
set "PROJ_ROOT=%ROOT%\.."

echo Cleaning MSVC build artifacts...

if exist "%BUILD%" (
    rmdir /S /Q "%BUILD%"
    echo   Removed %BUILD%
)

if exist "%DIST%\bgmdwnldr-desktop.exe" (
    del /Q "%DIST%\bgmdwnldr-desktop.exe"
    echo   Removed %DIST%\bgmdwnldr-desktop.exe
)

if exist "%DIST%\bgmdwnldr-desktop.pdb" (
    del /Q "%DIST%\bgmdwnldr-desktop.pdb"
    echo   Removed %DIST%\bgmdwnldr-desktop.pdb
)

if exist "%PROJ_ROOT%\bgmdwnldr-desktop.exe" (
    del /Q "%PROJ_ROOT%\bgmdwnldr-desktop.exe"
    echo   Removed %PROJ_ROOT%\bgmdwnldr-desktop.exe
)

echo Done.

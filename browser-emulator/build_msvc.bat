@echo off
setlocal enabledelayedexpansion

:: ============================================================================
:: Build script for browser-emulator on Windows with MSVC
:: Uses vswhere.exe to find Visual Studio, then compiles with cl.exe / link.exe
:: ============================================================================

:: Find Visual Studio installation
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Please install Visual Studio or Build Tools.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
    set "VSPATH=%%i"
)

if not defined VSPATH (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -all -products * -property installationPath`) do (
        set "VSPATH=%%i"
        goto :found_vs
    )
)
:found_vs

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

:: Setup paths
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build-msvc"
set "DIST=%ROOT%\dist"
set "PROJ_ROOT=%ROOT%\.."

if not exist "%BUILD%" mkdir "%BUILD%"
if not exist "%DIST%" mkdir "%DIST%"

:: Find dependencies
call :find_deps
if errorlevel 1 exit /b 1

:: Generate embedded shaders
echo.
echo Generating embedded shaders...
if exist "%BUILD%\embedded_shaders\embedded_shaders.c" (
    echo Using existing embedded shaders in %BUILD%\embedded_shaders\
) else (
    python "%PROJ_ROOT%\scripts\embed_shaders.py" "%BUILD%\embedded_shaders" "%PROJ_ROOT%\app\src\main\assets\triangle.vert.spv" "%PROJ_ROOT%\app\src\main\assets\triangle.frag.spv"
    if errorlevel 1 (
        echo ERROR: Failed to generate embedded shaders. Install Python or pre-generate them.
        exit /b 1
    )
)

:: Compile flags
set "INC="
set INC=%INC% /I"%ROOT%\include\win32"
set INC=%INC% /I"%ROOT%\include"
set INC=%INC% /I"%ROOT%\src"
set INC=%INC% /I"%ROOT%\third_party\quickjs"
set INC=%INC% /I"%ROOT%\third_party\mbedtls\include"
set INC=%INC% /I"%ROOT%\third_party\mbedtls"
set INC=%INC% /I"%ROOT%\third_party\mbedtls\tf-psa-crypto\include"
set INC=%INC% /I"%ROOT%\third_party\mbedtls\tf-psa-crypto"
set INC=%INC% /I"%ROOT%\third_party\mbedtls\tf-psa-crypto\core"
set INC=%INC% /I"%ROOT%\third_party\mbedtls\tf-psa-crypto\drivers\builtin\include"
set INC=%INC% /I"%ROOT%\third_party\mbedtls\tf-psa-crypto\drivers\builtin\src"
set INC=%INC% /I"%PROJ_ROOT%\app\src\main\cpp"
set INC=%INC% /I"%BUILD%\embedded_shaders"

if defined VULKAN_INC set "INC=%INC% %VULKAN_INC%"
if defined CURL_INC set "INC=%INC% %CURL_INC%"
if defined GLFW_INC set "INC=%INC% %GLFW_INC%"

set "DEFS=/DCONFIG_VERSION=\"2024-02-14\" /D_GNU_SOURCE /DBE_PLATFORM_WINDOWS /DCURL_STATICLIB /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DVK_USE_PLATFORM_WIN32_KHR"
set "CFLAGS=/W4 /O2 /Zi /nologo /MT /Fd%BUILD%\ %INC% %DEFS%"
set "CXXFLAGS=%CFLAGS% /EHsc /std:c++20"

set "QJSFLAGS=%CFLAGS% /wd4018 /wd4100 /wd4189 /FImsvc_quickjs_compat.h"
set "QJSCXXFLAGS=%CXXFLAGS% /wd4018 /wd4100 /wd4189 /FImsvc_quickjs_compat.h"

:: ============================================================================
:: Compile mbedtls
:: ============================================================================
echo.
echo Compiling mbedtls...
if not exist "%BUILD%\mbedtls\library" mkdir "%BUILD%\mbedtls\library"
if not exist "%BUILD%\mbedtls\core" mkdir "%BUILD%\mbedtls\core"
if not exist "%BUILD%\mbedtls\drivers" mkdir "%BUILD%\mbedtls\drivers"

:: Build file list for mbedtls/library (excluding mbedtls_config.c)
set "MBEDTLS_LIB_FILES="
for %%f in ("%ROOT%\third_party\mbedtls\library\*.c") do (
    if /I not "%%~nxf"=="mbedtls_config.c" (
        set "MBEDTLS_LIB_FILES=!MBEDTLS_LIB_FILES! %%f"
    )
)
pushd "%BUILD%\mbedtls\library"
cl %CFLAGS% /c %MBEDTLS_LIB_FILES%
if errorlevel 1 (
    popd
    exit /b 1
)
popd

pushd "%BUILD%\mbedtls\core"
cl %CFLAGS% /c "%ROOT%\third_party\mbedtls\tf-psa-crypto\core\*.c"
if errorlevel 1 (
    popd
    exit /b 1
)
popd

pushd "%BUILD%\mbedtls\drivers"
cl %CFLAGS% /c "%ROOT%\third_party\mbedtls\tf-psa-crypto\drivers\builtin\src\*.c"
if errorlevel 1 (
    popd
    exit /b 1
)
popd

:: ============================================================================
:: Compile quickjs
:: ============================================================================
echo.
echo Compiling quickjs...
cl %QJSFLAGS% /c "%ROOT%\third_party\quickjs\libregexp.c"       /Fo"%BUILD%\libregexp.obj"
if errorlevel 1 exit /b 1
cl %QJSFLAGS% /c "%ROOT%\third_party\quickjs\libunicode.c"      /Fo"%BUILD%\libunicode.obj"
if errorlevel 1 exit /b 1
cl %QJSFLAGS% /c "%ROOT%\third_party\quickjs\cutils.c"          /Fo"%BUILD%\cutils.obj"
if errorlevel 1 exit /b 1
cl %QJSFLAGS% /c "%ROOT%\third_party\quickjs\dtoa.c"            /Fo"%BUILD%\dtoa.obj"
if errorlevel 1 exit /b 1
cl %QJSFLAGS% /c "%ROOT%\third_party\quickjs\js_atom_cache.c"   /Fo"%BUILD%\js_atom_cache.obj"
if errorlevel 1 exit /b 1
cl %QJSFLAGS% /c "%ROOT%\third_party\quickjs\js_fast_dispatch.c" /Fo"%BUILD%\js_fast_dispatch.obj"
if errorlevel 1 exit /b 1
cl %QJSCXXFLAGS% /c "%ROOT%\third_party\quickjs\quickjs.cpp"    /Fo"%BUILD%\quickjs.obj"
if errorlevel 1 exit /b 1
cl %QJSCXXFLAGS% /c "%ROOT%\third_party\quickjs\quickjs_gc_unified.cpp" /Fo"%BUILD%\quickjs_gc_unified.obj"
if errorlevel 1 exit /b 1

:: ============================================================================
:: Compile browser-emulator library
:: ============================================================================
echo.
echo Compiling browser-emulator...
cl %CXXFLAGS% /c "%ROOT%\src\html_media_extract.cpp"  /Fo"%BUILD%\html_media_extract.obj"
if errorlevel 1 exit /b 1
cl %CXXFLAGS% /c "%ROOT%\src\js_quickjs.cpp"          /Fo"%BUILD%\js_quickjs.obj"
if errorlevel 1 exit /b 1
cl %CXXFLAGS% /c "%ROOT%\src\browser_api_impl.cpp"    /Fo"%BUILD%\browser_api_impl.obj"
if errorlevel 1 exit /b 1
cl %CXXFLAGS% /c "%ROOT%\src\html_dom.cpp"            /Fo"%BUILD%\html_dom.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%ROOT%\src\url_analyzer.c"          /Fo"%BUILD%\url_analyzer.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%ROOT%\src\tls_client.c"            /Fo"%BUILD%\tls_client.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%ROOT%\src\http_download.c"         /Fo"%BUILD%\http_download.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%ROOT%\src\platform\platform.c"     /Fo"%BUILD%\platform.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%ROOT%\src\platform\platform_windows.c" /Fo"%BUILD%\platform_windows.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%ROOT%\src\platform\http_curl.c"    /Fo"%BUILD%\http_curl.obj"
if errorlevel 1 exit /b 1

:: ============================================================================
:: Compile desktop app
:: ============================================================================
echo.
echo Compiling desktop app...
cl %CXXFLAGS% /c "%PROJ_ROOT%\app\src\main\cpp\main_windows.cpp"    /Fo"%BUILD%\main_windows.obj"
if errorlevel 1 exit /b 1
cl %CXXFLAGS% /c "%PROJ_ROOT%\app\src\main\cpp\vulkan_ui.cpp"       /Fo"%BUILD%\vulkan_ui.obj"
if errorlevel 1 exit /b 1
cl %CXXFLAGS% /c "%PROJ_ROOT%\app\src\main\cpp\ui_layout.cpp"       /Fo"%BUILD%\ui_layout.obj"
if errorlevel 1 exit /b 1
cl %CXXFLAGS% /c "%PROJ_ROOT%\app\src\main\cpp\mp4_metadata.cpp"    /Fo"%BUILD%\mp4_metadata.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%PROJ_ROOT%\app\src\main\cpp\default_album_art.c" /Fo"%BUILD%\default_album_art.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS%   /c "%BUILD%\embedded_shaders\embedded_shaders.c"       /Fo"%BUILD%\embedded_shaders.obj"
if errorlevel 1 exit /b 1

:: ============================================================================
:: Collect objects and link
:: ============================================================================
echo.
echo Linking...

:: Build response file incrementally to avoid command line too long
set "RSP=%BUILD%\link_objs.rsp"
if exist "%RSP%" del "%RSP%"

for %%f in ("%BUILD%\mbedtls\library\*.obj") do echo "%%f" >> "%RSP%"
for %%f in ("%BUILD%\mbedtls\core\*.obj")     do echo "%%f" >> "%RSP%"
for %%f in ("%BUILD%\mbedtls\drivers\*.obj")  do echo "%%f" >> "%RSP%"
echo "%BUILD%\libregexp.obj" >> "%RSP%"
echo "%BUILD%\libunicode.obj" >> "%RSP%"
echo "%BUILD%\cutils.obj" >> "%RSP%"
echo "%BUILD%\dtoa.obj" >> "%RSP%"
echo "%BUILD%\js_atom_cache.obj" >> "%RSP%"
echo "%BUILD%\js_fast_dispatch.obj" >> "%RSP%"
echo "%BUILD%\quickjs.obj" >> "%RSP%"
echo "%BUILD%\quickjs_gc_unified.obj" >> "%RSP%"
echo "%BUILD%\html_media_extract.obj" >> "%RSP%"
echo "%BUILD%\js_quickjs.obj" >> "%RSP%"
echo "%BUILD%\browser_api_impl.obj" >> "%RSP%"
echo "%BUILD%\html_dom.obj" >> "%RSP%"
echo "%BUILD%\url_analyzer.obj" >> "%RSP%"
echo "%BUILD%\tls_client.obj" >> "%RSP%"
echo "%BUILD%\http_download.obj" >> "%RSP%"
echo "%BUILD%\platform.obj" >> "%RSP%"
echo "%BUILD%\platform_windows.obj" >> "%RSP%"
echo "%BUILD%\http_curl.obj" >> "%RSP%"
echo "%BUILD%\main_windows.obj" >> "%RSP%"
echo "%BUILD%\vulkan_ui.obj" >> "%RSP%"
echo "%BUILD%\ui_layout.obj" >> "%RSP%"
echo "%BUILD%\mp4_metadata.obj" >> "%RSP%"
echo "%BUILD%\default_album_art.obj" >> "%RSP%"
echo "%BUILD%\embedded_shaders.obj" >> "%RSP%"

echo %VULKAN_LIB% >> "%RSP%"
echo %GLFW_LIB% >> "%RSP%"
echo %CURL_LIB% >> "%RSP%"
echo gdi32.lib >> "%RSP%"
echo user32.lib >> "%RSP%"
echo kernel32.lib >> "%RSP%"
echo shell32.lib >> "%RSP%"
echo ole32.lib >> "%RSP%"
echo uuid.lib >> "%RSP%"
echo winmm.lib >> "%RSP%"
echo advapi32.lib >> "%RSP%"
echo ws2_32.lib >> "%RSP%"
echo bcrypt.lib >> "%RSP%"
echo crypt32.lib >> "%RSP%"
echo Iphlpapi.lib >> "%RSP%"
echo Secur32.lib >> "%RSP%"
echo C:\vcpkg\installed\x64-windows-static\lib\zs.lib >> "%RSP%"

link /OUT:"%DIST%\bgmdwnldr-desktop.exe" /SUBSYSTEM:CONSOLE /MACHINE:X64 /DEBUG /nologo @"%RSP%"
if errorlevel 1 (
    echo ERROR: Link failed.
    exit /b 1
)

echo.
echo Build complete: %DIST%\bgmdwnldr-desktop.exe

:: Copy to project root for convenience
copy /Y "%DIST%\bgmdwnldr-desktop.exe" "%PROJ_ROOT%\bgmdwnldr-desktop.exe" >nul
echo Also copied to: %PROJ_ROOT%\bgmdwnldr-desktop.exe

goto :eof

:: ============================================================================
:: Subroutine: find_deps
:: ============================================================================
:find_deps

:: --- Vulkan ---
if defined VULKAN_SDK (
    if exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
        set "VULKAN_INC=/I"%VULKAN_SDK%\Include""
        set "VULKAN_LIB=%VULKAN_SDK%\Lib\vulkan-1.lib"
        echo Found Vulkan SDK: %VULKAN_SDK%
        goto :vulkan_done
    )
)

:: Fallback to freetext deps
if exist "%PROJ_ROOT%\..\freetext\deps\vulkan-headers\include\vulkan\vulkan.h" (
    set "VULKAN_INC=/I"%PROJ_ROOT%\..\freetext\deps\vulkan-headers\include""
    set "VULKAN_LIB=%PROJ_ROOT%\..\freetext\deps\vulkan-headers\lib\vulkan-1.lib"
    echo Found Vulkan fallback in freetext deps
    goto :vulkan_done
)

:: Fallback to temp vulkan-1.lib (headers still needed)
if exist "%LOCALAPPDATA%\Temp\vulkan-1.lib" (
    set "VULKAN_LIB=%LOCALAPPDATA%\Temp\vulkan-1.lib"
    echo WARNING: Found vulkan-1.lib in temp but no headers. Set VULKAN_SDK for headers.
) else (
    echo WARNING: Vulkan SDK not found. Set VULKAN_SDK or install Vulkan SDK.
)

:vulkan_done

:: --- curl ---
if exist "C:\vcpkg\installed\x64-windows-static\include\curl\curl.h" (
    set "CURL_INC=/I"C:\vcpkg\installed\x64-windows-static\include""
    set "CURL_LIB=C:\vcpkg\installed\x64-windows-static\lib\libcurl.lib"
    echo Found curl in C:\vcpkg (x64-windows-static)
    goto :curl_done
)
if exist "C:\vcpkg\installed\x64-windows\include\curl\curl.h" (
    set "CURL_INC=/I"C:\vcpkg\installed\x64-windows\include""
    set "CURL_LIB=C:\vcpkg\installed\x64-windows\lib\libcurl.lib"
    echo Found curl in C:\vcpkg (x64-windows)
    goto :curl_done
)

echo ERROR: Could not find curl. Set CURL_ROOT or install via vcpkg:
echo   vcpkg install curl
exit /b 1

:curl_done

:: --- glfw3 ---
if exist "C:\vcpkg\installed\x64-windows-static\include\GLFW\glfw3.h" (
    set "GLFW_INC=/I"C:\vcpkg\installed\x64-windows-static\include""
    set "GLFW_LIB=C:\vcpkg\installed\x64-windows-static\lib\glfw3.lib"
    echo Found glfw3 in C:\vcpkg (x64-windows-static)
    goto :glfw_done
)
if exist "C:\vcpkg\installed\x64-windows\include\GLFW\glfw3.h" (
    set "GLFW_INC=/I"C:\vcpkg\installed\x64-windows\include""
    set "GLFW_LIB=C:\vcpkg\installed\x64-windows\lib\glfw3.lib"
    echo Found glfw3 in C:\vcpkg (x64-windows)
    goto :glfw_done
)

echo ERROR: Could not find glfw3. Set GLFW_ROOT or install via vcpkg:
echo   vcpkg install glfw3
exit /b 1

:glfw_done

goto :eof

# Browser Emulator Module
# A platform-agnostic library for browser emulation (HTML parsing + QuickJS execution)

cmake_minimum_required(VERSION 3.16)
project(browser-emulator VERSION 1.0.0 LANGUAGES C CXX)

# =============================================================================
# Options
# =============================================================================
option(BE_BUILD_TESTS "Build tests" ON)
option(BE_BUILD_SHARED "Build as shared library" OFF)

# =============================================================================
# Platform detection (must be before find_package so we know which curl to use)
# =============================================================================
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(BE_PLATFORM_MACOS ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(BE_PLATFORM_ANDROID ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(BE_PLATFORM_WINDOWS ON)
else()
    set(BE_PLATFORM_LINUX ON)
endif()

# =============================================================================
# Find dependencies
# =============================================================================
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# =============================================================================
# Source files
# =============================================================================
set(BE_SOURCES
    src/html_media_extract.cpp
    src/js_quickjs.cpp
    src/browser_api_impl.cpp
    src/html_dom.cpp
    src/preorder_compaction_array.cpp
    src/css_parser.cpp
    src/url_analyzer.c
    src/tls_client.c
    src/http_download.c
    
    # Platform abstraction
    src/platform/platform.c
)

# Platform-specific sources
if(BE_PLATFORM_MACOS OR BE_PLATFORM_LINUX)
    list(APPEND BE_SOURCES
        src/platform/platform_posix.c
        src/platform/http_curl.c
    )
elseif(BE_PLATFORM_ANDROID)
    list(APPEND BE_SOURCES
        src/platform/platform_android.c
    )
elseif(BE_PLATFORM_WINDOWS)
    list(APPEND BE_SOURCES
        src/platform/platform_windows.c
        src/platform/http_curl.c
    )
endif()

# QuickJS sources
set(QUICKJS_SOURCES
    third_party/quickjs/quickjs.cpp
    third_party/quickjs/libregexp.c
    third_party/quickjs/libunicode.c
    third_party/quickjs/cutils.c
    third_party/quickjs/dtoa.c
    third_party/quickjs/quickjs_gc_unified.cpp
    third_party/quickjs/js_atom_cache.c
    third_party/quickjs/js_fast_dispatch.c
)

# mbedtls sources (for TLS on macOS/Linux)
if(BE_PLATFORM_MACOS OR BE_PLATFORM_LINUX OR BE_PLATFORM_WINDOWS)
    file(GLOB MBEDTLS_SOURCES 
        third_party/mbedtls/library/*.c
        third_party/mbedtls/tf-psa-crypto/core/*.c
        third_party/mbedtls/tf-psa-crypto/drivers/builtin/src/*.c
    )
    list(FILTER MBEDTLS_SOURCES EXCLUDE REGEX "mbedtls_config\\.c$")
endif()

# =============================================================================
# Library target
# =============================================================================
set(BE_ALL_SOURCES ${BE_SOURCES} ${QUICKJS_SOURCES})
if(BE_PLATFORM_MACOS OR BE_PLATFORM_LINUX OR BE_PLATFORM_WINDOWS)
    list(APPEND BE_ALL_SOURCES ${MBEDTLS_SOURCES})
endif()

if(BE_BUILD_SHARED)
    add_library(browser-emulator SHARED ${BE_ALL_SOURCES})
else()
    add_library(browser-emulator STATIC ${BE_ALL_SOURCES})
endif()

# =============================================================================
# Include directories
# =============================================================================
target_include_directories(browser-emulator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/tf-psa-crypto/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/tf-psa-crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/tf-psa-crypto/core
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/tf-psa-crypto/drivers/builtin/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/tf-psa-crypto/drivers/builtin/src
)

if(BE_PLATFORM_WINDOWS)
    target_include_directories(browser-emulator BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/win32
    )
endif()

# =============================================================================
# Compile definitions
# =============================================================================
target_compile_definitions(browser-emulator PRIVATE
    CONFIG_VERSION=\"2024-02-14\"
    _GNU_SOURCE
)

if(BE_PLATFORM_MACOS)
    target_compile_definitions(browser-emulator PRIVATE BE_PLATFORM_MACOS)
elseif(BE_PLATFORM_ANDROID)
    target_compile_definitions(browser-emulator PRIVATE BE_PLATFORM_ANDROID)
elseif(BE_PLATFORM_WINDOWS)
    target_compile_definitions(browser-emulator PRIVATE BE_PLATFORM_WINDOWS)
else()
    target_compile_definitions(browser-emulator PRIVATE BE_PLATFORM_LINUX)
endif()

# =============================================================================
# Compile options
# =============================================================================
if(NOT MSVC)
    target_compile_options(browser-emulator PRIVATE
        -Wall
        -Wextra
        -O2
        -g
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC 14+ is strict about mixing designated and non-designated
        # initializers in C++. QuickJS and our C++ stubs use this pattern.
        # C++20 officially supports designated initializers.
        target_compile_options(browser-emulator PRIVATE -fpermissive)
        target_compile_options(browser-emulator PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)
    endif()
else()
    target_compile_options(browser-emulator PRIVATE
        /W4
        /O2
        /Zi
    )
endif()

# Enable C++11 for QuickJS
target_compile_features(browser-emulator PRIVATE cxx_std_11)

# Disable some warnings for QuickJS
if(NOT MSVC)
    set_source_files_properties(${QUICKJS_SOURCES} PROPERTIES
        COMPILE_FLAGS "-Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable"
    )
else()
    set_source_files_properties(${QUICKJS_SOURCES} PROPERTIES
        COMPILE_FLAGS "/wd4018 /wd4100 /wd4189"
    )
endif()

# =============================================================================
# Link libraries
# =============================================================================
target_link_libraries(browser-emulator
    PUBLIC
        CURL::libcurl
        Threads::Threads
)

if(NOT BE_PLATFORM_WINDOWS)
    target_link_libraries(browser-emulator PUBLIC m)
endif()

if(BE_PLATFORM_MACOS)
    # CoreFoundation for macOS-specific features
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    target_link_libraries(browser-emulator PRIVATE ${COREFOUNDATION_LIBRARY})
elseif(BE_PLATFORM_WINDOWS)
    target_link_libraries(browser-emulator PRIVATE ws2_32 crypt32 shell32 bcrypt)
    if(MSVC)
        # Static-link MSVC runtime for a portable executable
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# =============================================================================
# Desktop App (GLFW + Vulkan UI)
# =============================================================================
option(BE_BUILD_DESKTOP_APP "Build GLFW desktop app" ON)
if(BE_BUILD_DESKTOP_APP AND (BE_PLATFORM_MACOS OR BE_PLATFORM_LINUX OR BE_PLATFORM_WINDOWS))
    find_package(Vulkan REQUIRED)

    # Embed shaders into the binary as C arrays
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake)
    set(EMBEDDED_SHADERS_DIR ${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders)
    file(MAKE_DIRECTORY ${EMBEDDED_SHADERS_DIR})
    embed_shaders(
        OUTPUT_HEADER ${EMBEDDED_SHADERS_DIR}/embedded_shaders.h
        OUTPUT_SOURCE ${EMBEDDED_SHADERS_DIR}/embedded_shaders.c
        SHADERS
            ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/assets/triangle.vert.spv
            ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/assets/triangle.frag.spv
    )

    if(BE_PLATFORM_MACOS)
        set(DESKTOP_APP_MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/main_macos.cpp)
    else()
        set(DESKTOP_APP_MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/main_windows.cpp)
    endif()
    set(DESKTOP_APP_SOURCES
        ${DESKTOP_APP_MAIN}
        ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/vulkan_ui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/ui_layout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/mp4_metadata.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/default_album_art.c
        ${EMBEDDED_SHADERS_DIR}/embedded_shaders.c
    )

    add_executable(bgmdwnldr-desktop ${DESKTOP_APP_SOURCES})
    target_compile_features(bgmdwnldr-desktop PRIVATE cxx_std_11)

    target_include_directories(bgmdwnldr-desktop PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs
        ${EMBEDDED_SHADERS_DIR}
    )

    if(BE_PLATFORM_WINDOWS)
        target_include_directories(bgmdwnldr-desktop BEFORE PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/win32
        )
        target_compile_definitions(bgmdwnldr-desktop PRIVATE BE_PLATFORM_WINDOWS GLFW_STATIC)
        find_package(glfw3 CONFIG REQUIRED)
        if(TARGET glfw)
            set(GLFW_TARGET glfw)
        elseif(TARGET glfw3)
            set(GLFW_TARGET glfw3)
        else()
            message(FATAL_ERROR "GLFW target not found. Ensure glfw3 is installed (e.g., via vcpkg).")
        endif()
    else()
        find_package(glfw3 REQUIRED)
        set(GLFW_TARGET glfw)
    endif()

    target_link_libraries(bgmdwnldr-desktop PRIVATE
        browser-emulator
        ${GLFW_TARGET}
        Vulkan::Vulkan
    )
    if(BE_PLATFORM_WINDOWS)
        target_link_libraries(bgmdwnldr-desktop PRIVATE gdi32 user32 ole32 advapi32 uuid shell32)
    endif()
endif()

# =============================================================================
# Tests
# =============================================================================
if(BE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# =============================================================================
# Installation
# =============================================================================
include(GNUInstallDirs)

install(TARGETS browser-emulator
    EXPORT browser-emulator-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT browser-emulator-targets
    FILE browser-emulator-targets.cmake
    NAMESPACE browser-emulator::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/browser-emulator
)

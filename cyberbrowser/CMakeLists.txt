# Browser Emulator Module
# A platform-agnostic library for browser emulation (HTML parsing + QuickJS execution)

cmake_minimum_required(VERSION 3.16)
project(browser-emulator VERSION 1.0.0 LANGUAGES C CXX)

# =============================================================================
# Options
# =============================================================================
option(BE_BUILD_TESTS "Build tests" ON)
option(BE_BUILD_SHARED "Build as shared library" OFF)
option(BE_BUILD_VULKAN "Build Vulkan renderer backend" OFF)

# =============================================================================
# Platform detection
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
find_package(Threads REQUIRED)

# =============================================================================
# Source files
# =============================================================================
set(BE_SOURCES
    src/html_media_extract.cpp
    src/js_quickjs.cpp
    src/browser/crypto_api.cpp
    src/browser/storage_api.cpp
    src/browser/css_api.cpp
    src/browser/service_worker_api.cpp
    src/browser/geolocation_api.cpp
    src/browser/console_api.cpp
    src/browser/dom_api.cpp
    src/browser/event_api.cpp
    src/browser/shadow_dom_api.cpp
    src/browser/misc_api.cpp
    src/browser/media_source_api.cpp
    src/browser/url_api.cpp
    src/browser/fetch_api.cpp
    src/browser/timer_api.cpp
    src/browser_api_impl.cpp
    src/html_dom.cpp
    src/preorder_compaction_array.cpp
    src/css_parser.cpp
    src/css_layout.cpp
    src/display_list.cpp
    src/text_shaper.cpp
    src/image_cache.cpp
    src/lockfree_hash_table.cpp
    src/url_analyzer.c
    src/session_state.c
    src/tls_client.c
    src/http_download.c
    
    # Platform abstraction
    src/platform/platform.c
    src/platform/platform_http.c
)

# Platform-specific sources
if(BE_PLATFORM_MACOS OR BE_PLATFORM_LINUX)
    list(APPEND BE_SOURCES
        src/platform/platform_posix.c
    )
elseif(BE_PLATFORM_ANDROID)
    list(APPEND BE_SOURCES
        src/platform/platform_android.c
    )
elseif(BE_PLATFORM_WINDOWS)
    list(APPEND BE_SOURCES
        src/platform/platform_windows.c
    )
endif()

# Vulkan renderer backend (optional)
if(BE_BUILD_VULKAN)
    list(APPEND BE_SOURCES
        src/vulkan_renderer.cpp
        src/vulkan_font_stub.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
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

if(BE_BUILD_VULKAN)
    find_package(Vulkan REQUIRED)
    target_include_directories(browser-emulator PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(browser-emulator PRIVATE ${Vulkan_LIBRARIES})
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
# CyberBrowser executable (hardcoded YouTube loader)
# =============================================================================
add_executable(cyberbrowser
    src/cyberbrowser_main.cpp
)

target_compile_features(cyberbrowser PRIVATE cxx_std_11)

target_include_directories(cyberbrowser
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

target_link_libraries(cyberbrowser
    PRIVATE
        browser-emulator
)

if(BE_PLATFORM_WINDOWS)
    target_include_directories(cyberbrowser BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/win32
    )
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

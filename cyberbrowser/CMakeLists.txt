# CyberBrowser
# A browser built from HTML parsing, CSS layout, QuickJS, and a software rasterizer.

cmake_minimum_required(VERSION 3.16)
project(cyberbrowser VERSION 1.0.0 LANGUAGES C)

# compile_commands.json, used by tools/gc_store_check.py
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# GNU C17: the code uses GNU extensions (statement expressions, __typeof__,
# __attribute__((cleanup)), range designators) supported by GCC and Clang.
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# MSVC's own C compiler does not implement those extensions. With Visual
# Studio, build with the bundled Clang toolset instead:
#   cmake .. -G "Visual Studio 17 2022" -A x64 -T ClangCL
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "cyberbrowser is written in GNU C and needs GCC or Clang. "
        "With Visual Studio use the ClangCL toolset (-T ClangCL), or build with MinGW.")
endif()

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
    src/html_media_extract.c
    src/js_quickjs.c
    src/browser/crypto_api.c
    src/browser/storage_api.c
    src/browser/css_api.c
    src/browser/service_worker_api.c
    src/browser/geolocation_api.c
    src/browser/console_api.c
    src/browser/dom_api.c
    src/browser/event_api.c
    src/browser/shadow_dom_api.c
    src/browser/misc_api.c
    src/browser/media_source_api.c
    src/browser/minimp4_impl.c
    src/browser/url_api.c
    src/browser/fetch_api.c
    src/browser/timer_api.c
    src/browser_api_impl.c
    src/html_dom.c
    src/preorder_compaction_array.c
    src/css_parser.c
    src/css_layout.c
    src/display_list.c
    src/text_shaper.c
    src/image_cache.c
    src/lockfree_hash_table.c
    src/url_analyzer.c
    src/cyber_profile.c
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
        src/vulkan_renderer.c
        src/vulkan_font_stub.c
    )
endif()

# QuickJS sources
set(QUICKJS_SOURCES
    src/quickjs/quickjs.c
    src/quickjs/libregexp.c
    src/quickjs/libunicode.c
    src/quickjs/cutils.c
    src/quickjs/dtoa.c
    src/quickjs/quickjs_gc_unified.c
    src/quickjs/gc_collector.c
    src/quickjs/gc_concurrent.c
    src/quickjs/gc_platform.c
    src/quickjs/job_pool.c
    src/quickjs/js_atom_cache.c
    src/quickjs/js_fast_dispatch.c
)

# mbedtls sources (for TLS on macOS/Linux)
if(BE_PLATFORM_MACOS OR BE_PLATFORM_LINUX OR BE_PLATFORM_WINDOWS)
    file(GLOB MBEDTLS_SOURCES 
        src/mbedtls/library/*.c
        src/mbedtls/tf-psa-crypto/core/*.c
        src/mbedtls/tf-psa-crypto/drivers/builtin/src/*.c
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
    add_library(cyberbrowser_lib SHARED ${BE_ALL_SOURCES})
else()
    add_library(cyberbrowser_lib STATIC ${BE_ALL_SOURCES})
endif()

# =============================================================================
# Include directories
# =============================================================================
target_include_directories(cyberbrowser_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs
        ${CMAKE_CURRENT_SOURCE_DIR}/src/minimp4
        ${CMAKE_CURRENT_SOURCE_DIR}/src/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/src/nanosvg
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/tf-psa-crypto/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/tf-psa-crypto
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/tf-psa-crypto/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/tf-psa-crypto/drivers/builtin/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls/tf-psa-crypto/drivers/builtin/src
)

if(BE_PLATFORM_WINDOWS)
    target_include_directories(cyberbrowser_lib BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win32
    )
endif()

if(BE_BUILD_VULKAN)
    find_package(Vulkan REQUIRED)
    target_include_directories(cyberbrowser_lib PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(cyberbrowser_lib PRIVATE ${Vulkan_LIBRARIES})
endif()

# =============================================================================
# Compile definitions
# =============================================================================
target_compile_definitions(cyberbrowser_lib PRIVATE
    CONFIG_VERSION=\"2024-02-14\"
    _GNU_SOURCE
)

if(BE_PLATFORM_MACOS)
    target_compile_definitions(cyberbrowser_lib PRIVATE BE_PLATFORM_MACOS)
elseif(BE_PLATFORM_ANDROID)
    target_compile_definitions(cyberbrowser_lib PRIVATE BE_PLATFORM_ANDROID)
elseif(BE_PLATFORM_WINDOWS)
    target_compile_definitions(cyberbrowser_lib PRIVATE BE_PLATFORM_WINDOWS)
else()
    target_compile_definitions(cyberbrowser_lib PRIVATE BE_PLATFORM_LINUX)
endif()

# =============================================================================
# Compile options
# =============================================================================
if(NOT MSVC)
    target_compile_options(cyberbrowser_lib PRIVATE
        -Wall
        -Wextra
        -O2
        -g
    )
else()
    target_compile_options(cyberbrowser_lib PRIVATE
        /W4
        /O2
        /Zi
    )
endif()

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
target_link_libraries(cyberbrowser_lib
    PUBLIC
        Threads::Threads
)

if(NOT BE_PLATFORM_WINDOWS)
    target_link_libraries(cyberbrowser_lib PUBLIC m)
endif()

if(BE_PLATFORM_MACOS)
    # CoreFoundation for macOS-specific features
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    target_link_libraries(cyberbrowser_lib PRIVATE ${COREFOUNDATION_LIBRARY})
elseif(BE_PLATFORM_WINDOWS)
    target_link_libraries(cyberbrowser_lib PRIVATE ws2_32 crypt32 shell32 bcrypt synchronization)
    if(MSVC)
        # Static-link MSVC runtime for a portable executable
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# =============================================================================
# CyberBrowser executable (start-page loader; defaults to Wikipedia, URL via
# argv[1] or CYBER_START_URL)
# =============================================================================
add_executable(cyberbrowser
    src/cyberbrowser_main.c
)

target_include_directories(cyberbrowser
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs
        ${CMAKE_CURRENT_SOURCE_DIR}/src/stb
)

target_link_libraries(cyberbrowser
    PRIVATE
        cyberbrowser_lib
)

if(BE_PLATFORM_WINDOWS)
    target_include_directories(cyberbrowser BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/win32
    )
    # Give the main executable the same generous C stack that the test
    # executables use.  YouTube's minified bundles can recurse deeply
    # through nested IIFEs during both parsing and execution; a 64 MiB
    # stack prevents C stack overflow in JS_Eval.
    if(MINGW)
        target_link_options(cyberbrowser PRIVATE "-Wl,--stack,67108864")
    endif()
endif()

# =============================================================================
# Flame graph viewer (Vulkan window showing flamegraph.png)
# =============================================================================
# Self-contained: compiles vulkan_renderer.c directly (independent of
# BE_BUILD_VULKAN) and links the system Vulkan loader + cyberbrowser_lib
# (for stb_image and the text-shaper symbols the renderer references).
if(BE_PLATFORM_WINDOWS)
    add_executable(flamegraph_viewer
        src/flamegraph_viewer.c
        src/vulkan_renderer.c
        src/vulkan_font_stub.c
        shaders/embedded_shaders.c
    )

    target_include_directories(flamegraph_viewer
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stb
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders
    )

    # MinGW ships the Vulkan headers + import library; fall back to the
    # Vulkan SDK package layout when not on MinGW.
    find_package(Vulkan)
    if(Vulkan_FOUND)
        target_include_directories(flamegraph_viewer PRIVATE ${Vulkan_INCLUDE_DIRS})
        target_link_libraries(flamegraph_viewer PRIVATE ${Vulkan_LIBRARIES})
    else()
        target_link_libraries(flamegraph_viewer PRIVATE vulkan-1)
    endif()

    target_link_libraries(flamegraph_viewer
        PRIVATE
            cyberbrowser_lib
    )
endif()

# =============================================================================
# Tests
# =============================================================================
if(BE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Static check that every store of a GC reference sets its dirty flag
# (CONCURRENT_GC.md, section 6).  Needs Python 3 and libclang:
#   cmake --build . --target gc_store_check
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(gc_store_check
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gc_store_check.py
                ${CMAKE_BINARY_DIR}/compile_commands.json
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking GC reference stores for dirty flags"
        VERBATIM)
endif()

# =============================================================================
# Installation
# =============================================================================
include(GNUInstallDirs)

install(TARGETS cyberbrowser_lib
    EXPORT cyberbrowser-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Public headers (formerly include/, now alongside the sources in src/)
install(FILES
    src/browser_api_impl.h
    src/browser_api_impl_handle_helpers.h
    src/browser_api_impl_internal.h
    src/browser_api_impl_types.h
    src/css_layout.h
    src/css_parser.h
    src/cyber_profile.h
    src/display_list.h
    src/gc_handle_access.h
    src/gc_value_helpers.h
    src/html_dom.h
    src/html_media_extract.h
    src/http_download.h
    src/image_cache.h
    src/js_quickjs.h
    src/lockfree_hash_table.h
    src/platform.h
    src/preorder_compaction_array.h
    src/session_state.h
    src/text_shaper.h
    src/tls_client.h
    src/url_analyzer.h
    src/url_utils.h
    src/utf8_filename.h
    src/vulkan_font_stub.h
    src/win32_compat.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT cyberbrowser-targets
    FILE cyberbrowser-targets.cmake
    NAMESPACE cyberbrowser::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cyberbrowser
)

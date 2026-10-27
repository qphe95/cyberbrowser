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
# JS values are 8 bytes, NaN-boxed (JS_PERF_DESIGN.md section 5.1, N3); ON
# keeps the 16-byte layout, the reference the 8-byte one was checked against.
option(QJS_VALUE16 "16-byte JS values instead of 8-byte NaN-boxed ones" OFF)
if(NOT QJS_VALUE16)
    add_compile_definitions(QJS_VALUE8)
endif()

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
    src/browser/image_api.c
    src/browser/layout_api.c
    src/browser/html_element_api.c
    src/browser/event_api.c
    src/browser/shadow_dom_api.c
    src/browser/misc_api.c
    src/browser/media_source_api.c
    src/media/media_buffer.c
    src/svg_render.c
    src/browser/url_api.c
    src/browser/platform_api.c
    src/browser/frame_api.c
    src/browser/fetch_api.c
    src/browser/timer_api.c
    src/browser_api_impl.c
    src/html_dom.c
    src/preorder_compaction_array.c
    src/css_parser.c
    src/css_style.c
    src/frame_arena.c
    src/job_system.c
    src/cyber_env.c
    src/det_clock.c
    src/dom_store.c
    src/dom_parse.c
    src/dom_snapshot.c
    src/dom_wrap.c
    src/dom_query.c
    src/dom_fetch.c
    src/url_whatwg.c
    src/css_layout.c
    src/display_list.c
    src/text_shaper.c
    src/font_loader.c
    src/font_instance.c
    src/font_registry.c
    src/image_cache.c
    src/lockfree_hash_table.c
    src/url_analyzer.c
    src/cyber_profile.c
    src/cyber_sampler.c
    src/session_state.c
    src/page_engine.c
    src/raster_cpu.c
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
    src/quickjs/js_struct_index.c
    src/quickjs/quickjs_jit.c
    src/quickjs/quickjs_t2.c
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/plutosvg
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/plutovg/include
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
    CONFIG_ALL_UNICODE
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

# Mbed TLS / PSA crypto are used from several threads (the script fetch pool,
# parallel range downloads), so they need their threading layer.
if(BE_PLATFORM_MACOS OR BE_PLATFORM_LINUX OR BE_PLATFORM_WINDOWS)
    target_compile_definitions(cyberbrowser_lib PUBLIC MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD)
endif()

# =============================================================================
# Image codecs (vendored under src/third_party)
#   libjpeg-turbo: SIMD JPEG decoding with DCT-domain downscaling.  Its build
#     system does not support add_subdirectory(), so it is built as an
#     ExternalProject; SIMD is used when NASM is installed (otherwise the
#     portable C code paths are compiled).
#   libwebp: decoder-only library through its own CMake project.
# =============================================================================
include(ExternalProject)
set(LJT_PREFIX ${CMAKE_BINARY_DIR}/libjpeg-turbo)
if(MSVC)
    set(LJT_LIB ${LJT_PREFIX}/lib/turbojpeg-static.lib)
else()
    set(LJT_LIB ${LJT_PREFIX}/lib/libturbojpeg.a)
endif()
set(LJT_CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${LJT_PREFIX}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_TURBOJPEG=ON
    -DWITH_JAVA=OFF
    -DREQUIRE_SIMD=OFF
)
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND LJT_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()
if(CMAKE_ASM_NASM_COMPILER)
    list(APPEND LJT_CMAKE_ARGS -DCMAKE_ASM_NASM_COMPILER=${CMAKE_ASM_NASM_COMPILER})
endif()
ExternalProject_Add(libjpeg_turbo_ext
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/libjpeg-turbo
    BINARY_DIR ${CMAKE_BINARY_DIR}/libjpeg-turbo-build
    CMAKE_ARGS ${LJT_CMAKE_ARGS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
    INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG> --target install
    BUILD_BYPRODUCTS ${LJT_LIB}
)
add_library(turbojpeg_static STATIC IMPORTED GLOBAL)
set_target_properties(turbojpeg_static PROPERTIES IMPORTED_LOCATION ${LJT_LIB})
add_dependencies(cyberbrowser_lib libjpeg_turbo_ext)
target_include_directories(cyberbrowser_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/libjpeg-turbo)

foreach(_opt WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP
        WEBP_BUILD_IMG2WEBP WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_LIBWEBPMUX
        WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS WEBP_BUILD_WEBP_JS)
    set(${_opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(src/third_party/libwebp EXCLUDE_FROM_ALL)
target_include_directories(cyberbrowser_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/libwebp/src)

# =============================================================================
# Link libraries
# =============================================================================
target_link_libraries(cyberbrowser_lib
    PUBLIC
        Threads::Threads
        turbojpeg_static
        webpdecoder
)

# PlutoSVG + plutovg (MIT, C99): SVG parsing and rasterization (svg_render.c).
file(GLOB PLUTOVG_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/plutovg/source/*.c)
add_library(plutosvg_static STATIC
    ${PLUTOVG_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/plutosvg/plutosvg.c)
target_include_directories(plutosvg_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/plutosvg>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/plutovg/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/plutovg/source)
target_compile_definitions(plutosvg_static
    PUBLIC PLUTOVG_BUILD_STATIC PLUTOSVG_BUILD_STATIC
    PRIVATE PLUTOVG_BUILD PLUTOSVG_BUILD)
if(NOT MSVC)
    target_compile_options(plutosvg_static PRIVATE -O2 -w)
endif()
target_link_libraries(cyberbrowser_lib PUBLIC plutosvg_static)

# Brotli decoder (MIT, C99): WOFF2 web fonts (font_loader.c).
file(GLOB BROTLI_DEC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/brotli/common/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/brotli/dec/*.c)
add_library(brotlidec_static STATIC ${BROTLI_DEC_SOURCES})
target_include_directories(brotlidec_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/brotli/include>)
if(NOT MSVC)
    target_compile_options(brotlidec_static PRIVATE -O2 -w)
endif()
target_link_libraries(cyberbrowser_lib PUBLIC brotlidec_static)

# ffmpeg (video demux + decode): the minimal static LGPL build made by
# tools/build_ffmpeg.sh.  Without it <video> plays nothing (media segments
# are not parsed, so nothing is ever buffered).
set(CYBER_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/install"
    CACHE PATH "Install prefix of the static ffmpeg build")
if(EXISTS "${CYBER_FFMPEG_DIR}/lib/libavcodec.a")
    message(STATUS "ffmpeg: ${CYBER_FFMPEG_DIR}")
    target_include_directories(cyberbrowser_lib PRIVATE "${CYBER_FFMPEG_DIR}/include")
    target_compile_definitions(cyberbrowser_lib PRIVATE CYBER_HAVE_FFMPEG)
    target_link_libraries(cyberbrowser_lib PUBLIC
        "${CYBER_FFMPEG_DIR}/lib/libavformat.a"
        "${CYBER_FFMPEG_DIR}/lib/libavcodec.a"
        "${CYBER_FFMPEG_DIR}/lib/libswscale.a"
        "${CYBER_FFMPEG_DIR}/lib/libswresample.a"
        "${CYBER_FFMPEG_DIR}/lib/libavutil.a")
    if(BE_PLATFORM_WINDOWS)
        target_link_libraries(cyberbrowser_lib PUBLIC bcrypt ole32 user32)
    endif()
    target_link_libraries(cyberbrowser_lib PUBLIC atomic)
else()
    message(STATUS "ffmpeg: not found (run tools/build_ffmpeg.sh); video decoding disabled")
endif()

if(NOT BE_PLATFORM_WINDOWS)
    target_link_libraries(cyberbrowser_lib PUBLIC m)
endif()

if(BE_PLATFORM_MACOS)
    # CoreFoundation for macOS-specific features
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    target_link_libraries(cyberbrowser_lib PRIVATE ${COREFOUNDATION_LIBRARY})
elseif(BE_PLATFORM_WINDOWS)
    target_link_libraries(cyberbrowser_lib PRIVATE ws2_32 crypt32 shell32 bcrypt synchronization winmm)
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

# The static libraries cyberbrowser_lib links publicly go in its export set.
install(TARGETS cyberbrowser_lib plutosvg_static brotlidec_static
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

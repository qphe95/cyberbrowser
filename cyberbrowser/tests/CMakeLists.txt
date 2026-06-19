# Tests for browser-emulator

# Main test executable (shared context model)
add_executable(browser-emulator-tests
    test_main.cpp
    test_browser_api_impl.cpp
    test_gc_unified.cpp
    test_gc_multithreaded.cpp
    test_exception_debug.cpp
    test_logical_operators.cpp
    test_nested_iife.cpp
    test_lazy_parsing.cpp
    test_preorder_compaction_array.cpp
    test_css_parser.cpp
    test_css_layout.cpp
)

# Standalone YouTube data test executable (complex test with its own main)
add_executable(youtube-data-test
    test_youtube_data_scripts.cpp
)

# Live YouTube fetch test (fetches fresh data from youtube.com)
add_executable(youtube-live-fetch-test
    test_youtube_live_fetch.cpp
)

# Enable C++11 for tests
target_compile_features(browser-emulator-tests PRIVATE cxx_std_11)
target_compile_features(youtube-data-test PRIVATE cxx_std_11)
target_compile_features(youtube-live-fetch-test PRIVATE cxx_std_11)

# Include directories
target_include_directories(browser-emulator-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)

target_include_directories(youtube-data-test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)

target_include_directories(youtube-live-fetch-test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)

# Link with the library
target_link_libraries(browser-emulator-tests
    PRIVATE
        browser-emulator
)

target_link_libraries(youtube-data-test
    PRIVATE
        browser-emulator
)

target_link_libraries(youtube-live-fetch-test
    PRIVATE
        browser-emulator
)

# Add tests
add_test(NAME browser-emulator-tests COMMAND browser-emulator-tests)
add_test(NAME youtube-data-test COMMAND youtube-data-test)
add_test(NAME youtube-live-fetch-test COMMAND youtube-live-fetch-test)

# Atom cache benchmark
add_executable(atom-cache-bench
    test_atom_cache_bench.cpp
)
target_compile_features(atom-cache-bench PRIVATE cxx_std_11)
target_include_directories(atom-cache-bench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(atom-cache-bench
    PRIVATE
        browser-emulator
)

# Windows compatibility headers
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(browser-emulator-tests BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
    target_include_directories(youtube-data-test BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
    target_include_directories(youtube-live-fetch-test BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
    target_include_directories(atom-cache-bench BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# Long video analysis test
add_executable(long-video-test
    test_long_video_analyze.cpp
)

# Simple URL analyze test (no QuickJS)
add_executable(analyze-long-video-test
    test_analyze_long_video.c
)

# YouTubei API probe (no QuickJS)
add_executable(youtubei-probe
    test_youtubei_probe.c
)

# Browser emulation test
add_executable(browser-emulation-test
    test_browser_emulation.c
)

# Fetch visitorData from watch page
add_executable(fetch-visitor-data-test
    test_fetch_visitor_data.c
)

# Simple youtubei GET test
add_executable(test-youtubei-get
    test_youtubei_get.c
)
target_compile_features(test-youtubei-get PRIVATE cxx_std_11)
target_include_directories(test-youtubei-get
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(test-youtubei-get
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(test-youtubei-get BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# Simple youtubei POST test
add_executable(test-youtubei-post
    test_youtubei_post.c
)
target_compile_features(test-youtubei-post PRIVATE cxx_std_11)
target_include_directories(test-youtubei-post
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(test-youtubei-post
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(test-youtubei-post BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# Watch page then youtubei test
add_executable(test-watch-then-youtubei
    test_watch_then_youtubei.c
)
target_compile_features(test-watch-then-youtubei PRIVATE cxx_std_11)
target_include_directories(test-watch-then-youtubei
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(test-watch-then-youtubei
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(test-watch-then-youtubei BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# Two GETs test
add_executable(test-two-gets
    test_two_gets.c
)
target_compile_features(test-two-gets PRIVATE cxx_std_11)
target_include_directories(test-two-gets
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(test-two-gets
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(test-two-gets BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# Two TLS connections test
add_executable(test-two-tls
    test_two_tls.c
)
target_compile_features(test-two-tls PRIVATE cxx_std_11)
target_include_directories(test-two-tls
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(test-two-tls
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(test-two-tls BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# Googlevideo connect test
add_executable(test-googlevideo-http
    test_googlevideo_http.c
)
target_compile_features(test-googlevideo-http PRIVATE cxx_std_11)
target_include_directories(test-googlevideo-http
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(test-googlevideo-http
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(test-googlevideo-http BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# Googlevideo file download test
add_executable(test-googlevideo-file
    test_googlevideo_file.c
)
target_compile_features(test-googlevideo-file PRIVATE cxx_std_11)
target_include_directories(test-googlevideo-file
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(test-googlevideo-file
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(test-googlevideo-file BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()
target_compile_features(long-video-test PRIVATE cxx_std_11)
target_include_directories(long-video-test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(long-video-test
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(long-video-test BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

target_compile_features(analyze-long-video-test PRIVATE cxx_std_11)
target_include_directories(analyze-long-video-test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(analyze-long-video-test
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(analyze-long-video-test BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

target_compile_features(youtubei-probe PRIVATE cxx_std_11)
target_include_directories(youtubei-probe
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(youtubei-probe
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(youtubei-probe BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

target_compile_features(browser-emulation-test PRIVATE cxx_std_11)
target_include_directories(browser-emulation-test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(browser-emulation-test
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(browser-emulation-test BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

target_compile_features(fetch-visitor-data-test PRIVATE cxx_std_11)
target_include_directories(fetch-visitor-data-test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/quickjs
)
target_link_libraries(fetch-visitor-data-test
    PRIVATE
        browser-emulator
)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_include_directories(fetch-visitor-data-test BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/include/win32)
endif()

# CLI downloader tool (macOS/Linux/Windows)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(bgmdwnldr
        download_cli.cpp
    )
    target_compile_features(bgmdwnldr PRIVATE cxx_std_11)
    target_include_directories(bgmdwnldr
        PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/third_party/quickjs
    )
    target_link_libraries(bgmdwnldr
        PRIVATE
            browser-emulator
    )
    # On Windows, rename output to match project branding
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set_target_properties(bgmdwnldr PROPERTIES OUTPUT_NAME "bgmdwnldr")
    endif()
endif()

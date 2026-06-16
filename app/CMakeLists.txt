cmake_minimum_required(VERSION 3.16)
project(bgmdwnldr-mac VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# Find dependencies
# =============================================================================
find_package(glfw3 REQUIRED)
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

# =============================================================================
# Browser Emulator (subdirectory build)
# =============================================================================
set(BE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BE_BUILD_DESKTOP_APP OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator browser-emulator-build)

# =============================================================================
# macOS App
# =============================================================================
add_executable(bgmdwnldr-mac
    src/main/cpp/vulkan_ui.cpp
    src/main/cpp/ui_layout.cpp
    src/main/cpp/mp4_metadata.cpp
    src/main/cpp/main_macos.cpp
    src/main/cpp/default_album_art.c
)

target_compile_definitions(bgmdwnldr-mac PRIVATE BE_PLATFORM_MACOS)

target_include_directories(bgmdwnldr-mac PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/third_party/quickjs
)

target_link_libraries(bgmdwnldr-mac PRIVATE
    browser-emulator
    glfw
    Vulkan::Vulkan
    Threads::Threads
)

# =============================================================================
# Resize invariance capture test
# =============================================================================
add_executable(test-resize-capture
    src/main/cpp/vulkan_ui.cpp
    src/main/cpp/ui_layout.cpp
    src/main/cpp/mp4_metadata.cpp
    src/main/cpp/test_resize_capture.cpp
    src/main/cpp/default_album_art.c
)
target_compile_definitions(test-resize-capture PRIVATE BE_PLATFORM_MACOS)
target_include_directories(test-resize-capture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/third_party/quickjs
)
target_link_libraries(test-resize-capture PRIVATE
    browser-emulator
    glfw
    Vulkan::Vulkan
    Threads::Threads
)
add_custom_command(TARGET test-resize-capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/triangle.vert.spv
    $<TARGET_FILE_DIR:test-resize-capture>/triangle.vert.spv
)
add_custom_command(TARGET test-resize-capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/triangle.frag.spv
    $<TARGET_FILE_DIR:test-resize-capture>/triangle.frag.spv
)

# =============================================================================
# Text-line render test (proves text size is invariant to window resize)
# =============================================================================
add_executable(test-text-render
    src/main/cpp/vulkan_ui.cpp
    src/main/cpp/ui_layout.cpp
    src/main/cpp/mp4_metadata.cpp
    src/main/cpp/test_text_render.cpp
    src/main/cpp/default_album_art.c
)
target_compile_definitions(test-text-render PRIVATE BE_PLATFORM_MACOS)
target_include_directories(test-text-render PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../browser-emulator/third_party/quickjs
)
target_link_libraries(test-text-render PRIVATE
    browser-emulator
    glfw
    Vulkan::Vulkan
    Threads::Threads
)
add_custom_command(TARGET test-text-render POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/triangle.vert.spv
    $<TARGET_FILE_DIR:test-text-render>/triangle.vert.spv
)
add_custom_command(TARGET test-text-render POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/triangle.frag.spv
    $<TARGET_FILE_DIR:test-text-render>/triangle.frag.spv
)

# =============================================================================
# MP4 metadata test
# =============================================================================
add_executable(test-mp4-metadata
    src/main/cpp/mp4_metadata.cpp
    src/main/cpp/test_mp4_metadata.cpp
)
target_compile_definitions(test-mp4-metadata PRIVATE BE_PLATFORM_MACOS)
target_include_directories(test-mp4-metadata PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp
)

# =============================================================================
# Copy shaders to build directory
# =============================================================================
add_custom_command(TARGET bgmdwnldr-mac POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/triangle.vert.spv
    $<TARGET_FILE_DIR:bgmdwnldr-mac>/triangle.vert.spv
)
add_custom_command(TARGET bgmdwnldr-mac POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main/assets/triangle.frag.spv
    $<TARGET_FILE_DIR:bgmdwnldr-mac>/triangle.frag.spv
)

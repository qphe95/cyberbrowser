# Install script for directory: C:/Users/qingping/Documents/cyberbrowser/cyberbrowser

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/cyberbrowser")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "C:/msys64/mingw64/bin/objdump.exe")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/build-release/tests/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/build-release/libcyberbrowser_lib.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/browser_api_impl.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/browser_api_impl_handle_helpers.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/browser_api_impl_internal.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/browser_api_impl_types.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/css_layout.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/css_parser.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/cyber_profile.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/display_list.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/gc_handle_access.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/gc_value_helpers.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/html_dom.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/html_media_extract.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/http_download.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/image_cache.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/js_quickjs.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/lockfree_hash_table.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/platform.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/preorder_compaction_array.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/session_state.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/text_shaper.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/tls_client.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/url_analyzer.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/url_utils.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/utf8_filename.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/vulkan_font_stub.h"
    "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/src/win32_compat.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/cyberbrowser/cyberbrowser-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/cyberbrowser/cyberbrowser-targets.cmake"
         "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/build-release/CMakeFiles/Export/015697941253fbb4af5672df4c6be64d/cyberbrowser-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/cyberbrowser/cyberbrowser-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/cyberbrowser/cyberbrowser-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/cyberbrowser" TYPE FILE FILES "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/build-release/CMakeFiles/Export/015697941253fbb4af5672df4c6be64d/cyberbrowser-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/cyberbrowser" TYPE FILE FILES "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/build-release/CMakeFiles/Export/015697941253fbb4af5672df4c6be64d/cyberbrowser-targets-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/build-release/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/qingping/Documents/cyberbrowser/cyberbrowser/build-release/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()

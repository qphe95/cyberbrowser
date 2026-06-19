#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "browser-emulator::browser-emulator" for configuration "Release"
set_property(TARGET browser-emulator::browser-emulator APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(browser-emulator::browser-emulator PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C;CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libbrowser-emulator.a"
  )

list(APPEND _cmake_import_check_targets browser-emulator::browser-emulator )
list(APPEND _cmake_import_check_files_for_browser-emulator::browser-emulator "${_IMPORT_PREFIX}/lib/libbrowser-emulator.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)

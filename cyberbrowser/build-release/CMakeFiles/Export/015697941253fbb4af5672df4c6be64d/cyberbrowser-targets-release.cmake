#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "cyberbrowser::cyberbrowser_lib" for configuration "Release"
set_property(TARGET cyberbrowser::cyberbrowser_lib APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(cyberbrowser::cyberbrowser_lib PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libcyberbrowser_lib.a"
  )

list(APPEND _cmake_import_check_targets cyberbrowser::cyberbrowser_lib )
list(APPEND _cmake_import_check_files_for_cyberbrowser::cyberbrowser_lib "${_IMPORT_PREFIX}/lib/libcyberbrowser_lib.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)

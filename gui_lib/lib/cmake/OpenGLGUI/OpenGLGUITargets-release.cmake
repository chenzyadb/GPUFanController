#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpenGLGUI::Static" for configuration "Release"
set_property(TARGET OpenGLGUI::Static APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenGLGUI::Static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libOpenGLGUI.a"
  )

list(APPEND _cmake_import_check_targets OpenGLGUI::Static )
list(APPEND _cmake_import_check_files_for_OpenGLGUI::Static "${_IMPORT_PREFIX}/lib/libOpenGLGUI.a" )

# Import target "OpenGLGUI::Shared" for configuration "Release"
set_property(TARGET OpenGLGUI::Shared APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpenGLGUI::Shared PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/libOpenGLGUI_shared.dll.a"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/libOpenGLGUI.dll"
  )

list(APPEND _cmake_import_check_targets OpenGLGUI::Shared )
list(APPEND _cmake_import_check_files_for_OpenGLGUI::Shared "${_IMPORT_PREFIX}/lib/libOpenGLGUI_shared.dll.a" "${_IMPORT_PREFIX}/bin/libOpenGLGUI.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)

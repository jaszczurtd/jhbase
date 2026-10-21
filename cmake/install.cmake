# Installation and the exported CMake package. The status headers and
# jb::status always install, so a parent project (HAL) ships the one copy of
# them inside its own package. The static library and the remaining headers
# install with a standalone build or when JHBASE_INSTALL_LIBRARY is set.
include(CMakePackageConfigHelpers)
option(JHBASE_INSTALL_LIBRARY "Install the static library and all public headers"
       ${PROJECT_IS_TOP_LEVEL})
set(JHBASE_REVISION "" CACHE STRING
    "Library revision recorded in the installed package; empty reads git HEAD")

# HAL passes its pin. A standalone build reads git HEAD and marks local
# changes; a tree without git (the Windows bundle) records "unknown".
set(jb_revision "${JHBASE_REVISION}")
if(jb_revision STREQUAL "")
  set(jb_revision "unknown")
  # Only a repository rooted here counts: git would otherwise report the
  # commit of an enclosing checkout.
  execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse --show-toplevel HEAD
    RESULT_VARIABLE jb_git_result OUTPUT_VARIABLE jb_git_output
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(jb_git_result EQUAL 0)
    string(REPLACE "\n" ";" jb_git_output "${jb_git_output}")
    list(GET jb_git_output 0 jb_git_top)
    file(REAL_PATH "${jb_git_top}" jb_git_top)
    file(REAL_PATH "${PROJECT_SOURCE_DIR}" jb_source_real)
    if(jb_git_top STREQUAL jb_source_real)
      list(GET jb_git_output 1 jb_revision)
      execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" status --porcelain
        OUTPUT_VARIABLE jb_git_changes ERROR_QUIET)
      if(NOT jb_git_changes STREQUAL "")
        string(APPEND jb_revision "-dirty")
      endif()
    endif()
  endif()
endif()
# The layout every public header asserts: natural alignment, 8-bit bytes,
# one-byte bool, 32-bit status and the pointer width of this toolchain.
math(EXPR jb_pointer_bits "${CMAKE_SIZEOF_VOID_P} * 8")
set(jb_abi_profile "natural-ptr${jb_pointer_bits}-status32-bool8")

set(jb_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/jhbase")
set(jb_status_headers abi.h array.h status.h)
list(TRANSFORM jb_status_headers PREPEND "${PROJECT_SOURCE_DIR}/include/jb/")
install(FILES ${jb_status_headers} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/jb"
        COMPONENT jhbase_status)
install(TARGETS jb_status EXPORT jhbaseStatusTargets COMPONENT jhbase_status)
install(EXPORT jhbaseStatusTargets NAMESPACE jb:: DESTINATION "${jb_cmake_dir}"
        COMPONENT jhbase_status)
configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/jhbaseConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/jhbaseConfig.cmake" INSTALL_DESTINATION "${jb_cmake_dir}")
write_basic_package_version_file("${PROJECT_BINARY_DIR}/jhbaseConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES "${PROJECT_BINARY_DIR}/jhbaseConfig.cmake"
              "${PROJECT_BINARY_DIR}/jhbaseConfigVersion.cmake"
        DESTINATION "${jb_cmake_dir}" COMPONENT jhbase_status)

if(JHBASE_INSTALL_LIBRARY)
  file(GLOB jb_library_headers "${PROJECT_SOURCE_DIR}/include/jb/*.h")
  list(REMOVE_ITEM jb_library_headers ${jb_status_headers})
  install(FILES ${jb_library_headers} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/jb"
          COMPONENT jhbase_library)
  install(TARGETS jhbase EXPORT jhbaseTargets
          ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT jhbase_library)
  install(EXPORT jhbaseTargets NAMESPACE jb:: DESTINATION "${jb_cmake_dir}"
          COMPONENT jhbase_library)
  if(JHBASE_HOST)
    install(TARGETS jb_host EXPORT jhbaseHostTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT jhbase_library)
    install(EXPORT jhbaseHostTargets NAMESPACE jb:: DESTINATION "${jb_cmake_dir}"
            COMPONENT jhbase_library)
  endif()
  if(JHBASE_UNICODE)
    install(TARGETS jb_unicode EXPORT jhbaseUnicodeTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT jhbase_library)
    install(EXPORT jhbaseUnicodeTargets NAMESPACE jb:: DESTINATION "${jb_cmake_dir}"
            COMPONENT jhbase_library)
    install(FILES "${jb_utf8proc_source}/LICENSE.md"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/jhbase/licenses/utf8proc"
            COMPONENT jhbase_library)
  endif()
endif()

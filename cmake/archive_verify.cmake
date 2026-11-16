# Configure and every build of jb::archive verify both checkouts: revision,
# origin, clean state and source bytes. Nothing is fetched or written here.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/archive_pin.cmake")
foreach(jb_archive_dependency IN ITEMS libarchive zlib)
  string(TOUPPER "${jb_archive_dependency}" jb_archive_prefix)
  get_filename_component(jb_archive_source
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/${jb_archive_dependency}" ABSOLUTE)
  jb_check_git_dependency("${jb_archive_source}" "${${jb_archive_prefix}_REPO}"
    "${${jb_archive_prefix}_REF}" jb_archive_error)
  if(NOT jb_archive_error STREQUAL "")
    message(FATAL_ERROR "${jb_archive_error}. Prepare it with "
      "cmake -P scripts/ensure_${jb_archive_dependency}.cmake")
  endif()
endforeach()

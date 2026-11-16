# Explicit preparation on Linux and Windows; configure only verifies.
# cmake [-DJB_VERIFY_ONLY=ON] -P scripts/ensure_libarchive.cmake
cmake_minimum_required(VERSION 3.22)
get_filename_component(jb_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${jb_root}/cmake/archive_pin.cmake")
jb_ensure_git_dependency("libarchive ${LIBARCHIVE_VERSION}" "${jb_root}/third_party/libarchive"
  "${LIBARCHIVE_REPO}" "${LIBARCHIVE_REF}" "${CMAKE_CURRENT_LIST_FILE}")

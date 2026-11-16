# Explicit preparation on Linux and Windows; configure only verifies.
# cmake [-DJB_VERIFY_ONLY=ON] -P scripts/ensure_utf8proc.cmake
cmake_minimum_required(VERSION 3.22)
get_filename_component(jb_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${jb_root}/cmake/utf8proc_pin.cmake")
jb_ensure_git_dependency("utf8proc ${UTF8PROC_VERSION}" "${jb_root}/third_party/utf8proc"
  "${UTF8PROC_REPO}" "${UTF8PROC_REF}" "${CMAKE_CURRENT_LIST_FILE}")

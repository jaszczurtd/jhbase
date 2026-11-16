# Read data only; usable by the staged-tree check without a source checkout.
include("${CMAKE_CURRENT_LIST_DIR}/git_dependency.cmake")
jb_read_git_pin("${CMAKE_CURRENT_LIST_DIR}/../third_party/utf8proc_version.conf" UTF8PROC)
if(NOT UTF8PROC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$"
    OR NOT UTF8PROC_UNICODE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Invalid utf8proc pin")
endif()

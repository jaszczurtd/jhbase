# Read data only; usable by the staged-tree check without source checkouts.
include("${CMAKE_CURRENT_LIST_DIR}/git_dependency.cmake")
jb_read_git_pin("${CMAKE_CURRENT_LIST_DIR}/../third_party/curl_version.conf" CURL)

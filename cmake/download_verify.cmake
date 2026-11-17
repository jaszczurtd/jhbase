# Configure and every build of jb::download verify the curl checkout: revision,
# origin, clean state and source bytes. Nothing is fetched or written here.
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/download_pin.cmake")
get_filename_component(jb_curl_source "${CMAKE_CURRENT_LIST_DIR}/../third_party/curl" ABSOLUTE)
jb_check_git_dependency("${jb_curl_source}" "${CURL_REPO}" "${CURL_REF}" jb_curl_error)
if(NOT jb_curl_error STREQUAL "")
  message(FATAL_ERROR "${jb_curl_error}. Prepare it with cmake -P scripts/ensure_curl.cmake")
endif()

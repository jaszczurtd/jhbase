include("${CMAKE_CURRENT_LIST_DIR}/unicode_test_pin.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/require_hash.cmake")

# Read-only verification, shared by configure, setup and dependency tests.
function(jb_check_unicode_fixture root out_error)
  if(IS_SYMLINK "${root}")
    set("${out_error}" "Unicode test directory must not be a symlink: ${root}" PARENT_SCOPE)
    return()
  endif()
  foreach(component IN ITEMS NORMALIZATION LICENSE)
    jb_check_hash("${root}/${UNICODE_TEST_${component}_FILE}"
      "${UNICODE_TEST_${component}_SHA256}" error)
    if(NOT error STREQUAL "")
      set("${out_error}" "${error}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set("${out_error}" "" PARENT_SCOPE)
endfunction()

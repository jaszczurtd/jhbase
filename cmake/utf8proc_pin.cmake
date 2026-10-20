# Read data only; usable by the staged-tree check without a source checkout.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../third_party/utf8proc_version.conf" jb_utf8proc_pin)
foreach(line IN LISTS jb_utf8proc_pin)
  if(line MATCHES "^(UTF8PROC_[A-Z_]+)=(.+)$")
    set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()
string(LENGTH "${UTF8PROC_REF}" jb_utf8proc_ref_length)
if(NOT jb_utf8proc_ref_length EQUAL 40 OR NOT UTF8PROC_REF MATCHES "^[a-f0-9]+$"
    OR NOT UTF8PROC_REPO MATCHES "^https://[^ ]+\\.git$"
    OR NOT UTF8PROC_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$"
    OR NOT UTF8PROC_UNICODE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "Invalid utf8proc pin")
endif()

# Pin validation also runs in a staged snapshot with no downloaded data.
include("${CMAKE_CURRENT_LIST_DIR}/utf8proc_pin.cmake")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../third_party/unicode_test_version.conf" jb_unicode_test_pin)
foreach(line IN LISTS jb_unicode_test_pin)
  if(line MATCHES "^(UNICODE_TEST_[A-Z0-9_]+)=(.+)$")
    set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()

# A digest verifies bytes; the full commit keeps the license URL reproducible.
if(NOT UNICODE_TEST_LICENSE_URL MATCHES
    "^https://raw[.]githubusercontent[.]com/unicode-org/[^/]+/([a-f0-9]+)/[^ ?#]+$")
  message(FATAL_ERROR "Unicode test license URL must use a full upstream commit")
endif()
string(LENGTH "${CMAKE_MATCH_1}" jb_unicode_license_ref_length)
if(NOT jb_unicode_license_ref_length EQUAL 40)
  message(FATAL_ERROR "Unicode test license URL must use a full upstream commit")
endif()
set(UNICODE_TEST_NORMALIZATION_FILE NormalizationTest.txt)
set(UNICODE_TEST_LICENSE_FILE LICENSE.txt)
foreach(component IN ITEMS NORMALIZATION LICENSE)
  string(LENGTH "${UNICODE_TEST_${component}_SHA256}" hash_length)
  if(NOT hash_length EQUAL 64 OR NOT UNICODE_TEST_${component}_SHA256 MATCHES "^[a-f0-9]+$"
      OR NOT UNICODE_TEST_${component}_URL MATCHES "^https://[^ ;]+$")
    message(FATAL_ERROR "Invalid Unicode test pin: ${component}")
  endif()
  string(REPLACE "{UNICODE_VERSION}" "${UTF8PROC_UNICODE_VERSION}"
    UNICODE_TEST_${component}_URL "${UNICODE_TEST_${component}_URL}")
endforeach()

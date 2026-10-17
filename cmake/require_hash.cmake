# SHA-256 check of a reviewed file, shared by the dependency and Ryu manifest
# checks so that each of them also runs on its own with cmake -P.
function(jb_require_hash path expected)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Missing prototype dependency: ${path}")
  endif()
  file(SHA256 "${path}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Prototype dependency differs from comparison base: ${path}")
  endif()
endfunction()

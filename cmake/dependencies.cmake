# Prototype-only, read-only dependencies. These hashes are generated from the
# refreshed comparison base; pins are recorded here once for configure and gate.
set(jb_hal_status_sha "4915f2c0274821e467306d31320cc40e254919173e12510dfcbe9b9b65b17bb0")
set(jb_hal_array_sha "d36440056d3bb799276c8a6e1e0be444bead7479bf6bbf67d926ecfc9a309845")
set(jb_unity_c_sha "8a65dd826bc83b4a8cc8602100e57640d01058dfa20eb282ea5dad63d540afeb")
set(jb_unity_h_sha "69381acea33db881dbc7eb451aad46cb0897af388e10e600fe1f37b1c0448900")
set(jb_unity_internals_sha "88e02a6eb1b3cf80b43ed8674d0d87e66dd536762a10aa2a25016c6b38b9bd30")
set(jb_unity_revision "f9879bf7d82108c3eefd5fc378983317898616f3")

function(jb_require_hash path expected)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Missing prototype dependency: ${path}")
  endif()
  file(SHA256 "${path}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Prototype dependency differs from comparison base: ${path}")
  endif()
endfunction()

jb_require_hash("${JHBASE_HAL_ROOT}/src/hal/core/hal_status.h" "${jb_hal_status_sha}")
jb_require_hash("${JHBASE_HAL_ROOT}/src/hal/core/hal_array.h" "${jb_hal_array_sha}")

function(jb_check_unity)
  jb_require_hash("${JHBASE_UNITY_ROOT}/src/unity.c" "${jb_unity_c_sha}")
  jb_require_hash("${JHBASE_UNITY_ROOT}/src/unity.h" "${jb_unity_h_sha}")
  jb_require_hash("${JHBASE_UNITY_ROOT}/src/unity_internals.h" "${jb_unity_internals_sha}")
endfunction()

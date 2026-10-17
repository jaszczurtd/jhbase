# Prototype-only, read-only dependencies. The HAL header hashes come from the
# refreshed comparison base; the Unity source hashes belong to the commit pinned
# in third_party/unity_version.conf. Configure checks them and downloads nothing.
set(jb_hal_status_sha "23c0f6acb560802301a4597a47ab4ddf420b4976e0494347a2f5c6ec3069a861")
set(jb_hal_array_sha "d36440056d3bb799276c8a6e1e0be444bead7479bf6bbf67d926ecfc9a309845")
set(jb_unity_c_sha "8a65dd826bc83b4a8cc8602100e57640d01058dfa20eb282ea5dad63d540afeb")
set(jb_unity_h_sha "69381acea33db881dbc7eb451aad46cb0897af388e10e600fe1f37b1c0448900")
set(jb_unity_internals_sha "88e02a6eb1b3cf80b43ed8674d0d87e66dd536762a10aa2a25016c6b38b9bd30")

include("${CMAKE_CURRENT_LIST_DIR}/require_hash.cmake")

jb_require_hash("${JHBASE_HAL_ROOT}/src/hal/core/hal_status.h" "${jb_hal_status_sha}")
jb_require_hash("${JHBASE_HAL_ROOT}/src/hal/core/hal_array.h" "${jb_hal_array_sha}")
include("${CMAKE_CURRENT_LIST_DIR}/ryu_verify.cmake")

function(jb_check_unity)
  if(NOT EXISTS "${JHBASE_UNITY_ROOT}/src/unity.c")
    message(FATAL_ERROR "Pinned Unity is missing at ${JHBASE_UNITY_ROOT}: run "
      "scripts/ensure_tools.sh unity, or point JHBASE_TOOLS_ROOT/JHBASE_UNITY_ROOT at it")
  endif()
  jb_require_hash("${JHBASE_UNITY_ROOT}/src/unity.c" "${jb_unity_c_sha}")
  jb_require_hash("${JHBASE_UNITY_ROOT}/src/unity.h" "${jb_unity_h_sha}")
  jb_require_hash("${JHBASE_UNITY_ROOT}/src/unity_internals.h" "${jb_unity_internals_sha}")
endfunction()

# The vendored Ryu subset must match its SHA-256 manifest byte for byte. Used by
# configure through dependencies.cmake and by the pre-commit hook on its own.
include("${CMAKE_CURRENT_LIST_DIR}/require_hash.cmake")
jb_require_manifest("${CMAKE_CURRENT_LIST_DIR}/../third_party/ryu" 10)

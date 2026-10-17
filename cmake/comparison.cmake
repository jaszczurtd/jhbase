# Upstream snapshots are used only for explicit comparison, never by jb::base.
set(JHBASE_COREJSON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.build/comparison/coreJSON"
    CACHE PATH "coreJSON v3.3.0 comparison checkout")
set(JHBASE_YYJSON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.build/comparison/yyjson"
    CACHE PATH "yyjson 0.12.0 comparison checkout")
jb_require_hash("${JHBASE_COREJSON_ROOT}/source/core_json.c"
  "c3f7269a2077223db023037154884b267dc979ba3c15af267f60599898e6dd7f")
jb_require_hash("${JHBASE_COREJSON_ROOT}/source/include/core_json.h"
  "d7da2bda60de43d9982b78fde8f5484ec7636a84af2bdd97436119d326e6336d")
jb_require_hash("${JHBASE_YYJSON_ROOT}/src/yyjson.c"
  "ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a")
jb_require_hash("${JHBASE_YYJSON_ROOT}/src/yyjson.h"
  "175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3")
add_library(compare_corejson STATIC "${JHBASE_COREJSON_ROOT}/source/core_json.c")
target_include_directories(compare_corejson PUBLIC "${JHBASE_COREJSON_ROOT}/source/include")
add_library(compare_yyjson STATIC "${JHBASE_YYJSON_ROOT}/src/yyjson.c")
target_include_directories(compare_yyjson PUBLIC "${JHBASE_YYJSON_ROOT}/src")
target_compile_definitions(compare_yyjson PUBLIC YYJSON_DISABLE_WRITER=1 YYJSON_DISABLE_UTILS=1)
add_executable(compare_parsers tests/compare_parsers.c)
target_link_libraries(compare_parsers PRIVATE jb::base compare_corejson compare_yyjson)
jb_warnings(compare_parsers)
if(NOT MSVC)
  target_compile_options(compare_corejson PRIVATE -fstack-usage)
  target_compile_options(compare_yyjson PRIVATE -fstack-usage)
endif()
if(JHBASE_SANITIZERS)
  foreach(target compare_corejson compare_yyjson)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endforeach()
endif()

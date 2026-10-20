# Behaviour check of the install rules. Installs the build twice into a
# scratch prefix (status component only, then everything) and builds a small
# C and C++ consumer from each prefix through find_package alone.
#
#   cmake -DJB_SOURCE=<repository> -DJB_BUILD=<build tree> -DJB_WORK=<work dir>
#         -DJB_CONFIG=<config> -DJB_GENERATOR=<generator> [-DJB_PLATFORM=<platform>]
#         -DJB_CC=<compiler> -DJB_CXX=<compiler> -P tests/check_install.cmake
cmake_minimum_required(VERSION 3.22)

function(fail message)
  message(FATAL_ERROR "check_install: ${message}")
endfunction()

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
  if(NOT result EQUAL 0)
    fail("${ARGN} failed:\n${output}")
  endif()
endfunction()

file(REMOVE_RECURSE "${JB_WORK}")
file(REAL_PATH "${JB_SOURCE}" source)
if(JB_UNICODE)
  include("${JB_SOURCE}/cmake/git_dependency.cmake")
  include("${JB_SOURCE}/cmake/utf8proc_pin.cmake")
  set(dependency "${JB_WORK}/dependency")
  file(COPY "${JB_SOURCE}/third_party/utf8proc/" DESTINATION "${dependency}")
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${UTF8PROC_REF}" error)
  if(NOT error STREQUAL "")
    fail("a clean dependency copy failed verification: ${error}")
  endif()
  file(APPEND "${dependency}/utf8proc.c" "\n/* mutation */\n")
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${UTF8PROC_REF}" error)
  if(error STREQUAL "")
    fail("a modified dependency passed verification")
  endif()
  find_program(dependency_git NAMES git REQUIRED)
  run("${dependency_git}" -C "${dependency}" update-index --assume-unchanged utf8proc.c)
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${UTF8PROC_REF}" error)
  if(error STREQUAL "")
    fail("an assume-unchanged dependency edit passed verification")
  endif()
  file(COPY "${JB_SOURCE}/third_party/utf8proc/utf8proc.c" DESTINATION "${dependency}")
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "0000000000000000000000000000000000000000" error)
  if(error STREQUAL "")
    fail("a wrong dependency pin passed verification")
  endif()
  jb_check_git_dependency("${dependency}" "https://example.invalid/wrong.git" "${UTF8PROC_REF}" error)
  if(error STREQUAL "")
    fail("a wrong dependency origin passed verification")
  endif()
  include("${JB_SOURCE}/cmake/unicode_fixture.cmake")
  set(fixture "${JB_WORK}/Unicode fixture")
  file(COPY "${JB_SOURCE}/tests/fixtures/unicode/" DESTINATION "${fixture}")
  jb_check_unicode_fixture("${fixture}" error)
  if(NOT error STREQUAL "")
    fail("clean Unicode test data failed verification: ${error}")
  endif()
  file(APPEND "${fixture}/NormalizationTest.txt" "\nmutation\n")
  jb_check_unicode_fixture("${fixture}" error)
  if(error STREQUAL "")
    fail("modified Unicode test data passed verification")
  endif()
  file(COPY "${JB_SOURCE}/tests/fixtures/unicode/NormalizationTest.txt" DESTINATION "${fixture}")
  file(REMOVE "${fixture}/LICENSE.txt")
  jb_check_unicode_fixture("${fixture}" error)
  if(error STREQUAL "")
    fail("Unicode test data without its license passed verification")
  endif()
endif()
set(platform)
if(JB_PLATFORM)
  set(platform -A "${JB_PLATFORM}")
endif()

# consumer NAME PREFIX LIBRARY: build and run a C and a C++ program that use
# the status headers and, with LIBRARY, the static library.
function(consumer name prefix library)
  set(dir "${JB_WORK}/${name}")
  set(code "#include <jb/array.h>\n#include <jb/status.h>\n")
  if(library)
    string(APPEND code "#include <jb/utf8.h>\n#include <jb/json_pointer.h>\n")
    if(JB_UNICODE)
      string(APPEND code "#include <jb/unicode.h>\n")
    endif()
  endif()
  string(APPEND code "int main(void) {\n  static const char text[] = \"ok\";\n"
    "  hal_status_t status = (COUNTOF(text) == 3u) ? HAL_OK : HAL_EINVAL;\n")
  if(library)
    string(APPEND code "  size_t bad = 0;\n"
      "  if (status == HAL_OK) status = jb_utf8_validate(text, 2u, &bad);\n")
    string(APPEND code "  const jb_json_span_t input = {\"true\", 4u}, path = {0, 0};\n"
      "  const jb_json_limits_t limits = {4u, 1u, 0u, 0u, 0u, 0u, 0u};\n"
      "  const jb_json_workspace_t scratch = {0, 0};\n"
      "  jb_json_token_t tokens[1];\n  jb_json_doc_t doc;\n  jb_json_node_t node;\n"
      "  size_t required = 1u;\n"
      "  if (status == HAL_OK) status = jb_json_parse(input, &limits, tokens, 1u, scratch, &doc, 0);\n"
      "  if (status == HAL_OK) status = jb_json_pointer_get(&doc, path, &node);\n"
      "  if (status == HAL_OK) status = jb_json_pointer_copy(node, 0, 0u, &required);\n"
      "  if (status == HAL_OK && required != 0u) return 3;\n")
    if(JB_UNICODE)
      string(APPEND code "  int32_t work[4];\n  const char *normalized = 0;\n  size_t length = 0;\n"
        "  if (status == HAL_OK) status = jb_unicode_nfc(\"abc\", 3u, work, 4u, 100u, &normalized, &length);\n"
        "  if (length != 3u) return 2;\n")
    endif()
  endif()
  string(APPEND code "  return hal_status_is_ok(status) ? 0 : 1;\n}\n")
  file(WRITE "${dir}/main.c" "${code}")
  file(WRITE "${dir}/main.cpp" "${code}")
  set(target jb::status)
  if(library)
    set(target jb::base)
    if(JB_UNICODE)
      set(target jb::unicode)
    endif()
  endif()
  file(WRITE "${dir}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.22)\nproject(consumer C CXX)\n"
    "find_package(jhbase 0.1 REQUIRED CONFIG)\n"
    "if(NOT jhbase_ABI_PROFILE MATCHES \"^natural-ptr(32|64)-status32-bool8$\")\n"
    "  message(FATAL_ERROR \"ABI profile not recorded: \${jhbase_ABI_PROFILE}\")\n"
    "endif()\n"
    "if(jhbase_REVISION STREQUAL \"\")\n"
    "  message(FATAL_ERROR \"library revision not recorded\")\n"
    "endif()\n"
    "if(NOT ${library} AND TARGET jb::base)\n"
    "  message(FATAL_ERROR \"the status package exports the library\")\n"
    "endif()\n"
    "add_executable(consumer_c main.c)\nadd_executable(consumer_cxx main.cpp)\n"
    "target_link_libraries(consumer_c PRIVATE ${target})\n"
    "target_link_libraries(consumer_cxx PRIVATE ${target})\n"
    "set_target_properties(consumer_c consumer_cxx PROPERTIES C_STANDARD 11 CXX_STANDARD 17)\n")
  run("${CMAKE_COMMAND}" -S "${dir}" -B "${dir}/build" -G "${JB_GENERATOR}" ${platform}
    "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_C_COMPILER=${JB_CC}"
    "-DCMAKE_CXX_COMPILER=${JB_CXX}" "-DCMAKE_BUILD_TYPE=${JB_CONFIG}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF)
  run("${CMAKE_COMMAND}" --build "${dir}/build" --config "${JB_CONFIG}")
  foreach(program consumer_c consumer_cxx)
    file(GLOB_RECURSE executable "${dir}/build/${program}" "${dir}/build/${program}.exe")
    list(LENGTH executable count)
    if(NOT count EQUAL 1)
      fail("${name}: ${program} not built")
    endif()
    run("${executable}")
  endforeach()
endfunction()

set(status_prefix "${JB_WORK}/status-prefix")
run("${CMAKE_COMMAND}" --install "${JB_BUILD}" --config "${JB_CONFIG}"
  --component jhbase_status --prefix "${status_prefix}")
file(GLOB_RECURSE archives "${status_prefix}/*.a" "${status_prefix}/*.lib")
if(archives)
  fail("the status component installed a library: ${archives}")
endif()
consumer(status "${status_prefix}" FALSE)

set(full_prefix "${JB_WORK}/full-prefix")
run("${CMAKE_COMMAND}" --install "${JB_BUILD}" --config "${JB_CONFIG}" --prefix "${full_prefix}")
consumer(full "${full_prefix}" TRUE)

# The exported targets carry install paths only, never the source tree.
file(GLOB_RECURSE exports "${full_prefix}/*.cmake")
foreach(export IN LISTS exports)
  file(READ "${export}" text)
  string(FIND "${text}" "${source}" position)
  if(NOT position EQUAL -1)
    fail("${export} refers to the source tree ${source}")
  endif()
endforeach()
set(relocated_prefix "${JB_WORK}/relocated prefix")
file(RENAME "${full_prefix}" "${relocated_prefix}")
consumer(relocated "${relocated_prefix}" TRUE)
message(STATUS "Installed status and library packages build C and C++ consumers")

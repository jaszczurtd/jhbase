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
    string(APPEND code "#include <jb/utf8.h>\n")
  endif()
  string(APPEND code "int main(void) {\n  static const char text[] = \"ok\";\n"
    "  hal_status_t status = (COUNTOF(text) == 3u) ? HAL_OK : HAL_EINVAL;\n")
  if(library)
    string(APPEND code "  size_t bad = 0;\n"
      "  if (status == HAL_OK) status = jb_utf8_validate(text, 2u, &bad);\n")
  endif()
  string(APPEND code "  return hal_status_is_ok(status) ? 0 : 1;\n}\n")
  file(WRITE "${dir}/main.c" "${code}")
  file(WRITE "${dir}/main.cpp" "${code}")
  set(target jb::status)
  if(library)
    set(target jb::base)
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
message(STATUS "Installed status and library packages build C and C++ consumers")

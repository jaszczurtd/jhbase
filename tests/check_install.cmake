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

function(reject reason)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
  if(result EQUAL 0 OR NOT output MATCHES "${reason}")
    fail("expected ${reason} from ${ARGN}, got ${result}:\n${output}")
  endif()
endfunction()

file(REMOVE_RECURSE "${JB_WORK}")
file(REAL_PATH "${JB_SOURCE}" source)
set(platform)
if(JB_PLATFORM)
  set(platform -A "${JB_PLATFORM}")
endif()
if(JB_UNICODE)
  include("${JB_SOURCE}/cmake/git_dependency.cmake")
  include("${JB_SOURCE}/cmake/utf8proc_pin.cmake")
  set(dependency "${JB_WORK}/external utf8proc ż")
  file(COPY "${JB_UTF8PROC}/" DESTINATION "${dependency}")
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${UTF8PROC_REF}" error)
  if(NOT error STREQUAL "")
    fail("a clean dependency copy failed verification: ${error}")
  endif()
  # HAL keeps jhbase and utf8proc in separate managed checkouts. This source
  # sample deliberately has no nested utf8proc, so a fallback cannot pass.
  set(external "${JB_WORK}/external Unicode")
  set(library "${external}/source")
  file(COPY "${JB_SOURCE}/CMakeLists.txt" "${JB_SOURCE}/cmake"
    "${JB_SOURCE}/include" "${JB_SOURCE}/src" DESTINATION "${library}")
  file(COPY "${JB_SOURCE}/third_party/ryu" "${JB_SOURCE}/third_party/utf8proc_version.conf"
    DESTINATION "${library}/third_party")
  file(WRITE "${external}/main.c"
    "#include <jb/unicode.h>\nint main(void) {\n"
    "  int32_t scratch[4];\n  const char *text = 0;\n  size_t length = 0;\n"
    "  return jb_unicode_nfc(\"abc\", 3u, scratch, 4u, 100u, &text, &length) == HAL_OK"
    " && length == 3u ? 0 : 1;\n}\n")
  file(WRITE "${external}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.22)\nproject(external_unicode C CXX)\n"
    "set(JHBASE_UNICODE ON CACHE BOOL \"\" FORCE)\n"
    "set(JHBASE_INSTALL_LIBRARY ON CACHE BOOL \"\" FORCE)\n"
    "if(DEFINED JB_PARENT_UTF8PROC)\n"
    "  set(JHBASE_UTF8PROC_DIR \"\${JB_PARENT_UTF8PROC}\")\nendif()\n"
    "add_subdirectory(source jhbase)\nadd_executable(probe main.c)\n"
    "target_link_libraries(probe PRIVATE jb::unicode)\n")
  set(configure_options -G "${JB_GENERATOR}" ${platform} "-DCMAKE_C_COMPILER=${JB_CC}"
    "-DCMAKE_CXX_COMPILER=${JB_CXX}" "-DCMAKE_BUILD_TYPE=${JB_CONFIG}")
  set(configure "${CMAKE_COMMAND}" -S "${external}" -B "${external}/build"
    ${configure_options})
  reject("Managed checkout is missing" ${configure}
    "-DJHBASE_UTF8PROC_DIR:PATH=${external}/missing")
  # A parent's normal variable shadows the PATH cache entry, so CMake does not
  # make it absolute. Git would resolve this path from the working directory
  # and the compiler from the jhbase source directory: different checkouts.
  file(RELATIVE_PATH relative "${CMAKE_CURRENT_BINARY_DIR}" "${dependency}")
  reject("JHBASE_UTF8PROC_DIR must be an absolute path"
    "${CMAKE_COMMAND}" -S "${external}" -B "${external}/relative" ${configure_options}
    "-DJB_PARENT_UTF8PROC=${relative}")
  file(SHA256 "${dependency}/.git/index" dependency_index)
  run(${configure} "-DJHBASE_UTF8PROC_DIR:PATH=${dependency}")
  set(build "${CMAKE_COMMAND}" --build "${external}/build" --config "${JB_CONFIG}")
  run(${build})
  file(GLOB_RECURSE probes "${external}/build/probe" "${external}/build/probe.exe")
  list(LENGTH probes count)
  if(NOT count EQUAL 1)
    fail("external Unicode probe not built")
  endif()
  run("${probes}")
  run("${CMAKE_COMMAND}" --install "${external}/build" --config "${JB_CONFIG}"
    --component jhbase_library --prefix "${external}/installed")
  file(SHA256 "${dependency}/LICENSE.md" expected_license)
  file(SHA256 "${external}/installed/share/jhbase/licenses/utf8proc/LICENSE.md" actual_license)
  if(NOT actual_license STREQUAL expected_license)
    fail("external Unicode license was not installed")
  endif()
  file(SHA256 "${dependency}/.git/index" actual_index)
  if(NOT actual_index STREQUAL dependency_index)
    fail("external Unicode configure/build changed the dependency index")
  endif()
  find_program(dependency_git NAMES git REQUIRED)
  run("${dependency_git}" -C "${dependency}" remote set-url origin "https://example.invalid/wrong.git")
  reject("Wrong origin" ${configure} "-DJHBASE_UTF8PROC_DIR:PATH=${dependency}")
  run("${dependency_git}" -C "${dependency}" remote set-url origin "${UTF8PROC_REPO}")
  run(${configure} "-DJHBASE_UTF8PROC_DIR:PATH=${dependency}")
  file(WRITE "${dependency}/unexpected.o" "unexpected content\n")
  reject("Modified managed checkout" ${build})
  file(REMOVE "${dependency}/unexpected.o")
  file(APPEND "${dependency}/utf8proc.c" "\n/* mutation */\n")
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${UTF8PROC_REF}" error)
  if(error STREQUAL "")
    fail("a modified dependency passed verification")
  endif()
  run("${dependency_git}" -C "${dependency}" update-index --assume-unchanged utf8proc.c)
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${UTF8PROC_REF}" error)
  if(error STREQUAL "")
    fail("an assume-unchanged dependency edit passed verification")
  endif()
  reject("Dependency source bytes differ" ${build})
  file(COPY "${JB_UTF8PROC}/utf8proc.c" DESTINATION "${dependency}")
  run(${build})
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "0000000000000000000000000000000000000000" error)
  if(error STREQUAL "")
    fail("a wrong dependency pin passed verification")
  endif()
  jb_check_git_dependency("${dependency}" "https://example.invalid/wrong.git" "${UTF8PROC_REF}" error)
  if(error STREQUAL "")
    fail("a wrong dependency origin passed verification")
  endif()
  # PCRE2-8 leaves its unused JIT submodule uninitialised. Only an explicit
  # allowance may accept that empty gitlink; files inside must still fail.
  run("${dependency_git}" -C "${dependency}" update-index --add --cacheinfo
    "160000,${UTF8PROC_REF},deps/unused")
  run("${dependency_git}" -c user.name=jhbase-test -c user.email=test@jhbase.invalid
    -C "${dependency}" commit -q -m "Dependency fixture with unused gitlink")
  file(MAKE_DIRECTORY "${dependency}/deps/unused")
  execute_process(COMMAND "${dependency_git}" -C "${dependency}" rev-parse HEAD
    OUTPUT_VARIABLE fixture_revision OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  reject("Wrong revision" ${build})
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${fixture_revision}" error)
  if(error STREQUAL "")
    fail("an unrequested gitlink passed verification")
  endif()
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${fixture_revision}" error
    EMPTY_SUBMODULES deps/unused)
  if(NOT error STREQUAL "")
    fail("an explicitly allowed empty submodule failed verification: ${error}")
  endif()
  file(WRITE "${dependency}/deps/unused/.hidden" "unexpected content\n")
  jb_check_git_dependency("${dependency}" "${UTF8PROC_REPO}" "${fixture_revision}" error
    EMPTY_SUBMODULES deps/unused)
  if(error STREQUAL "")
    fail("a file inside the unused submodule passed verification")
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
    if(JB_HOST)
      string(APPEND code "#include <jb/file.h>\n#include <jb/process.h>\n#include <jb/json_tree.h>\n")
    endif()
  endif()
  if(library AND JB_HOST)
    string(APPEND code "static bool canceled(void *context) { return context == 0; }\n")
  endif()
  string(APPEND code "int main(void) {\n  static const char text[] = \"ok\";\n"
    "  hal_status_t status = (COUNTOF(text) == 3u) ? HAL_OK : HAL_EINVAL;\n")
  if(library)
    if(JB_HOST)
      string(APPEND code "  jb_file_data_t file = {0, 0};\n"
        "  if (jb_file_read(\"\", 0u, &file) != HAL_EINVAL) return 4;\n"
        "  jb_file_release(&file);\n"
        "  jb_file_writer_t *writer = 0;\n  jb_file_lock_t *lock = 0;\n  jb_temp_dir_t *directory = 0;\n"
        "  if (jb_file_writer_abort(&writer) != HAL_OK || jb_file_lock_release(&lock) != HAL_OK || jb_temp_dir_remove(&directory) != HAL_OK) return 6;\n"
        "  jb_cancel_fn cancel = canceled;\n"
        "#ifdef _WIN32\n  const char *lock_path = \"C:/jb-cancel-probe\";\n#else\n  const char *lock_path = \"/jb-cancel-probe\";\n#endif\n"
        "  if (jb_file_lock_acquire(lock_path, 0, cancel, 0, &lock) != HAL_ECANCELED) return 7;\n"
        "  jb_process_result_t process;\n"
        "  if (jb_process_run(0, &process) != HAL_EINVAL || process.started) return 5;\n")
    endif()
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
    if(JB_HOST)
      string(APPEND code "  jb_json_tree_t *tree = 0;\n"
        "  const jb_json_tree_limits_t tree_limits = {1u, 0u, 0u, 0u};\n"
        "  const jb_json_tree_format_t format = {4u, 0u, false, false};\n"
        "  jb_json_tree_view_t view;\n  size_t count = 0u, written = 0u;\n"
        "  if (status == HAL_OK) status = jb_json_tree_create(node, &tree_limits, &tree);\n"
        "  if (status == HAL_OK) status = jb_json_tree_root(tree, &view);\n"
        "  if (status == HAL_OK) status = jb_json_tree_format(view, &format, jb_json_count_sink, &count, &written);\n"
        "  jb_json_tree_destroy(tree);\n"
        "  if (status == HAL_OK && (count != 4u || written != 4u)) return 8;\n")
    endif()
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
    if(JB_HOST)
      list(APPEND target jb::host)
    endif()
  endif()
  string(JOIN " " target ${target})
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

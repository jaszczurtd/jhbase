# Behaviour check of the VS Code workspace. The task documents must agree with
# the driver, and every driver action a task refers to must work on a scratch
# copy of the tree: select, build, test, refresh-intellisense, config-dump,
# clean and install-hooks. Nothing in the real checkout is touched.
#
#   cmake -DJB_SOURCE=<repository> -DJB_SCRATCH=<work dir>
#         [-DJB_TOOLS_ROOT=<tools>] [-DJB_UNITY_ROOT=<unity>] [-DJB_HAL_ROOT=<hal>]
#         -P tests/check_vscode_workspace.cmake
cmake_minimum_required(VERSION 3.22)

foreach(variable JB_SOURCE JB_SCRATCH)
  if(NOT DEFINED ${variable} OR ${variable} STREQUAL "")
    message(FATAL_ERROR "${variable} is required")
  endif()
endforeach()
file(REAL_PATH "${JB_SOURCE}" source)
set(driver "${source}/scripts/vscode_workspace.cmake")
set(tree "${JB_SCRATCH}/tree")

function(fail message)
  message(FATAL_ERROR "check_vscode_workspace: ${message}")
endfunction()

# The condition is CMake if() syntax in one string, evaluated as code so that
# quoted operands survive; variables of the calling scope are visible here.
function(require condition message)
  cmake_language(EVAL CODE "
    if(${condition})
      set(jb_condition_holds TRUE)
    else()
      set(jb_condition_holds FALSE)
    endif()")
  if(NOT jb_condition_holds)
    fail("${message}")
  endif()
endfunction()

# Runs the driver on the scratch tree; ARGN are its -D options.
function(run_driver out_var expect_success)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DJB_ROOT=${tree}" ${ARGN} -P "${driver}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(${out_var} "${out}${err}" PARENT_SCOPE)
  if(expect_success AND NOT rc EQUAL 0)
    fail("driver ${ARGN} failed (${rc}):\n${out}${err}")
  endif()
  if(NOT expect_success AND rc EQUAL 0)
    fail("driver ${ARGN} succeeded but had to fail:\n${out}${err}")
  endif()
endfunction()

function(read_json out_var relative)
  file(READ "${source}/${relative}" text)
  # A parse error here means the document is not plain JSON.
  string(JSON kind ERROR_VARIABLE error TYPE "${text}")
  if(error)
    fail("${relative}: ${error}")
  endif()
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# ---- documents -------------------------------------------------------------
read_json(tasks ".vscode/tasks.json")
read_json(settings ".vscode/settings.json")
read_json(keybindings ".vscode/keybindings.reference.json")
read_json(extensions ".vscode/extensions.json")

string(JSON cmake_setting GET "${settings}" "jhbase.cmake")
require("NOT cmake_setting STREQUAL \"\"" "settings.json: jhbase.cmake is empty")
string(JSON default_database GET "${settings}" "C_Cpp.default.compileCommands")

string(JSON task_count LENGTH "${tasks}" tasks)
math(EXPR last_task "${task_count} - 1")
set(labels)
set(driver_actions)
set(referenced_inputs)
foreach(index RANGE ${last_task})
  string(JSON label GET "${tasks}" tasks ${index} label)
  list(APPEND labels "${label}")
  string(JSON command GET "${tasks}" tasks ${index} command)
  string(JSON argument_count ERROR_VARIABLE error LENGTH "${tasks}" tasks ${index} args)
  if(error)
    fail("task '${label}' has no args array")
  endif()
  set(arguments)
  if(argument_count GREATER 0)
    math(EXPR last_argument "${argument_count} - 1")
    foreach(position RANGE ${last_argument})
      string(JSON argument GET "${tasks}" tasks ${index} args ${position})
      list(APPEND arguments "${argument}")
      if(argument MATCHES "\\$\\{input:([A-Za-z0-9]+)\\}")
        list(APPEND referenced_inputs "${CMAKE_MATCH_1}")
      endif()
    endforeach()
  endif()
  if(command STREQUAL "\${config:jhbase.cmake}")
    list(GET arguments 0 first)
    if(NOT first MATCHES "^-DJB_ACTION=([a-z-]+)$")
      fail("task '${label}' does not start with -DJB_ACTION=<action>")
    endif()
    list(APPEND driver_actions "${CMAKE_MATCH_1}")
    list(FIND arguments "-P" dash_p)
    require("dash_p GREATER 0" "task '${label}' passes no -P")
    math(EXPR script_position "${dash_p} + 1")
    list(GET arguments ${script_position} script)
    set(expected_script "\${workspaceFolder}/scripts/vscode_workspace.cmake")
    require("script STREQUAL expected_script"
      "task '${label}' runs ${script} instead of the workspace driver")
  elseif(command STREQUAL "./runalltests.sh")
    require("EXISTS \"${source}/runalltests.sh\"" "task '${label}': runalltests.sh is missing")
    string(JSON windows_command GET "${tasks}" tasks ${index} windows command)
    require("windows_command STREQUAL \"powershell\"" "task '${label}' lacks the Windows runner")
    require("EXISTS \"${source}/runalltests.ps1\"" "task '${label}': runalltests.ps1 is missing")
  else()
    fail("task '${label}' uses an unexpected command '${command}'")
  endif()
endforeach()
list(REMOVE_DUPLICATES driver_actions)

# Every ${input:...} reference names a declared input, and the picker lists
# exactly the driver's profiles.
string(JSON input_count LENGTH "${tasks}" inputs)
math(EXPR last_input "${input_count} - 1")
set(declared_inputs)
foreach(index RANGE ${last_input})
  string(JSON id GET "${tasks}" inputs ${index} id)
  list(APPEND declared_inputs "${id}")
  string(JSON type GET "${tasks}" inputs ${index} type)
  if(type STREQUAL "pickString")
    string(JSON option_count LENGTH "${tasks}" inputs ${index} options)
    math(EXPR last_option "${option_count} - 1")
    set(picker_options)
    foreach(position RANGE ${last_option})
      string(JSON option GET "${tasks}" inputs ${index} options ${position})
      list(APPEND picker_options "${option}")
    endforeach()
    string(JSON default GET "${tasks}" inputs ${index} default)
    require("default IN_LIST picker_options" "input '${id}': default is not an option")
  endif()
endforeach()
foreach(id IN LISTS referenced_inputs)
  require("id IN_LIST declared_inputs" "tasks reference an undeclared input '${id}'")
endforeach()
require("DEFINED picker_options" "tasks.json declares no profile picker")

foreach(index RANGE 0 5)
  string(JSON binding_label ERROR_VARIABLE error GET "${keybindings}" ${index} args)
  if(error)
    break()
  endif()
  require("binding_label IN_LIST labels" "keybinding refers to the missing task '${binding_label}'")
endforeach()
string(JSON recommendation GET "${extensions}" recommendations 0)
require("recommendation STREQUAL \"ms-vscode.cpptools\"" "extensions.json does not recommend cpptools first")

# ---- scratch copy ----------------------------------------------------------
file(REMOVE_RECURSE "${JB_SCRATCH}")
file(MAKE_DIRECTORY "${tree}")
file(GLOB entries LIST_DIRECTORIES true "${source}/*" "${source}/.[!.]*")
set(skipped ".build" ".git" "evidence" "NOW.md")
foreach(entry IN LISTS entries)
  get_filename_component(name "${entry}" NAME)
  if(name IN_LIST skipped)
    continue()
  endif()
  file(COPY "${entry}" DESTINATION "${tree}"
    PATTERN "jhbase.local.json" EXCLUDE
    PATTERN "c_cpp_properties.json" EXCLUDE)
endforeach()
require("EXISTS \"${tree}/CMakeLists.txt\"" "scratch copy lacks CMakeLists.txt")
require("NOT EXISTS \"${tree}/.vscode/jhbase.local.json\"" "scratch copy carries local state")

# Inner builds must not register this check again, and they need the tools
# and HAL inputs of the real checkout: the copy has no .build/tools and its
# relative HAL default would point beside the scratch directory.
set(inner_args "-DJHBASE_VSCODE_CHECK=OFF")
if(NOT DEFINED JB_TOOLS_ROOT OR JB_TOOLS_ROOT STREQUAL "")
  set(JB_TOOLS_ROOT "${source}/.build/tools")
endif()
if(NOT DEFINED JB_UNITY_ROOT OR JB_UNITY_ROOT STREQUAL "")
  set(JB_UNITY_ROOT "${JB_TOOLS_ROOT}/Unity")
endif()
if(NOT DEFINED JB_HAL_ROOT OR JB_HAL_ROOT STREQUAL "")
  get_filename_component(JB_HAL_ROOT "${source}/../JaszczurHAL" ABSOLUTE)
endif()
require("EXISTS \"${JB_UNITY_ROOT}/src/unity.c\"" "pinned Unity is missing below ${JB_UNITY_ROOT}; run scripts/ensure_tools.sh unity")
require("EXISTS \"${JB_HAL_ROOT}/tests/fixtures/device_tree/cases.json\"" "the HAL numeric fixture is missing below ${JB_HAL_ROOT}")
set(forwarded "-DJB_TOOLS_ROOT=${JB_TOOLS_ROOT}" "-DJB_UNITY_ROOT=${JB_UNITY_ROOT}"
  "-DJB_HAL_ROOT=${JB_HAL_ROOT}")

# ---- driver behaviour ------------------------------------------------------
run_driver(output TRUE -DJB_ACTION=list-profiles)
string(REGEX MATCHALL "-- ([a-z0-9-]+ - [^\n]*)" listed "${output}")
set(driver_profiles)
foreach(item IN LISTS listed)
  string(REGEX REPLACE "^-- " "" item "${item}")
  list(APPEND driver_profiles "${item}")
endforeach()
require("driver_profiles STREQUAL picker_options"
  "picker options ${picker_options} differ from the driver's profiles ${driver_profiles}")

run_driver(output TRUE -DJB_ACTION=config-dump)
require("output MATCHES \"profile: host \"" "config-dump does not report the default profile")
string(REGEX MATCH "compileCommands: ([^\n]*)" _ "${output}")
set(expected_database "\${workspaceFolder}/${CMAKE_MATCH_1}")
require("default_database STREQUAL expected_database"
  "settings.json compile database '${default_database}' is not the default profile's '${expected_database}'")

run_driver(output FALSE -DJB_ACTION=select -DJB_PROFILE=no-such-profile)
require("output MATCHES \"unknown profile\"" "an unknown profile was not reported")
run_driver(output FALSE -DJB_ACTION=select)
run_driver(output FALSE -DJB_ACTION=no-such-action)
run_driver(output FALSE -DJB_ACTION=build -DJB_CONFIG=Fast)

list(GET picker_options 2 sanitizer_option)
run_driver(output TRUE -DJB_ACTION=select "-DJB_PROFILE=${sanitizer_option}")
file(READ "${tree}/.vscode/jhbase.local.json" state)
string(JSON stored GET "${state}" profile)
require("stored STREQUAL \"host-sanitizers\"" "a picker entry was stored as '${stored}'")
run_driver(output TRUE -DJB_ACTION=config-dump)
require("output MATCHES \"profile: host-sanitizers \"" "config-dump ignores the stored profile")
require("output MATCHES \"JHBASE_SANITIZERS=ON\"" "the sanitizer profile has no sanitizer flag")

run_driver(output TRUE -DJB_ACTION=select -DJB_PROFILE=host)
run_driver(output TRUE -DJB_ACTION=build ${forwarded} "-DJB_CMAKE_ARGS=${inner_args}")
set(release_dir "${tree}/.build/vscode/host/Release")
require("EXISTS \"${release_dir}/compile_commands.json\"" "build left no compile database")
require("EXISTS \"${tree}/.vscode/c_cpp_properties.json\"" "build wrote no c_cpp_properties.json")
file(READ "${tree}/.vscode/c_cpp_properties.json" properties)
string(JSON configuration_name GET "${properties}" configurations 0 name)
require("configuration_name STREQUAL \"jhbase: host (Release)\"" "unexpected IntelliSense name '${configuration_name}'")
string(JSON configured_database GET "${properties}" configurations 0 compileCommands)
set(expected_release_database "\${workspaceFolder}/.build/vscode/host/Release/compile_commands.json")
require("configured_database STREQUAL expected_release_database"
  "IntelliSense points at '${configured_database}'")
string(JSON compiler GET "${properties}" configurations 0 compilerPath)
require("EXISTS \"${compiler}\"" "IntelliSense compilerPath '${compiler}' does not exist")
string(JSON c_standard GET "${properties}" configurations 0 cStandard)
require("c_standard STREQUAL \"c11\"" "IntelliSense C standard is '${c_standard}'")

# A filter proves the extra CTest arguments reach ctest as separate tokens.
run_driver(output TRUE -DJB_ACTION=test ${forwarded} "-DJB_CMAKE_ARGS=${inner_args}"
  "-DJB_CTEST_ARGS=-R ^utf8$")
# CTest 3.x prints "100% tests passed, 0 tests failed out of 1", CTest 4.x
# "100% tests passed out of 1".
require("output MATCHES \"100% tests passed(, 0 tests failed)? out of 1\"" "test did not run the filtered CTest suite:\n${output}")

run_driver(output TRUE -DJB_ACTION=build -DJB_CONFIG=Debug ${forwarded} "-DJB_CMAKE_ARGS=${inner_args}")
require("EXISTS \"${tree}/.build/vscode/host/Debug/compile_commands.json\"" "the Debug build has no own tree")
file(STRINGS "${tree}/.build/vscode/host/Debug/CMakeCache.txt" build_type REGEX "^CMAKE_BUILD_TYPE:")
require("build_type MATCHES \"=Debug$\"" "the Debug tree is configured as '${build_type}'")

run_driver(output TRUE -DJB_ACTION=install ${forwarded} "-DJB_CMAKE_ARGS=${inner_args}")
require("EXISTS \"${tree}/.build/install/host/Release/include/jb/status.h\""
  "install did not publish the active profile's headers")
require("EXISTS \"${tree}/.build/install/host/Release/lib/cmake/jhbase/jhbaseConfig.cmake\""
  "install did not publish its CMake package")
set(custom_prefix "${JB_SCRATCH}/installed with spaces")
run_driver(output TRUE -DJB_ACTION=install ${forwarded} "-DJB_CMAKE_ARGS=${inner_args}"
  "-DJB_INSTALL_PREFIX=${custom_prefix}")
require("EXISTS \"${custom_prefix}/include/jb/utf8.h\""
  "install ignored a prefix containing spaces")

file(REMOVE "${tree}/.vscode/c_cpp_properties.json")
run_driver(output TRUE -DJB_ACTION=refresh-intellisense ${forwarded} "-DJB_CMAKE_ARGS=${inner_args}")
require("EXISTS \"${tree}/.vscode/c_cpp_properties.json\"" "refresh-intellisense wrote no c_cpp_properties.json")
file(READ "${tree}/.vscode/c_cpp_properties.json" refreshed)
require("refreshed STREQUAL properties" "refresh-intellisense wrote a different document than build")

# clean removes the active profile only.
file(MAKE_DIRECTORY "${tree}/.build/vscode/host-clang")
file(WRITE "${tree}/.build/vscode/host-clang/marker" "keep\n")
run_driver(output TRUE -DJB_ACTION=clean)
require("NOT EXISTS \"${tree}/.build/vscode/host\"" "clean kept the active profile's trees")
require("NOT EXISTS \"${tree}/.vscode/c_cpp_properties.json\"" "clean kept c_cpp_properties.json")
require("EXISTS \"${tree}/.build/vscode/host-clang/marker\"" "clean removed another profile's tree")
require("EXISTS \"${tree}/.vscode/jhbase.local.json\"" "clean removed the stored profile")

# install-hooks: a note without git metadata, the hook path with it.
run_driver(output TRUE -DJB_ACTION=install-hooks)
require("output MATCHES \"Not a git checkout\"" "install-hooks did not notice a plain copy")
find_program(git_program NAMES git)
if(git_program)
  execute_process(COMMAND "${git_program}" -C "${tree}" init -q RESULT_VARIABLE rc)
  require("rc EQUAL 0" "git init failed in the scratch tree")
  run_driver(output TRUE -DJB_ACTION=install-hooks)
  execute_process(COMMAND "${git_program}" -C "${tree}" config core.hooksPath
    OUTPUT_VARIABLE hooks_path OUTPUT_STRIP_TRAILING_WHITESPACE)
  require("hooks_path STREQUAL \".githooks\"" "core.hooksPath is '${hooks_path}'")
endif()

# Every action the tasks use was exercised above.
set(exercised select build test install refresh-intellisense clean config-dump install-hooks)
foreach(action IN LISTS driver_actions)
  require("action IN_LIST exercised" "tasks.json uses the unchecked action '${action}'")
endforeach()

file(REMOVE_RECURSE "${JB_SCRATCH}")
message(STATUS "check_vscode_workspace: PASS (${task_count} tasks, ${input_count} inputs, profiles: ${driver_profiles})")

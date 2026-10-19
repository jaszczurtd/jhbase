# Workspace driver behind the VS Code tasks of this repository. One CMake
# script serves Linux, macOS and Windows, so the tasks need neither Python nor
# a shell-specific twin. Terminals can call it the same way:
#
#   cmake -DJB_ACTION=<action> [options] -P scripts/vscode_workspace.cmake
#
# Actions
#   select               store the build profile given by JB_PROFILE
#   build                configure and build the active profile
#   test                 build, then run CTest in the active profile
#   refresh-intellisense build, then rewrite .vscode/c_cpp_properties.json
#   clean                remove the active profile's build trees and the
#                        generated IntelliSense file, nothing else
#   config-dump          print the active profile and its paths
#   install-hooks        point core.hooksPath at the tracked .githooks
#   list-profiles        print the profiles as the task picker lists them
#
# Options
#   JB_PROFILE      profile id, or a picker entry "<id> - <detail>" (select)
#   JB_CONFIG       Release (default), Debug, RelWithDebInfo or MinSizeRel
#   JB_ROOT         repository root; default: the parent of this script
#   JB_TOOLS_ROOT   forwarded as JHBASE_TOOLS_ROOT (else the environment
#                   variable JHBASE_TOOLS_ROOT, else the CMake default)
#   JB_HAL_ROOT     forwarded as JHBASE_HAL_ROOT (same precedence)
#   JB_UNITY_ROOT   forwarded as JHBASE_UNITY_ROOT (same precedence); the gate
#                   and the Windows runner name the Unity checkout this way
#   JB_GENERATOR    CMake generator; default Ninja when available, otherwise
#                   NMake Makefiles on Windows, otherwise the CMake default
#   JB_CMAKE_ARGS   extra configure arguments, one string quoted like a
#                   command line, e.g. "-DJHBASE_LTO=ON -DX=\"a b\""
#   JB_CTEST_ARGS   extra CTest arguments, the same form, e.g. "-R ^utf8$"
#
# State lives in the ignored .vscode/jhbase.local.json; build trees in
# .build/vscode/<profile>/<config>/, inside the only build root the project
# accepts.
cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED JB_ROOT OR JB_ROOT STREQUAL "")
  get_filename_component(JB_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
file(REAL_PATH "${JB_ROOT}" jb_root)
set(jb_state "${jb_root}/.vscode/jhbase.local.json")
set(jb_cpp_properties "${jb_root}/.vscode/c_cpp_properties.json")
set(jb_workspace_build "${jb_root}/.build/vscode")
set(jb_default_profile host)
set(jb_default_config Release)

# Profiles: the host compilers the library and its tests are built with.
set(jb_profiles host host-clang host-sanitizers)
set(jb_profile_detail_host "default host compiler")
set(jb_profile_detail_host_clang "Clang")
set(jb_profile_detail_host_sanitizers "Clang with AddressSanitizer and UndefinedBehaviorSanitizer")

function(jb_profile_key profile out)
  string(REPLACE "-" "_" key "${profile}")
  set(${out} "${key}" PARENT_SCOPE)
endfunction()

function(jb_profile_detail profile out)
  jb_profile_key("${profile}" key)
  set(${out} "${jb_profile_detail_${key}}" PARENT_SCOPE)
endfunction()

function(jb_profile_list out)
  set(lines)
  foreach(profile IN LISTS jb_profiles)
    jb_profile_detail("${profile}" detail)
    list(APPEND lines "${profile} - ${detail}")
  endforeach()
  set(${out} "${lines}" PARENT_SCOPE)
endfunction()

function(jb_fail message)
  jb_profile_list(lines)
  string(REPLACE ";" "\n  " lines "${lines}")
  message(FATAL_ERROR "${message}\nProfiles:\n  ${lines}")
endfunction()

# Configure arguments of a profile; Clang is looked up once per run. A build
# stops without Clang; config-dump (lenient) reports the gap and goes on.
function(jb_profile_cmake_args profile out)
  set(lenient FALSE)
  if(ARGC GREATER 2 AND ARGV2 STREQUAL "LENIENT")
    set(lenient TRUE)
  endif()
  set(args)
  if(profile STREQUAL "host-clang" OR profile STREQUAL "host-sanitizers")
    find_program(jb_clang NAMES clang-18 clang)
    find_program(jb_clangxx NAMES clang++-18 clang++)
    if(jb_clang AND jb_clangxx)
      list(APPEND args "-DCMAKE_C_COMPILER=${jb_clang}" "-DCMAKE_CXX_COMPILER=${jb_clangxx}")
    elseif(lenient)
      message(STATUS "compiler: clang and clang++ (clang-18 preferred) not found on PATH; a build of ${profile} stops here")
    else()
      message(FATAL_ERROR "profile ${profile} needs clang and clang++ (clang-18 preferred) on PATH")
    endif()
  endif()
  if(profile STREQUAL "host-sanitizers")
    list(APPEND args -DJHBASE_SANITIZERS=ON)
  endif()
  set(${out} "${args}" PARENT_SCOPE)
endfunction()

function(jb_active_profile out)
  set(profile "${jb_default_profile}")
  if(EXISTS "${jb_state}")
    file(READ "${jb_state}" text)
    string(JSON profile ERROR_VARIABLE error GET "${text}" profile)
    if(error)
      message(FATAL_ERROR "cannot read ${jb_state}: ${error}")
    endif()
  endif()
  if(NOT profile IN_LIST jb_profiles)
    jb_fail("unknown profile '${profile}' in ${jb_state}; select a listed profile")
  endif()
  set(${out} "${profile}" PARENT_SCOPE)
endfunction()

function(jb_active_config out)
  set(config "${jb_default_config}")
  if(DEFINED JB_CONFIG AND NOT JB_CONFIG STREQUAL "")
    set(config "${JB_CONFIG}")
  endif()
  set(known Release Debug RelWithDebInfo MinSizeRel)
  if(NOT config IN_LIST known)
    message(FATAL_ERROR "JB_CONFIG must be one of: ${known}")
  endif()
  set(${out} "${config}" PARENT_SCOPE)
endfunction()

function(jb_generator out)
  if(DEFINED JB_GENERATOR AND NOT JB_GENERATOR STREQUAL "")
    set(${out} "${JB_GENERATOR}" PARENT_SCOPE)
    return()
  endif()
  find_program(jb_ninja NAMES ninja ninja-build)
  if(jb_ninja)
    set(${out} Ninja PARENT_SCOPE)
    return()
  endif()
  if(CMAKE_HOST_WIN32)
    find_program(jb_nmake NAMES nmake)
    if(jb_nmake)
      set(${out} "NMake Makefiles" PARENT_SCOPE)
      return()
    endif()
  endif()
  set(${out} "" PARENT_SCOPE)
endfunction()

function(jb_forwarded_root option variable out)
  if(DEFINED ${option} AND NOT ${option} STREQUAL "")
    set(${out} "${${option}}" PARENT_SCOPE)
  elseif(DEFINED ENV{${variable}} AND NOT "$ENV{${variable}}" STREQUAL "")
    set(${out} "$ENV{${variable}}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

function(jb_build_dir profile config out)
  set(${out} "${jb_workspace_build}/${profile}/${config}" PARENT_SCOPE)
endfunction()

function(jb_run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    list(JOIN ARGN " " text)
    message(FATAL_ERROR "command failed (${rc}): ${text}")
  endif()
endfunction()

function(jb_configure profile config build_dir)
  set(cmd "${CMAKE_COMMAND}" -S "${jb_root}" -B "${build_dir}")
  jb_generator(generator)
  if(generator)
    list(APPEND cmd -G "${generator}")
  endif()
  list(APPEND cmd "-DCMAKE_BUILD_TYPE=${config}" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DBUILD_TESTING=ON)
  jb_forwarded_root(JB_TOOLS_ROOT JHBASE_TOOLS_ROOT tools_root)
  if(tools_root)
    list(APPEND cmd "-DJHBASE_TOOLS_ROOT=${tools_root}")
  endif()
  jb_forwarded_root(JB_HAL_ROOT JHBASE_HAL_ROOT hal_root)
  if(hal_root)
    list(APPEND cmd "-DJHBASE_HAL_ROOT=${hal_root}")
  endif()
  jb_forwarded_root(JB_UNITY_ROOT JHBASE_UNITY_ROOT unity_root)
  if(unity_root)
    list(APPEND cmd "-DJHBASE_UNITY_ROOT=${unity_root}")
  endif()
  jb_profile_cmake_args("${profile}" profile_args)
  list(APPEND cmd ${profile_args})
  if(DEFINED JB_CMAKE_ARGS AND NOT JB_CMAKE_ARGS STREQUAL "")
    separate_arguments(extra NATIVE_COMMAND "${JB_CMAKE_ARGS}")
    list(APPEND cmd ${extra})
  endif()
  jb_run(${cmd})
endfunction()

function(jb_build profile config)
  jb_build_dir("${profile}" "${config}" build_dir)
  jb_configure("${profile}" "${config}" "${build_dir}")
  cmake_host_system_information(RESULT jobs QUERY NUMBER_OF_LOGICAL_CORES)
  jb_run("${CMAKE_COMMAND}" --build "${build_dir}" --config "${config}" --parallel "${jobs}")
endfunction()

# cpptools reads the compile database of the active profile; compilerPath lets
# it find the system headers of that compiler.
function(jb_write_cpp_properties profile config)
  jb_build_dir("${profile}" "${config}" build_dir)
  set(database "${build_dir}/compile_commands.json")
  if(NOT EXISTS "${database}")
    jb_generator(generator)
    message(FATAL_ERROR "no ${database}: the generator '${generator}' exports no "
      "compile database; install ninja or pass -DJB_GENERATOR=Ninja")
  endif()
  file(STRINGS "${build_dir}/CMakeCache.txt" compiler_line
    REGEX "^CMAKE_C_COMPILER:(FILEPATH|STRING|UNINITIALIZED)=")
  string(REGEX REPLACE "^[^=]*=" "" compiler "${compiler_line}")
  file(RELATIVE_PATH relative "${jb_root}" "${database}")
  file(MAKE_DIRECTORY "${jb_root}/.vscode")
  file(WRITE "${jb_cpp_properties}"
    "{\n"
    "    \"configurations\": [\n"
    "        {\n"
    "            \"name\": \"jhbase: ${profile} (${config})\",\n"
    "            \"compileCommands\": \"\${workspaceFolder}/${relative}\",\n"
    "            \"compilerPath\": \"${compiler}\",\n"
    "            \"cStandard\": \"c11\",\n"
    "            \"cppStandard\": \"c++17\"\n"
    "        }\n"
    "    ],\n"
    "    \"version\": 4\n"
    "}\n")
  message(STATUS "IntelliSense: ${relative} (${compiler})")
  message(STATUS "Wrote ${jb_cpp_properties}")
endfunction()

if(NOT DEFINED JB_ACTION OR JB_ACTION STREQUAL "")
  message(FATAL_ERROR "pass -DJB_ACTION=select|build|test|refresh-intellisense|clean|config-dump|install-hooks|list-profiles")
endif()

if(JB_ACTION STREQUAL "list-profiles")
  jb_profile_list(lines)
  foreach(line IN LISTS lines)
    message(STATUS "${line}")
  endforeach()
elseif(JB_ACTION STREQUAL "select")
  if(NOT DEFINED JB_PROFILE OR JB_PROFILE STREQUAL "")
    jb_fail("select needs -DJB_PROFILE=<id>")
  endif()
  # The GUI picker passes "<id> - <detail>"; a terminal passes the id alone.
  string(REGEX REPLACE " - .*$" "" profile "${JB_PROFILE}")
  string(STRIP "${profile}" profile)
  if(NOT profile IN_LIST jb_profiles)
    jb_fail("unknown profile '${profile}'")
  endif()
  file(MAKE_DIRECTORY "${jb_root}/.vscode")
  file(WRITE "${jb_state}" "{\n    \"schemaVersion\": 1,\n    \"profile\": \"${profile}\"\n}\n")
  jb_profile_detail("${profile}" detail)
  message(STATUS "Active profile: ${profile} (${detail}); stored in ${jb_state}")
elseif(JB_ACTION STREQUAL "build" OR JB_ACTION STREQUAL "refresh-intellisense")
  jb_active_profile(profile)
  jb_active_config(config)
  jb_build("${profile}" "${config}")
  jb_write_cpp_properties("${profile}" "${config}")
elseif(JB_ACTION STREQUAL "test")
  jb_active_profile(profile)
  jb_active_config(config)
  jb_build("${profile}" "${config}")
  jb_write_cpp_properties("${profile}" "${config}")
  jb_build_dir("${profile}" "${config}" build_dir)
  if(NOT DEFINED CMAKE_CTEST_COMMAND OR CMAKE_CTEST_COMMAND STREQUAL "")
    get_filename_component(cmake_dir "${CMAKE_COMMAND}" DIRECTORY)
    find_program(CMAKE_CTEST_COMMAND NAMES ctest HINTS "${cmake_dir}" REQUIRED)
  endif()
  set(cmd "${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}" --output-on-failure -C "${config}")
  if(DEFINED JB_CTEST_ARGS AND NOT JB_CTEST_ARGS STREQUAL "")
    separate_arguments(extra NATIVE_COMMAND "${JB_CTEST_ARGS}")
    list(APPEND cmd ${extra})
  endif()
  jb_run(${cmd})
elseif(JB_ACTION STREQUAL "clean")
  jb_active_profile(profile)
  # The profile id is validated above, so this path cannot leave .build/vscode.
  set(profile_dir "${jb_workspace_build}/${profile}")
  file(REMOVE_RECURSE "${profile_dir}" "${jb_cpp_properties}")
  message(STATUS "Removed ${profile_dir} and ${jb_cpp_properties}")
elseif(JB_ACTION STREQUAL "config-dump")
  jb_active_profile(profile)
  jb_active_config(config)
  jb_profile_detail("${profile}" detail)
  jb_profile_cmake_args("${profile}" profile_args LENIENT)
  jb_generator(generator)
  jb_build_dir("${profile}" "${config}" build_dir)
  jb_forwarded_root(JB_TOOLS_ROOT JHBASE_TOOLS_ROOT tools_root)
  jb_forwarded_root(JB_HAL_ROOT JHBASE_HAL_ROOT hal_root)
  jb_forwarded_root(JB_UNITY_ROOT JHBASE_UNITY_ROOT unity_root)
  file(RELATIVE_PATH relative "${jb_root}" "${build_dir}/compile_commands.json")
  message(STATUS "root: ${jb_root}")
  message(STATUS "profile: ${profile} (${detail})")
  message(STATUS "config: ${config}")
  message(STATUS "profileArgs: ${profile_args}")
  if(generator)
    message(STATUS "generator: ${generator}")
  else()
    message(STATUS "generator: CMake default")
  endif()
  message(STATUS "buildDir: ${build_dir}")
  message(STATUS "compileCommands: ${relative}")
  message(STATUS "cppProperties: ${jb_cpp_properties}")
  message(STATUS "state: ${jb_state}")
  if(tools_root)
    message(STATUS "toolsRoot: ${tools_root}")
  else()
    message(STATUS "toolsRoot: CMake default (.build/tools)")
  endif()
  if(hal_root)
    message(STATUS "halRoot: ${hal_root}")
  else()
    message(STATUS "halRoot: CMake default (../JaszczurHAL)")
  endif()
  if(unity_root)
    message(STATUS "unityRoot: ${unity_root}")
  else()
    message(STATUS "unityRoot: CMake default (<toolsRoot>/Unity)")
  endif()
elseif(JB_ACTION STREQUAL "install-hooks")
  find_program(jb_git NAMES git)
  if(NOT jb_git OR NOT EXISTS "${jb_root}/.git")
    message(STATUS "Not a git checkout; hooks not installed")
  else()
    jb_run("${jb_git}" -C "${jb_root}" config core.hooksPath .githooks)
    if(NOT CMAKE_HOST_WIN32)
      file(CHMOD "${jb_root}/.githooks/pre-commit" "${jb_root}/.githooks/commit-msg"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE)
    endif()
    message(STATUS "core.hooksPath = .githooks")
  endif()
else()
  message(FATAL_ERROR "unknown action '${JB_ACTION}'")
endif()

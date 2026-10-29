file(MAKE_DIRECTORY "${JB_WORK}")
set(includes "#include \"jb/json.h\"\n#include \"jb/utf8.h\"\n#include \"jb/json_number.h\"\n#include \"jb/json_writer.h\"\n#include \"jb/literal.h\"\n")
string(APPEND includes "#include \"jb/file.h\"\n#include \"jb/process.h\"\n")

function(probe name source should_pass language)
  if(language STREQUAL "CXX")
    set(compiler "${JB_CXX}")
    set(extension cpp)
    set(standard c++17)
  else()
    set(compiler "${JB_CC}")
    set(extension c)
    set(standard c11)
  endif()
  set(path "${JB_WORK}/${name}.${extension}")
  file(WRITE "${path}" "${source}")
  if(JB_MSVC)
    set(args /nologo "/std:${standard}" /WX /c
        "/I${JB_SOURCE}/include"
        "${path}" "/Fo${JB_WORK}/${name}.obj")
  else()
    set(args "-std=${standard}" -Wall -Wextra -Werror -pedantic
        "-I${JB_SOURCE}/include" -c
        "${path}" -o "${JB_WORK}/${name}.o" ${ARGN})
  endif()
  execute_process(COMMAND "${compiler}" ${args}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  file(WRITE "${JB_WORK}/${name}.log" "${output}${errors}")
  if(should_pass)
    if(NOT result STREQUAL "0")
      message(FATAL_ERROR "Expected ${name} to compile: ${output}${errors}")
    endif()
  else()
    if(result STREQUAL "0")
      message(FATAL_ERROR "Invalid ABI was accepted: ${name}")
    endif()
    if(NOT "${output}${errors}" MATCHES "jb JSON|jb requires")
      message(FATAL_ERROR "${name} failed without a jhbase ABI assertion: ${output}${errors}")
    endif()
  endif()
endfunction()

foreach(language C CXX)
  probe("natural_${language}" "${includes}" TRUE "${language}")
  probe("pack_before_${language}" "#pragma pack(push, 1)\n${includes}#pragma pack(pop)\n"
        FALSE "${language}")
  probe("pack_between_${language}"
        "#include \"jb/status.h\"\n#pragma pack(push, 1)\n${includes}#pragma pack(pop)\n"
        FALSE "${language}")
  probe("pack_after_json_${language}"
        "#include \"jb/json.h\"\n#pragma pack(push, 1)\n#include \"jb/json_number.h\"\n#include \"jb/json_writer.h\"\n#pragma pack(pop)\n"
        FALSE "${language}")
  if(NOT JB_MSVC)
    # The status keeps 32 bits under -fshort-enums (the ARM EABI default).
    probe("short_enum_${language}" "#include \"jb/status.h\"\n${includes}"
          TRUE "${language}" -fshort-enums)
    probe("packed_flag_${language}" "${includes}" FALSE "${language}" -fpack-struct=1)
  endif()
endforeach()
message(STATUS "Natural C/C++ layouts accepted; incompatible packing/enum flags rejected")

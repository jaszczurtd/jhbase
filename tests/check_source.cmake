# Check owned source text, including files outside Git in a clean source copy.
file(GLOB_RECURSE sources "${JB_SOURCE}/include/*.h" "${JB_SOURCE}/src/*.[ch]"
  "${JB_SOURCE}/doc/*.md" "${JB_SOURCE}/cmake/*.cmake" "${JB_SOURCE}/scripts/*.sh")
file(GLOB test_sources "${JB_SOURCE}/tests/*.[ch]" "${JB_SOURCE}/tests/*.cpp"
  "${JB_SOURCE}/tests/*.cmake" "${JB_SOURCE}/tests/*.ld")
list(APPEND sources ${test_sources})
file(GLOB_RECURSE examples "${JB_SOURCE}/examples/*.c")
list(APPEND sources ${examples})
list(APPEND sources "${JB_SOURCE}/CMakeLists.txt" "${JB_SOURCE}/runalltests.sh"
  "${JB_SOURCE}/runalltests.ps1" "${JB_SOURCE}/README.md" "${JB_SOURCE}/README.pl.md")
foreach(path IN LISTS sources)
  file(READ "${path}" content)
  if(content MATCHES "[ \t]+\n" OR content MATCHES "\r" OR NOT content MATCHES "\n$")
    message(FATAL_ERROR "Trailing whitespace, CR or missing final newline: ${path}")
  endif()
endforeach()

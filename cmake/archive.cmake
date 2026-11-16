# jb::archive: ZIP and tar.gz extraction over the pinned, unmodified libarchive
# and zlib checkouts below third_party, built through their own CMake projects
# with only the zlib codec, the TAR/ZIP readers and nothing else enabled.
# Configure and every build verify both checkouts; nothing is downloaded.
include("${CMAKE_CURRENT_LIST_DIR}/archive_verify.cmake")
get_property(jb_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT jb_multi_config AND CMAKE_BUILD_TYPE STREQUAL "")
  # libarchive would otherwise force Release into the cache of the whole build.
  message(FATAL_ERROR "JHBASE_ARCHIVE needs CMAKE_BUILD_TYPE with a single-configuration generator")
endif()
add_custom_target(jb_verify_archive
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/archive_verify.cmake"
  COMMENT "Verifying the clean libarchive and zlib pins" VERBATIM)

# Options are normal variables in this function's scope: both projects honor
# them through policy CMP0077, so the parent cache keeps only the zlib paths.
function(jb_add_archive_dependencies)
  get_filename_component(zlib_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/zlib" ABSOLUTE)
  get_filename_component(libarchive_source
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/libarchive" ABSOLUTE)
  # Both projects configure with their own language defaults: the strict C11
  # mode of jhbase hides POSIX types from libarchive's type checks (id_t).
  unset(CMAKE_C_STANDARD)
  unset(CMAKE_C_STANDARD_REQUIRED)
  unset(CMAKE_C_EXTENSIONS)
  set(ZLIB_BUILD_SHARED OFF)
  set(ZLIB_BUILD_STATIC ON)
  set(ZLIB_BUILD_TESTING OFF)
  set(ZLIB_INSTALL OFF)
  add_subdirectory("${zlib_source}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib" EXCLUDE_FROM_ALL)

  # FindZLIB in libarchive takes this target name as the library. The include
  # directory goes to the cache, where find_path keeps it instead of searching
  # the system; a different value set earlier by a parent is refused.
  if(DEFINED CACHE{ZLIB_INCLUDE_DIR} AND NOT ZLIB_INCLUDE_DIR STREQUAL zlib_source)
    message(FATAL_ERROR "ZLIB_INCLUDE_DIR already names another zlib: ${ZLIB_INCLUDE_DIR}")
  endif()
  set(ZLIB_INCLUDE_DIR "${zlib_source}" CACHE PATH "zlib headers of jb::archive" FORCE)
  set(ZLIB_LIBRARY zlibstatic)
  set(ZLIB_LIBRARIES zlibstatic)
  set(BUILD_SHARED_LIBS OFF)
  # ENABLE_LIBGCC stays as upstream sets it: it only accompanies PCRE regex
  # support, but turning it off fails configure where libc has no regex.h.
  foreach(option IN ITEMS TAR TAR_SHARED CPIO CPIO_SHARED CAT CAT_SHARED UNZIP UNZIP_SHARED
      TEST COVERAGE INSTALL WERROR CLANG_TIDY MBEDTLS NETTLE OPENSSL MD LIBB2 LZ4 LZO LZMA
      ZSTD BZip2 LIBXML2 BSDXML EXPAT WIN32_XMLLITE PCREPOSIX PCRE2POSIX CNG XATTR ACL ICONV)
    set(ENABLE_${option} OFF)
  endforeach()
  set(ENABLE_ZLIB ON)
  add_subdirectory("${libarchive_source}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/libarchive"
                   EXCLUDE_FROM_ALL)
  if(NOT TARGET archive_static OR NOT TARGET zlibstatic)
    message(FATAL_ERROR "libarchive or zlib did not define its static library")
  endif()
  # Exported under jhbase names that cannot collide with system libraries;
  # the build tree supplies the headers, the install tree needs none.
  set_target_properties(archive_static PROPERTIES OUTPUT_NAME jb_libarchive
    EXPORT_NAME libarchive INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${libarchive_source}/libarchive>")
  set_target_properties(zlibstatic PROPERTIES OUTPUT_NAME jb_zlib ARCHIVE_OUTPUT_NAME jb_zlib
    EXPORT_NAME zlib PUBLIC_HEADER "")
  add_dependencies(archive_static jb_verify_archive)
  add_dependencies(zlibstatic jb_verify_archive)
  set(jb_libarchive_source "${libarchive_source}" PARENT_SCOPE)
  set(jb_zlib_source "${zlib_source}" PARENT_SCOPE)
endfunction()
jb_add_archive_dependencies()

add_library(jb_archive STATIC src/host/archive.c)
add_library(jb::archive ALIAS jb_archive)
set_target_properties(jb_archive PROPERTIES EXPORT_NAME archive C_EXTENSIONS OFF)
target_compile_features(jb_archive PUBLIC c_std_11)
target_compile_definitions(jb_archive PRIVATE LIBARCHIVE_STATIC)
# zlib directly too: libarchive does not check gzip trailers, jb_archive does.
target_link_libraries(jb_archive PUBLIC jb::filesystem PRIVATE archive_static zlibstatic)
jb_warnings(jb_archive)
if(NOT PROJECT_IS_TOP_LEVEL)
  set_target_properties(jb_archive PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

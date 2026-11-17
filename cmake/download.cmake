# jb::download: HTTPS transfers over the pinned, unmodified curl checkout below
# third_party, built through its own CMake project as a static libcurl with HTTP
# only, the threaded resolver and the platform TLS backend: Schannel on Windows,
# the system OpenSSL (development package) on Linux. Configure and every build
# verify the checkout; nothing is downloaded.
include("${CMAKE_CURRENT_LIST_DIR}/download_verify.cmake")
add_custom_target(jb_verify_download
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/download_verify.cmake"
  COMMENT "Verifying the clean curl pin" VERBATIM)

# Options are normal variables in this function's scope: curl honors them
# through policy CMP0077, so the parent cache keeps only curl's own entries.
function(jb_add_download_dependencies)
  get_filename_component(curl_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/curl" ABSOLUTE)
  # curl declares CMake 3.18, so set(... CACHE) of its AUTO options (zlib,
  # brotli, zstd) would drop these normal variables under the old CMP0126.
  set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
  # curl configures with its own language defaults, like libarchive.
  unset(CMAKE_C_STANDARD)
  unset(CMAKE_C_STANDARD_REQUIRED)
  unset(CMAKE_C_EXTENSIONS)
  # Only the static library: no curl tool, tests, examples or documentation,
  # whatever Perl or other programs the host has.
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_STATIC_LIBS ON)
  set(BUILD_CURL_EXE OFF)
  set(BUILD_STATIC_CURL OFF)
  set(BUILD_TESTING OFF)
  set(BUILD_EXAMPLES OFF)
  set(BUILD_LIBCURL_DOCS OFF)
  set(BUILD_MISC_DOCS OFF)
  set(ENABLE_CURL_MANUAL OFF)
  set(CMAKE_DISABLE_FIND_PACKAGE_Perl ON)
  set(CURL_DISABLE_INSTALL ON)
  set(CURL_ENABLE_EXPORT_TARGET OFF)
  set(PICKY_COMPILER OFF)
  set(CURL_WERROR OFF)
  set(CURL_LTO OFF)
  set(ENABLE_DEBUG OFF)
  set(CURL_USE_PKGCONFIG OFF)
  set(CURL_USE_CMAKECONFIG OFF)
  # The TLS backend is chosen here, never detected.
  set(CURL_ENABLE_SSL ON)
  if(WIN32)
    set(CURL_USE_SCHANNEL ON)
    set(CURL_USE_OPENSSL OFF)
    # UTF-8 file names (CA bundle) reach the wide Windows API.
    set(ENABLE_UNICODE ON)
    set(CURL_DISABLE_CA_SEARCH ON)
    set(CURL_CA_SEARCH_SAFE OFF)
    set(USE_WIN32_IDN OFF)
    set(USE_WIN32_LDAP OFF)
  else()
    set(CURL_USE_OPENSSL ON)
  endif()
  foreach(library IN ITEMS MBEDTLS WOLFSSL GNUTLS RUSTLS LIBPSL LIBSSH2 LIBSSH GSASL GSSAPI
      LIBBACKTRACE LIBUV)
    set(CURL_USE_${library} OFF)
  endforeach()
  foreach(option IN ITEMS CURL_ZLIB CURL_BROTLI CURL_ZSTD USE_NGHTTP2 USE_NGTCP2 USE_QUICHE
      USE_LIBIDN2 USE_HTTPSRR USE_ECH USE_SSLS_EXPORT USE_APPLE_IDN ENABLE_ARES
      ENABLE_UNIX_SOCKETS CURL_CA_FALLBACK CURL_CA_NATIVE CURL_ENABLE_NTLM)
    set(${option} OFF)
  endforeach()
  set(ENABLE_THREADED_RESOLVER ON)
  set(ENABLE_IPV6 ON)
  # HTTP stays; Basic remains for proxy credentials. Digest is off: under
  # Schannel it goes through SSPI, which refused a standard Digest challenge
  # that the OpenSSL build answered. Netrc, cookies and the HSTS/alt-svc
  # caches would read or keep state outside the call.
  foreach(feature IN ITEMS DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP
      IPFS WEBSOCKETS COOKIES ALTSVC HSTS DOH NETRC AWS BINDLOCAL MIME FORM_API GETOPTIONS
      HEADERS_API LIBCURL_OPTION PROGRESS_METER BEARER_AUTH DIGEST_AUTH KERBEROS_AUTH NEGOTIATE_AUTH
      HTTPSIG)
    set(CURL_DISABLE_${feature} ON)
  endforeach()
  add_subdirectory("${curl_source}" "${CMAKE_CURRENT_BINARY_DIR}/third_party/curl" EXCLUDE_FROM_ALL)
  if(NOT TARGET libcurl_static)
    message(FATAL_ERROR "curl did not define its static library")
  endif()
  # The selected configuration as curl's own scope recorded it.
  get_directory_property(threaded DIRECTORY "${curl_source}" DEFINITION USE_RESOLV_THREADED)
  if(WIN32)
    get_directory_property(tls DIRECTORY "${curl_source}" DEFINITION USE_SCHANNEL)
  else()
    get_directory_property(tls DIRECTORY "${curl_source}" DEFINITION USE_OPENSSL)
  endif()
  if(NOT tls OR NOT threaded)
    message(FATAL_ERROR "curl was configured without the platform TLS backend or the threaded resolver")
  endif()
  get_directory_property(protocols DIRECTORY "${curl_source}" DEFINITION SUPPORT_PROTOCOLS)
  get_directory_property(features DIRECTORY "${curl_source}" DEFINITION SUPPORT_FEATURES)
  string(TOLOWER "${protocols}" protocols)
  if(NOT protocols STREQUAL "http https")
    message(FATAL_ERROR "curl protocols differ from the profile: ${protocols}")
  endif()
  separate_arguments(features UNIX_COMMAND "${features}")
  foreach(feature IN ITEMS AsynchDNS SSL threadsafe)
    if(NOT feature IN_LIST features)
      message(FATAL_ERROR "curl lacks the required feature ${feature}: ${features}")
    endif()
  endforeach()
  foreach(feature IN ITEMS libz brotli zstd HTTP2 HTTP3 IDN PSL alt-svc HSTS NTLM SPNEGO Kerberos
      GSS-API gsasl HTTPSRR ECH asyn-rr)
    if(feature IN_LIST features)
      message(FATAL_ERROR "curl enables ${feature} outside the profile: ${features}")
    endif()
  endforeach()
  if(NOT WIN32)
    # The system store of the distribution, as curl detected it.
    if(NOT CURL_CA_BUNDLE_SET AND NOT CURL_CA_PATH_SET)
      message(FATAL_ERROR "No system CA bundle or directory found; install ca-certificates")
    endif()
    set(jb_curl_ca_store "${CURL_CA_BUNDLE}" "${CURL_CA_PATH}")
    list(REMOVE_ITEM jb_curl_ca_store "")
    message(STATUS "jb::download CA store: ${jb_curl_ca_store}")
  endif()
  # Exported under a jhbase name that cannot collide with a system libcurl;
  # the build tree supplies the headers, the install tree needs none.
  set_target_properties(libcurl_static PROPERTIES OUTPUT_NAME jb_libcurl EXPORT_NAME libcurl
    INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${curl_source}/include>")
  add_dependencies(libcurl_static jb_verify_download)
  set(jb_curl_source "${curl_source}" PARENT_SCOPE)
endfunction()
jb_add_download_dependencies()

add_library(jb_download STATIC src/host/download.c)
add_library(jb::download ALIAS jb_download)
set_target_properties(jb_download PROPERTIES EXPORT_NAME download C_EXTENSIONS OFF)
target_compile_features(jb_download PUBLIC c_std_11)
# Downloads stream through the atomic file writer of jb::host.
target_link_libraries(jb_download PUBLIC jb::host PRIVATE libcurl_static)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(jb_download PRIVATE Threads::Threads)
endif()
jb_warnings(jb_download)
if(NOT PROJECT_IS_TOP_LEVEL)
  set_target_properties(jb_download PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

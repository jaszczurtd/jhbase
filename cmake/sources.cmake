# Sources of portable modules that a firmware library may compile into its own
# archive instead of linking jb::base. jhbase builds the same files into
# jb::base. JaszczurHAL includes this file to build the SHA-256 core into every
# HAL library, also where it has no jhbase targets (ESP-IDF components).
get_filename_component(jb_sources_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(JHBASE_SHA256_SOURCES "${jb_sources_root}/src/sha256.c")
unset(jb_sources_root)

#ifndef JB_FILESYSTEM_INTERNAL_H
#define JB_FILESYSTEM_INTERNAL_H

/* Platform helpers of jb::filesystem shared with other C host modules in
 * this repository. Not installed and not part of the public API. */
#include "jb/status.h"
#include <stdint.h>
#ifdef _WIN32
#include <wchar.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Map errno (POSIX) or GetLastError (Windows) to the filesystem statuses. */
hal_status_t jb_fs_native_status(uint32_t code);

#ifdef _WIN32
/* Convert a nonempty UTF-8 path to UTF-16; release with jb_heap_allocator(). */
hal_status_t jb_fs_native_path(const char *path, wchar_t **out);
#endif

#ifdef __cplusplus
}
#endif
#endif

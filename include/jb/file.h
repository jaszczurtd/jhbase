#ifndef JB_FILE_H
#define JB_FILE_H

/** @file file.h
 * Host file operations on UTF-8 paths. Link jb::host (C++17 runtime).
 * Linux and Windows only; host allocations never enter jb::base.
 * Calls are independent and do not change the process working directory.
 */
#include "jb/status.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Owned file bytes; data may be NULL for an empty file. No terminator.
 * Release with jb_file_release. Initialize to {0} before the first read;
 * release the previous contents before reusing this object as an output.
 */
typedef struct {
  unsigned char *data;
  size_t length;
} jb_file_data_t;

/** Read a regular file, following existing symlinks, with an explicit byte limit.
 * @param path Required nonempty NUL-terminated UTF-8 path, relative to cwd or absolute.
 * @param max_bytes Maximum accepted size, also enforced if the file grows.
 * @param out Required empty owner, assigned only on success.
 * @return HAL_OK, HAL_EINVAL (arguments), HAL_EPROTO (invalid UTF-8 path),
 * HAL_ESTATE (nonempty out), HAL_ENOENT, HAL_EPERM, HAL_ETYPE (nonregular file),
 * HAL_EOVERFLOW (limit), HAL_ENOMEM or HAL_EIO. No exception crosses the C API.
 */
hal_status_t jb_file_read(const char *path, size_t max_bytes, jb_file_data_t *out);

/** Release file bytes and reset both fields; NULL is allowed. */
void jb_file_release(jb_file_data_t *data);

/** Compare or atomically publish complete bytes, preserving an identical file.
 * @param path Required UTF-8 path. Missing parent directories are created for writes.
 * @param data Exact bytes; NULL is allowed only at zero length.
 * @param length Number of bytes, including any embedded NUL.
 * @param check_only Compare without creating directories or changing files.
 * @param changed Required result; true for missing/different content, false for identical.
 * @return HAL_OK or the file-read errors; HAL_EBUSY on a reported sharing
 * conflict. Windows can also report HAL_EPERM for a held replacement target.
 * Final symlinks/reparse points and nonregular destinations give HAL_ETYPE;
 * parent links follow the host filesystem. This is not a sandbox API.
 * A unique sibling temporary file is flushed and closed before replacement,
 * then removed on failure. POSIX also fsyncs the parent directory. Windows
 * uses FlushFileBuffers and MoveFileExW with REPLACE_EXISTING/WRITE_THROUGH.
 * New files use private permissions on POSIX; replacements preserve rwx bits.
 * Existing content retains mtime when identical. Concurrent writers use
 * last successful replacement wins; concurrent external edits are not locked.
 * On error changed is unchanged, except a POSIX directory flush failure after
 * publication sets changed=true: new bytes are visible but durability failed.
 */
hal_status_t jb_file_write_atomic(const char *path, const void *data, size_t length,
                                  bool check_only, bool *changed);

/** Resolve a path lexically against an explicit absolute UTF-8 base directory.
 * Relative base paths and Windows drive-relative paths give HAL_EINVAL.
 * No filesystem lookup, symlink resolution or cwd change is performed.
 * @param base Required absolute path, even when path is already absolute.
 * @param path Required nonempty UTF-8 path.
 * @param buffer Output with a NUL terminator; (NULL,0) measures.
 * @param capacity Available bytes, including the terminator.
 * @param required Required byte count INCLUDING the terminator.
 * @return HAL_OK, HAL_EOVERFLOW (full required size, buffer unchanged),
 * HAL_EINVAL, HAL_EPROTO, HAL_ENOMEM or HAL_EIO. Other errors preserve required.
 */
hal_status_t jb_path_resolve(const char *base, const char *path, char *buffer, size_t capacity,
                             size_t *required);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(sizeof(jb_file_data_t) == 2 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_file_data_t) == sizeof(size_t) &&
                     offsetof(jb_file_data_t, data) == 0 &&
                     offsetof(jb_file_data_t, length) == sizeof(size_t),
                 "jb requires a naturally aligned file buffer");
#endif

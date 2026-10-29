#ifndef JB_FILE_H
#define JB_FILE_H

/** @file file.h
 * Host file operations on UTF-8 paths. Link jb::host (C++17 runtime).
 * Linux and Windows only; host allocations never enter jb::base.
 * Calls are independent and do not change the process working directory.
 * Windows UTF-16 conversion does not add an extended-length path prefix;
 * OS and calling executable manifest limits still apply.
 */
#include "jb/cancel.h"
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

/** Single-owner opaque streaming writer. Release with commit or abort. */
typedef struct jb_file_writer jb_file_writer_t;
/** OS-owned exclusive lock. Do not unlink or replace its lock file. */
typedef struct jb_file_lock jb_file_lock_t;
/** Owned temporary directory. Its path is valid until removal. */
typedef struct jb_temp_dir jb_temp_dir_t;
/** Begin a bounded streaming atomic replacement; the destination stays intact.
 * @param path Required absolute UTF-8 path; missing parents are created.
 * @param max_bytes Maximum total bytes; 0 permits only an empty file.
 * @param out Required pointer to NULL; receives ownership only on success.
 * @return HAL_OK or file API errors, HAL_ESTATE for an occupied output.
 * Final links/reparse points and nonregular destinations are rejected.
 * Parent links follow the filesystem; this is not a sandbox. Single-owner
 * handles are not safe for concurrent calls. Abandonment/process death may
 * leave a temporary sibling; abort/commit are required for normal cleanup.
 * Existing POSIX rwx bits are sampled/copied at begin, not at commit.
 */
hal_status_t jb_file_writer_begin(const char *path, uint64_t max_bytes, jb_file_writer_t **out);

/** Append exact bytes using bounded memory.
 * @param writer Required live owner.
 * @param data Bytes; NULL is allowed only with length 0.
 * @param length Byte count, including NUL if present.
 * @return HAL_OK, HAL_EINVAL, HAL_EOVERFLOW (limit), HAL_ESTATE (failed writer),
 * or I/O errors. Any append error poisons a valid writer, preventing commit.
 */
hal_status_t jb_file_writer_write(jb_file_writer_t *writer, const void *data, size_t length);

/** Flush, compare and publish, consuming and nulling a valid owner on every outcome.
 * @param writer Required pointer to a live writer; NULL owner gives HAL_EINVAL.
 * @param changed Required result; unchanged on error except after a successful
 * rename followed by failed POSIX directory fsync, when it becomes true.
 * @return HAL_OK or file API errors; HAL_ESTATE if an append failed.
 * Identical bytes preserve destination mtime. Cleanup removes the temporary
 * sibling; an OS cleanup failure is reported but can leave that sibling.
 * Like write_atomic, publication does not lock external writers. Invalid
 * arguments do not consume the owner. Replacement uses the POSIX rwx bits
 * sampled at begin; later permission changes on the destination are not copied.
 */
hal_status_t jb_file_writer_commit(jb_file_writer_t **writer, bool *changed);

/** Close and delete the temporary sibling without publishing. Consumes and
 * nulls the owner, even on an OS cleanup error. NULL pointer/owner is allowed.
 * @return HAL_OK or an OS cleanup error; an error may leave the sibling. */
hal_status_t jb_file_writer_abort(jb_file_writer_t **writer);

/** Acquire an exclusive advisory lock on a persistent regular lock file.
 * @param path Required absolute UTF-8 path; its parent must already exist.
 * @param timeout_ms 0 tries once (HAL_EBUSY), UINT32_MAX waits indefinitely,
 * otherwise a monotonic timeout (HAL_ETIMEOUT). Callback time is additional.
 * @param canceled Optional callback polled before opening and while waiting.
 * @param context Passed unchanged to canceled; NULL allowed.
 * @param out Required pointer to NULL, assigned only when locked.
 * @return HAL_OK, HAL_EBUSY, HAL_ETIMEOUT, HAL_ECANCELED or file API errors.
 * The first lock attempt precedes the elapsed-time check; cancellation still
 * takes precedence. Later attempts respect the deadline.
 * New POSIX lock files request mode 0600, restricted by umask; existing
 * permissions are unchanged. File is never truncated or deleted. All cooperating callers
 * must keep the same file identity. Final links/reparse points are rejected.
 * Closing the owner or process exit releases the OS lock. Handles are not
 * inherited across exec/CreateProcess; a raw POSIX fork can inherit the lock.
 * Filesystem/network lock semantics still apply. Same-process independent
 * acquisitions contend as well. Single-owner handle; no concurrent release.
 */
hal_status_t jb_file_lock_acquire(const char *path, uint32_t timeout_ms, jb_cancel_fn canceled,
                                  void *context, jb_file_lock_t **out);
/** Release and null the owner even on a close error. Does not unlink the file.
 * NULL pointer/owner is allowed. @return HAL_OK or an OS close error. */
hal_status_t jb_file_lock_release(jb_file_lock_t **lock);

/** Create a unique owned directory beneath an existing absolute UTF-8 parent.
 * @param parent Required directory; existing parent links are followed.
 * @param out Required pointer to NULL, assigned only on success.
 * @return HAL_OK or file API errors; HAL_ESTATE for an occupied output.
 * POSIX mode is 0700. Windows grants inheritable full access only to the
 * current user through a protected DACL. Privileged OS access is not excluded.
 * Call remove for cleanup; process death does not remove directories.
 */
hal_status_t jb_temp_dir_create(const char *parent, jb_temp_dir_t **out);
/** Borrow the directory's absolute UTF-8 path; NULL owner returns NULL.
 * The caller must not rename/replace the owned root or free/change this string. */
const char *jb_temp_dir_path(const jb_temp_dir_t *directory);
/** Recursively remove owned contents and root without following child symlinks.
 * NULL pointer/owner is allowed. Consumes/nulls the owner only on success;
 * an error retains ownership for retry, though some contents may be removed.
 * @return HAL_OK or a filesystem error. The caller must stop users of the
 * directory first and must not replace entries concurrently with cleanup.
 * This API is not protection against hostile concurrent filesystem changes.
 */
hal_status_t jb_temp_dir_remove(jb_temp_dir_t **directory);

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

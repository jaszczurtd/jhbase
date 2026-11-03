#ifndef JB_FILESYSTEM_H
#define JB_FILESYSTEM_H

/** @file filesystem.h
 * Host filesystem and lexical path operations implemented in C11.
 * Paths are nonempty, NUL-terminated UTF-8. Windows uses UTF-16 Win32 calls;
 * Listing names that are not valid UTF-8 gives HAL_EPROTO. Recursive removal
 * uses native child names, including names that cannot be represented in UTF-8.
 * Calls do not change cwd. Relative paths use the process cwd, so callers
 * must coordinate changes to cwd made elsewhere in the process.
 * Parent symlinks/reparse points follow the OS rules. Removal does not follow
 * the final link or links found while descending. Descending uses opened
 * directories rather than a separate type check followed by a path lookup.
 * This is not a sandbox: callers still coordinate cwd and parent-path changes;
 * concurrent tree edits can cause partial failure.
 * Windows accepts drive paths, UNC paths and native device prefixes, including
 * explicit extended-length paths. Lexical operations parse these prefixes;
 * they do not add one or change the namespace. OS and executable manifest
 * limits apply to filesystem calls.
 * Buffer outputs may reuse an input's storage. The required-size object must
 * not overlap input strings or the output buffer; those overlaps give HAL_EINVAL.
 */
#include "jb/status.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Entry kind. Windows reparse points have LINK kind when not followed,
 * including reparse points other than symbolic links. */
typedef uint32_t jb_fs_kind_t;
/** Values stored in jb_fs_kind_t; their representation is always uint32_t. */
enum { JB_FS_REGULAR = 1, JB_FS_DIRECTORY = 2, JB_FS_LINK = 3, JB_FS_OTHER = 4 };

/** Metadata from one observation. size is the regular-file length in bytes;
 * it is zero for other kinds. This observation does not lock the entry. */
typedef struct {
  jb_fs_kind_t kind;
  uint64_t size;
} jb_fs_info_t;

/** Inspect an entry; use kind for file/directory checks without a second call.
 * @param path Required UTF-8 path.
 * @param follow_symlinks Follow the final link/reparse point when true.
 * When false, trailing separators are ignored to inspect the link itself.
 * @param out Required metadata, assigned only on success.
 * @return HAL_OK, HAL_ENOENT (including dangling links when followed),
 * HAL_EINVAL, HAL_EPROTO, HAL_EPERM, HAL_ENOMEM, HAL_EOVERFLOW or HAL_EIO.
 */
hal_status_t jb_fs_status(const char *path, bool follow_symlinks, jb_fs_info_t *out);

/** Get the process working directory as an absolute UTF-8 path.
 * @param buffer Output, NUL-terminated on success; (NULL,0) measures.
 * @param capacity Available bytes, including the terminator.
 * @param required Required result INCLUDING the terminator, also on overflow.
 * @return HAL_OK, HAL_EOVERFLOW or argument/encoding/allocation/I/O errors.
 * An undersized buffer is unchanged. Other errors preserve both outputs.
 * The directory can change between a sizing call and a later call.
 */
hal_status_t jb_fs_cwd(char *buffer, size_t capacity, size_t *required);

/** Test native absolute-path syntax, without accessing the filesystem.
 * @param path Required UTF-8 path. Windows root-relative paths (\foo) and
 * drive-relative paths (C:foo) are not absolute.
 * @param absolute Required result, unchanged on error.
 * @return HAL_OK, HAL_EINVAL, HAL_EPROTO or HAL_EUNSUPPORTED.
 */
hal_status_t jb_fs_path_is_absolute(const char *path, bool *absolute);

/** Normalize separators, dot and dot-dot components lexically.
 * No symlinks are resolved. Relative leading dot-dot components remain.
 * @param path Required UTF-8 path; Windows drive-relative paths stay relative.
 * @param buffer Output, or NULL with capacity 0 to measure.
 * @param capacity Available bytes including NUL.
 * @param required Required size including NUL, also on HAL_EOVERFLOW.
 * @return HAL_OK or argument/encoding/allocation/overflow errors.
 * An undersized buffer is unchanged; other failures preserve both outputs.
 */
hal_status_t jb_fs_path_normalize(const char *path, char *buffer, size_t capacity,
                                  size_t *required);

/** Join paths with native root rules, then normalize lexically.
 * An absolute path replaces base. On Windows a different drive also replaces
 * base; a root-relative path retains base's root name. A same-drive relative
 * path appends its relative part. This function does not require an absolute
 * base; use jb_path_resolve from jb/file.h for that stricter operation.
 * @param base Required nonempty UTF-8 path.
 * @param path Required nonempty UTF-8 path.
 * @param buffer Output, or NULL with capacity 0 to measure.
 * @param capacity Available bytes including NUL.
 * @param required Required size including NUL, also on HAL_EOVERFLOW.
 * @return HAL_OK or argument/encoding/allocation/overflow errors.
 * Output rules match jb_fs_path_normalize.
 */
hal_status_t jb_fs_path_join(const char *base, const char *path, char *buffer, size_t capacity,
                             size_t *required);

/** Extract the final component without resolving dot or dot-dot components.
 * A root or path ending in a separator has an empty filename.
 * @param path Required UTF-8 path.
 * @param buffer Output, or NULL with capacity 0 to measure.
 * @param capacity Available bytes including NUL.
 * @param required Required size including NUL, also on HAL_EOVERFLOW.
 * @return HAL_OK or argument/encoding/allocation/overflow errors.
 * Output rules match jb_fs_path_normalize.
 */
hal_status_t jb_fs_path_filename(const char *path, char *buffer, size_t capacity, size_t *required);

/** Extract the parent without resolving dot or dot-dot components.
 * A root is its own parent; a single relative filename has an empty parent.
 * Windows separators in the result use backslashes.
 * @param path Required UTF-8 path.
 * @param buffer Output, or NULL with capacity 0 to measure.
 * @param capacity Available bytes including NUL.
 * @param required Required size including NUL, also on HAL_EOVERFLOW.
 * @return HAL_OK or argument/encoding/allocation/overflow errors.
 * Output rules match jb_fs_path_normalize.
 */
hal_status_t jb_fs_path_parent(const char *path, char *buffer, size_t capacity, size_t *required);

/** Resolve an existing path through the filesystem, following links.
 * All components must exist. The result is absolute; no missing suffix is
 * appended. Windows preserves the extended-length prefix returned by the OS.
 * On HAL_EOVERFLOW caused by an OS path limit, required is zero because the
 * resolved result is unavailable; a larger output buffer cannot fix that error.
 * @param path Required UTF-8 path.
 * @param buffer Output, or NULL with capacity 0 to measure.
 * @param capacity Available bytes including NUL.
 * @param required Required size including NUL, also on HAL_EOVERFLOW.
 * @return HAL_OK, HAL_ENOENT or argument/encoding/allocation/overflow/I/O errors.
 * Output rules match jb_fs_path_normalize.
 */
hal_status_t jb_fs_realpath(const char *path, char *buffer, size_t capacity, size_t *required);

/** Called once per directory entry, excluding dot and dot-dot.
 * name and info are borrowed only for this call. name is the UTF-8 filename,
 * not a full path. Metadata does not follow the final link. Return HAL_OK to
 * continue; any other status stops traversal and is returned unchanged.
 * Callbacks must not throw across the C API. */
typedef hal_status_t (*jb_fs_entry_fn)(void *context, const char *name, const jb_fs_info_t *info);

/** List immediate directory entries in unspecified native order.
 * @param path Required UTF-8 directory path; a final link is followed.
 * @param entry Required callback; metadata errors stop before its call.
 * @param context Passed unchanged to entry; NULL allowed.
 * @return HAL_OK, the callback's status, or a filesystem/encoding error.
 * A concurrent edit can make an entry disappear or change during the call.
 */
hal_status_t jb_fs_list(const char *path, jb_fs_entry_fn entry, void *context);

/** Create a directory, optionally creating missing parent directories.
 * Existing directories (including links to directories) succeed. Other
 * existing entries give HAL_EEXIST. POSIX requests mode 0777 restricted by
 * umask; Windows uses inherited security. Parent creation may be partial on error.
 * @param path Required UTF-8 path.
 * @param parents Create missing parents when true.
 * @return HAL_OK or a filesystem/argument/encoding/allocation error.
 */
hal_status_t jb_fs_mkdir(const char *path, bool parents);

/** Remove one entry or recursively remove a directory and its contents.
 * Final links/reparse points are removed without following their targets,
 * even with trailing separators. Recursive traversal likewise skips link
 * targets. Missing paths give HAL_ENOENT; a nonempty directory without
 * recursive gives HAL_EBUSY. Roots and final dot/dot-dot are rejected.
 * A trailing separator on a regular file gives HAL_ETYPE. POSIX descendants
 * use directory descriptors and no-follow opens, including beyond PATH_MAX.
 * Windows pins opened entries against replacement/write opens, uses literal
 * extended paths for children and deletes through handles. Read-only attributes
 * are cleared for deletion; if deletion fails, restoration is attempted.
 * Existing incompatible handles can cause HAL_EBUSY. Traversal uses memory
 * and open handles proportional to directory depth and can exhaust either.
 * Windows device paths must name a drive, UNC share, or Volume{GUID}; other
 * device namespaces give HAL_EUNSUPPORTED. Their lexical parsing remains available.
 * A final component made only of dots/spaces is rejected on Windows unless
 * the input starts with the literal prefix \\?\ (backslashes). Win32 otherwise
 * folds those names to a parent or root. Exact dot and dot-dot are always rejected.
 * Alternate data stream syntax (any colon outside the drive prefix) gives
 * HAL_EUNSUPPORTED, including in the literal namespace; directory stream names
 * can otherwise alias a directory or volume root.
 * @param path Required UTF-8 path.
 * @param recursive Descend into ordinary directories when true.
 * @return HAL_OK or a filesystem/argument/encoding/allocation error.
 * Failure can leave a partially removed directory. No rollback is attempted.
 */
hal_status_t jb_fs_remove(const char *path, bool recursive);

/** Rename an entry, replacing an existing regular destination file.
 * An existing directory or link/reparse destination is rejected. Renaming
 * an entry to itself succeeds; distinct hard links to it remain unchanged.
 * Windows applies a case-only change to the same entry's name.
 * Cross-filesystem moves are unsupported;
 * no copy/delete fallback occurs. Source and destination parents must exist.
 * @param from Required UTF-8 source path.
 * @param to Required UTF-8 destination path.
 * @return HAL_OK or a filesystem/argument/encoding/allocation error.
 * Callers must prevent concurrent replacement of the destination.
 */
hal_status_t jb_fs_rename(const char *from, const char *to);

/** Compare filesystem identity, following links to existing entries.
 * @param left Required UTF-8 path.
 * @param right Required UTF-8 path.
 * @param equivalent Required result, assigned only on success.
 * @return HAL_OK, HAL_ENOENT or a filesystem/argument/encoding error.
 */
hal_status_t jb_fs_equivalent(const char *left, const char *right, bool *equivalent);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(JB_ALIGNOF(jb_fs_info_t) == JB_ALIGNOF(uint64_t) &&
                     offsetof(jb_fs_info_t, kind) == 0 &&
                     offsetof(jb_fs_info_t, size) % JB_ALIGNOF(uint64_t) == 0 &&
                     sizeof(jb_fs_info_t) == offsetof(jb_fs_info_t, size) + sizeof(uint64_t),
                 "jb requires naturally aligned filesystem metadata");
#endif

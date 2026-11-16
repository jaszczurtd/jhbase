#ifndef JB_ARCHIVE_H
#define JB_ARCHIVE_H

/** @file archive.h
 * Extraction of ZIP and gzip-compressed TAR archives on Linux and Windows.
 * Enable JHBASE_HOST and JHBASE_ARCHIVE, then link jb::archive; it builds the
 * pinned libarchive and zlib sources. The caller names the format; no other
 * format, compression filter or external decompression program is accepted.
 *
 * Entries are written into an existing empty directory with exclusive
 * creation, so an entry never replaces another one or a file that appears
 * in the directory. Writes never pass through a link: symbolic links are
 * created after all other entries, and every parent component is a
 * directory created by the same call. This is not a sandbox. The destination
 * must be a private directory that nothing else changes during the call;
 * parent links of the destination path follow the OS rules.
 * Calls do not change cwd and do not restore owners, times, ACLs or extended
 * attributes. Independent calls may run concurrently. Files are not flushed
 * to stable storage; the caller decides about durability before using them.
 */
#include "jb/cancel.h"
#include "jb/status.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Archive format selected by the caller; its representation is always uint32_t. */
typedef uint32_t jb_archive_format_t;
/** Values stored in jb_archive_format_t. ZIP reads the central directory. */
enum { JB_ARCHIVE_ZIP = 1, JB_ARCHIVE_TAR_GZIP = 2 };

/** Byte sizes of the diagnostic text fields in jb_archive_result_t. */
enum { JB_ARCHIVE_NAME_CAPACITY = 1024, JB_ARCHIVE_MESSAGE_CAPACITY = 256 };

/** Zero-initialize before setting fields. Limits are hard limits and are also
 * counted while reading, so wrong sizes in headers cannot bypass them.
 * A limit of 0 allows nothing of that kind. */
typedef struct {
  uint64_t max_entries;     /**< Headers read, including directories and excluded entries. */
  uint64_t max_total_bytes; /**< Bytes written to all regular files together. */
  uint64_t max_file_bytes;  /**< Bytes written to one regular file. */
  const char *const
      *exclusions;       /**< NULL-terminated exact entry names to skip, or NULL. Backslashes in
                            entry names compare as '/'. Every name must match an entry, else
                            HAL_ENOENT after the last entry. Children of an excluded directory
                            are not excluded. Excluded entries are not validated. */
  jb_cancel_fn canceled; /**< Polled before each entry and data block, and before each block of
                            a tar.gz read after the TAR end; NULL disables it. */
  void *context;         /**< Passed unchanged to canceled; NULL allowed. */
  jb_archive_format_t format; /**< Required: JB_ARCHIVE_ZIP or JB_ARCHIVE_TAR_GZIP. */
} jb_archive_options_t;

/** Always cleared first by jb_archive_extract when non-NULL. */
typedef struct {
  uint64_t entries;                     /**< Headers read, including the failing one. */
  uint64_t bytes;                       /**< Bytes written to regular files, including bytes a
                                           short write stored before a failure. */
  uint32_t native_error;                /**< errno/GetLastError of a failed OS call, or libarchive's
                                           error number; otherwise 0. */
  char entry[JB_ARCHIVE_NAME_CAPACITY]; /**< UTF-8 name of the entry being processed when
                                           the call failed, truncated at a character
                                           boundary; empty on success or before entries. */
  char message[JB_ARCHIVE_MESSAGE_CAPACITY]; /**< Short UTF-8 reason for a failure, from
                                                jhbase or libarchive; empty on success. */
} jb_archive_result_t;

/** Extract every entry of a ZIP or tar.gz archive into an empty directory.
 *
 * Entry names must be UTF-8. Both '/' and '\\' separate components; empty
 * and "." components are ignored. A ".." component, a leading separator or a
 * drive prefix gives HAL_EPERM. On Windows a component containing ':' gives
 * HAL_EPERM; reserved device names (CON, NUL, COM1 and similar, also with an
 * extension), the characters < > " | ? *, control characters and a trailing
 * dot or space give HAL_EUNSUPPORTED. Entry names and link targets are
 * limited to 4096 bytes (HAL_EOVERFLOW).
 *
 * Directories, regular files, symbolic links and TAR hard links are created;
 * other entry types and encrypted entries give HAL_EUNSUPPORTED. A directory
 * entry may repeat an existing directory; any other repeated name gives
 * HAL_EEXIST, also when the filesystem treats two names as one (case or
 * Windows short names). A parent component that is a file gives HAL_EEXIST,
 * one that is a link gives HAL_EPERM.
 *
 * POSIX files get mode 0666 plus the execute bits of the entry, restricted by
 * the umask; directories get 0777 restricted by the umask. Windows ignores
 * modes. A symbolic link target must be relative and may start with ".."
 * components only; their number must not exceed the depth of the link's
 * directory, and no ".." may follow a name (HAL_EPERM). Separators in the
 * target become native ones. On Windows a link becomes a directory link when
 * the destination filesystem resolves its target to a directory, with the
 * filesystem's own case and short-name rules; other links, also dangling
 * ones, are file links. Windows needs Developer Mode or the symbolic link
 * privilege (otherwise HAL_EPERM). A hard link must name a regular file
 * extracted earlier by this call (HAL_ENOENT otherwise) and carry no data
 * (HAL_EUNSUPPORTED).
 *
 * A tar.gz is read to its end. Every gzip member must match the CRC-32 and
 * length in its trailer, and nothing may follow the last member (HAL_EPROTO);
 * the pinned libarchive does not check trailers, so jhbase does.
 *
 * On Linux the calling thread uses the C.UTF-8 character type locale during
 * the call, also while canceled runs; the previous locale is restored before
 * return. A missing C.UTF-8 locale gives HAL_EUNSUPPORTED.
 *
 * @param archive Required UTF-8 path of a regular file, relative to cwd or absolute.
 * @param destination Required absolute UTF-8 path of an existing empty directory;
 * a final link gives HAL_ETYPE, a nonempty directory HAL_EEXIST.
 * @param options Required options; an unknown format gives HAL_EINVAL.
 * @param result Required diagnostics, cleared before any other check.
 * @return HAL_OK; HAL_EINVAL for arguments; HAL_ETYPE for a nonregular archive or a
 * destination that is not a directory; HAL_EPROTO for malformed data, a different
 * format or compression, damaged data (CRC, gzip trailers) or names that are not UTF-8;
 * HAL_EOVERFLOW for a limit; HAL_ECANCELED; HAL_ENOMEM; HAL_ENOENT, HAL_EPERM,
 * HAL_EEXIST or HAL_EUNSUPPORTED as described above; HAL_EIO or another
 * filesystem status for a failed OS call. Failure can leave a partial tree in
 * destination; remove it. Nothing is rolled back.
 */
hal_status_t jb_archive_extract(const char *archive, const char *destination,
                                const jb_archive_options_t *options, jb_archive_result_t *result);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(offsetof(jb_archive_options_t, max_entries) == 0 &&
                     offsetof(jb_archive_options_t, exclusions) == 3 * sizeof(uint64_t) &&
                     offsetof(jb_archive_options_t, canceled) ==
                         3 * sizeof(uint64_t) + sizeof(void *) &&
                     offsetof(jb_archive_options_t, format) ==
                         3 * sizeof(uint64_t) + 3 * sizeof(void *),
                 "jb requires naturally aligned archive options");
JB_STATIC_ASSERT(offsetof(jb_archive_result_t, bytes) == sizeof(uint64_t) &&
                     offsetof(jb_archive_result_t, native_error) == 2 * sizeof(uint64_t) &&
                     offsetof(jb_archive_result_t, entry) ==
                         2 * sizeof(uint64_t) + sizeof(uint32_t) &&
                     offsetof(jb_archive_result_t, message) ==
                         2 * sizeof(uint64_t) + sizeof(uint32_t) + JB_ARCHIVE_NAME_CAPACITY,
                 "jb requires naturally aligned archive diagnostics");
#endif

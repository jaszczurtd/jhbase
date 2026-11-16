# Archive extraction

`jb/archive.h` extracts ZIP and gzip-compressed TAR archives into an empty
directory on Linux and Windows. Enable `JHBASE_HOST=ON` and
`JHBASE_ARCHIVE=ON`, prepare the pinned libarchive and zlib checkouts
described in [dependencies](dependencies.md#optional-archive-extraction), then
link `jb::archive`. The module is written in C11 and allocates from the heap;
it serves host tools, not firmware.

## Calling the extractor

`jb_archive_extract(archive, destination, &options, &result)` reads one
archive file and writes its entries below `destination`:

- `archive` is the UTF-8 path of a regular file, relative to the working
  directory or absolute.
- `destination` is the absolute UTF-8 path of an existing, empty directory.
  Use a private staging directory, for example from `jb_temp_dir_create`,
  and publish its contents only after `HAL_OK`. A failed call can leave a
  partial tree; remove the whole directory then.
- `options.format` selects `JB_ARCHIVE_ZIP` or `JB_ARCHIVE_TAR_GZIP`. The
  module does not guess. A tar.gz passed as ZIP, an uncompressed TAR passed
  as tar.gz or any other format fails with `HAL_EPROTO`. ZIP entries come
  from the central directory. A tar.gz is read to its end, and every gzip
  member must match the CRC-32 and length in its trailer.
- `max_entries`, `max_total_bytes` and `max_file_bytes` are hard limits;
  0 allows nothing of that kind. Sizes declared in headers are checked before
  a file is created, and the data is counted again while it is read. The
  pinned libarchive also refuses data larger than its header declares.
- `exclusions` is a NULL-terminated list of exact entry names with `/`
  separators; a backslash in an entry name compares as `/`. Every name must
  match an entry, otherwise the call ends with `HAL_ENOENT` after the last
  entry and names the exclusion in `result.entry`. Excluded entries skip all
  name checks, which suits files the host cannot store. Children of an
  excluded directory are still extracted.
- `canceled` is polled before each entry and each data block, and before
  each block of a tar.gz read after the TAR end; `true` gives `HAL_ECANCELED`.

```c
const char *const skip[] = {"[Content_Types].xml", NULL};
jb_archive_options_t options;
memset(&options, 0, sizeof(options));
options.format = JB_ARCHIVE_ZIP;
options.max_entries = 20000;
options.max_total_bytes = UINT64_C(1) << 30;
options.max_file_bytes = UINT64_C(256) << 20;
options.exclusions = skip;
jb_archive_result_t result;
const hal_status_t status = jb_archive_extract("package.nupkg", staging, &options, &result);
if (status != HAL_OK)
  fprintf(stderr, "extraction failed (%d): %s %s\n", (int)status, result.message, result.entry);
```

## What the destination receives

Entry names must be UTF-8. Both `/` and `\` separate components, so an
archive gives the same tree on Linux and Windows. Empty and `.` components
are ignored. A `..` component, a leading separator or a drive prefix such as
`C:` gives `HAL_EPERM`, even when the path would stay inside. Names and link
targets longer than 4096 bytes give `HAL_EOVERFLOW`.

On Windows a component with `:` gives `HAL_EPERM`, because Win32 reads it as
a drive or an alternate data stream. Device names (`CON`, `PRN`, `AUX`,
`NUL`, `COM0`-`COM9`, `LPT0`-`LPT9`, their superscript forms, `CONIN$`,
`CONOUT$`), also with an extension, the characters `< > " | ? *`, control
characters and a trailing dot or space give `HAL_EUNSUPPORTED`. Linux accepts
these names.

The module creates directories, regular files, symbolic links and TAR hard
links. Device nodes, FIFOs, sockets and encrypted entries give
`HAL_EUNSUPPORTED`. Every file and directory is created exclusively, so a
second entry with the same name gives `HAL_EEXIST` instead of replacing the
first one. Only a directory may repeat. The same status reports names that
the filesystem treats as one, such as `Dir` and `dir` on NTFS or a Windows
short name; `result.message` then says the name collides with an existing
entry, while a name repeated in the archive says so.

Writes never pass through a link. Every parent component must be a directory
created by the same call: a parent that is a file gives `HAL_EEXIST`, one
that is a link gives `HAL_EPERM`. Symbolic links are created after all other
entries. A link target must be relative and may start with `..` components
only; their number must not exceed the depth of the link's directory, and no
`..` may follow a name (`HAL_EPERM`). Under these rules every link and every
chain of links stays inside the destination. Separators in targets become
native ones. A hard link must name a regular file extracted earlier by the
same call (`HAL_ENOENT` otherwise) and must not carry data
(`HAL_EUNSUPPORTED`).

On Linux a file gets mode 0666 plus the execute bits of the entry and a
directory gets 0777, both restricted by the umask. Owners, times, ACLs and
extended attributes are not restored. Files are not flushed to stable
storage; decide about durability before publishing them.

## Results and errors

`result` is cleared before any check. `entries` counts headers read,
including excluded entries and the one that failed. `bytes` counts bytes
written to regular files, also those a short write stored before a failure.
On failure `entry` holds the name being processed
(truncated at a character boundary, empty when the archive did not provide a
readable name) and `message` a short reason from jhbase or libarchive.
`native_error` holds `errno`, `GetLastError()` or libarchive's error number.
On success both texts are empty.

| Status | Meaning |
| --- | --- |
| `HAL_EINVAL` | Missing argument, unknown format, empty exclusion, relative destination. |
| `HAL_ETYPE` | The archive is not a regular file or the destination is not a directory. |
| `HAL_EEXIST` | The destination is not empty, or names repeat or collide. |
| `HAL_EPROTO` | Malformed or damaged data (including CRC and gzip trailers), data after the last gzip member, another format or compression, a name that is not UTF-8. |
| `HAL_EPERM` | An entry or link would leave the destination or pass through a link; a Windows link without the needed privilege. |
| `HAL_EUNSUPPORTED` | Entry type, encryption, hard link data, or a name Windows cannot store. |
| `HAL_EOVERFLOW` | A limit or the 4096-byte text limit. |
| `HAL_ENOENT` | Missing archive or destination, a hard link target or an unmatched exclusion. |
| `HAL_ECANCELED`, `HAL_ENOMEM`, `HAL_EIO` | Cancellation, memory, or an OS error reported in `native_error`. |

## Platform details

libarchive converts names according to the character type locale. On Linux
the calling thread therefore uses `C.UTF-8` during the call, also while the
`canceled` callback runs, and gets its previous locale back before return.
A system without `C.UTF-8` gives `HAL_EUNSUPPORTED`. Other threads keep
their locale.

Windows paths use UTF-16 without an extended-length prefix, so the usual
path length limits apply. Creating symbolic links needs Developer Mode or the
symbolic link privilege; without them an archive with links fails with
`HAL_EPERM`. A link becomes a directory link when the destination
filesystem resolves its target to a directory, with the filesystem's own
case and short-name rules; other links, also dangling ones, are file links.

The module is not a sandbox. It relies on a private destination that no
other process changes during the call; parent links of the destination path
follow the OS rules. Independent calls can run concurrently.

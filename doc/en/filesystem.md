# Filesystem operations in C

`jb/filesystem.h` provides directory operations, metadata and UTF-8 paths on
Linux and Windows. Enable `JHBASE_HOST=ON` and link `jb::filesystem`. This
target is implemented in C11 and does not require the C++ runtime.
`jb::host` also links it for existing file and process consumers.

Paths are nonempty, NUL-terminated UTF-8. The implementation uses POSIX on
Linux and UTF-16 Win32 functions on Windows. Invalid encoding gives
`HAL_EPROTO`, including a listed directory entry with an invalid UTF-8 name.
Recursive removal uses native child names and can remove such entries.
Relative paths use the process working directory; operations never change it.

## Paths

`jb_fs_path_normalize`, `jb_fs_path_join`, `jb_fs_path_parent` and
`jb_fs_path_filename` process native path syntax without opening files.
Windows drives and UNC follow Windows rules. `jb_path_resolve` from
`jb/file.h` uses the same implementation but requires an absolute base
and rejects drive-relative Windows input.
On POSIX it preserves a base made entirely of slashes, such as `//`.
The parent-path result uses native separators, including backslashes on Windows.

`jb_fs_cwd` reads the working directory. `jb_fs_realpath` resolves an
existing path through links; every component must exist. Windows keeps the
extended prefix returned by the OS. Lexical operations do not add that prefix;
the OS and executable manifest determine accepted path lengths.

For output paths, `(NULL, 0)` measures the required size including NUL.
A short buffer gives `HAL_EOVERFLOW`, reports the required size and preserves
the buffer. Other failures preserve both outputs. Paths can change between
the sizing call and a subsequent operation.
If `jb_fs_realpath` reports an OS path-length limit, it sets `required` to zero;
the result could not be resolved, so increasing the output buffer will not help.
The output buffer may reuse an input's storage. The required-size object
must not overlap an input string or the output buffer; these overlaps give
`HAL_EINVAL`.

## Entries and directories

`jb_fs_status` returns kind and regular-file size, with an explicit choice
of whether to follow the final link. `jb_fs_list` reports immediate entries
except `.` and `..` in unspecified native order. The callback borrows each
name and metadata only during its call; a callback error stops enumeration.
Metadata does not follow the final link.
With `follow_symlinks=false`, status also ignores trailing separators to
inspect the link itself.

`jb_fs_mkdir` can create missing parents. `jb_fs_remove` can descend into
directories, but removes symbolic links and Windows reparse points without
entering their targets, including input with trailing separators. Roots and
final `.` or `..` are rejected. A trailing separator on a regular file gives
`HAL_ETYPE`. Errors can leave a partly created or removed tree.

Removal opens POSIX directories with `openat(O_NOFOLLOW)` and uses `unlinkat`
relative to held descriptors. Windows holds entries against replacement and
write opens, descends using literal extended paths and deletes through handles.
It clears read-only attributes before deletion and attempts to restore them
if deletion fails. Distinct names such as `x` and `x. ` remain distinct while
descending. Memory and open handles grow with directory depth.
These operations do not provide filesystem isolation: callers still coordinate
cwd and parent-path changes, and concurrent tree edits can cause partial failure.

On Windows, removal accepts drive paths, UNC paths and device-prefixed
drive, UNC or `Volume{GUID}` paths. It rejects volume and share roots
before accessing them. Other device namespaces give `HAL_EUNSUPPORTED`
for removal; the lexical path functions still parse their native syntax.
Removal also rejects a final name containing only dots and spaces, which
Win32 can interpret as a parent or root. An exact `\\?\` prefix preserves
literal names and permits those entries; exact `.` and `..` remain rejected.
Removal does not support alternate data streams: a colon outside the drive
prefix gives `HAL_EUNSUPPORTED`, also with the literal prefix. Directory
stream names can otherwise identify a directory or volume root.

`jb_fs_rename` can replace a regular file, but rejects an existing directory
or link at the destination. A case-only rename updates the name on Windows;
distinct hard links to the same file remain unchanged. Cross-filesystem moves have no copy/delete
fallback. `jb_fs_equivalent` compares existing entries by identity after
following links.

Use [the existing file API](host.md) for bounded reads, atomic publication,
streaming writes, locks and private temporary directories. Those operations
retain their `jb/file.h` API and link through `jb::host`.

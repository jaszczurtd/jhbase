# Host files

The `jb/file.h` API reads files, resolves UTF-8 paths and publishes complete
output files on Linux and Windows. Enable `JHBASE_HOST=ON` and link `jb::host`.
The implementation requires a C++17 compiler and runtime; its public functions
can be called from C11. Host allocations and system headers stay outside
`jb::base`. The option is OFF by default and requires a Linux or Windows target.

```sh
cmake -S . -B .build/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DJHBASE_HOST=ON
cmake --build .build/host
ctest --test-dir .build/host --output-on-failure
```

Install normally with `cmake --install`. The exported package provides
`jb::host` when built with this option; a C application also needs the C++ linker
and runtime. The status-only package carries no host code.

## Reading and paths

`jb_file_read(path, max_bytes, out)` reads a regular file as exact bytes,
including NUL and CRLF. Initialize `jb_file_data_t` to zero and release it with
`jb_file_release` before reuse. Errors preserve an empty output; an occupied
output gives `HAL_ESTATE`. The size limit is checked before allocation and
during reading, including a growing file. A read follows existing links;
nonregular files give `HAL_ETYPE`. `jb_file_release(NULL)` is allowed.

`jb_path_resolve(base, path, buffer, capacity, required)` requires an explicit
absolute base. It joins and normalizes path components without looking up
links or changing cwd. Paths use host filesystem rules and UTF-8; Windows
converts them to UTF-16 paths for the system API. Drive-relative Windows paths
are rejected. `(NULL,0)` measures the result including its NUL terminator. A short buffer
gives `HAL_EOVERFLOW`, reports the full size and leaves every byte untouched.

## Comparing and publishing

`jb_file_write_atomic(path, data, length, check_only, changed)` first compares
the entire contents byte for byte. Identical files retain their mtime. Check mode sets `changed` for
a missing or different output and creates no directories or temporary files.
The caller decides whether that difference should cause a failing check.

A write creates missing parents, then an exclusive temporary sibling, writes
all bytes, flushes and closes it before replacement. POSIX also flushes the
parent directory; Windows uses `FlushFileBuffers` and
`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`. Failures remove the temporary
file. New POSIX files have private permissions; replacing a regular file
preserves its rwx permission bits. Final symlinks, reparse points and
nonregular destinations are rejected. Parent directories follow existing
links, so this API does not confine untrusted paths to a directory.

Calls do not lock external editors. With concurrent writers, the last
successful replacement wins. Errors preserve `changed`, except a POSIX
directory flush failure after replacement: `changed=true` then means the new
bytes are visible but their durability was not confirmed. An error does not
roll back newly created parent directories. Filesystem and sharing limits
still apply.

Functions that can fail return the shared `hal_status_t`. Invalid UTF-8 gives
`HAL_EPROTO`, allocation failure `HAL_ENOMEM`, file limits `HAL_EOVERFLOW`,
missing input `HAL_ENOENT` and access denial `HAL_EPERM`; other I/O failures
give `HAL_EIO` or `HAL_EBUSY`. A held Windows replacement target can also
report `HAL_EPERM` when the OS returns access denied. C++ exceptions are caught
at each C API boundary.

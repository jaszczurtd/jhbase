# Host files and processes

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

## Child processes

`jb/process.h` provides synchronous `jb_process_run(options, result)` through
`jb::host`. Set an absolute executable path and working directory. Arguments
are a NULL-terminated list excluding `argv[0]`; NULL means no arguments.
The API never invokes a shell or searches PATH. Windows converts UTF-8 to
UTF-16 and quotes arguments for the Microsoft C runtime. Programs using a
custom command-line parser may interpret those arguments differently.

An environment pointer of NULL inherits the parent's environment. A non-NULL
list replaces it completely; `{NULL}` supplies an empty environment. Entries
use `NAME=VALUE` and names must be unique, ignoring case on Windows. The call
does not change parent cwd or environment. Do not mutate the parent environment
concurrently with launch. Child stdin reads EOF.

Both output pipes are drained fairly with bounded buffers. An optional output
callback receives binary chunks on the calling thread, with stream identity.
Bytes remain ordered within each stream; the API promises no ordering between
stdout and stderr. A NULL callback discards output while still draining it.
Return a status other than `HAL_OK` to stop the call with that status.
Callbacks borrow their buffers, must return promptly and must not throw.

`timeout_ms` uses monotonic elapsed time from before launch; zero waits
indefinitely. An optional cancellation callback is polled before launch and
while waiting. Returning true gives `HAL_ECANCELED`. Another thread may request
cancellation through caller-owned atomic state. OS startup, cleanup and callback
execution are outside any hard real-time guarantee. Independent calls can run
concurrently; contexts and callback synchronization belong to the caller.

The result is initialized even on failure. `HAL_OK` means the process was
collected and output drained, regardless of its exit code. Inspect `exit_code`
and, on Linux, `term_signal`. Windows also places OS-forced termination in
`exit_code`; its numeric value is not an application result on a stopped call. `started` can be true for a Linux exec failure,
which remains an API error with `native_error` rather than an exit-code 127
success. Invalid input and callback errors have no native OS error code.
A timeout or cancellation chosen before exit is observed remains the API status
if the child exits naturally during cleanup. Thus `HAL_ETIMEOUT` with `exited=true`
and Linux `term_signal=0` is valid; the exit fields describe the collected child,
not whether it met the budget. A group cleanup error also retains the collected
main-process result.

Each child owns a Linux process group or Windows Job Object. Timeout,
cancellation, callback errors and I/O failures force termination and collect
the main process. Remaining owned descendants are also terminated when the
main process exits normally, so inherited output pipes cannot stall completion.
Once exit is observed, the API snapshots and drains the queued output before
considering a timeout or cancellation. Later writes from other processes are
excluded. This drain may finish after the deadline; output callback errors still
stop immediately. Output remaining on a stopped call may be discarded. This API therefore does
not launch persistent background services.

Linux descendants that deliberately change their group/session escape ownership;
the API does not provide sandboxing. Windows assigns the suspended child to
its job before allowing it to run and disallows breakaway. Other inherited
handles/descriptors are excluded. Grandchildren are reaped by their OS parent.
On Linux, do not ignore SIGCHLD, set `SA_NOCLDWAIT`, or reap this child elsewhere.

## Streaming atomic output

`jb_file_writer_begin(path, max_bytes, out)` creates a temporary sibling for an
absolute destination path, creating missing parents. Initialize the opaque owner
to NULL. `jb_file_writer_write` appends exact bytes with bounded memory; the
limit covers their total length and zero permits only an empty file. A failed
append poisons the writer, so a later commit cannot publish incomplete output.

`jb_file_writer_commit` flushes the file, compares both files in chunks and
preserves destination mtime when their contents match. Otherwise it uses the
same publication and durability helpers as `jb_file_write_atomic`, including
POSIX permission preservation. Existing rwx bits are sampled and applied at
`begin`; a later permission change on the destination is not copied at commit. `jb_file_writer_abort` closes and deletes the
sibling without publishing. Both consume and null a valid owner even on an OS
error; invalid commit arguments leave ownership intact. An OS cleanup failure
can leave the sibling on disk. Process death can also leave it behind.

A commit reports `changed` with the same rules as the one-shot API, including
publication followed by a failed POSIX directory fsync. Both APIs reject final
links/reparse points and nonregular destinations, follow parent links and do
not lock external writers. Handles have one owner and require caller-side
serialization. These APIs do not confine hostile filesystem activity.

## File locks

`jb_file_lock_acquire(path, timeout_ms, canceled, context, out)` opens a
persistent regular file without truncation and acquires an exclusive OS lock.
The UTF-8 path must be absolute and its parent must exist. Final links/reparse
points are rejected. Initialize the owner to NULL; an occupied output gives
`HAL_ESTATE`. Linux uses `flock`, Windows locks the first byte with `LockFileEx`.
Independent acquisitions contend even within the same process. A new POSIX lock
file requests mode 0600 subject to umask; opening an existing file keeps its mode.

Zero timeout tries once and returns `HAL_EBUSY` on contention; `UINT32_MAX`
waits indefinitely. Other values use a monotonic timeout and give `HAL_ETIMEOUT`.
The first lock attempt runs before checking elapsed time; cancellation still
takes precedence over an attempt. Later attempts respect the deadline.
The optional cancellation callback runs on the waiting thread before opening
and while waiting; true gives `HAL_ECANCELED`. It must return promptly without
throwing. The context may be NULL; cross-thread requests require atomic state.

`jb_file_lock_release` consumes and nulls the owner, including on a close error.
NULL pointer/owner is allowed. OS process termination also releases the lock.
Lock handles are excluded from exec/CreateProcess inheritance; a raw POSIX
fork can retain one. Never unlink or replace the lock file, including after
release: existing waiters may still refer to the old identity. Locks require
cooperating callers and retain the semantics of the underlying filesystem.

## Owned temporary directories

`jb_temp_dir_create(parent, out)` creates a unique directory beneath an existing
absolute UTF-8 parent. Initialize the owner to NULL. POSIX mode is 0700;
Windows uses a protected DACL granting inheritable full access only to the
current user. Privileged OS access remains possible.

`jb_temp_dir_path` borrows the absolute UTF-8 path until removal; NULL returns
NULL. Stop users of the directory before `jb_temp_dir_remove`, which removes
contents and root recursively without following child symlinks. Success consumes
and nulls the owner; failure retains it for retry and can leave partial cleanup.
NULL pointer/owner is allowed. Do not rename/replace the owned root or modify
entries concurrently with cleanup. This API does not defend against hostile
filesystem changes, and process death does not remove the directory.

## Platform details

Both host APIs share `jb_cancel_fn` from `jb/cancel.h`. Linux process launch
inherits the calling thread's signal mask and ignored dispositions; caught signal
handlers reset according to exec rules. The library does not install a parent
SIGINT handler. A CLI wanting orderly Ctrl+C cleanup must translate it into a
cancellation request and let run finish before exiting.

Linux sets `PR_SET_PDEATHSIG(SIGKILL)` before exec and checks the parent PID
after setting it, covering parent process death during setup. This kills only
the direct child when the launching thread dies. The setting is cleared on a
child fork, privileged exec or relevant credential changes; it can also be
changed by the launched program. It does not provide the Windows Job Object's
whole-job cleanup after abrupt caller death. See the
[Linux parent-death signal rules](https://man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html).

Windows launch temporarily suppresses loader error dialogs with
`SetThreadErrorMode` around `CreateProcessW`, then restores the calling thread's
previous mode on success or failure. Invalid executables therefore return an
API error instead of waiting for a dialog in an unattended caller.

Windows converts paths to UTF-16 but does not automatically add `\\?\`.
OS limits and the calling executable's long-path manifest/policy apply;
UTF-8 support alone does not promise long-path support. Explicit extended paths
must follow the rules of each underlying Win32 API. See
[Windows path limits](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation).
Temporary siblings and directories use 128 random bits from the system CNG
provider in their Windows names; exclusive creation still decides ownership.

OS errors are mapped consistently: oversized argument/environment input gives
`HAL_EOVERFLOW`, a nondirectory path component `HAL_ENOENT`, invalid executable
format `HAL_ETYPE`, transient resource exhaustion `HAL_EAGAIN`, and descriptor
exhaustion `HAL_ENOMEM`. A busy executable or a Windows sharing violation gives
`HAL_EBUSY`. The process result retains the original OS error code.

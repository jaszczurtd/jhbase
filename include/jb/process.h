#ifndef JB_PROCESS_H
#define JB_PROCESS_H

/** @file process.h
 * Synchronous Linux/Windows child processes. Enable JHBASE_HOST and link
 * jb::host. UTF-8 inputs are borrowed until the call returns. No shell or
 * PATH lookup is used; stdin reads EOF. Calls do not change the parent's cwd
 * or environment and may run concurrently with independent contexts. Windows
 * launch temporarily suppresses loader error dialogs on the calling thread
 * and restores its prior error mode. Windows paths are converted to UTF-16
 * without adding an extended-length prefix;
 * OS/manifest path limits apply. Linux inherits signal masks and ignored
 * dispositions; handlers caught by the caller reset according to exec rules.
 */
#include "jb/cancel.h"
#include "jb/status.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Output source. Chunks within each stream preserve byte order; ordering
 * between streams is unspecified. Output may contain NUL or invalid UTF-8. */
typedef uint32_t jb_process_stream_t;
/** Standard output stream identifiers. */
enum { JB_PROCESS_STDOUT = 0, JB_PROCESS_STDERR = 1 };

/** Consume borrowed bytes on the calling thread. Return HAL_OK to continue;
 * any other status stops the process group/job and is returned by run.
 * Callbacks must return promptly and must not throw C++ exceptions.
 * No callback is invoked after it requests a stop. */
typedef hal_status_t (*jb_process_output_fn)(void *context, jb_process_stream_t stream,
                                             const unsigned char *data, size_t length);
/** Zero-initialize before setting fields. No process handle survives run. */
typedef struct {
  const char *executable; /**< Required absolute executable path; no shell expansion. */
  const char *const
      *arguments; /**< NULL-terminated arguments excluding argv[0]; NULL means none. */
  const char *working_directory; /**< Required absolute directory for the child. */
  const char *const
      *environment;    /**< NULL inherits; otherwise a complete NULL-terminated NAME=VALUE list.
                          Empty list clears it. Names must be unique (case-insensitive on Windows). */
  uint32_t timeout_ms; /**< Monotonic budget from before launch; 0 waits indefinitely. */
  jb_process_output_fn output; /**< NULL drains and discards both streams with bounded memory. */
  jb_cancel_fn canceled;       /**< NULL disables cancellation polling. */
  void *context;               /**< Passed unchanged to both callbacks; NULL allowed. */
} jb_process_options_t;

/** Always initialized by run when non-NULL, even on error. */
typedef struct {
  bool started;          /**< OS process created; may be true on an exec failure on Linux. */
  bool exited;           /**< Main process exit observed and collected. */
  uint32_t exit_code;    /**< Native exit code when exited and term_signal == 0. On Windows this
                             includes OS-forced termination; use run's status to distinguish it. */
  int32_t term_signal;   /**< Linux terminating signal, or 0 (always 0 on Windows). */
  uint32_t native_error; /**< errno/GetLastError for an OS failure; otherwise 0. */
} jb_process_result_t;

/** Run, drain stdout/stderr fairly, and collect the main process. A nonzero
 * child exit is HAL_OK; inspect result. Returns HAL_EINVAL for invalid paths,
 * arguments or duplicate environment names, HAL_EPROTO for invalid UTF-8,
 * HAL_ENOENT/HAL_EPERM for missing/inaccessible executable or directory,
 * HAL_ENOMEM/HAL_EOVERFLOW for resource limits, HAL_EAGAIN for transient process
 * resource exhaustion, HAL_ETYPE for an invalid executable format,
 * HAL_EBUSY for a busy executable or sharing violation,
 * HAL_ETIMEOUT/HAL_ECANCELED,
 * the output callback's status, or HAL_EIO/HAL_EINTERNAL.
 *
 * On stop/error, terminate the owned process group (Linux) or Job Object
 * (Windows) and collect the main process. Also terminate remaining owned
 * descendants when the main process exits normally; callbacks drain only the
 * output queued when exit is observed; later writes by other processes are
 * excluded. Observed completion wins over pending timeout/cancellation, so
 * draining queued output may finish after the deadline. A stop status remains
 * HAL_ETIMEOUT/HAL_ECANCELED if chosen before exit was observed, even if the
 * child exits naturally during cleanup (exited=true, term_signal=0 on Linux).
 * Cleanup errors also retain the collected main-process result. Output callback errors
 * still stop the call immediately. Partial output may be discarded on stop/error.
 * Linux uses PR_SET_PDEATHSIG(SIGKILL) and checks getppid after setting it:
 * death of the launching thread kills the direct child, not its descendants.
 * The setting is cleared by a child fork and may be cleared by privileged exec
 * or credential changes. Windows job closure covers all job members instead.
 * The caller must use cancellation for graceful SIGINT handling; the library
 * installs no parent signal handlers. Linux descendants that deliberately leave their process group
 * escape ownership; this is not a sandbox. Windows descendants cannot break away from the job.
 * Grandchildren are reaped by their OS parent, not this API. Linux requires the caller not to
 * ignore SIGCHLD, set SA_NOCLDWAIT or reap this child elsewhere. Startup/OS cleanup and callbacks
 * are not hard real-time. Concurrent mutation of the parent environment during launch is
 * unsupported. No C++ exception crosses this C API. options and result must not overlap.
 */
hal_status_t jb_process_run(const jb_process_options_t *options, jb_process_result_t *result);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(sizeof(jb_process_options_t) == 8 * sizeof(void *) &&
                     JB_ALIGNOF(jb_process_options_t) == sizeof(void *) &&
                     offsetof(jb_process_options_t, arguments) == sizeof(void *) &&
                     offsetof(jb_process_options_t, working_directory) == 2 * sizeof(void *) &&
                     offsetof(jb_process_options_t, environment) == 3 * sizeof(void *) &&
                     offsetof(jb_process_options_t, timeout_ms) == 4 * sizeof(void *) &&
                     offsetof(jb_process_options_t, output) == 5 * sizeof(void *) &&
                     offsetof(jb_process_options_t, canceled) == 6 * sizeof(void *) &&
                     offsetof(jb_process_options_t, context) == 7 * sizeof(void *),
                 "jb requires naturally aligned process options");
JB_STATIC_ASSERT(sizeof(jb_process_result_t) == 16 && JB_ALIGNOF(jb_process_result_t) == 4 &&
                     offsetof(jb_process_result_t, started) == 0 &&
                     offsetof(jb_process_result_t, exited) == 1 &&
                     offsetof(jb_process_result_t, exit_code) == 4 &&
                     offsetof(jb_process_result_t, term_signal) == 8 &&
                     offsetof(jb_process_result_t, native_error) == 12,
                 "jb requires a naturally aligned process result");
#endif

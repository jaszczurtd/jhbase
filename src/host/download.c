#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "jb/download.h"
#include "jb/array.h"
#include "jb/file.h"
#include "jb/utf8.h"
#include <curl/curl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
typedef SRWLOCK jd_lock_t;
typedef CONDITION_VARIABLE jd_cond_t;
#else
#include <pthread.h>
#include <signal.h>
#include <time.h>
typedef pthread_mutex_t jd_lock_t;
typedef pthread_cond_t jd_cond_t;
#endif

/* Longest wait between two polls of cancellation and of the time limits, and
 * how long a stopped worker may take to end before it is abandoned. */
enum { JD_POLL_MS = 50, JD_STOP_GRACE_MS = 2 * JD_POLL_MS };
JB_STATIC_ASSERT(sizeof(curl_off_t) == sizeof(int64_t), "jb requires a 64-bit curl_off_t");

/* One attempt. libcurl runs it on a worker thread, because Schannel verifies
 * certificates and fetches CRLs in blocking calls that no libcurl option
 * limits. The calling thread keeps the callbacks and the limits; when it gives
 * up first, it marks the job abandoned and the worker cleans up later. */
typedef struct {
  jd_lock_t lock;
  jd_cond_t changed;
  unsigned refs; /* the caller and the worker */
  bool stop, done, abandoned, connected, overflow;
  uint64_t bytes, expected, retry_after_ms;
  CURLcode code;
  long http, proxy;
  hal_status_t write_status;
  /* The worker owns these until done; the writer then returns to the caller
   * unless the job was abandoned. */
  CURL *handle;
  jb_file_writer_t *writer;
  uint64_t max_bytes;
  char error[CURL_ERROR_SIZE];
} jd_job_t;

/* Worker threads still running, also abandoned ones; global cleanup waits. */
#ifdef _WIN32
static jd_lock_t jd_workers_lock = SRWLOCK_INIT;
static jd_cond_t jd_workers_idle = CONDITION_VARIABLE_INIT;
#else
static jd_lock_t jd_workers_lock = PTHREAD_MUTEX_INITIALIZER;
static jd_cond_t jd_workers_idle = PTHREAD_COND_INITIALIZER;
#endif
static unsigned long jd_workers;

static void jd_lock(jd_lock_t *lock) {
#ifdef _WIN32
  AcquireSRWLockExclusive(lock);
#else
  (void)pthread_mutex_lock(lock);
#endif
}

static void jd_unlock(jd_lock_t *lock) {
#ifdef _WIN32
  ReleaseSRWLockExclusive(lock);
#else
  (void)pthread_mutex_unlock(lock);
#endif
}

static void jd_notify(jd_cond_t *condition) {
#ifdef _WIN32
  WakeAllConditionVariable(condition);
#else
  (void)pthread_cond_broadcast(condition);
#endif
}

/* Wait for a notification or at most milliseconds; spurious wakeups return. */
static void jd_wait_for(jd_cond_t *condition, jd_lock_t *lock, uint32_t milliseconds) {
#ifdef _WIN32
  (void)SleepConditionVariableSRW(condition, lock, milliseconds, 0);
#else
  struct timespec until;
  (void)clock_gettime(CLOCK_MONOTONIC, &until);
  until.tv_sec += (time_t)(milliseconds / 1000u);
  until.tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec += 1;
    until.tv_nsec -= 1000000000L;
  }
  (void)pthread_cond_timedwait(condition, lock, &until);
#endif
}

/* A fine monotonic clock: a coarse Windows tick could read before the deadline
 * after libcurl's own timer already ended the attempt. */
static uint64_t jd_now_ms(void) {
#ifdef _WIN32
  LARGE_INTEGER counter, frequency;
  (void)QueryPerformanceCounter(&counter);
  (void)QueryPerformanceFrequency(&frequency);
  const uint64_t ticks = (uint64_t)counter.QuadPart, rate = (uint64_t)frequency.QuadPart;
  return ticks / rate * 1000u + ticks % rate * 1000u / rate;
#else
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static void jd_sleep_ms(uint32_t milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  struct timespec duration = {(time_t)(milliseconds / 1000u),
                              (long)(milliseconds % 1000u) * 1000000L};
  while (nanosleep(&duration, &duration) != 0) {
  }
#endif
}

/* curl takes long values, which have 32 bits on Windows. */
static long jd_long_value(uint64_t value) {
  return value > (uint64_t)LONG_MAX ? LONG_MAX : (long)value;
}

static hal_status_t jd_fail(jb_download_result_t *result, hal_status_t status, const char *text) {
  (void)jb_utf8_copy_valid_prefix(text, strlen(text), result->message, sizeof(result->message),
                                  NULL);
  return status;
}

static bool jd_canceled(const jb_download_options_t *options) {
  return options->canceled != NULL && options->canceled(options->context);
}

static bool jd_job_init(jd_job_t *job) {
#ifdef _WIN32
  InitializeSRWLock(&job->lock);
  InitializeConditionVariable(&job->changed);
  return true;
#else
  pthread_condattr_t attributes;
  if (pthread_condattr_init(&attributes) != 0)
    return false;
  const bool condition = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0 &&
                         pthread_cond_init(&job->changed, &attributes) == 0;
  (void)pthread_condattr_destroy(&attributes);
  if (!condition)
    return false;
  if (pthread_mutex_init(&job->lock, NULL) != 0) {
    (void)pthread_cond_destroy(&job->changed);
    return false;
  }
  return true;
#endif
}

static void jd_job_destroy(jd_job_t *job) {
#ifndef _WIN32
  (void)pthread_mutex_destroy(&job->lock);
  (void)pthread_cond_destroy(&job->changed);
#endif
  free(job);
}

static void jd_job_release(jd_job_t *job) {
  jd_lock(&job->lock);
  const bool last = --job->refs == 0u;
  jd_unlock(&job->lock);
  if (last)
    jd_job_destroy(job);
}

/* libcurl fixes this signature (curl_write_callback), so data stays non-const. */
// cppcheck-suppress constParameterCallback
static size_t jd_write(char *data, size_t size, size_t count, void *context) {
  jd_job_t *job = context;
  /* curl passes size 1; the product cannot overflow for its buffers. */
  const size_t length = size * count;
  const hal_status_t status = jb_file_writer_write(job->writer, data, length);
  /* Also the bytes a failed append stored before its error. */
  uint64_t stored = 0;
  (void)jb_file_writer_size(job->writer, &stored);
  jd_lock(&job->lock);
  job->bytes = stored;
  if (status != HAL_OK)
    job->write_status = status;
  jd_unlock(&job->lock);
  return status == HAL_OK ? length : CURL_WRITEFUNC_ERROR;
}

/* The last line of a header block: libcurl has settled the body size of the
 * response by now. A 2xx body announced over max_bytes ends the attempt before
 * any of it is read; redirect and error bodies are never stored. */
// cppcheck-suppress constParameterCallback
static size_t jd_header(char *data, size_t size, size_t count, void *context) {
  jd_job_t *job = context;
  const size_t length = size * count;
  if (length == 0u || length > 2u || (data[0] != '\r' && data[0] != '\n'))
    return length;
  long code = 0;
  curl_off_t announced = -1;
  if (curl_easy_getinfo(job->handle, CURLINFO_RESPONSE_CODE, &code) != CURLE_OK ||
      code / 100 != 2 ||
      curl_easy_getinfo(job->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &announced) != CURLE_OK ||
      announced < 0 || (uint64_t)announced <= job->max_bytes)
    return length;
  jd_lock(&job->lock);
  job->overflow = true;
  job->expected = (uint64_t)announced;
  jd_unlock(&job->lock);
  return 0u;
}

/* Each helper keeps the first failure, so the caller checks once. */
static CURLcode jd_long(CURL *handle, CURLoption option, long value, CURLcode previous) {
  return previous != CURLE_OK ? previous : curl_easy_setopt(handle, option, value);
}

static CURLcode jd_text(CURL *handle, CURLoption option, const char *value, CURLcode previous) {
  return previous != CURLE_OK ? previous : curl_easy_setopt(handle, option, value);
}

static CURLcode jd_pointer(CURL *handle, CURLoption option, void *value, CURLcode previous) {
  return previous != CURLE_OK ? previous : curl_easy_setopt(handle, option, value);
}

static CURLcode jd_configure(jd_job_t *job, const char *url, const jb_download_options_t *options,
                             uint64_t remaining_ms) {
  CURL *handle = job->handle;
  CURLcode code = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, job->error);
  code = jd_text(handle, CURLOPT_URL, url, code);
  code = jd_text(handle, CURLOPT_PROTOCOLS_STR, "https", code);
  code = jd_text(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https", code);
  code = jd_long(handle, CURLOPT_FOLLOWLOCATION, 1L, code);
  code = jd_long(handle, CURLOPT_MAXREDIRS, jd_long_value(options->max_redirects), code);
  code = jd_long(handle, CURLOPT_FAILONERROR, 1L, code);
  code = jd_long(handle, CURLOPT_NOSIGNAL, 1L, code);
  code = jd_long(handle, CURLOPT_SSL_VERIFYPEER, 1L, code);
  code = jd_long(handle, CURLOPT_SSL_VERIFYHOST, 2L, code);
  /* libcurl also ends the connection phase at the total limit. */
  code =
      jd_long(handle, CURLOPT_CONNECTTIMEOUT_MS, jd_long_value(options->connect_timeout_ms), code);
  code = jd_long(handle, CURLOPT_TIMEOUT_MS, jd_long_value(remaining_ms), code);
  /* An empty proxy disables the proxy environment variables; an empty no-proxy
   * list makes an explicit proxy apply to every host, whatever no_proxy says. */
  code = jd_text(handle, CURLOPT_PROXY, options->proxy != NULL ? options->proxy : "", code);
  if (options->proxy != NULL) {
    /* Credentials in the proxy URL use libcurl's default, Basic; the build
     * has no other HTTP authentication method. */
    code = jd_text(handle, CURLOPT_NOPROXY, "", code);
    code = jd_long(handle, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L, code);
  }
  if (options->ca_file != NULL) {
    /* The bundle replaces the system store, including the Linux CA directory,
     * for the server and for an https proxy. */
    code = jd_text(handle, CURLOPT_CAINFO, options->ca_file, code);
    code = jd_text(handle, CURLOPT_PROXY_CAINFO, options->ca_file, code);
#ifndef _WIN32
    code = jd_text(handle, CURLOPT_CAPATH, NULL, code);
    code = jd_text(handle, CURLOPT_PROXY_CAPATH, NULL, code);
#endif
  }
  if (code == CURLE_OK)
    code = curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, jd_header);
  code = jd_pointer(handle, CURLOPT_HEADERDATA, job, code);
  if (code == CURLE_OK)
    code = curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, jd_write);
  return jd_pointer(handle, CURLOPT_WRITEDATA, job, code);
}

static CURLcode jd_multi_error(CURLMcode code) {
  return code == CURLM_OUT_OF_MEMORY ? CURLE_OUT_OF_MEMORY : CURLE_FAILED_INIT;
}

static bool jd_stop_requested(jd_job_t *job) {
  jd_lock(&job->lock);
  const bool stop = job->stop;
  jd_unlock(&job->lock);
  return stop;
}

/* Publish the announced size and whether TLS (also through a proxy) is up. */
static void jd_note_progress(jd_job_t *job) {
  curl_off_t expected = -1, connected = 0;
  if (curl_easy_getinfo(job->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &expected) != CURLE_OK)
    expected = -1;
  if (curl_easy_getinfo(job->handle, CURLINFO_APPCONNECT_TIME_T, &connected) != CURLE_OK)
    connected = 0;
  jd_lock(&job->lock);
  if (expected > 0)
    job->expected = (uint64_t)expected;
  if (connected > 0)
    job->connected = true;
  jd_unlock(&job->lock);
}

/* Run the handle on a private multi handle until it ends or the caller asks
 * for a stop. Quick exit detaches a resolver thread still waiting for a slow
 * DNS server instead of joining it; that thread ends when the lookup returns. */
static CURLcode jd_drive(jd_job_t *job) {
  CURLM *multi = curl_multi_init();
  if (multi == NULL)
    return CURLE_OUT_OF_MEMORY;
  CURLcode result = CURLE_OK;
  CURLMcode code = curl_multi_setopt(multi, CURLMOPT_QUICK_EXIT, 1L);
  if (code == CURLM_OK)
    code = curl_multi_add_handle(multi, job->handle);
  if (code != CURLM_OK) {
    (void)curl_multi_cleanup(multi);
    return jd_multi_error(code);
  }
  for (;;) {
    int running = 0;
    code = curl_multi_perform(multi, &running);
    if (code != CURLM_OK) {
      result = jd_multi_error(code);
      break;
    }
    jd_note_progress(job);
    if (running == 0) {
      int queued = 0;
      const CURLMsg *message = curl_multi_info_read(multi, &queued);
      result = message != NULL && message->msg == CURLMSG_DONE ? message->data.result
                                                               : CURLE_FAILED_INIT;
      break;
    }
    if (jd_stop_requested(job)) {
      result = CURLE_ABORTED_BY_CALLBACK;
      break;
    }
    code = curl_multi_poll(multi, NULL, 0, JD_POLL_MS, NULL);
    if (code != CURLM_OK) {
      result = jd_multi_error(code);
      break;
    }
  }
  (void)curl_multi_remove_handle(multi, job->handle);
  (void)curl_multi_cleanup(multi);
  return result;
}

static void jd_worker(jd_job_t *job) {
#ifndef _WIN32
  /* A peer closing the connection must not stop the process. libcurl already
   * sends with MSG_NOSIGNAL where it can; this thread and the resolver threads
   * it starts block SIGPIPE, and one raised meanwhile is consumed below. */
  sigset_t pipe_set;
  (void)sigemptyset(&pipe_set);
  (void)sigaddset(&pipe_set, SIGPIPE);
  (void)pthread_sigmask(SIG_BLOCK, &pipe_set, NULL);
#endif
  const CURLcode code = jd_drive(job);
  long http = 0, proxy = 0;
  curl_off_t after = 0;
  if (curl_easy_getinfo(job->handle, CURLINFO_RESPONSE_CODE, &http) != CURLE_OK)
    http = 0;
  if (curl_easy_getinfo(job->handle, CURLINFO_HTTP_CONNECTCODE, &proxy) != CURLE_OK)
    proxy = 0;
  if (curl_easy_getinfo(job->handle, CURLINFO_RETRY_AFTER, &after) != CURLE_OK || after < 0)
    after = 0;
  curl_easy_cleanup(job->handle);
  job->handle = NULL;
  jd_lock(&job->lock);
  job->code = code;
  job->http = http;
  job->proxy = proxy;
  job->retry_after_ms = (uint64_t)after > UINT64_MAX / 1000u ? UINT64_MAX : (uint64_t)after * 1000u;
  job->done = true;
  const bool abandoned = job->abandoned;
  jd_notify(&job->changed);
  jd_unlock(&job->lock);
  if (abandoned)
    (void)jb_file_writer_abort(&job->writer);
#ifndef _WIN32
  sigset_t pending;
  if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
    const struct timespec immediately = {0, 0};
    (void)sigtimedwait(&pipe_set, NULL, &immediately);
  }
#endif
  jd_job_release(job);
  jd_lock(&jd_workers_lock);
  if (--jd_workers == 0u)
    jd_notify(&jd_workers_idle);
  jd_unlock(&jd_workers_lock);
}

#ifdef _WIN32
static unsigned __stdcall jd_worker_entry(void *context) {
  jd_worker(context);
  return 0;
}
#else
static void *jd_worker_entry(void *context) {
  jd_worker(context);
  return NULL;
}
#endif

static bool jd_start_worker(jd_job_t *job) {
  jd_lock(&jd_workers_lock);
  ++jd_workers;
  jd_unlock(&jd_workers_lock);
#ifdef _WIN32
  const uintptr_t thread = _beginthreadex(NULL, 0, jd_worker_entry, job, 0, NULL);
  const bool started = thread != 0u;
  if (started)
    (void)CloseHandle((HANDLE)thread);
#else
  bool started = false;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) == 0) {
    pthread_t thread;
    started = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) == 0 &&
              pthread_create(&thread, &attributes, jd_worker_entry, job) == 0;
    (void)pthread_attr_destroy(&attributes);
  }
#endif
  if (!started) {
    jd_lock(&jd_workers_lock);
    if (--jd_workers == 0u)
      jd_notify(&jd_workers_idle);
    jd_unlock(&jd_workers_lock);
  }
  return started;
}

static bool jd_transient_http(long status) {
  return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 ||
         status == 504;
}

static hal_status_t jd_http_status(long status) {
  if (status == 404 || status == 410)
    return HAL_ENOENT;
  if (status == 401 || status == 403 || status == 407)
    return HAL_EAUTH;
  return jd_transient_http(status) ? HAL_EAGAIN : HAL_EPROTO;
}

static hal_status_t jd_option_status(CURLcode code) {
  switch (code) {
  case CURLE_OUT_OF_MEMORY:
    return HAL_ENOMEM;
  case CURLE_UNKNOWN_OPTION:
  case CURLE_NOT_BUILT_IN:
  case CURLE_UNSUPPORTED_PROTOCOL:
    return HAL_EUNSUPPORTED;
  default:
    return HAL_EINVAL;
  }
}

static hal_status_t jd_transfer_status(const jd_job_t *job, bool *transient) {
  *transient = false;
  const CURLcode code = job->code;
  /* A proxy that refused the tunnel decides, whichever code libcurl chose. */
  if ((code == CURLE_COULDNT_CONNECT || code == CURLE_HTTP_RETURNED_ERROR) && job->proxy != 0 &&
      job->proxy / 100 != 2) {
    *transient = jd_transient_http(job->proxy);
    return jd_http_status(job->proxy);
  }
  switch (code) {
  case CURLE_OK:
    return job->http >= 200 && job->http < 300 ? HAL_OK : jd_http_status(job->http);
  case CURLE_HTTP_RETURNED_ERROR:
    *transient = jd_transient_http(job->http);
    return jd_http_status(job->http);
  case CURLE_COULDNT_RESOLVE_PROXY:
  case CURLE_COULDNT_RESOLVE_HOST:
    *transient = true;
    return HAL_ENOENT;
  case CURLE_OPERATION_TIMEDOUT:
    *transient = true;
    return HAL_ETIMEOUT;
  case CURLE_COULDNT_CONNECT:
  case CURLE_PARTIAL_FILE:
  case CURLE_SEND_ERROR:
  case CURLE_RECV_ERROR:
  case CURLE_GOT_NOTHING:
    *transient = true;
    return HAL_EIO;
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_ISSUER_ERROR:
  case CURLE_SSL_INVALIDCERTSTATUS:
  case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    return HAL_EAUTH;
  case CURLE_SSL_CONNECT_ERROR:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CIPHER:
  case CURLE_WEIRD_SERVER_REPLY:
  case CURLE_PROXY:
    return HAL_EPROTO;
  case CURLE_UNSUPPORTED_PROTOCOL:
    return HAL_EPERM;
  case CURLE_URL_MALFORMAT:
  case CURLE_SSL_CACERT_BADFILE:
    return HAL_EINVAL;
  case CURLE_TOO_MANY_REDIRECTS:
  case CURLE_FILESIZE_EXCEEDED:
    return HAL_EOVERFLOW;
  case CURLE_WRITE_ERROR:
    if (job->overflow)
      return HAL_EOVERFLOW;
    return job->write_status != HAL_OK ? job->write_status : HAL_EIO;
  case CURLE_ABORTED_BY_CALLBACK:
    return HAL_ECANCELED;
  case CURLE_OUT_OF_MEMORY:
    return HAL_ENOMEM;
  case CURLE_NOT_BUILT_IN:
  case CURLE_UNKNOWN_OPTION:
    return HAL_EUNSUPPORTED;
  default:
    return HAL_EIO;
  }
}

/* Parse a URL and copy its lower-case scheme; any scheme parses, so the
 * caller decides about schemes this build lacks. */
static hal_status_t jd_url_scheme(const char *url, unsigned int flags, char *scheme,
                                  size_t capacity, jb_download_result_t *result) {
  CURLU *parsed = curl_url();
  if (parsed == NULL)
    return jd_fail(result, HAL_ENOMEM, "out of memory");
  hal_status_t status = HAL_OK;
  char *text = NULL;
  CURLUcode code = curl_url_set(parsed, CURLUPART_URL, url, flags | CURLU_NON_SUPPORT_SCHEME);
  if (code == CURLUE_OK)
    code = curl_url_get(parsed, CURLUPART_SCHEME, &text, 0);
  if (code == CURLUE_OUT_OF_MEMORY)
    status = jd_fail(result, HAL_ENOMEM, "out of memory");
  else if (code != CURLUE_OK)
    status = jd_fail(result, HAL_EINVAL, curl_url_strerror(code));
  else if (strlen(text) >= capacity)
    status = jd_fail(result, HAL_EINVAL, "unsupported URL scheme");
  else
    memcpy(scheme, text, strlen(text) + 1u);
  curl_free(text);
  curl_url_cleanup(parsed);
  return status;
}

/* Only https URLs with a host, and only proxy schemes libcurl can use;
 * checked before any file or network access. */
static hal_status_t jd_check_urls(const char *url, const char *proxy,
                                  jb_download_result_t *result) {
  static const char *const proxy_schemes[] = {"http",    "https",  "socks4",
                                              "socks4a", "socks5", "socks5h"};
  char scheme[16];
  hal_status_t status = jd_url_scheme(url, 0u, scheme, sizeof(scheme), result);
  if (status == HAL_OK && strcmp(scheme, "https") != 0)
    status = jd_fail(result, HAL_EPERM, "only https URLs are allowed");
  if (status != HAL_OK || proxy == NULL)
    return status;
  /* libcurl reads a proxy without a scheme as an HTTP proxy, whatever its host
   * name; parsing it with a default scheme only checks the rest. */
  if (strstr(proxy, "://") == NULL)
    return jd_url_scheme(proxy, CURLU_DEFAULT_SCHEME, scheme, sizeof(scheme), result);
  status = jd_url_scheme(proxy, 0u, scheme, sizeof(scheme), result);
  if (status != HAL_OK)
    return status;
  for (size_t index = 0; index < COUNTOF(proxy_schemes); ++index) {
    if (strcmp(scheme, proxy_schemes[index]) == 0)
      return HAL_OK;
  }
  return jd_fail(result, HAL_EINVAL, "unsupported proxy scheme");
}

/* Before the worker starts or when it failed to: the caller owns everything. */
static hal_status_t jd_discard(jd_job_t *job, hal_status_t status) {
  (void)jb_file_writer_abort(&job->writer);
  if (job->handle != NULL)
    curl_easy_cleanup(job->handle);
  jd_job_destroy(job);
  return status;
}

/* Watch the worker on the calling thread: report progress, poll cancellation
 * and enforce the deadline and the connection limit. A stop request gives the
 * worker a short grace to end; then HAL_OK means the worker is done and the
 * writer is the caller's again, with stopped naming the request if any.
 * A worker held longer by a blocking system call is abandoned, and the
 * returned status says why. */
static hal_status_t jd_watch(jd_job_t *job, const jb_download_options_t *options, uint32_t attempt,
                             uint64_t started, uint64_t deadline, jb_download_result_t *result,
                             hal_status_t *stopped, bool *transient) {
  /* libcurl's own connection limit gets a short lead before the caller's. */
  const uint64_t connect_limit = (uint64_t)options->connect_timeout_ms + 2u * JD_POLL_MS;
  uint64_t reported_bytes = 0, reported_expected = 0;
  *stopped = HAL_OK;
  for (;;) {
    jd_lock(&job->lock);
    if (!job->done)
      jd_wait_for(&job->changed, &job->lock, JD_POLL_MS);
    const bool done = job->done, connected = job->connected;
    const uint64_t bytes = job->bytes, expected = job->expected;
    jd_unlock(&job->lock);
    result->bytes = bytes;
    if (options->progress != NULL && (bytes != reported_bytes || expected != reported_expected)) {
      reported_bytes = bytes;
      reported_expected = expected;
      options->progress(options->context, attempt, bytes, expected);
    }
    if (done)
      return HAL_OK;
    const uint64_t now = jd_now_ms();
    if (jd_canceled(options))
      *stopped = HAL_ECANCELED;
    else if (now >= deadline || (!connected && now - started >= connect_limit))
      *stopped = HAL_ETIMEOUT;
    else
      continue;
    /* Only a connection limit leaves time for another attempt. */
    *transient = *stopped == HAL_ETIMEOUT && now < deadline;
    const uint64_t until = now + JD_STOP_GRACE_MS;
    jd_lock(&job->lock);
    job->stop = true;
    for (uint64_t current = now; !job->done && current < until; current = jd_now_ms())
      jd_wait_for(&job->changed, &job->lock, (uint32_t)(until - current));
    const bool finished = job->done;
    if (!finished)
      job->abandoned = true;
    result->bytes = job->bytes;
    jd_unlock(&job->lock);
    if (finished)
      return HAL_OK;
    /* The worker deletes the temporary file when libcurl returns. */
    return jd_fail(result, *stopped,
                   *stopped == HAL_ECANCELED ? "canceled" : "the attempt outlived its time limit");
  }
}

/* One transfer into a new temporary file; publishes it on success. */
static hal_status_t jd_attempt(const char *url, const char *destination,
                               const jb_download_options_t *options, uint32_t attempt,
                               uint64_t deadline, jb_download_result_t *result, bool *transient,
                               uint64_t *retry_after_ms) {
  *transient = false;
  *retry_after_ms = 0u;
  const uint64_t started = jd_now_ms();
  jd_job_t *job = calloc(1u, sizeof(*job));
  if (job == NULL || !jd_job_init(job)) {
    free(job);
    return jd_fail(result, HAL_ENOMEM, "out of memory");
  }
  job->refs = 2u;
  job->write_status = HAL_OK;
  job->max_bytes = options->max_bytes;
  job->handle = curl_easy_init();
  if (job->handle == NULL)
    return jd_discard(job, jd_fail(result, HAL_ENOMEM, "cannot create a transfer"));
  hal_status_t status = jb_file_writer_begin(destination, options->max_bytes, &job->writer);
  if (status != HAL_OK)
    return jd_discard(job, jd_fail(result, status, "cannot create the destination file"));
  const CURLcode configured = jd_configure(job, url, options, deadline - started);
  if (configured != CURLE_OK) {
    result->transfer_error = (int32_t)configured;
    return jd_discard(
        job, jd_fail(result, jd_option_status(configured), curl_easy_strerror(configured)));
  }
  result->attempts = attempt;
  if (!jd_start_worker(job))
    return jd_discard(job, jd_fail(result, HAL_EAGAIN, "cannot start a transfer thread"));
  hal_status_t stopped = HAL_OK;
  bool stop_transient = false;
  status = jd_watch(job, options, attempt, started, deadline, result, &stopped, &stop_transient);
  if (status != HAL_OK) {
    *transient = stop_transient;
    jd_job_release(job);
    return status;
  }
  /* The worker is done; its results are stable and the writer is ours. */
  result->http_status = job->http > 0 && job->http <= 999 ? (uint32_t)job->http : 0u;
  result->proxy_status = job->proxy > 0 && job->proxy <= 999 ? (uint32_t)job->proxy : 0u;
  result->transfer_error = (int32_t)job->code;
  *retry_after_ms = job->retry_after_ms;
  status = jd_transfer_status(job, transient);
  if (stopped == HAL_ECANCELED) {
    status = HAL_ECANCELED;
    *transient = false;
  } else if (stopped == HAL_ETIMEOUT && job->code == CURLE_ABORTED_BY_CALLBACK) {
    /* The worker obeyed the caller's time limit. */
    status = HAL_ETIMEOUT;
    *transient = stop_transient;
  } else if (status == HAL_OK && jd_canceled(options)) {
    /* A cancellation that arrived with the last fragment still wins. */
    status = HAL_ECANCELED;
  }
  if (status == HAL_OK) {
    bool changed = false;
    status = jb_file_writer_commit(&job->writer, &changed);
    if (status != HAL_OK)
      (void)jd_fail(result, status, "cannot publish the destination file");
  } else {
    (void)jb_file_writer_abort(&job->writer);
    if (status == HAL_ECANCELED)
      (void)jd_fail(result, status, "canceled");
    else if (status == HAL_ETIMEOUT && job->code == CURLE_ABORTED_BY_CALLBACK)
      (void)jd_fail(result, status, "the attempt outlived its time limit");
    else if (job->overflow)
      (void)jd_fail(result, status, "the announced body is larger than max_bytes");
    else if (job->code == CURLE_WRITE_ERROR)
      (void)jd_fail(result, status, "cannot write the destination file");
    else if (job->code == CURLE_OK)
      (void)jd_fail(result, status, "the final response is not 2xx");
    else
      (void)jd_fail(result, status,
                    job->error[0] != '\0' ? job->error : curl_easy_strerror(job->code));
  }
  jd_job_release(job);
  return status;
}

/* Wait before another attempt; false when canceled. */
static bool jd_delay(const jb_download_options_t *options, uint64_t delay_ms) {
  const uint64_t end = jd_now_ms() + delay_ms;
  for (;;) {
    if (jd_canceled(options))
      return false;
    const uint64_t now = jd_now_ms();
    if (now >= end)
      return true;
    jd_sleep_ms(end - now < JD_POLL_MS ? (uint32_t)(end - now) : JD_POLL_MS);
  }
}

hal_status_t jb_download_file(const char *url, const char *destination,
                              const jb_download_options_t *options, jb_download_result_t *result) {
  if (result == NULL)
    return HAL_EINVAL;
  memset(result, 0, sizeof(*result));
  if (url == NULL || destination == NULL || options == NULL || options->connect_timeout_ms == 0u ||
      options->timeout_ms == 0u || options->max_attempts == 0u)
    return jd_fail(result, HAL_EINVAL, "invalid arguments");
  hal_status_t status = jd_check_urls(url, options->proxy, result);
  if (status != HAL_OK)
    return status;
  const uint64_t deadline = jd_now_ms() + options->timeout_ms;
  for (uint32_t attempt = 1u;; ++attempt) {
    memset(result->message, 0, sizeof(result->message));
    bool transient = false;
    uint64_t retry_after_ms = 0u;
    if (jd_canceled(options))
      return jd_fail(result, HAL_ECANCELED, "canceled");
    /* A retry delay ends before the deadline, but sleeping may overshoot it. */
    if (jd_now_ms() >= deadline)
      return jd_fail(result, HAL_ETIMEOUT, "the time budget ended before another attempt");
    status = jd_attempt(url, destination, options, attempt, deadline, result, &transient,
                        &retry_after_ms);
    if (status == HAL_OK || !transient || attempt == options->max_attempts)
      return status;
    const uint64_t delay =
        retry_after_ms > options->retry_delay_ms ? retry_after_ms : options->retry_delay_ms;
    const uint64_t now = jd_now_ms();
    if (now >= deadline || delay >= deadline - now)
      return status;
    if (!jd_delay(options, delay))
      return jd_fail(result, HAL_ECANCELED, "canceled before another attempt");
  }
}

hal_status_t jb_download_info(jb_download_info_t *out) {
  if (out == NULL)
    return HAL_EINVAL;
  memset(out, 0, sizeof(*out));
  const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
  if (info == NULL || info->version == NULL)
    return HAL_EUNSUPPORTED;
  const char *backend = info->ssl_version != NULL ? info->ssl_version : "";
  if (strlen(info->version) >= sizeof(out->version) || strlen(backend) >= sizeof(out->tls_backend))
    return HAL_EOVERFLOW;
  memcpy(out->version, info->version, strlen(info->version) + 1u);
  memcpy(out->tls_backend, backend, strlen(backend) + 1u);
  out->version_number = info->version_num;
  for (const char *const *protocol = info->protocols; protocol != NULL && *protocol != NULL;
       ++protocol)
    out->https = out->https || strcmp(*protocol, "https") == 0;
  out->async_dns = (info->features & CURL_VERSION_ASYNCHDNS) != 0;
  out->threadsafe = (info->features & CURL_VERSION_THREADSAFE) != 0;
  return HAL_OK;
}

hal_status_t jb_download_global_init(void) {
  jb_download_info_t info;
  const hal_status_t status = jb_download_info(&info);
  if (status != HAL_OK)
    return status;
  const curl_version_info_data *features = curl_version_info(CURLVERSION_NOW);
  if (!info.https || !info.async_dns || !info.threadsafe || info.tls_backend[0] == '\0' ||
      (features->features & CURL_VERSION_SSL) == 0)
    return HAL_EUNSUPPORTED;
  const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (code == CURLE_OK)
    return HAL_OK;
  return code == CURLE_OUT_OF_MEMORY ? HAL_ENOMEM : HAL_EIO;
}

void jb_download_global_cleanup(void) {
  /* Abandoned attempts end when their blocking system call returns. */
  jd_lock(&jd_workers_lock);
  while (jd_workers != 0u) {
#ifdef _WIN32
    (void)SleepConditionVariableSRW(&jd_workers_idle, &jd_workers_lock, INFINITE, 0);
#else
    (void)pthread_cond_wait(&jd_workers_idle, &jd_workers_lock);
#endif
  }
  jd_unlock(&jd_workers_lock);
  curl_global_cleanup();
}

#ifndef JB_DOWNLOAD_H
#define JB_DOWNLOAD_H

/** @file download.h
 * HTTPS downloads into a file on Linux and Windows. Enable JHBASE_HOST and
 * JHBASE_DOWNLOAD, then link jb::download; it builds the pinned libcurl with
 * Schannel on Windows and the system OpenSSL on Linux. No curl type or header
 * reaches this API.
 *
 * Only HTTPS is accepted, for the first URL and for every redirect. The
 * server certificate and host name are always verified; Schannel also checks
 * revocation of the whole chain. A response body is streamed into a temporary
 * sibling of the destination and published atomically only after a complete
 * transfer with a final 2xx status. A failed, canceled or retried attempt
 * deletes its temporary file; the destination keeps its previous content.
 * Independent calls may run concurrently between jb_download_global_init and
 * jb_download_global_cleanup.
 *
 * libcurl runs each attempt on a worker thread; the callbacks of
 * jb_download_options_t and every limit stay on the calling thread.
 */
#include "jb/cancel.h"
#include "jb/status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Report progress of the current attempt on the calling thread whenever received
 * or expected changed. received counts bytes stored by this attempt; expected
 * is the announced body size or 0 when unknown. attempt starts at 1. Return
 * promptly; use canceled to stop. */
typedef void (*jb_download_progress_fn)(void *context, uint32_t attempt, uint64_t received,
                                        uint64_t expected);

/** Byte sizes of the text fields in jb_download_result_t and jb_download_info_t. */
enum {
  JB_DOWNLOAD_MESSAGE_CAPACITY = 256,
  JB_DOWNLOAD_VERSION_CAPACITY = 32,
  JB_DOWNLOAD_BACKEND_CAPACITY = 64
};

/** Zero-initialize before setting fields. Every time limit is a hard limit. */
typedef struct {
  uint64_t max_bytes;  /**< Body bytes stored by one attempt; 0 allows only an empty body.
                          A larger announced size fails before the body is read. */
  const char *ca_file; /**< NULL uses the system store: the Windows certificate store, or the
                          bundle/directory of the distribution found when the library was built
                          on Linux. Otherwise the UTF-8 path of a PEM bundle that replaces it. */
  const char *proxy;   /**< NULL connects directly and ignores proxy environment variables;
                          otherwise an explicit proxy URL such as "http://host:3128"
                          (http, https, socks4, socks4a, socks5 or socks5h; without a scheme an
                          HTTP proxy). Credentials in it use Basic authentication. */
  jb_download_progress_fn progress; /**< Optional progress report; NULL disables it. */
  jb_cancel_fn canceled;            /**< Polled on the calling thread before each attempt, at least
                                       every 50 ms while an attempt or a retry delay runs, and once
                                       more before a complete body is published; NULL disables it. */
  void *context;                    /**< Passed unchanged to progress and canceled; NULL allowed. */
  uint32_t connect_timeout_ms;      /**< Required; limit of one connection, also DNS and TLS. */
  uint32_t timeout_ms;     /**< Required; budget of the whole call from its start, shared by all
                              attempts and retry delays. */
  uint32_t retry_delay_ms; /**< Wait before another attempt; a longer Retry-After of the
                              server wins. A wait that would end the budget stops retrying. */
  uint32_t max_attempts;   /**< Required; transfers including the first one. */
  uint32_t max_redirects;  /**< Redirects followed per attempt; 0 refuses any redirect. */
} jb_download_options_t;

/** Always cleared first by jb_download_file when non-NULL. */
typedef struct {
  uint64_t bytes;    /**< Body bytes stored by the last attempt, also a failed one, including bytes
                        a failed write stored before its error. */
  uint32_t attempts; /**< Transfers started; 0 when the call failed before the first. */
  uint32_t http_status;   /**< Final HTTP status of the last attempt, or 0 without a response. */
  uint32_t proxy_status;  /**< Last proxy answer to CONNECT, or 0 without a proxy answer. */
  int32_t transfer_error; /**< libcurl result code of the last attempt; 0 on success and when
                             a check of jhbase failed after a complete transfer. */
  char message[JB_DOWNLOAD_MESSAGE_CAPACITY]; /**< Short UTF-8 reason for a failure from
                                                 libcurl or jhbase; empty on success. */
} jb_download_result_t;

/** Linked libcurl as reported at run time. */
typedef struct {
  char version[JB_DOWNLOAD_VERSION_CAPACITY]; /**< libcurl version text; a build from an upstream
                                                 git tag reports e.g. "8.23.0-DEV". */
  char tls_backend[JB_DOWNLOAD_BACKEND_CAPACITY]; /**< e.g. "Schannel" or "OpenSSL/3.0.13". */
  uint32_t version_number;                        /**< 0xMMmmpp, e.g. 0x081700 for 8.23.0. */
  bool https;                                     /**< The https protocol is built in. */
  bool async_dns;  /**< Name resolution runs asynchronously, so timeouts also bound it. */
  bool threadsafe; /**< Global initialization is thread-safe. */
} jb_download_info_t;

/** Initialize libcurl and check the required features (HTTPS, TLS backend,
 * asynchronous DNS, thread-safe initialization). Call before starting threads
 * that download; every successful call needs one jb_download_global_cleanup.
 * @return HAL_OK, HAL_EUNSUPPORTED for a library without a required feature,
 * HAL_ENOMEM or HAL_EIO. */
hal_status_t jb_download_global_init(void);

/** Release one successful jb_download_global_init after all downloads ended.
 * Waits for worker threads of abandoned attempts (see jb_download_file). */
void jb_download_global_cleanup(void);

/** Report the linked libcurl; usable before jb_download_global_init.
 * @param out Required; cleared first.
 * @return HAL_OK, HAL_EINVAL for NULL or HAL_EOVERFLOW for a text that does not fit. */
hal_status_t jb_download_info(jb_download_info_t *out);

/** Download one HTTPS resource into destination.
 *
 * Each attempt creates a new connection and a new temporary file. Another
 * attempt follows only a transient failure of the previous one: connection,
 * name resolution, a connection timeout shorter than the remaining budget,
 * an interrupted transfer, or HTTP 408, 429, 500, 502, 503 or 504 from the
 * server or from a proxy refusing the CONNECT tunnel. Verification, policy,
 * write and cancellation failures end the call. A name lookup that outlives
 * its attempt keeps running on a detached libcurl thread until it returns.
 *
 * Cancellation, the deadline and, before TLS is up, the connection limit are
 * enforced on the calling thread. A worker that does not stop within 100 ms,
 * because a blocking system call holds it (Schannel fetching a CRL, a stuck
 * disk write), is abandoned: the call returns, and the worker deletes its
 * temporary file when the system call returns.
 *
 * On POSIX the worker blocks SIGPIPE and consumes one raised on it; the
 * caller's signal mask and the process disposition are unchanged.
 *
 * @param url Required UTF-8 https URL.
 * @param destination Required absolute UTF-8 path; missing parents are created.
 * A final link or a nonregular destination is rejected. The parent directory
 * receives temporary siblings.
 * @param options Required; see jb_download_options_t.
 * @param result Required diagnostics, cleared before any other check.
 * @return HAL_OK; HAL_EINVAL for arguments, a malformed URL or proxy URL, a
 * proxy scheme other than http, https, socks4, socks4a, socks5 and socks5h, or
 * an unreadable ca_file; HAL_EPERM for a URL or redirect other than https;
 * HAL_EAUTH when TLS verification rejects the server (certificate, host name,
 * revocation) or for HTTP 401, 403 and 407 (also from the proxy); HAL_ENOENT for HTTP 404 and 410
 * or an unknown host; HAL_EAGAIN for the transient HTTP statuses above;
 * HAL_EPROTO for another HTTP status, another TLS handshake failure or a
 * protocol failure; HAL_EOVERFLOW for max_bytes or max_redirects;
 * HAL_ETIMEOUT; HAL_ECANCELED; HAL_ENOMEM; HAL_EUNSUPPORTED for an option the
 * linked libcurl lacks; a file API status for the destination; HAL_EIO for
 * other transfer failures.
 */
hal_status_t jb_download_file(const char *url, const char *destination,
                              const jb_download_options_t *options, jb_download_result_t *result);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(offsetof(jb_download_options_t, max_bytes) == 0 &&
                     offsetof(jb_download_options_t, ca_file) == sizeof(uint64_t) &&
                     offsetof(jb_download_options_t, connect_timeout_ms) ==
                         sizeof(uint64_t) + 5 * sizeof(void *),
                 "jb requires naturally aligned download options");
JB_STATIC_ASSERT(offsetof(jb_download_result_t, attempts) == sizeof(uint64_t) &&
                     offsetof(jb_download_result_t, message) == sizeof(uint64_t) + 16,
                 "jb requires naturally aligned download diagnostics");
#endif

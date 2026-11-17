#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "https_fixture.h"
#include "jb/array.h"
#include "jb/download.h"
#include "jb/file.h"
#include "jb/filesystem.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

/* Body sizes: one small resource and one large enough to span many reads. */
enum { LARGE_BYTES = 8u << 20, NO_LENGTH_BYTES = 64u << 10, HOLD_BYTES = 256u << 10 };

static const char ok_body[] = "jhbase download test\n";
static const char second_body[] = "second attempt\n";
static char *large_body;
static const char *openssl_tool;
static char test_root[4096], pki_dir[4096], www_good[4096], www_alt[4096], out_dir[4096];
static char ca_file[4096], other_ca_file[4096], diagnostics[4096];
static hf_server_t *crl_server, *crl_staller, *router, *proxy;
static hf_tls_t *good, *alt, *wrong, *untrusted, *revoked, *stalled1, *stalled2;

#ifdef __linux__
/* Faults for writes to regular files, which only the download writer makes
 * while a test arms them; they run on the transfer's worker thread. */
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static int fail_file_write_errno;    /* nonzero: writes fail with this errno */
static size_t partial_file_write;    /* the first failing write still stores this many bytes */
static unsigned block_file_write_ms; /* the next write sleeps this long first */
static bool raise_on_file_write;     /* the next write raises SIGPIPE on its thread */
static int raised_on_write;

static void set_write_faults(int error, size_t partial, unsigned block_ms, bool raise_signal) {
  (void)pthread_mutex_lock(&write_lock);
  fail_file_write_errno = error;
  partial_file_write = partial;
  block_file_write_ms = block_ms;
  raise_on_file_write = raise_signal;
  (void)pthread_mutex_unlock(&write_lock);
}

ssize_t __real_write(int descriptor, const void *data, size_t size);
ssize_t __wrap_write(int descriptor, const void *data, size_t size);
ssize_t __wrap_write(int descriptor, const void *data, size_t size) {
  struct stat info;
  if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode))
    return __real_write(descriptor, data, size);
  (void)pthread_mutex_lock(&write_lock);
  const int error = fail_file_write_errno;
  const size_t partial = partial_file_write;
  const unsigned block_ms = block_file_write_ms;
  const bool raise_signal = raise_on_file_write;
  partial_file_write = 0;
  block_file_write_ms = 0;
  raise_on_file_write = false;
  raised_on_write += raise_signal ? 1 : 0;
  (void)pthread_mutex_unlock(&write_lock);
  if (block_ms != 0u)
    hf_sleep_ms(block_ms);
  if (raise_signal)
    (void)raise(SIGPIPE);
  if (error != 0 && partial != 0u)
    return __real_write(descriptor, data, size < partial ? size : partial);
  if (error != 0) {
    errno = error;
    return -1;
  }
  return __real_write(descriptor, data, size);
}

/* Lookups of slow.invalid sleep in the resolver thread; the count tells when
 * a detached libcurl resolver thread has left the wrapper. */
static pthread_mutex_t slow_lock = PTHREAD_MUTEX_INITIALIZER;
static int slow_lookups;
int __real_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result);
int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result);
int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                       struct addrinfo **result) {
  if (node == NULL || strcmp(node, "slow.invalid") != 0)
    return __real_getaddrinfo(node, service, hints, result);
  (void)pthread_mutex_lock(&slow_lock);
  ++slow_lookups;
  (void)pthread_mutex_unlock(&slow_lock);
  struct timespec duration = {3, 0};
  while (nanosleep(&duration, &duration) != 0) {
  }
  (void)pthread_mutex_lock(&slow_lock);
  --slow_lookups;
  (void)pthread_mutex_unlock(&slow_lock);
  return EAI_NONAME;
}

/* fsync of a regular file fails while set: the publish step of the writer. */
int __real_fsync(int descriptor);
static int fail_fsync_errno;
int __wrap_fsync(int descriptor);
int __wrap_fsync(int descriptor) {
  struct stat info;
  if (fail_fsync_errno != 0 && fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode)) {
    errno = fail_fsync_errno;
    return -1;
  }
  return __real_fsync(descriptor);
}

static volatile sig_atomic_t sigpipe_count;
static void count_sigpipe(int signal_number) {
  (void)signal_number;
  sigpipe_count = sigpipe_count + 1;
}
#endif

void setUp(void) {}

void tearDown(void) {}

static const char *join(char *out, size_t capacity, const char *base, const char *relative) {
  size_t required = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_join(base, relative, out, capacity, &required));
  return out;
}

static void write_bytes(const char *path, const void *data, size_t length) {
  bool changed = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path, data, length, false, &changed));
}

/* A complete HTTP response with LF line ends: s_server on Windows reads it in
 * text mode, which keeps LF-only bytes unchanged. declared < 0 omits the
 * Content-Length header (the body ends with the connection). */
static void write_response(const char *directory, const char *name, const char *status,
                           const char *headers, const char *body, size_t length, long declared) {
  char head[1024], path[4096];
  int written;
  if (declared < 0)
    written = snprintf(head, sizeof(head), "HTTP/1.1 %s\n%sConnection: close\n\n", status, headers);
  else
    written =
        snprintf(head, sizeof(head), "HTTP/1.1 %s\n%sContent-Length: %ld\nConnection: close\n\n",
                 status, headers, declared);
  TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(head));
  char *response = malloc((size_t)written + length + 1u);
  TEST_ASSERT_NOT_NULL(response);
  memcpy(response, head, (size_t)written);
  if (length > 0u)
    memcpy(response + written, body, length);
  write_bytes(join(path, sizeof(path), directory, name), response, (size_t)written + length);
  free(response);
}

static void url(char *out, size_t capacity, uint16_t port, const char *path) {
  const int written = snprintf(out, capacity, "https://localhost:%u/%s", (unsigned)port, path);
  TEST_ASSERT_TRUE(written > 0 && (size_t)written < capacity);
}

static jb_download_options_t defaults(void) {
  jb_download_options_t options;
  memset(&options, 0, sizeof(options));
  options.max_bytes = 64u << 20;
  options.ca_file = ca_file;
  options.connect_timeout_ms = 10000u;
  options.timeout_ms = 60000u;
  options.retry_delay_ms = 10u;
  options.max_attempts = 1u;
  options.max_redirects = 5u;
  return options;
}

/* A fresh destination directory per test, with a space and non-ASCII name. */
static void destination(char *directory, size_t directory_capacity, char *file,
                        size_t file_capacity, const char *test) {
  char name[256];
  (void)snprintf(name, sizeof(name), "%s pobrane \xc5\xbc\xc3\xb3\xc5\x82w", test);
  join(directory, directory_capacity, out_dir, name);
  (void)jb_fs_remove(directory, true);
  join(file, file_capacity, directory, "plik \xc4\x85.bin");
}

static hal_status_t count_entry(void *context, const char *name, const jb_fs_info_t *info) {
  (void)name;
  (void)info;
  ++*(size_t *)context;
  return HAL_OK;
}

/* No temporary sibling survives: the directory holds only the expected file. */
static void assert_entries(const char *directory, size_t expected) {
  size_t count = 0;
  const hal_status_t status = jb_fs_list(directory, count_entry, &count);
  if (status == HAL_ENOENT && expected == 0u)
    return;
  TEST_ASSERT_EQUAL(HAL_OK, status);
  TEST_ASSERT_EQUAL_size_t(expected, count);
}

static size_t entries_of(const char *directory) {
  size_t count = 0;
  return jb_fs_list(directory, count_entry, &count) == HAL_OK ? count : SIZE_MAX;
}

/* Wait until an abandoned attempt has deleted its temporary file. */
static void wait_for_entries(const char *directory, size_t expected, uint32_t limit_ms) {
  for (uint32_t waited = 0; entries_of(directory) != expected && waited < limit_ms; waited += 50u)
    hf_sleep_ms(50u);
  TEST_ASSERT_EQUAL_size_t(expected, entries_of(directory));
}

static void assert_content(const char *path, const void *data, size_t length) {
  jb_file_data_t file = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path, (size_t)LARGE_BYTES * 2u, &file));
  TEST_ASSERT_EQUAL_size_t(length, file.length);
  if (length > 0u)
    TEST_ASSERT_EQUAL_MEMORY(data, file.data, length);
  jb_file_release(&file);
}

static void assert_absent(const char *path) {
  jb_fs_info_t info;
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_status(path, false, &info));
}

typedef struct {
  uint32_t calls;
  uint32_t attempt;
  uint64_t received;
  uint64_t expected;
  uint64_t cancel_at_bytes; /* cancel once this many bytes arrived; 0: never */
  uint64_t cancel_at_ms;    /* cancel once this much time passed; 0: never */
  uint64_t started_ms;
  bool cancel;
} probe_t;

static void probe_progress(void *context, uint32_t attempt, uint64_t received, uint64_t expected) {
  probe_t *probe = context;
  ++probe->calls;
  probe->attempt = attempt;
  probe->received = received;
  probe->expected = expected;
  if (probe->cancel_at_bytes > 0u && received >= probe->cancel_at_bytes)
    probe->cancel = true;
}

static bool probe_canceled(void *context) {
  probe_t *probe = context;
  if (probe->cancel_at_ms > 0u && hf_now_ms() - probe->started_ms >= probe->cancel_at_ms)
    probe->cancel = true;
  return probe->cancel;
}

static void plan(const hf_action_t *actions, size_t count, hf_action_kind_t fallback) {
  const hf_action_t rest = {fallback, 0u, 0u};
  hf_router_plan(router, actions, count, rest);
}

static void test_info_reports_the_linked_library(void) {
  jb_download_info_t info;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_info(NULL));
  TEST_ASSERT_EQUAL(HAL_OK, jb_download_info(&info));
  /* The pin names the release; the text from its git tag may carry -DEV. */
  char *end = NULL;
  const unsigned long major = strtoul(JB_EXPECTED_CURL_VERSION, &end, 10);
  const unsigned long minor = strtoul(end + 1, &end, 10);
  const unsigned long patch = strtoul(end + 1, &end, 10);
  TEST_ASSERT_EQUAL_HEX32((uint32_t)(major << 16 | minor << 8 | patch), info.version_number);
  TEST_ASSERT_EQUAL_INT(
      0, strncmp(info.version, JB_EXPECTED_CURL_VERSION, strlen(JB_EXPECTED_CURL_VERSION)));
#ifdef _WIN32
  TEST_ASSERT_EQUAL_STRING("Schannel", info.tls_backend);
#else
  TEST_ASSERT_EQUAL_INT(0, strncmp(info.tls_backend, "OpenSSL/", 8u));
#endif
  TEST_ASSERT_TRUE(info.https);
  TEST_ASSERT_TRUE(info.async_dns);
  TEST_ASSERT_TRUE(info.threadsafe);
}

static void test_arguments_and_urls_are_checked_first(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "arguments");
  url(address, sizeof(address), hf_tls_port(good), "ok");
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, file, &options, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(NULL, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, result.attempts);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, NULL, &options, &result));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, file, NULL, &result));
  jb_download_options_t broken = options;
  broken.max_attempts = 0u;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, file, &broken, &result));
  broken = options;
  broken.timeout_ms = 0u;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, file, &broken, &result));
  broken = options;
  broken.connect_timeout_ms = 0u;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, file, &broken, &result));
  /* Policy checks happen before any connection or file access. */
  const hf_action_t forward = {HF_FORWARD, hf_tls_port(good), 0u};
  plan(&forward, 1u, HF_FORWARD);
  const uint32_t before = hf_server_connections(router);
  char plain[512];
  (void)snprintf(plain, sizeof(plain), "http://localhost:%u/ok", (unsigned)hf_server_port(router));
  TEST_ASSERT_EQUAL(HAL_EPERM, jb_download_file(plain, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, result.attempts);
  TEST_ASSERT_TRUE(result.message[0] != '\0');
  TEST_ASSERT_EQUAL(HAL_EPERM, jb_download_file("ftp://localhost/ok", file, &options, &result));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file("https://", file, &options, &result));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file("not a url", file, &options, &result));
  /* A proxy libcurl could not use is refused before the first attempt. */
  jb_download_options_t proxied = options;
  char routed_ok[512];
  url(routed_ok, sizeof(routed_ok), hf_server_port(router), "ok");
  proxied.proxy = "ftp://127.0.0.1:1";
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(routed_ok, file, &proxied, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, result.attempts);
  proxied.proxy = "";
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(routed_ok, file, &proxied, &result));
  TEST_ASSERT_EQUAL_UINT32(before, hf_server_connections(router));
  assert_absent(directory);
  /* A destination that cannot be created stops before the first transfer. */
  char blocker[4096], below[4096];
  join(blocker, sizeof(blocker), out_dir, "a file, not a directory");
  write_bytes(blocker, "x", 1u);
  join(below, sizeof(below), blocker, "child.bin");
  char routed[512];
  url(routed, sizeof(routed), hf_server_port(router), "ok");
  TEST_ASSERT_NOT_EQUAL(HAL_OK, jb_download_file(routed, below, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, result.attempts);
  TEST_ASSERT_EQUAL_UINT32(before, hf_server_connections(router));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, "relative.bin", &options, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, result.attempts);
}

static void test_trusted_download_publishes_exact_bytes(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  probe_t probe = {0};
  options.progress = probe_progress;
  options.canceled = probe_canceled;
  options.context = &probe;
  destination(directory, sizeof(directory), file, sizeof(file), "trusted");
  url(address, sizeof(address), hf_tls_port(good), "ok");
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  assert_content(file, ok_body, sizeof(ok_body) - 1u);
  assert_entries(directory, 1u);
  TEST_ASSERT_EQUAL_UINT64(sizeof(ok_body) - 1u, result.bytes);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_EQUAL_UINT32(200u, result.http_status);
  TEST_ASSERT_EQUAL_INT32(0, result.transfer_error);
  TEST_ASSERT_EQUAL_STRING("", result.message);
  TEST_ASSERT_TRUE(probe.calls > 0u);
  TEST_ASSERT_EQUAL_UINT32(1u, probe.attempt);
  TEST_ASSERT_EQUAL_UINT64(sizeof(ok_body) - 1u, probe.received);
  TEST_ASSERT_EQUAL_UINT64(sizeof(ok_body) - 1u, probe.expected);
  /* The same bytes again, then an empty body. */
  TEST_ASSERT_EQUAL(HAL_OK, jb_download_file(address, file, &options, &result));
  assert_content(file, ok_body, sizeof(ok_body) - 1u);
  url(address, sizeof(address), hf_tls_port(good), "empty");
  options.max_bytes = 0u;
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  assert_content(file, NULL, 0u);
  assert_entries(directory, 1u);
}

static void test_failure_keeps_the_previous_destination(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "previous");
  write_bytes(file, "previous", 8u);
  url(address, sizeof(address), hf_tls_port(good), "notfound");
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_download_file(address, file, &options, &result));
  url(address, sizeof(address), hf_tls_port(untrusted), "ok");
  TEST_ASSERT_EQUAL(HAL_EAUTH, jb_download_file(address, file, &options, &result));
  /* A cancellation requested with the last fragment wins over publishing. */
  probe_t probe = {0};
  probe.cancel_at_bytes = sizeof(ok_body) - 1u;
  options.progress = probe_progress;
  options.canceled = probe_canceled;
  options.context = &probe;
  url(address, sizeof(address), hf_tls_port(good), "ok");
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT64(sizeof(ok_body) - 1u, probe.received);
  assert_content(file, "previous", 8u);
  assert_entries(directory, 1u);
}

static void test_redirects_follow_https_only(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "redirects");
  url(address, sizeof(address), hf_tls_port(good), "redirect-https");
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  assert_content(file, ok_body, sizeof(ok_body) - 1u);
  TEST_ASSERT_EQUAL_UINT32(200u, result.http_status);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(file, false));
  options.max_redirects = 0u;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_download_file(address, file, &options, &result));
  options.max_redirects = 3u;
  options.max_attempts = 3u;
  url(address, sizeof(address), hf_tls_port(good), "loop");
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  /* A redirect to plain HTTP never reaches the plain server. */
  const uint32_t before = hf_server_connections(crl_server);
  url(address, sizeof(address), hf_tls_port(good), "redirect-http");
  TEST_ASSERT_EQUAL(HAL_EPERM, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_EQUAL_UINT32(before, hf_server_connections(crl_server));
  assert_entries(directory, 0u);
}

static void test_verification_failures_are_final(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  options.max_attempts = 3u;
  destination(directory, sizeof(directory), file, sizeof(file), "verification");
  url(address, sizeof(address), hf_tls_port(untrusted), "ok");
  TEST_ASSERT_EQUAL(HAL_EAUTH, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_NOT_EQUAL(0, result.transfer_error);
  TEST_ASSERT_TRUE(result.message[0] != '\0');
  url(address, sizeof(address), hf_tls_port(wrong), "ok");
  TEST_ASSERT_EQUAL(HAL_EAUTH, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  /* The bundle replaces the trust store: another CA does not vouch for good. */
  url(address, sizeof(address), hf_tls_port(good), "ok");
  options.ca_file = other_ca_file;
  TEST_ASSERT_EQUAL(HAL_EAUTH, jb_download_file(address, file, &options, &result));
  /* The system store does not contain the test CA. */
  options.ca_file = NULL;
  TEST_ASSERT_EQUAL(HAL_EAUTH, jb_download_file(address, file, &options, &result));
  char missing[4096];
  options.ca_file = join(missing, sizeof(missing), out_dir, "missing ca.pem");
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_download_file(address, file, &options, &result));
  assert_entries(directory, 0u);
}

#ifdef _WIN32
/* Schannel checks revocation of the whole chain through the CRL points. */
static void test_schannel_checks_revocation(void) {
  char directory[4096], file[4096], address[512], request[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "revocation");
  url(address, sizeof(address), hf_tls_port(revoked), "ok");
  TEST_ASSERT_EQUAL(HAL_EAUTH, jb_download_file(address, file, &options, &result));
  /* The good server of the same CA passes, so the CRL itself is valid. */
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.message, "revoked"), result.message);
  TEST_ASSERT_TRUE(hf_server_connections(crl_server) > 0u);
  hf_server_last_request(crl_server, request, sizeof(request));
  TEST_ASSERT_EQUAL_INT(0, strncmp(request, "GET /root.crl ", 14u));
  assert_entries(directory, 0u);
}

/* Schannel fetches CRLs in a blocking call that no libcurl option limits; a
 * CRL server that never answers must not hold the call past cancellation or
 * the connection limit. Each case uses its own CRL address, so no cached
 * failure of the other one decides it. */
static void test_blocking_revocation_check_is_bounded(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  probe_t probe = {0};
  destination(directory, sizeof(directory), file, sizeof(file), "stalled crl");
  options.canceled = probe_canceled;
  options.context = &probe;
  probe.started_ms = hf_now_ms();
  probe.cancel_at_ms = 300u;
  url(address, sizeof(address), hf_tls_port(stalled1), "ok");
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - probe.started_ms < 1500u);
  options.canceled = NULL;
  options.connect_timeout_ms = 500u;
  url(address, sizeof(address), hf_tls_port(stalled2), "ok");
  const uint64_t started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - started < 2000u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_TRUE(hf_server_connections(crl_staller) > 0u);
  /* The abandoned attempts delete their files once Schannel gives up. */
  wait_for_entries(directory, 0u, 60000u);
}
#endif

static void test_http_errors_are_not_retried(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  options.max_attempts = 3u;
  destination(directory, sizeof(directory), file, sizeof(file), "http errors");
  url(address, sizeof(address), hf_tls_port(good), "notfound");
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(404u, result.http_status);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_EQUAL_UINT64(0u, result.bytes);
  url(address, sizeof(address), hf_tls_port(good), "forbidden");
  TEST_ASSERT_EQUAL(HAL_EAUTH, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(403u, result.http_status);
  /* A final status outside 2xx without a redirect is not a success. */
  url(address, sizeof(address), hf_tls_port(good), "notmodified");
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(304u, result.http_status);
  TEST_ASSERT_EQUAL_INT32(0, result.transfer_error);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  assert_entries(directory, 0u);
}

static void test_transient_failures_are_retried(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  options.max_attempts = 3u;
  destination(directory, sizeof(directory), file, sizeof(file), "retries");
  /* 503 from the first backend, success from the second. */
  const hf_action_t busy_then_good[] = {{HF_FORWARD, hf_tls_port(alt), 0u},
                                        {HF_FORWARD, hf_tls_port(good), 0u}};
  plan(busy_then_good, COUNTOF(busy_then_good), HF_CLOSE);
  const uint32_t before = hf_server_connections(router);
  url(address, sizeof(address), hf_server_port(router), "retry");
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  assert_content(file, second_body, sizeof(second_body) - 1u);
  TEST_ASSERT_EQUAL_UINT32(2u, result.attempts);
  TEST_ASSERT_EQUAL_UINT32(before + 2u, hf_server_connections(router));
  /* A truncated body (Content-Length larger than the data), then success. */
  plan(busy_then_good, COUNTOF(busy_then_good), HF_CLOSE);
  url(address, sizeof(address), hf_server_port(router), "flaky");
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  assert_content(file, ok_body, sizeof(ok_body) - 1u);
  TEST_ASSERT_EQUAL_UINT32(2u, result.attempts);
  /* The TLS stream breaks in the middle of the body, then success. */
  const hf_action_t cut_then_good[] = {{HF_CUT, hf_tls_port(good), HOLD_BYTES},
                                       {HF_FORWARD, hf_tls_port(good), 0u}};
  plan(cut_then_good, COUNTOF(cut_then_good), HF_CLOSE);
  url(address, sizeof(address), hf_server_port(router), "large");
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  assert_content(file, large_body, LARGE_BYTES);
  TEST_ASSERT_EQUAL_UINT32(2u, result.attempts);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(file, false));
  /* Exhausted attempts report the last failure and publish nothing. */
  url(address, sizeof(address), hf_tls_port(good), "unavailable");
  TEST_ASSERT_EQUAL(HAL_EAGAIN, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(3u, result.attempts);
  TEST_ASSERT_EQUAL_UINT32(503u, result.http_status);
  url(address, sizeof(address), hf_tls_port(good), "truncated");
  options.max_attempts = 2u;
  TEST_ASSERT_EQUAL(HAL_EIO, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(2u, result.attempts);
  TEST_ASSERT_EQUAL_UINT64(10u, result.bytes);
  assert_entries(directory, 0u);
}

static void test_retry_after_is_honored_within_the_budget(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  options.max_attempts = 2u;
  options.retry_delay_ms = 0u;
  destination(directory, sizeof(directory), file, sizeof(file), "retry after");
  const hf_action_t busy_then_alt[] = {{HF_FORWARD, hf_tls_port(good), 0u},
                                       {HF_FORWARD, hf_tls_port(alt), 0u}};
  plan(busy_then_alt, COUNTOF(busy_then_alt), HF_CLOSE);
  url(address, sizeof(address), hf_server_port(router), "busy");
  uint64_t started = hf_now_ms();
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  TEST_ASSERT_TRUE(hf_now_ms() - started >= 1000u);
  TEST_ASSERT_EQUAL_UINT32(2u, result.attempts);
  assert_content(file, second_body, sizeof(second_body) - 1u);
  /* A Retry-After past the budget ends the call without waiting for it. */
  plan(busy_then_alt, COUNTOF(busy_then_alt), HF_CLOSE);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(file, false));
  options.timeout_ms = 900u;
  started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_EAGAIN, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - started < 900u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  assert_entries(directory, 0u);
}

static void test_timeouts_bound_the_whole_call(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "timeouts");
  plan(NULL, 0u, HF_STALL);
  url(address, sizeof(address), hf_server_port(router), "ok");
  /* Short connection limits retry until the shared budget ends. */
  options.max_attempts = 100u;
  options.retry_delay_ms = 0u;
  options.connect_timeout_ms = 300u;
  options.timeout_ms = 1500u;
  uint64_t started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT, jb_download_file(address, file, &options, &result));
  uint64_t elapsed = hf_now_ms() - started;
  TEST_ASSERT_TRUE(elapsed >= 1400u && elapsed < 4000u);
  TEST_ASSERT_TRUE(result.attempts >= 2u);
  /* The budget also bounds a later attempt that stalls in the body: 503,
   * a delay, then a transfer that stops after part of the body. */
  const hf_action_t busy_then_held[] = {{HF_FORWARD, hf_tls_port(alt), 0u},
                                        {HF_HOLD, hf_tls_port(good), HOLD_BYTES}};
  plan(busy_then_held, COUNTOF(busy_then_held), HF_CLOSE);
  url(address, sizeof(address), hf_server_port(router), "slow");
  options.connect_timeout_ms = 10000u;
  options.timeout_ms = 3000u;
  options.retry_delay_ms = 1000u;
  options.max_attempts = 2u;
  started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT, jb_download_file(address, file, &options, &result));
  elapsed = hf_now_ms() - started;
  TEST_ASSERT_TRUE(elapsed >= 2900u && elapsed < 3700u);
  TEST_ASSERT_EQUAL_UINT32(2u, result.attempts);
  TEST_ASSERT_TRUE(result.bytes > 0u);
  plan(NULL, 0u, HF_STALL);
  url(address, sizeof(address), hf_server_port(router), "ok");
  /* One connection limit longer than the budget: a single attempt, even
   * without a retry delay. */
  options.connect_timeout_ms = 10000u;
  options.retry_delay_ms = 0u;
  options.timeout_ms = 800u;
  started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT, jb_download_file(address, file, &options, &result));
  elapsed = hf_now_ms() - started;
  TEST_ASSERT_TRUE(elapsed >= 700u && elapsed < 3000u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  assert_entries(directory, 0u);
}

static void test_cancellation_removes_the_partial_file(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  probe_t probe = {0};
  options.progress = probe_progress;
  options.canceled = probe_canceled;
  options.context = &probe;
  destination(directory, sizeof(directory), file, sizeof(file), "cancel");
  /* The router holds the transfer after part of the body has arrived. */
  const hf_action_t hold = {HF_HOLD, hf_tls_port(good), HOLD_BYTES};
  plan(&hold, 1u, HF_CLOSE);
  url(address, sizeof(address), hf_server_port(router), "large");
  probe.cancel_at_bytes = 1u;
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(result.bytes > 0u && result.bytes < LARGE_BYTES);
  assert_entries(directory, 0u);
  /* A request canceled before the call starts no attempt. */
  memset(&probe, 0, sizeof(probe));
  probe.cancel = true;
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(0u, result.attempts);
  /* Cancellation reaches a connection that stalls in the TLS handshake, before
   * libcurl would call the progress callback. */
  memset(&probe, 0, sizeof(probe));
  plan(NULL, 0u, HF_STALL);
  url(address, sizeof(address), hf_server_port(router), "ok");
  probe.started_ms = hf_now_ms();
  probe.cancel_at_ms = 300u;
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - probe.started_ms < 2000u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_EQUAL_UINT32(0u, probe.calls);
  /* Cancellation also ends a retry delay. */
  memset(&probe, 0, sizeof(probe));
  probe.started_ms = hf_now_ms();
  probe.cancel_at_ms = 2000u;
  options.max_attempts = 3u;
  options.retry_delay_ms = 30000u;
  url(address, sizeof(address), hf_tls_port(good), "unavailable");
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - probe.started_ms < 10000u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_EQUAL_UINT32(503u, result.http_status);
  TEST_ASSERT_EQUAL_STRING("canceled before another attempt", result.message);
  assert_entries(directory, 0u);
}

static void test_size_limits(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "limits");
  options.max_bytes = 1000u;
  options.max_attempts = 3u;
  /* An announced size over the limit fails before the body. */
  url(address, sizeof(address), hf_tls_port(good), "large");
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_INT32(23, result.transfer_error); /* CURLE_WRITE_ERROR from the header check */
  TEST_ASSERT_EQUAL_STRING("the announced body is larger than max_bytes", result.message);
  TEST_ASSERT_EQUAL_UINT64(0u, result.bytes);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  /* A zero limit refuses an announced byte too, even when the body never
   * arrives: no retry of the interrupted transfer. */
  options.max_bytes = 0u;
  url(address, sizeof(address), hf_tls_port(good), "short-declared");
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_EQUAL_INT32(23, result.transfer_error);
  options.max_bytes = 1000u;
  /* Without Content-Length the file writer enforces it while streaming. */
  url(address, sizeof(address), hf_tls_port(good), "nolength");
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(result.bytes <= 1000u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  /* With max_bytes 0 libcurl has no size limit; the file writer refuses the
   * first byte and its status reaches the caller. */
  options.max_bytes = 0u;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_EQUAL_INT32(23, result.transfer_error); /* CURLE_WRITE_ERROR */
  TEST_ASSERT_EQUAL_UINT64(0u, result.bytes);
  options.max_bytes = NO_LENGTH_BYTES;
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  TEST_ASSERT_EQUAL_UINT64(NO_LENGTH_BYTES, result.bytes);
  assert_content(file, large_body, NO_LENGTH_BYTES);
  assert_entries(directory, 1u);
}

#ifdef __linux__
static void test_write_failure_is_final_and_cleaned_up(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  options.max_attempts = 3u;
  destination(directory, sizeof(directory), file, sizeof(file), "write failure");
  url(address, sizeof(address), hf_tls_port(good), "large");
  set_write_faults(EIO, 0u, 0u, false);
  const hal_status_t status = jb_download_file(address, file, &options, &result);
  set_write_faults(0, 0u, 0u, false);
  TEST_ASSERT_EQUAL(HAL_EIO, status);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  TEST_ASSERT_EQUAL_STRING("cannot write the destination file", result.message);
  /* The status of the file API reaches the caller unchanged. */
  set_write_faults(EPERM, 0u, 0u, false);
  const hal_status_t denied = jb_download_file(address, file, &options, &result);
  set_write_faults(0, 0u, 0u, false);
  TEST_ASSERT_EQUAL(HAL_EPERM, denied);
  /* Bytes a failed write stored before its error count as stored. */
  set_write_faults(ENOSPC, 7u, 0u, false);
  const hal_status_t full = jb_download_file(address, file, &options, &result);
  set_write_faults(0, 0u, 0u, false);
  TEST_ASSERT_EQUAL(HAL_EIO, full);
  TEST_ASSERT_EQUAL_UINT64(7u, result.bytes);
  assert_entries(directory, 0u);
}

/* A write held by the system does not hold the call: it returns at the
 * cancellation or the deadline, and the worker deletes its file later. */
static void test_blocked_attempt_returns_at_its_limits(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  probe_t probe = {0};
  destination(directory, sizeof(directory), file, sizeof(file), "blocked write");
  write_bytes(file, "previous", 8u);
  url(address, sizeof(address), hf_tls_port(good), "ok");
  options.canceled = probe_canceled;
  options.context = &probe;
  probe.started_ms = hf_now_ms();
  probe.cancel_at_ms = 300u;
  set_write_faults(0, 0u, 2000u, false);
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - probe.started_ms < 1000u);
  assert_content(file, "previous", 8u);
  wait_for_entries(directory, 1u, 5000u);
  options.canceled = NULL;
  options.timeout_ms = 800u;
  set_write_faults(0, 0u, 2000u, false);
  const uint64_t started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT, jb_download_file(address, file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - started < 1500u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  wait_for_entries(directory, 1u, 5000u);
  assert_content(file, "previous", 8u);
}

/* A SIGPIPE raised on the transfer's thread never reaches the handler, and
 * the caller keeps its own mask and no pending signal. */
static void test_sigpipe_during_a_transfer_is_consumed(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "sigpipe");
  url(address, sizeof(address), hf_tls_port(good), "ok");
  const sig_atomic_t before = sigpipe_count;
  (void)pthread_mutex_lock(&write_lock);
  const int raised_before = raised_on_write;
  (void)pthread_mutex_unlock(&write_lock);
  set_write_faults(0, 0u, 0u, true);
  const hal_status_t status = jb_download_file(address, file, &options, &result);
  set_write_faults(0, 0u, 0u, false);
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, status, result.message);
  (void)pthread_mutex_lock(&write_lock);
  const int raised = raised_on_write - raised_before;
  (void)pthread_mutex_unlock(&write_lock);
  TEST_ASSERT_EQUAL_INT(1, raised);
  TEST_ASSERT_EQUAL_INT((int)before, (int)sigpipe_count);
  sigset_t pending, mask;
  TEST_ASSERT_EQUAL_INT(0, sigpending(&pending));
  TEST_ASSERT_EQUAL_INT(0, sigismember(&pending, SIGPIPE));
  TEST_ASSERT_EQUAL_INT(0, pthread_sigmask(SIG_BLOCK, NULL, &mask));
  TEST_ASSERT_EQUAL_INT(0, sigismember(&mask, SIGPIPE));
  assert_content(file, ok_body, sizeof(ok_body) - 1u);
}

/* A failed publish step is the result, and the previous file stays. */
static void test_publish_failure_keeps_the_previous_destination(void) {
  char directory[4096], file[4096], address[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "publish failure");
  write_bytes(file, "previous", 8u);
  url(address, sizeof(address), hf_tls_port(good), "ok");
  fail_fsync_errno = EIO;
  const hal_status_t status = jb_download_file(address, file, &options, &result);
  fail_fsync_errno = 0;
  TEST_ASSERT_EQUAL(HAL_EIO, status);
  TEST_ASSERT_EQUAL_STRING("cannot publish the destination file", result.message);
  TEST_ASSERT_EQUAL_UINT64(sizeof(ok_body) - 1u, result.bytes);
  assert_content(file, "previous", 8u);
  assert_entries(directory, 1u);
}

static int slow_lookups_running(void) {
  (void)pthread_mutex_lock(&slow_lock);
  const int running = slow_lookups;
  (void)pthread_mutex_unlock(&slow_lock);
  return running;
}

/* A DNS server slower than the limits does not hold the call: libcurl
 * detaches the resolver thread, which ends when the lookup returns. */
static void test_slow_name_resolution_is_bounded(void) {
  char directory[4096], file[4096];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  probe_t probe = {0};
  destination(directory, sizeof(directory), file, sizeof(file), "slow dns");
  options.connect_timeout_ms = 500u;
  options.timeout_ms = 1500u;
  uint64_t started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT,
                    jb_download_file("https://slow.invalid:1/ok", file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - started < 2000u);
  TEST_ASSERT_EQUAL_UINT32(1u, result.attempts);
  /* libcurl itself ended the attempt (CURLE_OPERATION_TIMEDOUT): the worker
   * did not wait for the resolver, so nothing had to be abandoned. */
  TEST_ASSERT_EQUAL_INT32(28, result.transfer_error);
  options.connect_timeout_ms = 10000u;
  options.timeout_ms = 10000u;
  options.canceled = probe_canceled;
  options.context = &probe;
  probe.started_ms = hf_now_ms();
  probe.cancel_at_ms = 300u;
  started = hf_now_ms();
  TEST_ASSERT_EQUAL(HAL_ECANCELED,
                    jb_download_file("https://slow.invalid:1/ok", file, &options, &result));
  TEST_ASSERT_TRUE(hf_now_ms() - started < 2000u);
  /* The stopped worker ended within its grace (CURLE_ABORTED_BY_CALLBACK). */
  TEST_ASSERT_EQUAL_INT32(42, result.transfer_error);
  TEST_ASSERT_TRUE(slow_lookups_running() > 0);
  for (uint32_t waited = 0; slow_lookups_running() > 0 && waited < 10000u; waited += 50u)
    hf_sleep_ms(50u);
  TEST_ASSERT_EQUAL_INT(0, slow_lookups_running());
  /* The detached threads finish after leaving the wrapper. */
  hf_sleep_ms(200u);
  assert_entries(directory, 0u);
}
#endif

static void set_environment(const char *name, const char *value) {
#ifdef _WIN32
  TEST_ASSERT_EQUAL_INT(0, _putenv_s(name, value != NULL ? value : ""));
#else
  TEST_ASSERT_EQUAL_INT(0, value != NULL ? setenv(name, value, 1) : unsetenv(name));
#endif
}

static void test_proxy_is_explicit(void) {
  static const char *const variables[] = {"https_proxy", "HTTPS_PROXY", "ALL_PROXY", "all_proxy"};
  char directory[4096], file[4096], address[512], proxy_url[128], expected[128], request[512];
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  destination(directory, sizeof(directory), file, sizeof(file), "proxy");
  url(address, sizeof(address), hf_tls_port(good), "ok");
  (void)snprintf(proxy_url, sizeof(proxy_url), "http://127.0.0.1:%u",
                 (unsigned)hf_server_port(proxy));
  (void)snprintf(expected, sizeof(expected), "CONNECT localhost:%u HTTP/1.1",
                 (unsigned)hf_tls_port(good));
  options.proxy = proxy_url;
  uint32_t before = hf_server_connections(proxy);
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  TEST_ASSERT_EQUAL_UINT32(before + 1u, hf_server_connections(proxy));
  hf_server_last_request(proxy, request, sizeof(request));
  TEST_ASSERT_EQUAL_STRING(expected, request);
  assert_content(file, ok_body, sizeof(ok_body) - 1u);
  /* Proxy variables of the environment are ignored without an explicit proxy,
   * and no_proxy does not bypass an explicit one. */
  for (size_t index = 0; index < COUNTOF(variables); ++index)
    set_environment(variables[index], proxy_url);
  options.proxy = NULL;
  before = hf_server_connections(proxy);
  const hal_status_t direct = jb_download_file(address, file, &options, &result);
  const uint32_t after_direct = hf_server_connections(proxy);
  set_environment("NO_PROXY", "localhost");
  set_environment("no_proxy", "localhost");
  options.proxy = proxy_url;
  const hal_status_t proxied = jb_download_file(address, file, &options, &result);
  const uint32_t after_proxied = hf_server_connections(proxy);
  for (size_t index = 0; index < COUNTOF(variables); ++index)
    set_environment(variables[index], NULL);
  set_environment("NO_PROXY", NULL);
  set_environment("no_proxy", NULL);
  TEST_ASSERT_EQUAL(HAL_OK, direct);
  TEST_ASSERT_EQUAL_UINT32(before, after_direct);
  TEST_ASSERT_EQUAL(HAL_OK, proxied);
  TEST_ASSERT_EQUAL_UINT32(before + 1u, after_proxied);
  /* 407 from the proxy is final and reported as the proxy status. */
  char with_credentials[160];
  (void)snprintf(with_credentials, sizeof(with_credentials), "http://user:pass@127.0.0.1:%u",
                 (unsigned)hf_server_port(proxy));
  hf_proxy_require(proxy, "Basic realm=\"jhbase\"", "Basic dXNlcjpwYXNz");
  options.max_attempts = 3u;
  const hal_status_t refused = jb_download_file(address, file, &options, &result);
  const uint32_t refused_status = result.proxy_status, refused_attempts = result.attempts;
  options.proxy = with_credentials;
  const hal_status_t basic = jb_download_file(address, file, &options, &result);
  /* Only Basic is built in, so a proxy offering only Digest refuses, alike on
   * every platform. */
  hf_proxy_require(proxy, "Digest realm=\"jhbase\", nonce=\"0123456789abcdef\", qop=\"auth\"",
                   "Digest username=\"user\"");
  options.max_attempts = 1u;
  const hal_status_t digest = jb_download_file(address, file, &options, &result);
  const uint32_t digest_status = result.proxy_status;
  hf_proxy_require(proxy, NULL, NULL);
  TEST_ASSERT_EQUAL(HAL_EAUTH, refused);
  TEST_ASSERT_EQUAL_UINT32(407u, refused_status);
  TEST_ASSERT_EQUAL_UINT32(1u, refused_attempts);
  TEST_ASSERT_EQUAL(HAL_OK, basic);
  TEST_ASSERT_EQUAL(HAL_EAUTH, digest);
  TEST_ASSERT_EQUAL_UINT32(407u, digest_status);
  /* A proxy that cannot reach the server answers 502, a transient failure. */
  char closed[512];
  url(closed, sizeof(closed), hf_unused_port(), "ok");
  options.proxy = proxy_url;
  options.max_attempts = 2u;
  TEST_ASSERT_EQUAL(HAL_EAGAIN, jb_download_file(closed, file, &options, &result));
  TEST_ASSERT_EQUAL_UINT32(502u, result.proxy_status);
  TEST_ASSERT_EQUAL_UINT32(2u, result.attempts);
  /* Without a scheme the proxy is HTTP, whatever its host name says. */
  char bare[128];
  (void)snprintf(bare, sizeof(bare), "ftp.localhost:%u", (unsigned)hf_server_port(proxy));
  options.proxy = bare;
  before = hf_server_connections(proxy);
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_download_file(address, file, &options, &result),
                            result.message);
  TEST_ASSERT_EQUAL_UINT32(before + 1u, hf_server_connections(proxy));
  assert_entries(directory, 1u);
}

typedef struct {
  char address[512];
  char file[4096];
  hal_status_t status;
} parallel_t;

static void parallel_download(void *context) {
  parallel_t *work = context;
  jb_download_options_t options = defaults();
  jb_download_result_t result;
  work->status = jb_download_file(work->address, work->file, &options, &result);
}

static void test_independent_calls_run_concurrently(void) {
  char directory[4096], unused[4096];
  parallel_t work[2];
  hf_thread_handle_t *threads[COUNTOF(work)];
  destination(directory, sizeof(directory), unused, sizeof(unused), "parallel");
  for (size_t index = 0; index < COUNTOF(work); ++index) {
    char name[32];
    (void)snprintf(name, sizeof(name), "copy %u.bin", (unsigned)index);
    url(work[index].address, sizeof(work[index].address),
        index == 0u ? hf_tls_port(good) : hf_tls_port(alt), "large");
    join(work[index].file, sizeof(work[index].file), directory, name);
    work[index].status = HAL_NONE;
    threads[index] = hf_thread_start(parallel_download, &work[index]);
    TEST_ASSERT_NOT_NULL(threads[index]);
  }
  for (size_t index = 0; index < COUNTOF(work); ++index)
    hf_thread_join(threads[index]);
  for (size_t index = 0; index < COUNTOF(work); ++index) {
    TEST_ASSERT_EQUAL(HAL_OK, work[index].status);
    assert_content(work[index].file, large_body, LARGE_BYTES);
  }
  assert_entries(directory, COUNTOF(work));
}

static void prepare_responses(void) {
  char location[256];
  write_response(www_good, "ok", "200 OK", "", ok_body, sizeof(ok_body) - 1u,
                 (long)sizeof(ok_body) - 1);
  write_response(www_good, "empty", "200 OK", "", NULL, 0u, 0);
  write_response(www_good, "notfound", "404 Not Found", "", "missing\n", 8u, 8);
  write_response(www_good, "forbidden", "403 Forbidden", "", "denied\n", 7u, 7);
  write_response(www_good, "notmodified", "304 Not Modified", "", NULL, 0u, -1);
  write_response(www_good, "unavailable", "503 Service Unavailable", "", "busy\n", 5u, 5);
  write_response(www_good, "truncated", "200 OK", "", "0123456789", 10u, 1000);
  write_response(www_good, "short-declared", "200 OK", "", NULL, 0u, 1);
  write_response(www_good, "large", "200 OK", "", large_body, LARGE_BYTES, (long)LARGE_BYTES);
  write_response(www_good, "nolength", "200 OK", "", large_body, NO_LENGTH_BYTES, -1);
  (void)snprintf(location, sizeof(location), "Location: https://localhost:%u/ok\n",
                 (unsigned)hf_tls_port(good));
  write_response(www_good, "redirect-https", "302 Found", location, NULL, 0u, 0);
  (void)snprintf(location, sizeof(location), "Location: http://localhost:%u/ok\n",
                 (unsigned)hf_server_port(crl_server));
  write_response(www_good, "redirect-http", "302 Found", location, NULL, 0u, 0);
  write_response(www_good, "loop", "302 Found", "Location: /loop\n", NULL, 0u, 0);
  /* Same names, different behavior: the router chooses the backend. */
  write_response(www_good, "retry", "200 OK", "", second_body, sizeof(second_body) - 1u,
                 (long)sizeof(second_body) - 1);
  write_response(www_alt, "retry", "503 Service Unavailable", "", "busy\n", 5u, 5);
  write_response(www_good, "flaky", "200 OK", "", ok_body, sizeof(ok_body) - 1u,
                 (long)sizeof(ok_body) - 1);
  write_response(www_alt, "flaky", "200 OK", "", "0123456789", 10u, 1000);
  write_response(www_good, "busy", "503 Service Unavailable", "Retry-After: 1\n", "busy\n", 5u, 5);
  write_response(www_alt, "busy", "200 OK", "", second_body, sizeof(second_body) - 1u,
                 (long)sizeof(second_body) - 1);
  write_response(www_alt, "large", "200 OK", "", large_body, LARGE_BYTES, (long)LARGE_BYTES);
  write_response(www_alt, "slow", "503 Service Unavailable", "", "busy\n", 5u, 5);
  write_response(www_good, "slow", "200 OK", "", large_body, LARGE_BYTES, (long)LARGE_BYTES);
}

static bool serve_crl(const char *name) {
  char path[4096], route[64];
  jb_file_data_t data = {0};
  size_t required = 0;
  if (jb_fs_path_join(pki_dir, name, path, sizeof(path), &required) != HAL_OK ||
      jb_file_read(path, 1u << 20, &data) != HAL_OK)
    return false;
  (void)snprintf(route, sizeof(route), "/%s", name);
  const bool served = hf_http_set(crl_server, route, data.data, data.length);
  jb_file_release(&data);
  return served;
}

static hf_tls_t *start_tls(const char *www, const char *certificate, const char *chain) {
  hf_tls_t *tls = hf_tls_start(openssl_tool, www, certificate, "../pki/leaf.key", chain,
                               diagnostics, sizeof(diagnostics));
  if (tls == NULL)
    fprintf(stderr, "openssl s_server with %s did not start:\n%s\n", certificate, diagnostics);
  return tls;
}

/* Copy a PKI file to a path with spaces and non-ASCII characters. */
static bool copy_ca(const char *name, const char *target_name, char *target, size_t capacity) {
  char source[4096], folder[4096];
  jb_file_data_t data = {0};
  size_t required = 0;
  bool changed = false;
  const bool copied =
      jb_fs_path_join(pki_dir, name, source, sizeof(source), &required) == HAL_OK &&
      jb_file_read(source, 1u << 20, &data) == HAL_OK &&
      jb_fs_path_join(test_root, "zaufane CA \xc4\x85\xc4\x99", folder, sizeof(folder),
                      &required) == HAL_OK &&
      jb_fs_path_join(folder, target_name, target, capacity, &required) == HAL_OK &&
      jb_file_write_atomic(target, data.data, data.length, false, &changed) == HAL_OK;
  jb_file_release(&data);
  return copied;
}

static bool start_fixture(void) {
  size_t required = 0;
  if (jb_fs_path_join(test_root, "pki", pki_dir, sizeof(pki_dir), &required) != HAL_OK ||
      jb_fs_path_join(test_root, "www-good", www_good, sizeof(www_good), &required) != HAL_OK ||
      jb_fs_path_join(test_root, "www-alt", www_alt, sizeof(www_alt), &required) != HAL_OK ||
      jb_fs_path_join(test_root, "out", out_dir, sizeof(out_dir), &required) != HAL_OK ||
      jb_fs_mkdir(pki_dir, true) != HAL_OK || jb_fs_mkdir(www_good, true) != HAL_OK ||
      jb_fs_mkdir(www_alt, true) != HAL_OK || jb_fs_mkdir(out_dir, true) != HAL_OK)
    return false;
  crl_server = hf_http_start();
  crl_staller = hf_router_start();
  router = hf_router_start();
  proxy = hf_proxy_start();
  if (crl_server == NULL || crl_staller == NULL || router == NULL || proxy == NULL)
    return false;
  const hf_action_t stall = {HF_STALL, 0u, 0u};
  hf_router_plan(crl_staller, NULL, 0u, stall);
  if (hf_pki_create(openssl_tool, pki_dir, hf_server_port(crl_server), hf_server_port(crl_staller),
                    diagnostics, sizeof(diagnostics)) != HAL_OK) {
    fprintf(stderr, "Cannot create the test PKI with %s:\n%s\n", openssl_tool, diagnostics);
    return false;
  }
  if (!serve_crl("root.crl") || !serve_crl("other.crl") ||
      !copy_ca("root.pem", "g\xc5\x82\xc3\xb3wny CA.pem", ca_file, sizeof(ca_file)) ||
      !copy_ca("other.pem", "obcy CA.pem", other_ca_file, sizeof(other_ca_file)))
    return false;
  good = start_tls(www_good, "../pki/leaf.pem", NULL);
  alt = start_tls(www_alt, "../pki/leaf.pem", NULL);
  wrong = start_tls(www_good, "../pki/wrong.pem", NULL);
  untrusted = start_tls(www_good, "../pki/untrusted.pem", NULL);
  revoked = start_tls(www_good, "../pki/revoked.pem", NULL);
  stalled1 = start_tls(www_good, "../pki/stall1.pem", "../pki/stallca.pem");
  stalled2 = start_tls(www_good, "../pki/stall2.pem", "../pki/stallca.pem");
  return good != NULL && alt != NULL && wrong != NULL && untrusted != NULL && revoked != NULL &&
         stalled1 != NULL && stalled2 != NULL;
}

static void stop_fixture(void) {
  hf_tls_stop(&good);
  hf_tls_stop(&alt);
  hf_tls_stop(&wrong);
  hf_tls_stop(&untrusted);
  hf_tls_stop(&revoked);
  hf_tls_stop(&stalled1);
  hf_tls_stop(&stalled2);
  hf_server_stop(&crl_staller);
  hf_server_stop(&router);
  hf_server_stop(&proxy);
  hf_server_stop(&crl_server);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: test_download WORK_PREFIX OPENSSL\n");
    return 2;
  }
  openssl_tool = argv[2];
  char cwd[4096], name[4096];
  size_t required = 0;
#ifdef _WIN32
  const unsigned long process = GetCurrentProcessId();
#else
  const unsigned long process = (unsigned long)getpid();
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = count_sigpipe;
  (void)sigemptyset(&action.sa_mask);
  (void)sigaction(SIGPIPE, &action, NULL);
#endif
  const int count = snprintf(name, sizeof(name), "%s-%lu", argv[1], process);
  large_body = malloc(LARGE_BYTES);
  if (large_body == NULL || count < 0 || (size_t)count >= sizeof(name) ||
      jb_fs_cwd(cwd, sizeof(cwd), &required) != HAL_OK ||
      jb_fs_path_join(cwd, name, test_root, sizeof(test_root), &required) != HAL_OK ||
      jb_fs_mkdir(test_root, true) != HAL_OK || !hf_init()) {
    fprintf(stderr, "Cannot prepare the download fixture\n");
    return 2;
  }
  /* Printable bytes without CR or ^Z, with a line every 64 characters. */
  for (size_t index = 0; index < LARGE_BYTES; ++index)
    large_body[index] =
        index % 64u == 63u ? '\n' : (char)('a' + (index * 7u + index / 4096u) % 26u);
  const hal_status_t initialized = jb_download_global_init();
  if (initialized != HAL_OK || !start_fixture()) {
    fprintf(stderr, "Cannot start the download fixture (%d)\n", (int)initialized);
    stop_fixture();
    if (initialized == HAL_OK)
      jb_download_global_cleanup();
    return 2;
  }
  prepare_responses();
  UNITY_BEGIN();
  RUN_TEST(test_info_reports_the_linked_library);
  RUN_TEST(test_arguments_and_urls_are_checked_first);
  RUN_TEST(test_trusted_download_publishes_exact_bytes);
  RUN_TEST(test_failure_keeps_the_previous_destination);
  RUN_TEST(test_redirects_follow_https_only);
  RUN_TEST(test_verification_failures_are_final);
#ifdef _WIN32
  RUN_TEST(test_schannel_checks_revocation);
  RUN_TEST(test_blocking_revocation_check_is_bounded);
#endif
  RUN_TEST(test_http_errors_are_not_retried);
  RUN_TEST(test_transient_failures_are_retried);
  RUN_TEST(test_retry_after_is_honored_within_the_budget);
  RUN_TEST(test_timeouts_bound_the_whole_call);
  RUN_TEST(test_cancellation_removes_the_partial_file);
  RUN_TEST(test_size_limits);
#ifdef __linux__
  RUN_TEST(test_write_failure_is_final_and_cleaned_up);
  RUN_TEST(test_sigpipe_during_a_transfer_is_consumed);
  RUN_TEST(test_publish_failure_keeps_the_previous_destination);
  RUN_TEST(test_blocked_attempt_returns_at_its_limits);
  RUN_TEST(test_slow_name_resolution_is_bounded);
#endif
  RUN_TEST(test_proxy_is_explicit);
  RUN_TEST(test_independent_calls_run_concurrently);
  int failures = UNITY_END();
  stop_fixture();
  jb_download_global_cleanup();
  hf_shutdown();
  free(large_body);
#ifdef __linux__
  if (sigpipe_count != 0) {
    fprintf(stderr, "SIGPIPE reached the process %d times\n", (int)sigpipe_count);
    ++failures;
  }
#endif
  if (failures == 0 && jb_fs_remove(test_root, true) != HAL_OK)
    return 3;
  return failures == 0 ? 0 : 1;
}

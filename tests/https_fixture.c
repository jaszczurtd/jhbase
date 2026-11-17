#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "https_fixture.h"
#include "jb/array.h"
#include "jb/file.h"
#include "jb/filesystem.h"
#include "jb/process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* windows.h then leaves out the old winsock.h, whatever the include order. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET hf_socket_t;
#define HF_NO_SOCKET INVALID_SOCKET
typedef HANDLE hf_thread_t;
typedef CRITICAL_SECTION hf_mutex_t;
#else
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int hf_socket_t;
#define HF_NO_SOCKET (-1)
typedef pthread_t hf_thread_t;
typedef pthread_mutex_t hf_mutex_t;
#endif

enum {
  HF_POLL_MS = 100,
  HF_PLAN_CAPACITY = 16,
  HF_PATHS = 8,
  HF_LINE = 512,
  HF_HEADER = 4096,
  HF_TAIL = 4096,
  HF_HEADER_WAIT_MS = 10000,
  HF_TLS_START_MS = 20000
};
typedef enum { HF_ROUTER = 1, HF_PROXY, HF_HTTP } hf_mode_t;

static void hf_mutex_init(hf_mutex_t *mutex) {
#ifdef _WIN32
  InitializeCriticalSection(mutex);
#else
  (void)pthread_mutex_init(mutex, NULL);
#endif
}

static void hf_mutex_destroy(hf_mutex_t *mutex) {
#ifdef _WIN32
  DeleteCriticalSection(mutex);
#else
  (void)pthread_mutex_destroy(mutex);
#endif
}

static void hf_lock(hf_mutex_t *mutex) {
#ifdef _WIN32
  EnterCriticalSection(mutex);
#else
  (void)pthread_mutex_lock(mutex);
#endif
}

static void hf_unlock(hf_mutex_t *mutex) {
#ifdef _WIN32
  LeaveCriticalSection(mutex);
#else
  (void)pthread_mutex_unlock(mutex);
#endif
}

uint64_t hf_now_ms(void) {
#ifdef _WIN32
  return (uint64_t)GetTickCount64();
#else
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

void hf_sleep_ms(uint32_t milliseconds) {
#ifdef _WIN32
  Sleep(milliseconds);
#else
  struct timespec duration = {(time_t)(milliseconds / 1000u),
                              (long)(milliseconds % 1000u) * 1000000L};
  while (nanosleep(&duration, &duration) != 0) {
  }
#endif
}

/* Keep the last capacity - 1 bytes of a stream, NUL-terminated. */
static void hf_tail_append(char *tail, size_t capacity, size_t *length, const unsigned char *data,
                           size_t count) {
  if (count >= capacity - 1u) {
    data += count - (capacity - 1u);
    count = capacity - 1u;
  }
  if (*length + count > capacity - 1u) {
    const size_t drop = *length + count - (capacity - 1u);
    memmove(tail, tail + drop, *length - drop);
    *length -= drop;
  }
  memcpy(tail + *length, data, count);
  *length += count;
  tail[*length] = '\0';
}

static void hf_copy(char *out, size_t capacity, const char *text) {
  const size_t length = strlen(text) < capacity - 1u ? strlen(text) : capacity - 1u;
  memcpy(out, text, length);
  out[length] = '\0';
}

bool hf_init(void) {
#ifdef _WIN32
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  return true;
#endif
}

void hf_shutdown(void) {
#ifdef _WIN32
  (void)WSACleanup();
#endif
}

static void hf_close(hf_socket_t socket_handle) {
#ifdef _WIN32
  (void)closesocket(socket_handle);
#else
  (void)close(socket_handle);
#endif
}

/* 127.0.0.1:<service> numerically, without name resolution. */
static struct addrinfo *hf_loopback(const char *service) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
  struct addrinfo *address = NULL;
  return getaddrinfo("127.0.0.1", service, &hints, &address) == 0 ? address : NULL;
}

static hf_socket_t hf_listen(uint16_t *port) {
  struct addrinfo *address = hf_loopback("0");
  if (address == NULL)
    return HF_NO_SOCKET;
  hf_socket_t listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
  if (listener != HF_NO_SOCKET &&
      (bind(listener, address->ai_addr, (socklen_t)address->ai_addrlen) != 0 ||
       listen(listener, 16) != 0)) {
    hf_close(listener);
    listener = HF_NO_SOCKET;
  }
  freeaddrinfo(address);
  if (listener == HF_NO_SOCKET)
    return HF_NO_SOCKET;
  struct sockaddr_storage bound;
  socklen_t length = (socklen_t)sizeof(bound);
  char service[16];
  if (getsockname(listener, (struct sockaddr *)&bound, &length) != 0 ||
      getnameinfo((struct sockaddr *)&bound, length, NULL, 0, service, (socklen_t)sizeof(service),
                  NI_NUMERICSERV) != 0) {
    hf_close(listener);
    return HF_NO_SOCKET;
  }
  *port = (uint16_t)strtoul(service, NULL, 10);
  return listener;
}

static hf_socket_t hf_connect(uint16_t port) {
  char service[16];
  (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
  struct addrinfo *address = hf_loopback(service);
  if (address == NULL)
    return HF_NO_SOCKET;
  hf_socket_t connection = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
  if (connection != HF_NO_SOCKET &&
      connect(connection, address->ai_addr, (socklen_t)address->ai_addrlen) != 0) {
    hf_close(connection);
    connection = HF_NO_SOCKET;
  }
  freeaddrinfo(address);
  return connection;
}

/* Wait up to one poll interval; negative on a socket error. */
static int hf_wait(hf_socket_t first, hf_socket_t second, bool *first_ready, bool *second_ready) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(first, &readable);
  if (second != HF_NO_SOCKET)
    FD_SET(second, &readable);
  struct timeval timeout = {0, HF_POLL_MS * 1000};
#ifdef _WIN32
  const int count = select(0, &readable, NULL, NULL, &timeout);
#else
  const int highest = second != HF_NO_SOCKET && second > first ? second : first;
  const int count = select(highest + 1, &readable, NULL, NULL, &timeout);
#endif
  *first_ready = count > 0 && FD_ISSET(first, &readable);
  *second_ready = count > 0 && second != HF_NO_SOCKET && FD_ISSET(second, &readable);
  return count;
}

static long hf_recv(hf_socket_t connection, char *buffer, size_t capacity) {
#ifdef _WIN32
  return recv(connection, buffer, (int)capacity, 0);
#else
  return (long)recv(connection, buffer, capacity, 0);
#endif
}

static bool hf_send_all(hf_socket_t connection, const char *data, size_t length) {
  while (length > 0u) {
#ifdef _WIN32
    const int sent = send(connection, data, length > 65536u ? 65536 : (int)length, 0);
#else
    const ssize_t sent = send(connection, data, length, MSG_NOSIGNAL);
#endif
    if (sent <= 0)
      return false;
    data += sent;
    length -= (size_t)sent;
  }
  return true;
}

struct hf_server {
  hf_mutex_t lock;
  hf_thread_handle_t *thread;
  hf_socket_t listener;
  uint16_t port;
  hf_mode_t mode;
  bool stop;
  uint32_t connections;
  hf_action_t plan[HF_PLAN_CAPACITY];
  size_t plan_count, plan_next;
  hf_action_t fallback;
  char last_request[HF_LINE];
  char challenge[HF_LINE];
  char credentials[HF_LINE];
  struct {
    char path[64];
    unsigned char *data;
    size_t length;
  } paths[HF_PATHS];
  size_t path_count;
};

static bool hf_stopping(hf_server_t *server) {
  hf_lock(&server->lock);
  const bool stop = server->stop;
  hf_unlock(&server->lock);
  return stop;
}

static void hf_drain(hf_server_t *server, hf_socket_t client) {
  char buffer[4096];
  while (!hf_stopping(server)) {
    bool ready = false, unused = false;
    if (hf_wait(client, HF_NO_SOCKET, &ready, &unused) < 0)
      return;
    if (ready && hf_recv(client, buffer, sizeof(buffer)) <= 0)
      return;
  }
}

/* Relay both directions; after limit bytes towards the client either return
 * (cut) or stop relaying and keep the client connected (hold). */
static void hf_relay(hf_server_t *server, hf_socket_t client, hf_socket_t backend, size_t limit,
                     bool hold) {
  char buffer[16384];
  size_t forwarded = 0;
  while (!hf_stopping(server)) {
    bool client_ready = false, backend_ready = false;
    if (hf_wait(client, backend, &client_ready, &backend_ready) < 0)
      return;
    if (client_ready) {
      const long count = hf_recv(client, buffer, sizeof(buffer));
      if (count <= 0 || !hf_send_all(backend, buffer, (size_t)count))
        return;
    }
    if (backend_ready) {
      const long count = hf_recv(backend, buffer, sizeof(buffer));
      if (count <= 0)
        return;
      const size_t allowed = (size_t)count < limit - forwarded ? (size_t)count : limit - forwarded;
      if (!hf_send_all(client, buffer, allowed))
        return;
      forwarded += allowed;
      if (forwarded >= limit) {
        if (hold)
          hf_drain(server, client);
        return;
      }
    }
  }
}

/* Read up to the blank line that ends an HTTP header. */
static bool hf_read_header(hf_server_t *server, hf_socket_t client, char *buffer, size_t capacity) {
  size_t used = 0;
  buffer[0] = '\0';
  for (uint32_t waited = 0; waited < HF_HEADER_WAIT_MS && !hf_stopping(server);) {
    bool ready = false, unused = false;
    if (hf_wait(client, HF_NO_SOCKET, &ready, &unused) < 0)
      return false;
    if (!ready) {
      waited += HF_POLL_MS;
      continue;
    }
    const long count = hf_recv(client, buffer + used, capacity - 1u - used);
    if (count <= 0)
      return false;
    used += (size_t)count;
    buffer[used] = '\0';
    if (strstr(buffer, "\r\n\r\n") != NULL)
      return true;
    if (used >= capacity - 1u)
      return false;
  }
  return false;
}

static void hf_record(hf_server_t *server, const char *header) {
  char line[HF_LINE];
  const char *end = strstr(header, "\r\n");
  size_t length = end != NULL ? (size_t)(end - header) : strlen(header);
  if (length >= sizeof(line))
    length = sizeof(line) - 1u;
  memcpy(line, header, length);
  line[length] = '\0';
  hf_lock(&server->lock);
  hf_copy(server->last_request, sizeof(server->last_request), line);
  hf_unlock(&server->lock);
}

static void hf_route(hf_server_t *server, hf_socket_t client) {
  hf_lock(&server->lock);
  const hf_action_t action =
      server->plan_next < server->plan_count ? server->plan[server->plan_next++] : server->fallback;
  hf_unlock(&server->lock);
  if (action.kind == HF_STALL) {
    hf_drain(server, client);
  } else if (action.kind == HF_FORWARD || action.kind == HF_CUT || action.kind == HF_HOLD) {
    const hf_socket_t backend = hf_connect(action.port);
    if (backend != HF_NO_SOCKET) {
      hf_relay(server, client, backend, action.kind == HF_FORWARD ? SIZE_MAX : action.cut_after,
               action.kind == HF_HOLD);
      hf_close(backend);
    }
  }
}

static void hf_tunnel(hf_server_t *server, hf_socket_t client) {
  static const char prefix[] = "CONNECT localhost:";
  static const char established[] = "HTTP/1.1 200 Connection established\r\n\r\n";
  static const char refused[] = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
  char header[HF_HEADER];
  if (!hf_read_header(server, client, header, sizeof(header)))
    return;
  hf_record(server, header);
  char expected[HF_LINE + 32], challenge[HF_LINE + 160];
  hf_lock(&server->lock);
  const bool required = server->challenge[0] != '\0';
  (void)snprintf(expected, sizeof(expected), "\r\nProxy-Authorization: %s", server->credentials);
  const int written = snprintf(challenge, sizeof(challenge),
                               "HTTP/1.1 407 Proxy Authentication Required\r\n"
                               "Proxy-Authenticate: %s\r\n"
                               "Content-Length: 0\r\nConnection: close\r\n\r\n",
                               server->challenge);
  hf_unlock(&server->lock);
  if (required && strstr(header, expected) == NULL) {
    if (written > 0 && (size_t)written < sizeof(challenge))
      (void)hf_send_all(client, challenge, (size_t)written);
    return;
  }
  char *end = NULL;
  const unsigned long port = strncmp(header, prefix, sizeof(prefix) - 1u) == 0
                                 ? strtoul(header + sizeof(prefix) - 1u, &end, 10)
                                 : 0u;
  const hf_socket_t backend = port > 0u && port <= 65535u && end != NULL && *end == ' '
                                  ? hf_connect((uint16_t)port)
                                  : HF_NO_SOCKET;
  if (backend == HF_NO_SOCKET) {
    (void)hf_send_all(client, refused, sizeof(refused) - 1u);
    return;
  }
  if (hf_send_all(client, established, sizeof(established) - 1u))
    hf_relay(server, client, backend, SIZE_MAX, false);
  hf_close(backend);
}

static void hf_respond(hf_server_t *server, hf_socket_t client) {
  static const char missing[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                "Connection: close\r\n\r\n";
  char header[HF_HEADER];
  if (!hf_read_header(server, client, header, sizeof(header)))
    return;
  hf_record(server, header);
  const unsigned char *data = NULL;
  size_t length = 0;
  bool found = false;
  if (strncmp(header, "GET ", 4u) == 0) {
    const char *path = header + 4;
    const char *end = strchr(path, ' ');
    const size_t path_length = end != NULL ? (size_t)(end - path) : 0u;
    hf_lock(&server->lock);
    for (size_t index = 0; index < server->path_count && !found; ++index) {
      if (strlen(server->paths[index].path) == path_length &&
          memcmp(server->paths[index].path, path, path_length) == 0) {
        data = server->paths[index].data;
        length = server->paths[index].length;
        found = true;
      }
    }
    hf_unlock(&server->lock);
  }
  if (!found) {
    (void)hf_send_all(client, missing, sizeof(missing) - 1u);
    return;
  }
  char response[256];
  const int written = snprintf(response, sizeof(response),
                               "HTTP/1.1 200 OK\r\nContent-Type: application/pkix-crl\r\n"
                               "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                               (unsigned long)length);
  if (written > 0 && (size_t)written < sizeof(response) &&
      hf_send_all(client, response, (size_t)written))
    (void)hf_send_all(client, (const char *)data, length);
}

static void hf_server_loop(void *context) {
  hf_server_t *server = context;
  while (!hf_stopping(server)) {
    bool ready = false, unused = false;
    if (hf_wait(server->listener, HF_NO_SOCKET, &ready, &unused) <= 0 || !ready)
      continue;
    const hf_socket_t client = accept(server->listener, NULL, NULL);
    if (client == HF_NO_SOCKET)
      continue;
    hf_lock(&server->lock);
    ++server->connections;
    hf_unlock(&server->lock);
    if (server->mode == HF_ROUTER)
      hf_route(server, client);
    else if (server->mode == HF_PROXY)
      hf_tunnel(server, client);
    else
      hf_respond(server, client);
    hf_close(client);
  }
}

struct hf_thread {
  hf_thread_t thread;
  void (*work)(void *context);
  void *context;
};

#ifdef _WIN32
static unsigned __stdcall hf_thread_entry(void *context) {
  hf_thread_handle_t *thread = context;
  thread->work(thread->context);
  return 0;
}
#else
static void *hf_thread_entry(void *context) {
  hf_thread_handle_t *thread = context;
  thread->work(thread->context);
  return NULL;
}
#endif

hf_thread_handle_t *hf_thread_start(void (*work)(void *context), void *context) {
  hf_thread_handle_t *thread = calloc(1u, sizeof(*thread));
  if (thread == NULL)
    return NULL;
  thread->work = work;
  thread->context = context;
#ifdef _WIN32
  const uintptr_t handle = _beginthreadex(NULL, 0, hf_thread_entry, thread, 0, NULL);
  const bool started = handle != 0u;
  thread->thread = (HANDLE)handle;
#else
  const bool started = pthread_create(&thread->thread, NULL, hf_thread_entry, thread) == 0;
#endif
  if (!started) {
    free(thread);
    return NULL;
  }
  return thread;
}

void hf_thread_join(hf_thread_handle_t *thread) {
#ifdef _WIN32
  (void)WaitForSingleObject(thread->thread, INFINITE);
  (void)CloseHandle(thread->thread);
#else
  (void)pthread_join(thread->thread, NULL);
#endif
  free(thread);
}

static hf_server_t *hf_server_start(hf_mode_t mode) {
  hf_server_t *server = calloc(1u, sizeof(*server));
  if (server == NULL)
    return NULL;
  server->mode = mode;
  server->fallback.kind = HF_CLOSE;
  server->listener = hf_listen(&server->port);
  if (server->listener == HF_NO_SOCKET) {
    free(server);
    return NULL;
  }
  hf_mutex_init(&server->lock);
  server->thread = hf_thread_start(hf_server_loop, server);
  if (server->thread == NULL) {
    hf_close(server->listener);
    hf_mutex_destroy(&server->lock);
    free(server);
    return NULL;
  }
  return server;
}

hf_server_t *hf_router_start(void) { return hf_server_start(HF_ROUTER); }

hf_server_t *hf_proxy_start(void) { return hf_server_start(HF_PROXY); }

hf_server_t *hf_http_start(void) { return hf_server_start(HF_HTTP); }

void hf_router_plan(hf_server_t *server, const hf_action_t *actions, size_t count,
                    hf_action_t fallback) {
  hf_lock(&server->lock);
  server->plan_count = count < HF_PLAN_CAPACITY ? count : HF_PLAN_CAPACITY;
  if (server->plan_count > 0u)
    memcpy(server->plan, actions, server->plan_count * sizeof(actions[0]));
  server->plan_next = 0;
  server->fallback = fallback;
  hf_unlock(&server->lock);
}

bool hf_http_set(hf_server_t *server, const char *path, const unsigned char *data, size_t length) {
  if (strlen(path) >= sizeof(server->paths[0].path))
    return false;
  unsigned char *copy = malloc(length > 0u ? length : 1u);
  if (copy == NULL)
    return false;
  memcpy(copy, data, length);
  hf_lock(&server->lock);
  const bool stored = server->path_count < HF_PATHS;
  if (stored) {
    hf_copy(server->paths[server->path_count].path, sizeof(server->paths[0].path), path);
    server->paths[server->path_count].data = copy;
    server->paths[server->path_count].length = length;
    ++server->path_count;
  }
  hf_unlock(&server->lock);
  if (!stored)
    free(copy);
  return stored;
}

void hf_proxy_require(hf_server_t *server, const char *challenge, const char *credentials) {
  hf_lock(&server->lock);
  hf_copy(server->challenge, sizeof(server->challenge), challenge != NULL ? challenge : "");
  hf_copy(server->credentials, sizeof(server->credentials),
          challenge != NULL && credentials != NULL ? credentials : "");
  hf_unlock(&server->lock);
}

uint16_t hf_unused_port(void) {
  uint16_t port = 0;
  const hf_socket_t probe = hf_listen(&port);
  if (probe == HF_NO_SOCKET)
    return 0;
  hf_close(probe);
  return port;
}

uint16_t hf_server_port(const hf_server_t *server) { return server->port; }

uint32_t hf_server_connections(hf_server_t *server) {
  hf_lock(&server->lock);
  const uint32_t connections = server->connections;
  hf_unlock(&server->lock);
  return connections;
}

void hf_server_last_request(hf_server_t *server, char *out, size_t capacity) {
  hf_lock(&server->lock);
  hf_copy(out, capacity, server->last_request);
  hf_unlock(&server->lock);
}

void hf_server_stop(hf_server_t **server) {
  if (server == NULL || *server == NULL)
    return;
  hf_server_t *owned = *server;
  *server = NULL;
  hf_lock(&owned->lock);
  owned->stop = true;
  hf_unlock(&owned->lock);
  hf_thread_join(owned->thread);
  hf_close(owned->listener);
  for (size_t index = 0; index < owned->path_count; ++index)
    free(owned->paths[index].data);
  hf_mutex_destroy(&owned->lock);
  free(owned);
}

typedef struct {
  char *text;
  size_t capacity;
  size_t length;
} hf_capture_t;

static hal_status_t hf_capture(void *context, jb_process_stream_t stream, const unsigned char *data,
                               size_t length) {
  (void)stream;
  hf_capture_t *capture = context;
  if (capture->capacity > 1u)
    hf_tail_append(capture->text, capture->capacity, &capture->length, data, length);
  return HAL_OK;
}

hal_status_t hf_openssl(const char *openssl, const char *directory, const char *const *arguments,
                        char *output, size_t capacity) {
  hf_capture_t capture = {output, capacity, 0u};
  if (capacity > 0u)
    output[0] = '\0';
  jb_process_options_t options;
  memset(&options, 0, sizeof(options));
  options.executable = openssl;
  options.arguments = arguments;
  options.working_directory = directory;
  options.timeout_ms = 120000u;
  options.output = hf_capture;
  options.context = &capture;
  jb_process_result_t result;
  hal_status_t status = jb_process_run(&options, &result);
  if (status == HAL_OK && (!result.exited || result.exit_code != 0u))
    status = HAL_EIO;
  return status;
}

static hal_status_t hf_write_text(const char *directory, const char *name, const char *text) {
  char path[4096];
  size_t required = 0;
  bool changed = false;
  hal_status_t status = jb_fs_path_join(directory, name, path, sizeof(path), &required);
  if (status == HAL_OK)
    status = jb_file_write_atomic(path, text, strlen(text), false, &changed);
  return status;
}

/* One configuration file serves req, x509 and ca; the first five %u are the
 * CRL port, the last two the port of a CRL server that never answers. */
static const char hf_config_format[] =
    "[ req ]\n"
    "distinguished_name = req_dn\n"
    "prompt = no\n"
    "[ req_dn ]\n"
    "CN = jhbase download test\n"
    "[ root_ca ]\n"
    "database = root-index.txt\n"
    "crlnumber = root-crlnumber\n"
    "default_md = sha256\n"
    "default_crl_days = 2\n"
    "[ other_ca ]\n"
    "database = other-index.txt\n"
    "crlnumber = other-crlnumber\n"
    "default_md = sha256\n"
    "default_crl_days = 2\n"
    "[ root_ext ]\n"
    "basicConstraints = critical,CA:true\n"
    "keyUsage = critical,keyCertSign,cRLSign\n"
    "subjectKeyIdentifier = hash\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/root.crl\n"
    "[ other_ext ]\n"
    "basicConstraints = critical,CA:true\n"
    "keyUsage = critical,keyCertSign,cRLSign\n"
    "subjectKeyIdentifier = hash\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/other.crl\n"
    "[ leaf_ext ]\n"
    "basicConstraints = critical,CA:false\n"
    "keyUsage = critical,digitalSignature\n"
    "extendedKeyUsage = serverAuth\n"
    "subjectKeyIdentifier = hash\n"
    "authorityKeyIdentifier = keyid\n"
    "subjectAltName = DNS:localhost\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/root.crl\n"
    "[ wrong_ext ]\n"
    "basicConstraints = critical,CA:false\n"
    "keyUsage = critical,digitalSignature\n"
    "extendedKeyUsage = serverAuth\n"
    "subjectKeyIdentifier = hash\n"
    "authorityKeyIdentifier = keyid\n"
    "subjectAltName = DNS:wrong.invalid\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/root.crl\n"
    "[ untrusted_ext ]\n"
    "basicConstraints = critical,CA:false\n"
    "keyUsage = critical,digitalSignature\n"
    "extendedKeyUsage = serverAuth\n"
    "subjectKeyIdentifier = hash\n"
    "authorityKeyIdentifier = keyid\n"
    "subjectAltName = DNS:localhost\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/other.crl\n"
    "[ stallca_ext ]\n"
    "basicConstraints = critical,CA:true\n"
    "keyUsage = critical,keyCertSign,cRLSign\n"
    "subjectKeyIdentifier = hash\n"
    "authorityKeyIdentifier = keyid\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/root.crl\n"
    "[ stall1_ext ]\n"
    "basicConstraints = critical,CA:false\n"
    "keyUsage = critical,digitalSignature\n"
    "extendedKeyUsage = serverAuth\n"
    "subjectKeyIdentifier = hash\n"
    "authorityKeyIdentifier = keyid\n"
    "subjectAltName = DNS:localhost\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/stall1.crl\n"
    "[ stall2_ext ]\n"
    "basicConstraints = critical,CA:false\n"
    "keyUsage = critical,digitalSignature\n"
    "extendedKeyUsage = serverAuth\n"
    "subjectKeyIdentifier = hash\n"
    "authorityKeyIdentifier = keyid\n"
    "subjectAltName = DNS:localhost\n"
    "crlDistributionPoints = URI:http://127.0.0.1:%u/stall2.crl\n";

hal_status_t hf_pki_create(const char *openssl, const char *directory, uint16_t crl_port,
                           uint16_t stall_port, char *output, size_t capacity) {
  char config[6144];
  const unsigned port = crl_port, stall = stall_port;
  const int written = snprintf(config, sizeof(config), hf_config_format, port, port, port, port,
                               port, port, stall, stall);
  if (written < 0 || (size_t)written >= sizeof(config))
    return HAL_EOVERFLOW;
  hal_status_t status = hf_write_text(directory, "test.cnf", config);
  if (status == HAL_OK)
    status = hf_write_text(directory, "root-index.txt", "");
  if (status == HAL_OK)
    status = hf_write_text(directory, "other-index.txt", "");
  if (status == HAL_OK)
    status = hf_write_text(directory, "root-crlnumber", "01\n");
  if (status == HAL_OK)
    status = hf_write_text(directory, "other-crlnumber", "01\n");
#define HF_KEY(name)                                                                               \
  "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", name
#define HF_CA(name, subject, serial)                                                               \
  "req", "-config", "test.cnf", "-new", "-x509", "-key", name ".key", "-subj", subject, "-days",   \
      "2", "-sha256", "-set_serial", serial, "-extensions", name "_ext", "-out", name ".pem"
#define HF_LEAF(name, issuer, serial)                                                              \
  "x509", "-req", "-in", "leaf.csr", "-CA", issuer ".pem", "-CAkey", issuer ".key", "-set_serial", \
      serial, "-days", "2", "-sha256", "-extfile", "test.cnf", "-extensions", name "_ext", "-out", \
      name ".pem"
  static const char *const commands[][26] = {
      {HF_KEY("root.key"), NULL},
      {HF_KEY("other.key"), NULL},
      {HF_KEY("leaf.key"), NULL},
      {HF_KEY("stallca.key"), NULL},
      {HF_CA("root", "/CN=jhbase test root", "1"), NULL},
      {HF_CA("other", "/CN=jhbase untrusted root", "2"), NULL},
      {"req", "-config", "test.cnf", "-new", "-key", "leaf.key", "-subj", "/CN=localhost", "-out",
       "leaf.csr", NULL},
      {HF_LEAF("leaf", "root", "16"), NULL},
      {HF_LEAF("wrong", "root", "17"), NULL},
      {"x509",     "-req",        "-in",      "leaf.csr", "-CA",         "root.pem", "-CAkey",
       "root.key", "-set_serial", "18",       "-days",    "2",           "-sha256",  "-extfile",
       "test.cnf", "-extensions", "leaf_ext", "-out",     "revoked.pem", NULL},
      {HF_LEAF("untrusted", "other", "19"), NULL},
      /* Windows matches CRLs by issuer, so the stall leaves need an issuer
       * whose CRL was never fetched: an intermediate under the root. */
      {"req", "-config", "test.cnf", "-new", "-key", "stallca.key", "-subj",
       "/CN=jhbase stall intermediate", "-out", "stallca.csr", NULL},
      {"x509",        "-req",        "-in",      "stallca.csr", "-CA",
       "root.pem",    "-CAkey",      "root.key", "-set_serial", "22",
       "-days",       "2",           "-sha256",  "-extfile",    "test.cnf",
       "-extensions", "stallca_ext", "-out",     "stallca.pem", NULL},
      {HF_LEAF("stall1", "stallca", "20"), NULL},
      {HF_LEAF("stall2", "stallca", "21"), NULL},
      {"ca", "-config", "test.cnf", "-name", "root_ca", "-keyfile", "root.key", "-cert", "root.pem",
       "-revoke", "revoked.pem", NULL},
      {"ca", "-config", "test.cnf", "-name", "root_ca", "-keyfile", "root.key", "-cert", "root.pem",
       "-gencrl", "-out", "root-crl.pem", NULL},
      {"ca", "-config", "test.cnf", "-name", "other_ca", "-keyfile", "other.key", "-cert",
       "other.pem", "-gencrl", "-out", "other-crl.pem", NULL},
      {"crl", "-in", "root-crl.pem", "-outform", "DER", "-out", "root.crl", NULL},
      {"crl", "-in", "other-crl.pem", "-outform", "DER", "-out", "other.crl", NULL},
  };
#undef HF_KEY
#undef HF_CA
#undef HF_LEAF
  for (size_t index = 0; index < COUNTOF(commands) && status == HAL_OK; ++index)
    status = hf_openssl(openssl, directory, commands[index], output, capacity);
  return status;
}

struct hf_tls {
  hf_mutex_t lock;
  hf_thread_handle_t *thread;
  bool stop, ready, finished;
  hal_status_t status;
  uint16_t port;
  char accept[32];
  char *openssl, *directory, *certificate, *key, *chain;
  const char *arguments[11];
  char tail[HF_TAIL];
  size_t tail_length;
};

static hal_status_t hf_tls_output(void *context, jb_process_stream_t stream,
                                  const unsigned char *data, size_t length) {
  (void)stream;
  hf_tls_t *tls = context;
  hf_lock(&tls->lock);
  hf_tail_append(tls->tail, sizeof(tls->tail), &tls->tail_length, data, length);
  if (strstr(tls->tail, "ACCEPT\n") != NULL)
    tls->ready = true;
  hf_unlock(&tls->lock);
  return HAL_OK;
}

static bool hf_tls_canceled(void *context) {
  hf_tls_t *tls = context;
  hf_lock(&tls->lock);
  const bool stop = tls->stop;
  hf_unlock(&tls->lock);
  return stop;
}

static void hf_tls_loop(void *context) {
  hf_tls_t *tls = context;
  jb_process_options_t options;
  memset(&options, 0, sizeof(options));
  options.executable = tls->openssl;
  options.arguments = tls->arguments;
  options.working_directory = tls->directory;
  options.output = hf_tls_output;
  options.canceled = hf_tls_canceled;
  options.context = tls;
  jb_process_result_t result;
  const hal_status_t status = jb_process_run(&options, &result);
  hf_lock(&tls->lock);
  tls->finished = true;
  tls->status = status;
  hf_unlock(&tls->lock);
}

static char *hf_duplicate(const char *text) {
  const size_t length = strlen(text) + 1u;
  char *copy = malloc(length);
  if (copy != NULL)
    memcpy(copy, text, length);
  return copy;
}

static void hf_tls_free(hf_tls_t *tls) {
  free(tls->openssl);
  free(tls->directory);
  free(tls->certificate);
  free(tls->key);
  free(tls->chain);
  hf_mutex_destroy(&tls->lock);
  free(tls);
}

static void hf_tls_join(hf_tls_t *tls) {
  hf_lock(&tls->lock);
  tls->stop = true;
  hf_unlock(&tls->lock);
  hf_thread_join(tls->thread);
}

hf_tls_t *hf_tls_start(const char *openssl, const char *www_directory, const char *certificate,
                       const char *key, const char *chain, char *output, size_t capacity) {
  if (capacity > 0u)
    output[0] = '\0';
  hf_tls_t *tls = calloc(1u, sizeof(*tls));
  if (tls == NULL)
    return NULL;
  hf_mutex_init(&tls->lock);
  tls->openssl = hf_duplicate(openssl);
  tls->directory = hf_duplicate(www_directory);
  tls->certificate = hf_duplicate(certificate);
  tls->key = hf_duplicate(key);
  tls->chain = chain != NULL ? hf_duplicate(chain) : NULL;
  /* A free port: the short gap until s_server binds it is acceptable here. */
  tls->port = hf_unused_port();
  if (tls->openssl == NULL || tls->directory == NULL || tls->certificate == NULL ||
      tls->key == NULL || (chain != NULL && tls->chain == NULL) || tls->port == 0u) {
    hf_tls_free(tls);
    return NULL;
  }
  (void)snprintf(tls->accept, sizeof(tls->accept), "127.0.0.1:%u", (unsigned)tls->port);
  /* The intermediate goes to the client with the leaf; NULL ends the list early. */
  const char *const arguments[COUNTOF(tls->arguments)] = {"s_server",
                                                          "-accept",
                                                          tls->accept,
                                                          "-cert",
                                                          tls->certificate,
                                                          "-key",
                                                          tls->key,
                                                          "-HTTP",
                                                          tls->chain != NULL ? "-cert_chain" : NULL,
                                                          tls->chain,
                                                          NULL};
  for (size_t index = 0; index < COUNTOF(arguments); ++index)
    tls->arguments[index] = arguments[index];
  tls->thread = hf_thread_start(hf_tls_loop, tls);
  if (tls->thread == NULL) {
    hf_tls_free(tls);
    return NULL;
  }
  bool ready = false, finished = false;
  for (uint32_t waited = 0; waited < HF_TLS_START_MS && !ready && !finished; waited += 20u) {
    hf_sleep_ms(20u);
    hf_lock(&tls->lock);
    ready = tls->ready;
    finished = tls->finished;
    hf_unlock(&tls->lock);
  }
  if (!ready || finished) {
    hf_tls_join(tls);
    if (capacity > 0u)
      hf_copy(output, capacity, tls->tail);
    hf_tls_free(tls);
    return NULL;
  }
  return tls;
}

uint16_t hf_tls_port(const hf_tls_t *tls) { return tls->port; }

void hf_tls_stop(hf_tls_t **tls) {
  if (tls == NULL || *tls == NULL)
    return;
  hf_tls_t *owned = *tls;
  *tls = NULL;
  hf_tls_join(owned);
  hf_tls_free(owned);
}

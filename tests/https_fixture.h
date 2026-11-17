#ifndef JB_HTTPS_FIXTURE_H
#define JB_HTTPS_FIXTURE_H
/* Local HTTPS servers for the download tests, without public network.
 * TLS comes from `openssl s_server -HTTP` processes, an implementation
 * independent of the code under test; the certificates and CRLs come from the
 * openssl tool. In-process plain TCP helpers inject faults (router), tunnel
 * CONNECT requests (proxy) and serve CRLs over HTTP (Schannel fetches them to
 * check revocation). All helpers listen on 127.0.0.1 only. */
#include "jb/status.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hf_server hf_server_t;
typedef struct hf_tls hf_tls_t;
typedef struct hf_thread hf_thread_handle_t;

/** What the router does with one accepted connection. */
typedef enum {
  HF_FORWARD = 1, /* relay to port */
  HF_CLOSE,       /* close at once */
  HF_STALL,       /* keep open without sending anything */
  HF_CUT,         /* relay to port, close after cut_after bytes towards the client */
  HF_HOLD         /* relay to port, then stop after cut_after bytes and keep it open */
} hf_action_kind_t;

typedef struct {
  hf_action_kind_t kind;
  uint16_t port;
  size_t cut_after;
} hf_action_t;

/* Process-wide socket setup (Winsock); call once before servers. */
bool hf_init(void);
void hf_shutdown(void);
/* Monotonic milliseconds and a plain sleep. */
uint64_t hf_now_ms(void);
void hf_sleep_ms(uint32_t milliseconds);
/* Run work(context) on a new thread; join it exactly once. */
hf_thread_handle_t *hf_thread_start(void (*work)(void *context), void *context);
void hf_thread_join(hf_thread_handle_t *thread);

/* Plan-driven fault injection: each accepted connection takes the next plan
 * entry, then the fallback. */
hf_server_t *hf_router_start(void);
void hf_router_plan(hf_server_t *server, const hf_action_t *actions, size_t count,
                    hf_action_t fallback);
/* HTTP proxy: answers one CONNECT localhost:<port> and tunnels to that port;
 * 502 when that port refuses the connection. */
hf_server_t *hf_proxy_start(void);
/* Answer 407 with this Proxy-Authenticate challenge unless the CONNECT has a
 * Proxy-Authorization header starting with credentials; NULL challenge accepts
 * every CONNECT again. */
void hf_proxy_require(hf_server_t *server, const char *challenge, const char *credentials);
/* A loopback port nothing listens on at the moment of the call. */
uint16_t hf_unused_port(void);
/* Plain HTTP responder for registered paths; others get 404. */
hf_server_t *hf_http_start(void);
bool hf_http_set(hf_server_t *server, const char *path, const unsigned char *data, size_t length);

uint16_t hf_server_port(const hf_server_t *server);
uint32_t hf_server_connections(hf_server_t *server);
/* First request line of the last connection that sent one (CONNECT or GET). */
void hf_server_last_request(hf_server_t *server, char *out, size_t capacity);
void hf_server_stop(hf_server_t **server);

/* Run the openssl tool in directory and wait; HAL_OK only for exit code 0.
 * output receives the tail of stdout/stderr for diagnostics. */
hal_status_t hf_openssl(const char *openssl, const char *directory, const char *const *arguments,
                        char *output, size_t capacity);

/* Root and untrusted CAs, leaves for localhost (good, revoked, untrusted
 * issuer) and wrong.invalid; CRL distribution points name
 * http://127.0.0.1:<crl_port>/root.crl and /other.crl. Two more localhost
 * leaves, stall1.pem and stall2.pem, come from the intermediate stallca.pem
 * and name /stall1.crl and /stall2.crl on stall_port. Files in directory: root.pem, root.crl,
 * other.crl (DER), leaf.pem, wrong.pem, revoked.pem, untrusted.pem, stall1.pem, stall2.pem and
 * leaf.key. */
hal_status_t hf_pki_create(const char *openssl, const char *directory, uint16_t crl_port,
                           uint16_t stall_port, char *output, size_t capacity);

/* openssl s_server -HTTP serving files from www_directory (relative names of
 * complete HTTP responses) with certificate, key and optional chain (NULL for
 * none) paths relative to it. */
hf_tls_t *hf_tls_start(const char *openssl, const char *www_directory, const char *certificate,
                       const char *key, const char *chain, char *output, size_t capacity);
uint16_t hf_tls_port(const hf_tls_t *tls);
void hf_tls_stop(hf_tls_t **tls);
#endif
